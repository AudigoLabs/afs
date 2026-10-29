use afs::{Afs, ObjectId, Stream, StreamSet, lookup_table_len};
use afs_difftest::c::CAfs;
use afs_difftest::configs::{MIN_IO, PRIMARY, SINGLE_SUB_BLOCK, SMALL, TINY};
use afs_difftest::diff::Params;
use afs_difftest::power_loss::{check_model, check_model_c, generate, run_c, run_rust};
use afs_difftest::sim::{Rng, SimStorage, block_on};
use afs_difftest::v1;

fn written_by_both(params: &Params, seeds: std::ops::Range<u64>) {
    let mut verified = (0, 0);
    for seed in seeds {
        let mut rng = Rng::new(seed ^ 0xc0ffee);
        let script = generate(params, &SimStorage::new(params.config), &mut rng, 80);
        let c_image = SimStorage::new(params.config);
        run_c(&c_image, &script);
        let r_image = SimStorage::new(params.config);
        let model = run_rust(params, &r_image, &script);
        assert!(
            c_image.same_image(&r_image),
            "[{} seed={seed}] images differ: {:?}",
            params.name,
            c_image.first_difference(&r_image)
        );
        verified.0 += check_model(params, &c_image, &model, seed);
        verified.1 += check_model_c(params, &r_image, &model, seed);
    }
    println!(
        "STATS cross_read {}: rust read {} C-written objects, C read {} rust-written objects",
        params.name, verified.0, verified.1
    );
    assert!(verified.0 > 0 && verified.1 > 0);
}

#[test]
fn cross_read_primary() {
    written_by_both(&PRIMARY, 0..100);
}

#[test]
fn cross_read_small() {
    written_by_both(&SMALL, 0..300);
}

#[test]
fn cross_read_tiny() {
    written_by_both(&TINY, 0..300);
}

#[test]
fn cross_read_min_io() {
    written_by_both(&MIN_IO, 0..300);
}

#[test]
fn cross_read_single_sub_block() {
    written_by_both(&SINGLE_SUB_BLOCK, 0..300);
}

fn v1_images(params: &Params, seeds: std::ops::Range<u64>) {
    let config = params.config;
    let mut checked = 0;
    for seed in seeds {
        let mut rng = Rng::new(seed ^ 0x5eed_0001);
        let storage = SimStorage::new(config);
        let count = rng.range(1, 6) as usize;
        let objects = v1::populate(&storage, &mut rng, count, params.v1_max_size, params.streams);
        let size = *params.buffer_sizes.first().unwrap();

        let (mut c, _) = CAfs::mount(storage.clone(), false);
        let mut lookup = vec![0u32; lookup_table_len(config.num_blocks)];
        let mut rw = vec![0u8; config.min_read_write_size as usize];
        let mut afs: Afs<'_, SimStorage> = block_on(Afs::mount(storage.clone(), config, &mut rw, &mut lookup)).unwrap();
        let mut buffer = vec![0u8; size];
        let mut cursor = afs::ListCursor::new();
        let mut listed: Vec<u16> = std::iter::from_fn(|| afs.list_next(&mut cursor)).map(ObjectId::get).collect();
        let mut expected_ids: Vec<u16> = objects.iter().map(|o| o.id).collect();
        listed.sort();
        expected_ids.sort();
        assert_eq!(listed, expected_ids);
        assert_eq!(c.list().len(), objects.len());

        for object in &objects {
            let id = ObjectId::new(object.id).unwrap();
            let mut reader = block_on(afs.open_all(id, &mut buffer)).unwrap();
            let total: usize = object.chunks.iter().map(|(_, d)| d.len()).sum();
            assert_eq!(block_on(reader.size(&mut afs, StreamSet::ALL)).unwrap(), total as u64);
            let mut flat = Vec::new();
            let mut chunk = vec![0u8; 5000];
            while let Some((stream, n)) = block_on(reader.read(&mut afs, &mut chunk)).unwrap() {
                flat.push((stream.index(), chunk[..n].to_vec()));
            }
            let expected: Vec<(u8, Vec<u8>)> = object.chunks.clone();
            let merge = |v: Vec<(u8, Vec<u8>)>| {
                v.into_iter().fold(Vec::<(u8, Vec<u8>)>::new(), |mut acc, (s, d)| {
                    match acc.last_mut() {
                        Some((last, data)) if *last == s => data.extend(d),
                        _ => acc.push((s, d)),
                    }
                    acc
                })
            };
            assert_eq!(merge(flat), merge(expected), "[{} seed={seed}] v1 object {} wildcard", params.name, object.id);
            reader.close(&mut afs);

            for stream in Stream::ALL {
                let expected = object.stream_data(stream.index());
                let mut reader = block_on(afs.open(id, stream, &mut buffer)).unwrap();
                assert_eq!(block_on(reader.size(&mut afs)).unwrap(), expected.len() as u64);
                let mut actual = vec![0u8; expected.len() + 1];
                let mut n = 0;
                loop {
                    let r = block_on(reader.read(&mut afs, &mut actual[n..])).unwrap();
                    n += r;
                    if r == 0 || n == actual.len() {
                        break;
                    }
                }
                assert_eq!(
                    &actual[..n],
                    &expected[..],
                    "[{} seed={seed}] v1 object {} {stream:?}",
                    params.name,
                    object.id
                );
                if expected.len() > 10 {
                    let at = rng.below(expected.len() as u64 - 1);
                    let mut reader2 = {
                        reader.close(&mut afs);
                        block_on(afs.open(id, stream, &mut buffer)).unwrap()
                    };
                    assert_eq!(block_on(reader2.seek(&mut afs, at)).unwrap(), at);
                    let mut byte = [0u8];
                    assert_eq!(block_on(reader2.read(&mut afs, &mut byte)).unwrap(), 1);
                    assert_eq!(byte[0], expected[at as usize]);
                    reader2.close(&mut afs);
                } else {
                    reader.close(&mut afs);
                }

                let mut obj = c.open(object.id, stream.index(), size).unwrap();
                assert_eq!(c.size(&mut obj, 0), expected.len() as u64);
                let mut actual = vec![0u8; expected.len() + 1];
                let mut n = 0;
                loop {
                    let r = c.read(&mut obj, &mut actual[n..]);
                    n += r;
                    if r == 0 || n == actual.len() {
                        break;
                    }
                }
                assert_eq!(&actual[..n], &expected[..], "[{} seed={seed}] C v1 object {}", params.name, object.id);
                c.close(&mut obj);
            }
            checked += 1;
        }
    }
    println!("STATS v1 {}: {checked} v1 objects read by both", params.name);
    assert!(checked > 0);
}

#[test]
fn v1_primary() {
    v1_images(&PRIMARY, 0..40);
}

#[test]
fn v1_small() {
    v1_images(&SMALL, 0..300);
}

#[test]
fn v1_tiny() {
    v1_images(&TINY, 0..300);
}

#[test]
fn v1_min_io() {
    v1_images(&MIN_IO, 0..300);
}
