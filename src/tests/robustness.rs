use std::string::String;
use std::vec;
use std::vec::Vec;

use super::storage::{MemStorage, TestAfs, block_on, buffer, leak, random_data};
use crate::{Afs, Config, Error, ListCursor, ObjectId, Stream, StreamSet, lookup_table_len};

const CONFIG: Config = Config { block_size: 4096, num_blocks: 12, sub_blocks_per_block: 4, min_read_write_size: 128 };

struct Rng(u64);

impl Rng {
    fn next(&mut self) -> u64 {
        self.0 = self.0.wrapping_mul(6_364_136_223_846_793_005).wrapping_add(1_442_695_040_888_963_407);
        self.0 >> 17
    }

    fn below(&mut self, n: u64) -> u64 {
        self.next() % n.max(1)
    }
}

fn mount(storage: &MemStorage) -> Result<TestAfs, Error<super::storage::Fault>> {
    let rw = leak(0u8, CONFIG.min_read_write_size as usize);
    let lookup = leak(0u32, lookup_table_len(CONFIG.num_blocks));
    block_on(Afs::mount_with(storage.clone(), CONFIG, rw, lookup, |_, _, _| {}))
}

fn populate(afs: &mut TestAfs, rng: &mut Rng) {
    for i in 0..rng.below(6) + 1 {
        let Ok(mut writer) = afs.create(buffer(256 << rng.below(3))) else { return };
        for _ in 0..rng.below(20) {
            let data = random_data(rng.below(3000) as usize + 1, rng.next());
            let stream = Stream::from_index(rng.below(16) as u8).unwrap();
            if block_on(writer.write(afs, stream, &data)).is_err() {
                break;
            }
        }
        if i % 3 != 2 {
            let _ = block_on(writer.close(afs));
        }
    }
}

fn exercise(afs: &mut TestAfs, rng: &mut Rng) {
    let mut cursor = ListCursor::new();
    let mut ids = Vec::new();
    while let Some(id) = afs.list_next(&mut cursor) {
        ids.push(id);
    }
    ids.push(ObjectId::new(rng.below(u16::MAX as u64) as u16 + 1).unwrap());
    let mut text = String::new();
    let _ = block_on(afs.dump(&mut text));
    for &id in &ids {
        let _ = afs.dump_object(&mut text, id);
        let _ = afs.num_blocks(id);
        if let Ok(mut reader) = block_on(afs.open_all(id, buffer(512))) {
            let _ = block_on(reader.size(afs, StreamSet::from_bits(rng.below(u16::MAX as u64) as u16)));
            let mut chunk = vec![0u8; 700];
            while let Ok(Some(_)) = block_on(reader.read(afs, &mut chunk)) {}
            let _ = block_on(reader.seek(afs, rng.below(20000)));
            let _ = block_on(reader.read(afs, &mut chunk));
            reader.close(afs);
        }
        for stream in Stream::ALL {
            if let Ok(mut reader) = block_on(afs.open(id, stream, buffer(256))) {
                let _ = block_on(reader.size(afs));
                let _ = block_on(reader.seek(afs, rng.below(5000)));
                let position = reader.position();
                let mut chunk = vec![0u8; 333];
                while let Ok(1..) = block_on(reader.read(afs, &mut chunk)) {}
                reader.set_position(&position);
                let _ = block_on(reader.seek(afs, rng.below(5000)));
                let _ = block_on(reader.read(afs, &mut chunk));
                let _ = block_on(reader.size(afs));
                reader.close(afs);
            }
        }
    }
    for block in 0..CONFIG.num_blocks + 1 {
        let _ = block_on(afs.dump_block(&mut text, block, rng.below(4) as u32));
    }
    if let Some(&id) = ids.first() {
        let _ = block_on(afs.delete(id));
    }
    let _ = block_on(afs.prepare_storage(rng.below(4) as u16 + 1));
    populate(afs, rng);
    let _ = block_on(afs.wipe(rng.below(2) == 0));
}

fn corrupt(storage: &MemStorage, rng: &mut Rng) {
    let total = CONFIG.block_size as u64 * CONFIG.num_blocks as u64;
    for _ in 0..rng.below(12) + 1 {
        let address = match rng.below(4) {
            0 => rng.below(CONFIG.num_blocks as u64) * CONFIG.block_size as u64 + rng.below(8),
            1 => (rng.below(CONFIG.num_blocks as u64) + 1) * CONFIG.block_size as u64 - 128 + rng.below(72),
            _ => rng.below(total),
        };
        let length = rng.below(8) as usize + 1;
        let value: Vec<u8> = (0..length)
            .map(|_| match rng.below(4) {
                0 => 0,
                1 => 0xff,
                _ => rng.next() as u8,
            })
            .collect();
        storage.raw_write(address, &value[..length.min((total - address) as usize)]);
    }
}

#[test]
fn corrupted_storage_returns_errors_without_panicking() {
    for seed in 0..3000 {
        let mut rng = Rng(seed);
        let storage = MemStorage::new(CONFIG);
        let mut afs = mount(&storage).unwrap();
        populate(&mut afs, &mut rng);
        corrupt(&storage, &mut rng);
        let mut afs = mount(&storage).unwrap();
        exercise(&mut afs, &mut rng);
    }
}

#[test]
fn storage_faults_propagate_without_panicking() {
    for seed in 0..1500 {
        let mut rng = Rng(seed);
        let storage = MemStorage::new(CONFIG);
        let mut afs = mount(&storage).unwrap();
        populate(&mut afs, &mut rng);
        let ops = storage.0.borrow().ops;
        storage.0.borrow_mut().fail_after = Some(ops + rng.below(400));
        match mount(&storage) {
            Ok(mut afs) => exercise(&mut afs, &mut rng),
            Err(error) => assert_eq!(error, Error::Storage(super::storage::Fault)),
        }
    }
}

#[test]
fn invalid_configuration_and_buffers_are_rejected() {
    let storage = MemStorage::new(CONFIG);
    let bad_configs = [
        Config { num_blocks: 0, ..CONFIG },
        Config { num_blocks: u16::MAX, ..CONFIG },
        Config { min_read_write_size: 64, ..CONFIG },
        Config { block_size: 0, ..CONFIG },
        Config { block_size: 4096 + 64, ..CONFIG },
        Config { block_size: 1 << 29, ..CONFIG },
        Config { sub_blocks_per_block: 0, ..CONFIG },
        Config { sub_blocks_per_block: 3, ..CONFIG },
        Config { sub_blocks_per_block: 64, ..CONFIG },
    ];
    for config in bad_configs {
        let rw = leak(0u8, 4096);
        let lookup = leak(0u32, 4096);
        let result: Result<TestAfs, _> = block_on(Afs::mount(storage.clone(), config, rw, lookup));
        assert!(matches!(result, Err(Error::InvalidConfig)), "{config:?}");
    }
    let small_rw: Result<TestAfs, _> = block_on(Afs::mount(storage.clone(), CONFIG, leak(0u8, 64), leak(0u32, 64)));
    assert!(matches!(small_rw, Err(Error::InvalidBuffer)));
    let small_lookup: Result<TestAfs, _> =
        block_on(Afs::mount(storage.clone(), CONFIG, leak(0u8, 128), leak(0u32, 12)));
    assert!(matches!(small_lookup, Err(Error::InvalidBuffer)));

    let mut afs = mount(&storage).unwrap();
    for size in [0, 64, 192, 384, 768] {
        assert!(matches!(afs.create(buffer(size)), Err(Error::InvalidBuffer)), "{size}");
    }
    let id = ObjectId::new(1).unwrap();
    assert_eq!(block_on(afs.open(id, Stream::S0, buffer(256))).err(), Some(Error::NotFound));
    assert_eq!(block_on(afs.delete(id)), Err(Error::NotFound));

    let mut writers = Vec::new();
    for _ in 0..8 {
        writers.push(afs.create(buffer(256)).unwrap());
    }
    assert!(matches!(afs.create(buffer(256)), Err(Error::TooManyOpen)));
    let open_id = writers[0].id();
    block_on(writers[0].write(&mut afs, Stream::S3, b"data")).unwrap();
    assert_eq!(block_on(afs.wipe(false)), Err(Error::ObjectOpen));
    for writer in writers {
        block_on(writer.close(&mut afs)).unwrap();
    }
    let reader = block_on(afs.open(open_id, Stream::S3, buffer(256))).unwrap();
    assert_eq!(block_on(afs.delete(open_id)), Err(Error::ObjectOpen));
    reader.close(&mut afs);
    block_on(afs.delete(open_id)).unwrap();
}

#[test]
fn failed_close_keeps_the_writer_open() {
    let config = Config { block_size: 4096, num_blocks: 2, sub_blocks_per_block: 4, min_read_write_size: 128 };
    let storage = MemStorage::new(config);
    let rw = leak(0u8, 128);
    let lookup = leak(0u32, lookup_table_len(2));
    let mut afs: TestAfs = block_on(Afs::mount(storage.clone(), config, rw, lookup)).unwrap();
    let mut filler = afs.create(buffer(256)).unwrap();
    assert_eq!(block_on(filler.write(&mut afs, Stream::S0, &vec![1; 10000])), Err(Error::StorageFull));
    let writer = afs.create(buffer(256)).unwrap();
    let id = writer.id();
    let error = block_on(writer.close(&mut afs)).unwrap_err();
    assert_eq!(error.error, Error::StorageFull);
    let mut cursor = ListCursor::new();
    let listed: Vec<ObjectId> = std::iter::from_fn(|| afs.list_next(&mut cursor)).collect();
    assert!(listed.contains(&id));
    assert!(afs.has_open_objects());
}
