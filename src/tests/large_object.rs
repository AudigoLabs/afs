use std::vec;

use super::storage::{MemStorage, block_on, buffer, mount};
use crate::{Config, ObjectId, Stream};

const CONFIG: Config =
    Config { block_size: 4 * 1024 * 1024, num_blocks: 1100, sub_blocks_per_block: 256, min_read_write_size: 512 };
const MARKERS: u32 = 1060;

fn write_markers(close: bool) -> (MemStorage, ObjectId) {
    let storage = MemStorage::new(CONFIG);
    let mut afs = mount(&storage);
    let mut writer = afs.create(buffer(32 * 1024)).unwrap();
    let id = writer.id();
    let zeros = vec![0u8; CONFIG.block_size as usize];
    for i in 0..MARKERS {
        block_on(writer.write(&mut afs, Stream::S0, &zeros)).unwrap();
        block_on(writer.write(&mut afs, Stream::S1, &i.to_le_bytes())).unwrap();
    }
    if close {
        block_on(writer.close(&mut afs)).unwrap();
    }
    assert!(afs.num_blocks(id) > 1024);
    (storage, id)
}

#[test]
fn seek_past_4_gib() {
    let (storage, id) = write_markers(true);
    let mut afs = mount(&storage);
    let mut reader = block_on(afs.open(id, Stream::S1, buffer(32 * 1024))).unwrap();
    assert_eq!(block_on(reader.size(&mut afs)).unwrap(), MARKERS as u64 * 4);
    for marker in [1040u64, 1059, 3, 1058] {
        let buf = reader.close(&mut afs);
        reader = block_on(afs.open(id, Stream::S1, buf)).unwrap();
        assert_eq!(block_on(reader.seek(&mut afs, marker * 4)).unwrap(), marker * 4);
        let mut value = [0u8; 4];
        assert_eq!(block_on(reader.read(&mut afs, &mut value)).unwrap(), 4);
        assert_eq!(u32::from_le_bytes(value) as u64, marker);
    }
}

#[test]
fn size_of_unclosed_object_past_4_gib() {
    let (storage, id) = write_markers(false);
    let mut afs = mount(&storage);
    let mut reader = block_on(afs.open(id, Stream::S1, buffer(32 * 1024))).unwrap();
    let mut data = vec![0u8; MARKERS as usize * 4];
    let mut total = 0;
    loop {
        let n = block_on(reader.read(&mut afs, &mut data[total..])).unwrap();
        if n == 0 {
            break;
        }
        total += n;
    }
    assert!(total > 1024 * 4);
    for (i, marker) in data[..total].chunks_exact(4).enumerate() {
        assert_eq!(u32::from_le_bytes([marker[0], marker[1], marker[2], marker[3]]), i as u32);
    }
    let buf = reader.close(&mut afs);
    let mut reader = block_on(afs.open(id, Stream::S1, buf)).unwrap();
    assert_eq!(block_on(reader.size(&mut afs)).unwrap(), total as u64);
}
