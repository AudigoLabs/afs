use afs::{Afs, Config, ObjectId, Stream, lookup_table_len};
use afs_difftest::c::{CAfs, CObject};
use afs_difftest::sim::{SimStorage, block_on};

const CONFIG: Config =
    Config { block_size: 4 * 1024 * 1024, num_blocks: 1100, sub_blocks_per_block: 256, min_read_write_size: 512 };
const BUFFER_SIZE: usize = 32 * 1024;
const MARKERS: u32 = 1060;

struct Large {
    c_storage: SimStorage,
    r_storage: SimStorage,
    c: CAfs,
    c_obj: CObject,
    id: u16,
}

fn object_id(id: u16) -> ObjectId {
    ObjectId::new(id).unwrap()
}

fn write_large_object(close: bool, lookup: &mut [u32], rw: &mut [u8], buffer: &mut [u8]) -> Large {
    let c_storage = SimStorage::new(CONFIG);
    let r_storage = SimStorage::new(CONFIG);
    let (mut c, _) = CAfs::mount(c_storage.clone(), false);
    let mut afs: Afs<'_, SimStorage> = block_on(Afs::mount(r_storage.clone(), CONFIG, rw, lookup)).unwrap();
    let check = || assert_eq!(c_storage.take_trace(), r_storage.take_trace());
    check();
    let (id, mut c_obj) = c.create(BUFFER_SIZE);
    let mut writer = afs.create(buffer).unwrap();
    assert_eq!(writer.id().get(), id);
    let zeros = vec![0u8; 4 * 1024 * 1024];
    for i in 0..MARKERS {
        assert!(c.write(&mut c_obj, 0, &zeros));
        block_on(writer.write(&mut afs, Stream::S0, &zeros)).unwrap();
        let marker = i.to_le_bytes();
        assert!(c.write(&mut c_obj, 1, &marker));
        block_on(writer.write(&mut afs, Stream::S1, &marker)).unwrap();
        check();
    }
    if close {
        assert!(c.close(&mut c_obj));
        block_on(writer.close(&mut afs)).unwrap();
        check();
    }
    assert!(c_storage.same_image(&r_storage));
    assert!(afs.num_blocks(object_id(id)) > 1024);
    Large { c_storage, r_storage, c, c_obj, id }
}

#[test]
fn seek_past_4_gib_is_correct_in_rust_and_wraps_in_c() {
    let mut lookup = vec![0u32; lookup_table_len(CONFIG.num_blocks)];
    let mut rw = vec![0u8; 512];
    let mut buffer = vec![0u8; BUFFER_SIZE];
    let Large { c_storage, r_storage, c, id, .. } = write_large_object(true, &mut lookup, &mut rw, &mut buffer);
    let mut afs: Afs<'_, SimStorage> = block_on(Afs::mount(r_storage.clone(), CONFIG, &mut rw, &mut lookup)).unwrap();
    let (mut c, _) = {
        drop(c);
        CAfs::mount(c_storage.clone(), false)
    };
    let check = || assert_eq!(c_storage.take_trace(), r_storage.take_trace());
    check();

    let marker_index = 1040u64;
    let mut c_reader = c.open(id, 1, BUFFER_SIZE).unwrap();
    let mut reader = block_on(afs.open(object_id(id), Stream::S1, &mut buffer)).unwrap();
    assert_eq!(c.size(&mut c_reader, 0), MARKERS as u64 * 4);
    assert_eq!(block_on(reader.size(&mut afs)).unwrap(), MARKERS as u64 * 4);
    check();
    assert!(c.seek(&mut c_reader, marker_index * 4));
    assert_eq!(block_on(reader.seek(&mut afs, marker_index * 4)).unwrap(), marker_index * 4);
    let c_trace = c_storage.take_trace();
    let r_trace = r_storage.take_trace();
    let common = c_trace.iter().zip(&r_trace).take_while(|(a, b)| a == b).count();
    assert!(common > 0 && common < c_trace.len().min(r_trace.len()));

    let mut value = [0u8; 4];
    assert_eq!(block_on(reader.read(&mut afs, &mut value)).unwrap(), 4);
    assert_eq!(u32::from_le_bytes(value) as u64, marker_index);
    assert_eq!(c.read(&mut c_reader, &mut value), 4);
    let c_value = u32::from_le_bytes(value) as u64;
    assert!(c_value + 1000 < marker_index, "C read marker {c_value}");
}

#[test]
fn size_of_unclosed_object_past_4_gib_is_correct_in_rust_and_wraps_in_c() {
    let mut lookup = vec![0u32; lookup_table_len(CONFIG.num_blocks)];
    let mut rw = vec![0u8; 512];
    let mut buffer = vec![0u8; BUFFER_SIZE];
    let Large { c_storage, r_storage, c, c_obj, id } = write_large_object(false, &mut lookup, &mut rw, &mut buffer);
    drop(c);
    drop(c_obj);
    let (mut c, _) = CAfs::mount(c_storage.clone(), false);
    let mut afs: Afs<'_, SimStorage> = block_on(Afs::mount(r_storage.clone(), CONFIG, &mut rw, &mut lookup)).unwrap();
    assert_eq!(c_storage.take_trace(), r_storage.take_trace());

    let mut reader = block_on(afs.open(object_id(id), Stream::S1, &mut buffer)).unwrap();
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
    for (i, marker) in data[..total].as_chunks::<4>().0.iter().enumerate() {
        assert_eq!(u32::from_le_bytes(*marker), i as u32);
    }
    let buffer = reader.close(&mut afs);

    let mut reader = block_on(afs.open(object_id(id), Stream::S1, buffer)).unwrap();
    assert_eq!(block_on(reader.size(&mut afs)).unwrap(), total as u64);
    let mut c_reader = c.open(id, 1, BUFFER_SIZE).unwrap();
    let c_size = c.size(&mut c_reader, 0);
    assert!(c_size > total as u64, "C size {c_size}, actual {total}");
}
