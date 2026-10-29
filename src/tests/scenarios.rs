use std::vec;
use std::vec::Vec;

use super::expect::Expect;
use super::storage::{BLOCK_SIZE, CONFIG, MemStorage, TestAfs, block_on, buffer, mount, random_data};
use crate::{ListCursor, ObjectId, Stream, StreamSet};

struct Fixture {
    storage: MemStorage,
    afs: TestAfs,
}

impl Fixture {
    fn new() -> Self {
        let storage = MemStorage::new(CONFIG);
        let afs = mount(&storage);
        Self { storage, afs }
    }

    fn remount(&mut self) {
        assert!(!self.afs.has_open_objects());
        self.afs = mount(&self.storage);
    }

    fn set_next_object_id(&mut self, object_id: ObjectId) -> bool {
        for _ in 0..=u16::MAX as u32 {
            let seed = self.afs.lookup.seed;
            if self.afs.lookup.next_object_id() == object_id.get() {
                self.afs.lookup.seed = seed;
                return true;
            }
        }
        false
    }

    fn write_object(&mut self, buffer_size: usize, writes: &[(Stream, &[u8])]) -> ObjectId {
        let mut writer = self.afs.create(buffer(buffer_size)).unwrap();
        for (stream, data) in writes {
            block_on(writer.write(&mut self.afs, *stream, data)).unwrap();
        }
        let id = writer.id();
        block_on(writer.close(&mut self.afs)).unwrap();
        id
    }

    fn list(&self) -> Vec<ObjectId> {
        let mut cursor = ListCursor::new();
        std::iter::from_fn(|| self.afs.list_next(&mut cursor)).collect()
    }
}

fn generate_v1_block(storage: &MemStorage, block: u16, object_id: u16, data: &[u8]) {
    let mut raw = Vec::new();
    raw.extend_from_slice(b"AFS1");
    raw.extend_from_slice(&object_id.to_le_bytes());
    raw.extend_from_slice(&0u16.to_le_bytes());
    for stream in [1u32, 2] {
        raw.extend_from_slice(&((0xd0 | stream) << 24 | data.len() as u32).to_le_bytes());
        raw.extend_from_slice(data);
    }
    raw.extend_from_slice(&(0xedu32 << 24).to_le_bytes());
    storage.raw_write(block as u64 * BLOCK_SIZE as u64, &raw);
}

fn s(index: u32) -> u32 {
    index << 28
}

fn o(index: u64) -> u64 {
    index << 60
}

#[test]
fn empty() {
    let f = Fixture::new();
    Expect::start(&f.storage).end();
}

#[test]
fn read_v1() {
    let mut f = Fixture::new();
    let object_id = ObjectId::new(0x1234).unwrap();
    let write_data = random_data(8, 1);
    generate_v1_block(&f.storage, 0, object_id.get(), &write_data);
    f.remount();

    Expect::start(&f.storage)
        .block_header_v1(object_id.get(), 0)
        .data_chunk(1, &write_data)
        .data_chunk(2, &write_data)
        .end_chunk()
        .end();

    for stream in [Stream::S1, Stream::S2] {
        let mut reader = block_on(f.afs.open(object_id, stream, buffer(1024))).unwrap();
        assert_eq!(block_on(reader.size(&mut f.afs)).unwrap(), write_data.len() as u64);
        let mut read_data = vec![0u8; write_data.len()];
        assert_eq!(block_on(reader.read(&mut f.afs, &mut read_data)).unwrap(), read_data.len());
        assert_eq!(read_data, write_data);
        assert_eq!(block_on(reader.read(&mut f.afs, &mut read_data)).unwrap(), 0);
        reader.close(&mut f.afs);
    }

    let mut reader = block_on(f.afs.open_all(object_id, buffer(1024))).unwrap();
    assert_eq!(block_on(reader.size(&mut f.afs, StreamSet::ALL)).unwrap(), write_data.len() as u64 * 2);
    let mut read_data = vec![0u8; write_data.len()];
    for stream in [Stream::S1, Stream::S2] {
        assert_eq!(block_on(reader.read(&mut f.afs, &mut read_data)).unwrap(), Some((stream, read_data.len())));
        assert_eq!(read_data, write_data);
    }
    assert_eq!(block_on(reader.read(&mut f.afs, &mut read_data)).unwrap(), None);
    reader.close(&mut f.afs);
}

#[test]
fn write_single_small_chunk() {
    let mut f = Fixture::new();
    let write_data = random_data(8, 2);
    let object_id = f.write_object(1024, &[(Stream::S0, &write_data)]);

    Expect::start(&f.storage)
        .block_header(object_id.get(), 0)
        .data_chunk(0, &write_data)
        .end_chunk()
        .unused_until_footer()
        .footer()
        .seek_chunk(&[write_data.len() as u32])
        .unused_until_block_end()
        .end();

    let mut reader = block_on(f.afs.open(object_id, Stream::S0, buffer(1024))).unwrap();
    assert_eq!(block_on(reader.size(&mut f.afs)).unwrap(), write_data.len() as u64);
    let mut read_data = vec![0u8; write_data.len()];
    assert_eq!(block_on(reader.read(&mut f.afs, &mut read_data)).unwrap(), read_data.len());
    assert_eq!(read_data, write_data);
    assert_eq!(block_on(reader.read(&mut f.afs, &mut read_data)).unwrap(), 0);
    reader.close(&mut f.afs);
}

#[test]
fn write_multiple_streams_small_chunk() {
    let mut f = Fixture::new();
    let write_data = random_data(8, 3);
    let pattern = [1u8, 1, 2, 1, 2, 2, 1];
    let writes: Vec<(Stream, &[u8])> =
        pattern.iter().map(|&i| (Stream::from_index(i).unwrap(), write_data.as_slice())).collect();
    let object_id = f.write_object(1024, &writes);

    let mut expect = Expect::start(&f.storage);
    expect.block_header(object_id.get(), 0);
    for &stream in &pattern {
        expect.data_chunk(stream, &write_data);
    }
    expect
        .end_chunk()
        .unused_until_footer()
        .footer()
        .seek_chunk(&[s(1) | 32, s(2) | 24])
        .unused_until_block_end()
        .end();

    let mut reader = block_on(f.afs.open_all(object_id, buffer(1024))).unwrap();
    let size = |reader: &mut crate::ObjectReader<'_>, afs: &mut TestAfs, bits| {
        block_on(reader.size(afs, StreamSet::from_bits(bits))).unwrap()
    };
    assert_eq!(size(&mut reader, &mut f.afs, 1 << 1), 8 * 4);
    assert_eq!(size(&mut reader, &mut f.afs, 1 << 2), 8 * 3);
    assert_eq!(size(&mut reader, &mut f.afs, 1 << 1 | 1 << 2), 8 * 7);
    let mut read_data = vec![0u8; 8];
    for &stream in &pattern {
        let result = block_on(reader.read(&mut f.afs, &mut read_data)).unwrap();
        assert_eq!(result, Some((Stream::from_index(stream).unwrap(), 8)));
        assert_eq!(read_data, write_data);
    }
    assert_eq!(block_on(reader.read(&mut f.afs, &mut read_data)).unwrap(), None);
    reader.close(&mut f.afs);
}

#[test]
fn write_single_large_chunk() {
    let mut f = Fixture::new();
    let write_data = random_data(1024 * 1024, 4);
    let object_id = f.write_object(1024, &[(Stream::S0, &write_data)]);

    Expect::start(&f.storage)
        .block_header(object_id.get(), 0)
        .data_chunk(0, &write_data[..0x7fff4])
        .seek_chunk(&[0x7fff4])
        .data_chunk(0, &write_data[0x7fff4..0x7fff4 * 2])
        .seek_chunk(&[0xfffe8])
        .data_chunk(0, &write_data[0xfffe8..0xfffe8 + 0x18])
        .end_chunk()
        .unused_until_footer()
        .footer()
        .seek_chunk(&[write_data.len() as u32])
        .unused_until_block_end()
        .end();

    let mut reader = block_on(f.afs.open(object_id, Stream::S0, buffer(1024))).unwrap();
    assert_eq!(block_on(reader.size(&mut f.afs)).unwrap(), write_data.len() as u64);
    let mut read_data = vec![0u8; write_data.len()];
    assert_eq!(block_on(reader.read(&mut f.afs, &mut read_data)).unwrap(), read_data.len());
    assert_eq!(read_data, write_data);
    assert_eq!(block_on(reader.read(&mut f.afs, &mut read_data)).unwrap(), 0);
    reader.close(&mut f.afs);
}

#[test]
fn write_multiple_large_chunks() {
    let mut f = Fixture::new();
    let d = random_data(1024 * 1024, 5);
    let writes: Vec<(Stream, &[u8])> =
        (0..10).map(|i| (Stream::from_index(i % 2 + 1).unwrap(), d.as_slice())).collect();
    let object_id = f.write_object(1024, &writes);
    let id = object_id.get();

    Expect::start(&f.storage)
        .block_header(id, 0)
        .data_chunk(1, &d[..0x7fff4])
        .seek_chunk(&[s(1) | 0x7fff4])
        .data_chunk(1, &d[0x7fff4..0x7fff4 + 0x7fff4])
        .seek_chunk(&[s(1) | 0xfffe8])
        .data_chunk(1, &d[0xfffe8..0xfffe8 + 0x18])
        .data_chunk(2, &d[..0x7ffd8])
        .seek_chunk(&[s(1) | 0x100000, s(2) | 0x7ffd8])
        .data_chunk(2, &d[0x7ffd8..0x7ffd8 + 0x7fff0])
        .seek_chunk(&[s(1) | 0x100000, s(2) | 0xfffc8])
        .data_chunk(2, &d[0xfffc8..0xfffc8 + 0x38])
        .data_chunk(1, &d[..0x7ffb4])
        .seek_chunk(&[s(1) | 0x17ffb4, s(2) | 0x100000])
        .data_chunk(1, &d[0x7ffb4..0x7ffb4 + 0x7fff0])
        .seek_chunk(&[s(1) | 0x1fffa4, s(2) | 0x100000])
        .data_chunk(1, &d[0xfffa4..0xfffa4 + 0x5c])
        .data_chunk(2, &d[..0x7ff90])
        .seek_chunk(&[s(1) | 0x200000, s(2) | 0x17ff90])
        .data_chunk(2, &d[0x7ff90..0x7ff90 + 0x7ff70])
        .footer()
        .seek_chunk(&[s(1) | 0x200000, s(2) | 0x1fff00])
        .unused_until_block_end()
        .block_header(id, 1)
        .offset_chunk(&[o(1) | 0x200000, o(2) | 0x1fff00])
        .data_chunk(2, &d[0xfff00..0xfff00 + 0x100])
        .data_chunk(1, &d[..0x7fedc])
        .seek_chunk(&[s(1) | 0x7fedc, s(2) | 0x100])
        .data_chunk(1, &d[0x7fedc..0x7fedc + 0x7fff0])
        .seek_chunk(&[s(1) | 0xffecc, s(2) | 0x100])
        .data_chunk(1, &d[0xffecc..0xffecc + 0x134])
        .data_chunk(2, &d[..0x7feb8])
        .seek_chunk(&[s(1) | 0x100000, s(2) | 0x7ffb8])
        .data_chunk(2, &d[0x7feb8..0x7feb8 + 0x7fff0])
        .seek_chunk(&[s(1) | 0x100000, s(2) | 0xfffa8])
        .data_chunk(2, &d[0xffea8..0xffea8 + 0x158])
        .data_chunk(1, &d[..0x7fe94])
        .seek_chunk(&[s(1) | 0x17fe94, s(2) | 0x100100])
        .data_chunk(1, &d[0x7fe94..0x7fe94 + 0x7fff0])
        .seek_chunk(&[s(1) | 0x1ffe84, s(2) | 0x100100])
        .data_chunk(1, &d[0xffe84..0xffe84 + 0x17c])
        .data_chunk(2, &d[..0x7fe70])
        .seek_chunk(&[s(1) | 0x200000, s(2) | 0x17ff70])
        .data_chunk(2, &d[0x7fe70..0x7fe70 + 0x7ff70])
        .footer()
        .seek_chunk(&[s(1) | 0x200000, s(2) | 0x1ffee0])
        .unused_until_block_end()
        .block_header(id, 2)
        .offset_chunk(&[o(1) | 0x400000, o(2) | 0x3ffde0])
        .data_chunk(2, &d[0xffde0..0xffde0 + 0x220])
        .data_chunk(1, &d[..0x7fdbc])
        .seek_chunk(&[s(1) | 0x7fdbc, s(2) | 0x220])
        .data_chunk(1, &d[0x7fdbc..0x7fdbc + 0x7fff0])
        .seek_chunk(&[s(1) | 0xffdac, s(2) | 0x220])
        .data_chunk(1, &d[0xffdac..0xffdac + 0x254])
        .data_chunk(2, &d[..0x7fd98])
        .seek_chunk(&[s(1) | 0x100000, s(2) | 0x7ffb8])
        .data_chunk(2, &d[0x7fd98..0x7fd98 + 0x7fff0])
        .seek_chunk(&[s(1) | 0x100000, s(2) | 0xfffa8])
        .data_chunk(2, &d[0xffd88..0xffd88 + 0x278])
        .end_chunk()
        .unused_until_footer()
        .footer()
        .seek_chunk(&[s(1) | 0x100000, s(2) | 0x100220])
        .unused_until_block_end()
        .end();

    let mut reader = block_on(f.afs.open_all(object_id, buffer(1024))).unwrap();
    assert_eq!(block_on(reader.size(&mut f.afs, StreamSet::from_bits(0x6))).unwrap(), d.len() as u64 * 10);
    let mut read_data = vec![0u8; d.len()];
    for i in 0..10u8 {
        let mut length = 0;
        while length < read_data.len() {
            let (stream, n) = block_on(reader.read(&mut f.afs, &mut read_data[length..])).unwrap().unwrap();
            assert_eq!(stream.index(), i % 2 + 1);
            length += n;
        }
        assert_eq!(read_data, d);
    }
    assert_eq!(block_on(reader.read(&mut f.afs, &mut read_data)).unwrap(), None);
    reader.close(&mut f.afs);
}

fn cumulative(sizes: &[usize]) -> Vec<u32> {
    sizes
        .iter()
        .scan(0u32, |sum, &size| {
            *sum += size as u32;
            Some(*sum)
        })
        .collect()
}

fn check_sequential_reads(f: &mut Fixture, object_id: ObjectId, buffer_size: usize, data: &[u8], sizes: &[usize]) {
    let total: usize = sizes.iter().sum();
    let mut reader = block_on(f.afs.open(object_id, Stream::S0, buffer(buffer_size))).unwrap();
    assert_eq!(block_on(reader.size(&mut f.afs)).unwrap(), total as u64);
    let mut read_data = vec![0u8; data.len()];
    for &size in sizes {
        assert_eq!(block_on(reader.read(&mut f.afs, &mut read_data[..size])).unwrap(), size);
        assert_eq!(read_data[..size], data[..size]);
    }
    assert_eq!(block_on(reader.read(&mut f.afs, &mut read_data)).unwrap(), 0);
    reader.close(&mut f.afs);
}

#[test]
fn empty_space_at_end_of_regions() {
    let mut f = Fixture::new();
    let d = random_data(512 * 1024, 6);
    let sizes = [0x7fff0, 0x7fff2, 0x7fff3, 0x7fff4, 0x7fff4, 0x7fff4, 0x7fff4, 0x7ff73, 0x100];
    let writes: Vec<(Stream, &[u8])> = sizes.iter().map(|&n| (Stream::S0, &d[..n])).collect();
    let object_id = f.write_object(16 * 1024, &writes);
    let c = cumulative(&sizes);

    let mut e = Expect::start(&f.storage);
    e.block_header(object_id.get(), 0);
    for i in 0..7 {
        e.data_chunk(0, &d[..sizes[i]]).unused(0x7fff4 - sizes[i] as u64).seek_chunk(&[c[i]]);
    }
    e.data_chunk(0, &d[..sizes[7]])
        .unused(0x7ff74 - sizes[7] as u64)
        .footer()
        .seek_chunk(&[c[7]])
        .unused_until_block_end()
        .block_header(object_id.get(), 1)
        .offset_chunk(&[c[7] as u64])
        .data_chunk(0, &d[..sizes[8]])
        .end_chunk()
        .unused_until_footer()
        .footer()
        .seek_chunk(&[sizes[8] as u32])
        .unused_until_block_end()
        .end();

    check_sequential_reads(&mut f, object_id, 16 * 1024, &d, &sizes);
}

#[test]
fn small_data_at_end_of_block() {
    let mut f = Fixture::new();
    let d = random_data(512 * 1024, 7);
    let sizes = [0x7fff4, 0x7fff4, 0x7fff4, 0x7fff4, 0x7fff4, 0x7fff4, 0x7fff4, 0x7ff6e, 0x2, 0x100];
    let writes: Vec<(Stream, &[u8])> = sizes.iter().map(|&n| (Stream::S0, &d[..n])).collect();
    let object_id = f.write_object(1024, &writes);
    let c = cumulative(&sizes);

    let mut e = Expect::start(&f.storage);
    e.block_header(object_id.get(), 0);
    for i in 0..7 {
        e.data_chunk(0, &d[..sizes[i]]).seek_chunk(&[c[i]]);
    }
    e.data_chunk(0, &d[..sizes[7]])
        .data_chunk(0, &d[..sizes[8]])
        .footer()
        .seek_chunk(&[c[8]])
        .unused_until_block_end()
        .block_header(object_id.get(), 1)
        .offset_chunk(&[c[8] as u64])
        .data_chunk(0, &d[..sizes[9]])
        .end_chunk()
        .unused_until_footer()
        .footer()
        .seek_chunk(&[sizes[9] as u32])
        .unused_until_block_end()
        .end();

    check_sequential_reads(&mut f, object_id, 1024, &d, &sizes);
}

#[test]
fn tail_stream_read() {
    let mut f = Fixture::new();
    let d = random_data(1024 * 1024, 8);
    let manifest: Vec<u8> = (0..50u32).map(|i| (i * 7 + 13) as u8).collect();
    let mut writes: Vec<(Stream, &[u8])> = (0..143).map(|_| (Stream::S0, d.as_slice())).collect();
    writes.push((Stream::S5, &manifest));
    let object_id = f.write_object(16 * 1024, &writes);
    let total_written = 143 * d.len() as u64;
    assert!(f.afs.num_blocks(object_id) >= 34);

    f.storage.reset_read_stats();
    let mut reader = block_on(f.afs.open(object_id, Stream::S5, buffer(16 * 1024))).unwrap();
    let mut read_data = vec![0u8; manifest.len()];
    assert_eq!(block_on(reader.read(&mut f.afs, &mut read_data)).unwrap(), manifest.len());
    assert_eq!(read_data, manifest);
    assert_eq!(block_on(reader.read(&mut f.afs, &mut read_data)).unwrap(), 0);
    let (_, bytes) = f.storage.read_stats();
    assert!(bytes < 512 * 1024);
    assert!(bytes < total_written / 100);

    f.storage.reset_read_stats();
    assert_eq!(block_on(reader.size(&mut f.afs)).unwrap(), manifest.len() as u64);
    let (_, bytes) = f.storage.read_stats();
    assert!(bytes < 4 * 1024);
    reader.close(&mut f.afs);
}

#[test]
fn empty_stream_read() {
    let mut f = Fixture::new();
    let d = random_data(1024 * 1024, 9);
    let writes: Vec<(Stream, &[u8])> = (0..6).map(|_| (Stream::S0, d.as_slice())).collect();
    let object_id = f.write_object(16 * 1024, &writes);

    f.storage.reset_read_stats();
    let mut reader = block_on(f.afs.open(object_id, Stream::S7, buffer(16 * 1024))).unwrap();
    assert_eq!(block_on(reader.size(&mut f.afs)).unwrap(), 0);
    let mut read_data = [0u8; 16];
    assert_eq!(block_on(reader.read(&mut f.afs, &mut read_data)).unwrap(), 0);
    let (_, bytes) = f.storage.read_stats();
    assert!(bytes < 128 * 1024);
    reader.close(&mut f.afs);
}

#[test]
fn sparse_stream_read_and_seek() {
    let mut f = Fixture::new();
    let d = random_data(1024 * 1024, 10);
    let sparse: Vec<u8> = (0..3000u32).map(|i| (i * 31 + 7) as u8).collect();
    let mut writes: Vec<(Stream, &[u8])> = (0..12).map(|_| (Stream::S0, d.as_slice())).collect();
    writes.push((Stream::S3, &sparse[..2000]));
    writes.extend((0..12).map(|_| (Stream::S0, d.as_slice())));
    writes.push((Stream::S3, &sparse[2000..]));
    let object_id = f.write_object(16 * 1024, &writes);

    let read_all = |f: &mut Fixture, reader: &mut crate::StreamReader<'_>, length: usize| {
        let mut out = vec![0u8; sparse.len()];
        let mut total = 0;
        while total < length {
            let n = block_on(reader.read(&mut f.afs, &mut out[total..])).unwrap();
            assert!(n > 0);
            total += n;
        }
        assert_eq!(total, length);
        assert_eq!(block_on(reader.read(&mut f.afs, &mut out)).unwrap(), 0);
        out.truncate(length);
        out
    };

    let mut reader = block_on(f.afs.open(object_id, Stream::S3, buffer(16 * 1024))).unwrap();
    assert_eq!(block_on(reader.size(&mut f.afs)).unwrap(), sparse.len() as u64);
    assert_eq!(read_all(&mut f, &mut reader, sparse.len()), sparse);
    reader.close(&mut f.afs);

    let mut reader = block_on(f.afs.open(object_id, Stream::S3, buffer(16 * 1024))).unwrap();
    assert_eq!(block_on(reader.seek(&mut f.afs, 1500)).unwrap(), 1500);
    assert_eq!(read_all(&mut f, &mut reader, sparse.len() - 1500), sparse[1500..]);
    reader.close(&mut f.afs);
}

fn write_wipe_objects(f: &mut Fixture) {
    let d = random_data(1024 * 1024, 11);
    for _ in 0..5 {
        f.write_object(
            1024,
            &[(Stream::S0, &d), (Stream::S0, &d), (Stream::S0, &d), (Stream::S0, &d[..d.len() - 24]), (Stream::S0, &d)],
        );
    }
    for _ in 5..10 {
        f.write_object(1024, &[(Stream::S0, &d)]);
    }
}

#[test]
fn insecure_wipe() {
    let mut f = Fixture::new();
    write_wipe_objects(&mut f);
    block_on(f.afs.wipe(false)).unwrap();
    assert!(f.list().is_empty());
    assert_eq!(f.afs.size(), 0);
    assert!(!f.storage.is_empty());
}

#[test]
fn secure_wipe() {
    let mut f = Fixture::new();
    write_wipe_objects(&mut f);
    block_on(f.afs.wipe(true)).unwrap();
    Expect::start(&f.storage).end();
}

#[test]
fn deleted_object_blocks_not_reused_with_object_id() {
    let mut f = Fixture::new();
    let d = random_data(1024 * 1024, 12);
    let writes: Vec<(Stream, &[u8])> = (0..12).map(|_| (Stream::S0, d.as_slice())).collect();
    let object_id = f.write_object(16 * 1024, &writes);
    assert!(f.afs.num_blocks(object_id) >= 3);
    block_on(f.afs.delete(object_id)).unwrap();

    assert!(f.set_next_object_id(object_id));
    assert_eq!(f.write_object(16 * 1024, &[(Stream::S0, &d)]), object_id);

    f.remount();
    assert_eq!(f.afs.num_blocks(object_id), 1);
}

#[test]
fn orphaned_blocks_not_reused_with_object_id() {
    let mut f = Fixture::new();
    let d = random_data(1024 * 1024, 13);
    let writes: Vec<(Stream, &[u8])> = (0..12).map(|_| (Stream::S0, d.as_slice())).collect();
    let object_id = f.write_object(16 * 1024, &writes);
    assert!(f.afs.num_blocks(object_id) >= 3);
    f.storage.wipe_block_header(0);
    f.remount();

    assert!(f.set_next_object_id(object_id));
    assert_eq!(f.write_object(16 * 1024, &[(Stream::S0, &d)]), object_id);

    f.remount();
    assert_eq!(f.afs.num_blocks(object_id), 1);
}

#[test]
fn missing_block_read() {
    let mut f = Fixture::new();
    let d = random_data(1024 * 1024, 14);
    let mut writes: Vec<(Stream, &[u8])> = (0..20).map(|_| (Stream::S0, d.as_slice())).collect();
    writes.push((Stream::S5, &d[..50]));
    let object_id = f.write_object(16 * 1024, &writes);
    assert!(f.afs.num_blocks(object_id) >= 5);

    f.storage.wipe_block_header(2);
    f.remount();

    let mut reader = block_on(f.afs.open(object_id, Stream::S5, buffer(16 * 1024))).unwrap();
    let mut read_data = vec![0u8; d.len()];
    assert_eq!(block_on(reader.read(&mut f.afs, &mut read_data)).unwrap(), 0);
    reader.close(&mut f.afs);
}

#[test]
fn complete() {
    let mut f = Fixture::new();
    let data = [random_data(256 * 1024, 15), random_data(256 * 1024, 16)];

    let mut writer1 = f.afs.create(buffer(1024)).unwrap();
    let object_id1 = writer1.id();
    for _ in 0..30 {
        block_on(writer1.write(&mut f.afs, Stream::S0, &data[0])).unwrap();
    }
    let mut writer2 = f.afs.create(buffer(1024)).unwrap();
    let object_id2 = writer2.id();
    for i in 0..100 {
        block_on(writer1.write(&mut f.afs, Stream::S0, &data[0])).unwrap();
        block_on(writer2.write(&mut f.afs, Stream::S0, &data[0])).unwrap();
        if i % 7 == 0 {
            block_on(writer1.write(&mut f.afs, Stream::S1, &data[1])).unwrap();
            block_on(writer2.write(&mut f.afs, Stream::S1, &data[1])).unwrap();
        }
    }
    block_on(writer1.close(&mut f.afs)).unwrap();
    block_on(writer2.close(&mut f.afs)).unwrap();
    assert!(!f.afs.is_full());

    assert_eq!(f.list(), [object_id1, object_id2]);

    let mut cursor = ListCursor::new();
    let mut first_object_id = None;
    while let Some(id) = f.afs.list_next(&mut cursor) {
        first_object_id.get_or_insert(id);
        let mut reader = block_on(f.afs.open(id, Stream::S1, buffer(1024))).unwrap();
        assert_eq!(block_on(reader.seek(&mut f.afs, 0x1231f0)).unwrap(), 0x1231f0);
        assert_eq!(block_on(reader.size(&mut f.afs)).unwrap(), 3932160);
        let position = reader.position();
        let buf = reader.close(&mut f.afs);
        let mut reader = block_on(f.afs.open(id, Stream::S1, buf)).unwrap();
        reader.set_position(&position);
        let mut value = [0u8; 4];
        assert_eq!(block_on(reader.read(&mut f.afs, &mut value)).unwrap(), 4);
        let at = 0x1231f0 % data[1].len();
        assert_eq!(value, data[1][at..at + 4]);
        reader.close(&mut f.afs);
    }
    let first_object_id = first_object_id.unwrap();

    let mut reader = block_on(f.afs.open_all(first_object_id, buffer(1024))).unwrap();
    let mut lengths = [0u64; 2];
    let mut chunk = vec![0u8; 1024 * 1024];
    while let Some((stream, n)) = block_on(reader.read(&mut f.afs, &mut chunk)).unwrap() {
        lengths[stream.index() as usize] += n as u64;
    }
    let size = |reader: &mut crate::ObjectReader<'_>, afs: &mut TestAfs, bits| {
        block_on(reader.size(afs, StreamSet::from_bits(bits))).unwrap()
    };
    assert_eq!(size(&mut reader, &mut f.afs, 0x1), 34078720);
    assert_eq!(size(&mut reader, &mut f.afs, 0x2), 3932160);
    assert_eq!(size(&mut reader, &mut f.afs, 0x3), 38010880);
    reader.close(&mut f.afs);
    assert_eq!(lengths, [34078720, 3932160]);

    let mut reader = block_on(f.afs.open_all(first_object_id, buffer(1024))).unwrap();
    assert_eq!(block_on(reader.seek(&mut f.afs, 0x901111)).unwrap(), 0x901111);
    let mut value = [0u8; 4];
    assert_eq!(block_on(reader.read(&mut f.afs, &mut value)).unwrap().map(|(_, n)| n), Some(4));
    assert_eq!(value, data[0][0x1111..0x1115]);
    reader.close(&mut f.afs);

    let mut cursor = ListCursor::new();
    while let Some(id) = f.afs.list_next(&mut cursor) {
        for stream in [Stream::S0, Stream::S1] {
            let expected = &data[stream.index() as usize];
            let mut reader = block_on(f.afs.open(id, stream, buffer(1024))).unwrap();
            let mut chunk = vec![0u8; 200 * 1024 - 1];
            let mut total = 0usize;
            loop {
                let n = block_on(reader.read(&mut f.afs, &mut chunk)).unwrap();
                if n == 0 {
                    break;
                }
                for (i, &b) in chunk[..n].iter().enumerate() {
                    assert_eq!(b, expected[(total + i) % expected.len()]);
                }
                total += n;
            }
            reader.close(&mut f.afs);
            if id == object_id1 {
                assert_eq!(total, if stream == Stream::S0 { 34078720 } else { 3932160 });
                assert_eq!(f.afs.num_blocks(id), 10);
            } else if id == object_id2 {
                assert_eq!(total, if stream == Stream::S0 { 26214400 } else { 3932160 });
                assert_eq!(f.afs.num_blocks(id), 8);
            }
        }
    }
    assert_eq!(f.afs.size(), 18);
}
