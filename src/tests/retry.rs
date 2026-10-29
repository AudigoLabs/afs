use std::vec;
use std::vec::Vec;

use super::storage::{Fault, MemStorage, TestAfs, block_on, buffer, mount, random_data};
use crate::{Config, Error, ListCursor, ObjectId, Stream};

const CONFIG: Config = Config { block_size: 8192, num_blocks: 16, sub_blocks_per_block: 4, min_read_write_size: 256 };

fn write_object(afs: &mut TestAfs, chunks: &[(Stream, Vec<u8>)]) -> ObjectId {
    let mut writer = afs.create(buffer(512)).unwrap();
    for (stream, data) in chunks {
        block_on(writer.write(afs, *stream, data)).unwrap();
    }
    let id = writer.id();
    block_on(writer.close(afs)).unwrap();
    id
}

fn chunks(seed: u64) -> Vec<(Stream, Vec<u8>)> {
    (0..12u64)
        .map(|i| (Stream::from_index((i % 3) as u8).unwrap(), random_data(700 + i as usize * 211, seed + i)))
        .collect()
}

fn stream_data(chunks: &[(Stream, Vec<u8>)], stream: Stream) -> Vec<u8> {
    chunks.iter().filter(|(s, _)| *s == stream).flat_map(|(_, d)| d.iter().copied()).collect()
}

fn read_stream(afs: &mut TestAfs, id: ObjectId, stream: Stream) -> Vec<u8> {
    let mut reader = block_on(afs.open(id, stream, buffer(512))).unwrap();
    let mut out = Vec::new();
    let mut chunk = vec![0u8; 333];
    loop {
        let n = block_on(reader.read(afs, &mut chunk)).unwrap();
        if n == 0 {
            break;
        }
        out.extend_from_slice(&chunk[..n]);
    }
    reader.close(afs);
    out
}

#[test]
fn reads_resume_after_storage_errors() {
    let storage = MemStorage::new(CONFIG);
    let mut afs = mount(&storage);
    let chunks = chunks(1);
    let id = write_object(&mut afs, &chunks);
    for fault in 0..40 {
        let mut afs = mount(&storage);
        let mut reader = block_on(afs.open(id, Stream::S1, buffer(512))).unwrap();
        storage.fail_once_after(fault);
        let mut out = Vec::new();
        let mut chunk = vec![0u8; 333];
        loop {
            match block_on(reader.read(&mut afs, &mut chunk)) {
                Ok(0) => break,
                Ok(n) => out.extend_from_slice(&chunk[..n]),
                Err(error) => assert_eq!(error, Error::Storage(Fault)),
            }
        }
        assert_eq!(out, stream_data(&chunks, Stream::S1), "fault after {fault} ops");
        reader.close(&mut afs);
        storage.clear_fault();
    }
}

#[test]
fn close_resumes_after_storage_errors() {
    let mut completed_without_fault = false;
    for fault in 0..200 {
        let storage = MemStorage::new(CONFIG);
        let mut afs = mount(&storage);
        let chunks = chunks(2);
        let mut writer = afs.create(buffer(512)).unwrap();
        for (stream, data) in &chunks {
            block_on(writer.write(&mut afs, *stream, data)).unwrap();
        }
        let id = writer.id();
        storage.fail_once_after(fault);
        let mut attempts = 0;
        loop {
            attempts += 1;
            match block_on(writer.close(&mut afs)) {
                Ok(_) => break,
                Err(error) => {
                    assert_eq!(error.error, Error::Storage(Fault));
                    writer = error.writer;
                }
            }
        }
        completed_without_fault |= storage.clear_fault() && attempts == 1;
        let blocks = afs.num_blocks(id);
        assert_eq!(afs.size(), blocks, "fault after {fault} ops");
        let mut afs = mount(&storage);
        assert_eq!(afs.size(), blocks);
        for stream in [Stream::S0, Stream::S1, Stream::S2] {
            assert_eq!(read_stream(&mut afs, id, stream), stream_data(&chunks, stream), "fault after {fault} ops");
        }
        if completed_without_fault {
            return;
        }
    }
    panic!("close never completed without hitting the fault");
}

#[test]
fn first_flush_retries_reuse_the_failed_allocation() {
    for fault in 0..2 {
        let storage = MemStorage::new(CONFIG);
        let mut afs = mount(&storage);
        let data = random_data(100, 3);
        let mut writer = afs.create(buffer(512)).unwrap();
        block_on(writer.write(&mut afs, Stream::S0, &data)).unwrap();
        let id = writer.id();
        storage.fail_once_after(fault);
        let error = block_on(writer.close(&mut afs)).unwrap_err();
        assert_eq!(error.error, Error::Storage(Fault));
        assert_eq!(afs.size(), fault as u16);
        block_on(error.writer.close(&mut afs)).unwrap();
        assert_eq!(afs.size(), 1);
        assert_eq!(afs.num_blocks(id), 1);
        let mut afs = mount(&storage);
        assert_eq!(read_stream(&mut afs, id, Stream::S0), data);
    }
}

fn list(afs: &TestAfs) -> Vec<ObjectId> {
    let mut cursor = ListCursor::new();
    std::iter::from_fn(|| afs.list_next(&mut cursor)).collect()
}

#[test]
fn wipe_resumes_after_storage_errors() {
    for secure in [false, true] {
        for fault in 0..12 {
            let storage = MemStorage::new(CONFIG);
            let mut afs = mount(&storage);
            for seed in 0..3 {
                write_object(&mut afs, &chunks(seed * 100));
            }
            storage.fail_once_after(fault);
            if block_on(afs.wipe(secure)).is_err() {
                block_on(afs.wipe(secure)).unwrap();
            }
            storage.clear_fault();
            assert!(list(&afs).is_empty());
            assert_eq!(afs.size(), 0);
            if secure {
                assert!(storage.is_empty(), "fault after {fault} ops");
            }
            let afs = mount(&storage);
            assert!(list(&afs).is_empty(), "fault after {fault} ops");
        }
    }
}

#[test]
fn prepare_storage_only_counts_blocks_that_were_erased() {
    for fault in 0..6 {
        let storage = MemStorage::new(CONFIG);
        let mut afs = mount(&storage);
        for seed in 0..2 {
            let id = write_object(&mut afs, &chunks(seed * 100));
            block_on(afs.delete(id)).unwrap();
        }
        storage.fail_once_after(fault);
        let _ = block_on(afs.prepare_storage(CONFIG.num_blocks));
        storage.clear_fault();
        for block in 0..CONFIG.num_blocks {
            if afs.lookup.raw_value(block) == 0 {
                let start = block as u64 * CONFIG.block_size as u64;
                let end = start + CONFIG.block_size as u64;
                assert_eq!(storage.first_nonzero(start, end), None, "block {block} marked erased, fault after {fault}");
            }
        }
    }
}
