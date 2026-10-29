use std::collections::{BTreeMap, BTreeSet};

use afs::{Afs, Error, ObjectId, ObjectWriter, Stream, lookup_table_len};

use crate::c::{CAfs, CObject};
use crate::diff::{Buffers, Pair, Params, Stats};
use crate::sim::{FailMode, Rng, SimStorage, block_on};

#[derive(Clone, Debug)]
pub enum ScriptOp {
    Create { buffer_size: usize },
    Write { writer: usize, stream: u8, length: usize, data_seed: u64 },
    Close { writer: usize },
    Delete { object_id: u16 },
    Wipe { secure: bool },
    Prepare { num_blocks: u16 },
}

fn data(length: usize, seed: u64) -> Vec<u8> {
    let mut buf = vec![0u8; length];
    Rng::new(seed).fill(&mut buf);
    buf
}

pub struct Script {
    pub ops: Vec<ScriptOp>,
    pub mount_ops: u64,
    pub total_ops: u64,
}

pub fn generate(params: &Params, storage: &SimStorage, rng: &mut Rng, length: usize) -> Script {
    let (mut c, _) = CAfs::mount(storage.clone(), false);
    let mount_ops = storage.inner().num_ops;
    let mut ops = Vec::new();
    let mut writers: Vec<Option<(u16, CObject)>> = Vec::new();
    let mut closed: BTreeSet<u16> = BTreeSet::new();
    let block_size = params.config.block_size as u64;
    for _ in 0..length {
        let open: Vec<usize> = (0..writers.len()).filter(|&i| writers[i].is_some()).collect();
        let op = match rng.below(100) {
            0..=14 if open.len() < 3 => ScriptOp::Create { buffer_size: *rng.pick(params.buffer_sizes) },
            15..=69 if !open.is_empty() => {
                let length = match rng.below(10) {
                    0..=5 => rng.range(1, params.small_write as u64),
                    6..=8 => rng.range(1, params.medium_write as u64),
                    _ => rng.range(block_size / 4, block_size * 2),
                } as usize;
                ScriptOp::Write {
                    writer: *rng.pick(&open),
                    stream: rng.below(params.streams as u64) as u8,
                    length,
                    data_seed: rng.next_u64(),
                }
            }
            70..=84 if !open.is_empty() => ScriptOp::Close { writer: *rng.pick(&open) },
            85..=93 if !closed.is_empty() => {
                let ids: Vec<u16> = closed.iter().copied().collect();
                ScriptOp::Delete { object_id: *rng.pick(&ids) }
            }
            94..=95 if open.is_empty() => ScriptOp::Wipe { secure: rng.chance(1, 2) },
            96..=99 => ScriptOp::Prepare { num_blocks: rng.range(1, params.config.num_blocks.min(32) as u64) as u16 },
            _ => continue,
        };
        match &op {
            ScriptOp::Create { buffer_size } => {
                let (id, obj) = c.create(*buffer_size);
                writers.push(Some((id, obj)));
            }
            ScriptOp::Write { writer, stream, length, data_seed } => {
                let (_, obj) = writers[*writer].as_mut().unwrap();
                c.write(obj, *stream, &data(*length, *data_seed));
            }
            ScriptOp::Close { writer } => {
                let (id, obj) = writers[*writer].as_mut().unwrap();
                if c.close(obj) {
                    closed.insert(*id);
                    writers[*writer] = None;
                }
            }
            ScriptOp::Delete { object_id } => {
                c.delete(*object_id);
                closed.remove(object_id);
            }
            ScriptOp::Wipe { secure } => {
                c.wipe(*secure);
                closed.clear();
            }
            ScriptOp::Prepare { num_blocks } => c.prepare_storage(*num_blocks),
        }
        ops.push(op);
    }
    let total_ops = storage.inner().num_ops;
    Script { ops, mount_ops, total_ops }
}

pub fn run_c(storage: &SimStorage, script: &Script) {
    let (mut c, _) = CAfs::mount(storage.clone(), false);
    let mut writers: Vec<Option<CObject>> = Vec::new();
    for op in &script.ops {
        match op {
            ScriptOp::Create { buffer_size } => writers.push(Some(c.create(*buffer_size).1)),
            ScriptOp::Write { writer, stream, length, data_seed } => {
                c.write(writers[*writer].as_mut().unwrap(), *stream, &data(*length, *data_seed));
            }
            ScriptOp::Close { writer } => {
                if c.close(writers[*writer].as_mut().unwrap()) {
                    writers[*writer] = None;
                }
            }
            ScriptOp::Delete { object_id } => c.delete(*object_id),
            ScriptOp::Wipe { secure } => c.wipe(*secure),
            ScriptOp::Prepare { num_blocks } => c.prepare_storage(*num_blocks),
        }
    }
}

#[derive(Default)]
pub struct Model {
    pub streams: BTreeMap<u16, [Vec<u8>; 16]>,
    pub intact: BTreeSet<u16>,
    pub damaged: BTreeSet<u16>,
}

pub fn run_rust(params: &Params, storage: &SimStorage, script: &Script) -> Model {
    let config = params.config;
    let mut lookup = vec![0u32; lookup_table_len(config.num_blocks)];
    let mut rw = vec![0u8; config.min_read_write_size as usize];
    let max = *params.buffer_sizes.iter().max().unwrap();
    let mut arena = vec![0u8; max * (script.ops.len() + 1)];
    let mut buffers = arena.chunks_exact_mut(max);
    let mut afs: Afs<'_, SimStorage> = block_on(Afs::mount(storage.clone(), config, &mut rw, &mut lookup)).unwrap();
    let mut writers: Vec<Option<ObjectWriter<'_>>> = Vec::new();
    let mut model = Model::default();
    for op in &script.ops {
        let result = match op {
            ScriptOp::Create { buffer_size } => {
                let writer = afs.create(&mut buffers.next().unwrap()[..*buffer_size]).unwrap();
                model.streams.insert(writer.id().get(), Default::default());
                writers.push(Some(writer));
                Ok(())
            }
            ScriptOp::Write { writer, stream, length, data_seed } => {
                let w = writers[*writer].as_mut().unwrap();
                let bytes = data(*length, *data_seed);
                let result = block_on(w.write(&mut afs, Stream::from_index(*stream).unwrap(), &bytes));
                match result {
                    Ok(()) => model.streams.get_mut(&w.id().get()).unwrap()[*stream as usize].extend_from_slice(&bytes),
                    Err(_) => {
                        model.damaged.insert(w.id().get());
                    }
                }
                result
            }
            ScriptOp::Close { writer } => {
                let w = writers[*writer].take().unwrap();
                let id = w.id().get();
                match block_on(w.close(&mut afs)) {
                    Ok(_) => {
                        if !model.damaged.contains(&id) {
                            model.intact.insert(id);
                        }
                        Ok(())
                    }
                    Err(e) => {
                        writers[*writer] = Some(e.writer);
                        Err(e.error)
                    }
                }
            }
            ScriptOp::Delete { object_id } => {
                model.intact.remove(object_id);
                block_on(afs.delete(ObjectId::new(*object_id).unwrap()))
            }
            ScriptOp::Wipe { secure } => {
                model.intact.clear();
                block_on(afs.wipe(*secure))
            }
            ScriptOp::Prepare { num_blocks } => block_on(afs.prepare_storage(*num_blocks)),
        };
        match result {
            Ok(()) | Err(Error::StorageFull) => {}
            Err(Error::Storage(_)) => break,
            Err(e) => panic!("unexpected rust error {e:?}"),
        }
    }
    model
}

pub fn run_seed(params: &Params, seed: u64) -> Stats {
    let mut rng = Rng::new(seed ^ 0xdead_beef);
    let base = SimStorage::new(params.config);
    if params.v1_objects > 0 && rng.chance(1, 3) {
        let count = rng.range(1, params.v1_objects as u64) as usize;
        crate::v1::populate(&base, &mut rng, count, params.v1_max_size, params.streams);
    }
    let script = generate(params, &base.deep_clone(), &mut rng, 60);
    let span = script.total_ops - script.mount_ops;
    let cut = script.mount_ops + rng.below(span + 1);
    let torn = if rng.chance(1, 2) { rng.below(64) as u32 } else { 0 };

    let c_image = base.deep_clone();
    c_image.fail_at(cut, FailMode::Ignore, torn);
    run_c(&c_image, &script);
    let r_image = base.deep_clone();
    r_image.fail_at(cut, FailMode::Error, torn);
    let model = run_rust(params, &r_image, &script);
    assert!(
        c_image.same_image(&r_image),
        "[{} seed={seed}] images differ after power loss at op {cut}: {:?}",
        params.name,
        c_image.first_difference(&r_image)
    );

    let c_storage = c_image.deep_clone();
    let r_storage = c_image.deep_clone();
    let mut buffers = Buffers::new(params, 64);
    let mut pair = Pair::new(params, seed, &mut buffers, c_storage, r_storage);
    pair.compare_state();
    pair.read_everything();
    pair.stats.model_objects_verified += check_model(params, &pair.r_storage.deep_clone(), &model, seed);
    for _ in 0..60 {
        pair.step();
        if pair.stopped {
            break;
        }
    }
    pair.compare_state();
    pair.finish();
    pair.stats.clone()
}

pub fn check_model_c(params: &Params, storage: &SimStorage, model: &Model, seed: u64) -> u64 {
    let (mut c, _) = CAfs::mount(storage.clone(), false);
    let size = *params.buffer_sizes.first().unwrap();
    for &id in &model.intact {
        for (stream, expected) in model.streams[&id].iter().enumerate() {
            let mut obj = c
                .open(id, stream as u8, size)
                .unwrap_or_else(|| panic!("[{} seed={seed}] C: object {id} missing", params.name));
            assert_eq!(c.size(&mut obj, 0), expected.len() as u64);
            let mut actual = vec![0u8; expected.len() + 1];
            let mut total = 0;
            loop {
                let n = c.read(&mut obj, &mut actual[total..]);
                total += n;
                if n == 0 || total == actual.len() {
                    break;
                }
            }
            assert_eq!(total, expected.len(), "[{} seed={seed}] C: object {id} stream {stream} length", params.name);
            assert!(
                actual[..total] == expected[..],
                "[{} seed={seed}] C: object {id} stream {stream} data",
                params.name
            );
            c.close(&mut obj);
        }
    }
    model.intact.len() as u64
}

pub fn check_model(params: &Params, storage: &SimStorage, model: &Model, seed: u64) -> u64 {
    let config = params.config;
    let mut lookup = vec![0u32; lookup_table_len(config.num_blocks)];
    let mut rw = vec![0u8; config.min_read_write_size as usize];
    let mut afs: Afs<'_, SimStorage> = block_on(Afs::mount(storage.clone(), config, &mut rw, &mut lookup)).unwrap();
    let mut buffer = vec![0u8; *params.buffer_sizes.first().unwrap()];
    for &id in &model.intact {
        let streams = &model.streams[&id];
        for stream in Stream::ALL {
            let mut reader = block_on(afs.open(ObjectId::new(id).unwrap(), stream, &mut buffer))
                .unwrap_or_else(|e| panic!("[{} seed={seed}] closed object {id} missing: {e:?}", params.name));
            let expected = &streams[stream.index() as usize];
            let mut actual = vec![0u8; expected.len() + 1];
            let mut total = 0;
            loop {
                let n = block_on(reader.read(&mut afs, &mut actual[total..])).unwrap();
                total += n;
                if n == 0 || total == actual.len() {
                    break;
                }
            }
            assert_eq!(total, expected.len(), "[{} seed={seed}] object {id} {stream:?} length", params.name);
            assert!(actual[..total] == expected[..], "[{} seed={seed}] object {id} {stream:?} data", params.name);
            assert_eq!(block_on(reader.size(&mut afs)).unwrap(), expected.len() as u64);
            reader.close(&mut afs);
        }
    }
    model.intact.len() as u64
}
