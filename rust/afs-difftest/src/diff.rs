use std::collections::{BTreeMap, HashMap};
use std::slice::ChunksExactMut;

use afs::{
    Afs, CloseError, Config, Error, ListCursor, ObjectId, ObjectReader, ObjectWriter, ReadPosition, Stream,
    StreamReader, StreamSet, lookup_table_len,
};

use crate::c::{CAfs, CObject, Found, ObjectListEntryC, ReadPositionC};
use crate::sim::{Rng, SimStorage, block_on};
use crate::v1;

#[derive(Clone, Debug)]
pub struct Params {
    pub name: &'static str,
    pub config: Config,
    pub buffer_sizes: &'static [usize],
    pub small_write: usize,
    pub medium_write: usize,
    pub large_write: usize,
    pub ops: usize,
    pub streams: u8,
    pub v1_objects: usize,
    pub v1_max_size: usize,
    pub full_dumps: bool,
}

#[derive(Default, Debug, Clone)]
pub struct Stats {
    pub seeds: u64,
    pub api_calls: u64,
    pub trace_checks: u64,
    pub storage_ops: u64,
    pub bytes_compared: u64,
    pub mounts: u64,
    pub mount_reads: u64,
    pub mount_read_bytes: u64,
    pub stopped_on_c_assert_path: u64,
    pub storage_full: u64,
    pub failed_closes: u64,
    pub model_objects_verified: u64,
    pub ops: BTreeMap<&'static str, u64>,
}

impl Stats {
    pub fn merge(&mut self, other: &Stats) {
        self.seeds += other.seeds;
        self.api_calls += other.api_calls;
        self.trace_checks += other.trace_checks;
        self.storage_ops += other.storage_ops;
        self.bytes_compared += other.bytes_compared;
        self.mounts += other.mounts;
        self.mount_reads += other.mount_reads;
        self.mount_read_bytes += other.mount_read_bytes;
        self.stopped_on_c_assert_path += other.stopped_on_c_assert_path;
        self.storage_full += other.storage_full;
        self.failed_closes += other.failed_closes;
        self.model_objects_verified += other.model_objects_verified;
        for (k, v) in &other.ops {
            *self.ops.entry(k).or_default() += v;
        }
    }
}

pub type RAfs<'a> = Afs<'a, SimStorage>;

enum RHandle<'a> {
    Writer(ObjectWriter<'a>),
    Stream(StreamReader<'a>),
    Object(ObjectReader<'a>),
}

struct Slot<'a> {
    c: CObject,
    r: RHandle<'a>,
    id: u16,
    stream: u8,
    at_end: bool,
    close_failed: bool,
}

struct Saved {
    stream: u8,
    c: ReadPositionC,
    r: ReadPosition,
    at_end: bool,
}

pub struct Pair<'a> {
    pub params: Params,
    pub seed: u64,
    pub step: usize,
    pub rng: Rng,
    pub c: Option<CAfs>,
    pub r: Option<RAfs<'a>>,
    pub c_storage: SimStorage,
    pub r_storage: SimStorage,
    slots: Vec<Slot<'a>>,
    free: Vec<&'a mut [u8]>,
    fresh: ChunksExactMut<'a, u8>,
    saved: HashMap<u16, Vec<Saved>>,
    cursor: Option<(ObjectListEntryC, ListCursor)>,
    pub stats: Stats,
    pub stopped: bool,
}

fn r_mount<'a>(
    storage: SimStorage,
    config: Config,
    rw: &'a mut [u8],
    lookup: &'a mut [u32],
    with_callback: bool,
) -> (RAfs<'a>, Found) {
    let mut found = Vec::new();
    let afs = if with_callback {
        block_on(Afs::mount_with(storage, config, rw, lookup, |id, stream, data| {
            found.push((id.get(), stream.map_or(u8::MAX, Stream::index), data.to_vec()));
        }))
    } else {
        block_on(Afs::mount(storage, config, rw, lookup))
    };
    (afs.expect("rust mount failed"), found)
}

pub struct Buffers {
    pub rw: Vec<u8>,
    pub lookup: Vec<u32>,
    pub arena: Vec<u8>,
}

impl Buffers {
    pub fn new(params: &Params, slots: usize) -> Self {
        let max = *params.buffer_sizes.iter().max().unwrap();
        Self {
            rw: vec![0; params.config.min_read_write_size as usize],
            lookup: vec![0; lookup_table_len(params.config.num_blocks)],
            arena: vec![0; max * slots],
        }
    }
}

impl<'a> Pair<'a> {
    pub fn new(
        params: &Params,
        seed: u64,
        buffers: &'a mut Buffers,
        c_storage: SimStorage,
        r_storage: SimStorage,
    ) -> Self {
        let max = *params.buffer_sizes.iter().max().unwrap();
        let mut pair = Pair {
            params: params.clone(),
            seed,
            step: 0,
            rng: Rng::new(seed.wrapping_mul(0x2545_f491_4f6c_dd1d) ^ 0x5eed),
            c: None,
            r: None,
            c_storage,
            r_storage,
            slots: Vec::new(),
            free: Vec::new(),
            fresh: buffers.arena.chunks_exact_mut(max),
            saved: HashMap::new(),
            cursor: None,
            stats: Stats { seeds: 1, ..Stats::default() },
            stopped: false,
        };
        let with_callback = pair.rng.chance(1, 2);
        pair.mount_both(&mut buffers.rw, &mut buffers.lookup, with_callback);
        pair
    }

    fn mount_both(&mut self, rw: &'a mut [u8], lookup: &'a mut [u32], with_callback: bool) {
        self.c_storage.reset_stats();
        self.r_storage.reset_stats();
        let (c, c_found) = CAfs::mount(self.c_storage.clone(), with_callback);
        let (r, r_found) = r_mount(self.r_storage.clone(), self.params.config, rw, lookup, with_callback);
        self.c = Some(c);
        self.r = Some(r);
        let (c_reads, c_bytes) = self.c_storage.read_stats();
        let (r_reads, r_bytes) = self.r_storage.read_stats();
        assert!(
            r_reads <= c_reads && r_bytes <= c_bytes,
            "{}: mount reads {r_reads}/{r_bytes} > {c_reads}/{c_bytes}",
            self.ctx()
        );
        self.stats.mounts += 1;
        self.stats.mount_reads += c_reads;
        self.stats.mount_read_bytes += c_bytes;
        assert_eq!(c_found, r_found, "{}: object_found callbacks differ", self.ctx());
        self.check("mount");
    }

    pub fn ctx(&self) -> String {
        format!("[{} seed={} step={}]", self.params.name, self.seed, self.step)
    }

    pub fn check(&mut self, what: &str) {
        let c = self.c_storage.take_trace();
        let r = self.r_storage.take_trace();
        if c != r {
            let index = c.iter().zip(&r).position(|(a, b)| a != b).unwrap_or(c.len().min(r.len()));
            panic!(
                "{} {what}: storage traces differ at op {index} (c={} ops, rust={} ops)\n  c: {:?}\n  r: {:?}",
                self.ctx(),
                c.len(),
                r.len(),
                c.get(index),
                r.get(index)
            );
        }
        self.stats.api_calls += 1;
        self.stats.trace_checks += 1;
        self.stats.storage_ops += c.len() as u64;
        *self.stats.ops.entry(static_name(what)).or_default() += 1;
    }

    fn c(&mut self) -> &mut CAfs {
        self.c.as_mut().unwrap()
    }

    fn r(&mut self) -> &mut RAfs<'a> {
        self.r.as_mut().unwrap()
    }

    fn take_buffer(&mut self, size: usize) -> Option<&'a mut [u8]> {
        let buf = match self.free.iter().position(|b| b.len() >= size) {
            Some(i) => self.free.swap_remove(i),
            None => self.fresh.next()?,
        };
        Some(&mut buf[..size])
    }

    fn pick_buffer_size(&mut self) -> usize {
        *self.rng.pick(self.params.buffer_sizes)
    }

    fn c_list(&mut self) -> Vec<u16> {
        self.c().list()
    }

    fn open_ids(&self) -> Vec<u16> {
        self.slots.iter().map(|s| s.id).collect()
    }

    pub fn compare_state(&mut self) {
        let c_list = self.c_list();
        let mut cursor = ListCursor::new();
        let r = self.r.as_ref().unwrap();
        let r_list: Vec<u16> = std::iter::from_fn(|| r.list_next(&mut cursor)).map(ObjectId::get).collect();
        assert_eq!(c_list, r_list, "{}: object lists differ", self.ctx());
        let c_size = self.c().size_blocks();
        let c_full = self.c().is_full();
        let r = self.r.as_ref().unwrap();
        assert_eq!((c_size, c_full), (r.size(), r.is_full()), "{}: size/full differ", self.ctx());
        for id in c_list {
            let c_blocks = self.c().num_blocks(id);
            let r_blocks = self.r.as_ref().unwrap().num_blocks(ObjectId::new(id).unwrap());
            assert_eq!(c_blocks, r_blocks, "{}: num_blocks({id}) differ", self.ctx());
        }
        self.check("state");
    }

    fn random_data(&mut self) -> Vec<u8> {
        let p = &self.params;
        let (small, medium, large) = (p.small_write, p.medium_write, p.large_write);
        let length = match self.rng.below(100) {
            0..=69 => self.rng.range(1, small as u64),
            70..=97 => self.rng.range(1, medium as u64),
            _ => self.rng.range(1, large as u64),
        };
        let length = length as usize;
        let mut data = vec![0u8; length];
        if self.rng.chance(1, 4) {
            data.fill(self.rng.below(256) as u8);
        } else {
            self.rng.fill(&mut data);
        }
        data
    }

    fn random_stream(&mut self) -> u8 {
        let streams = self.params.streams;
        match self.rng.below(4) {
            0 | 1 => self.rng.below(2.min(streams as u64)) as u8,
            _ => self.rng.below(streams as u64) as u8,
        }
    }

    fn stop(&mut self) {
        self.stats.stopped_on_c_assert_path += 1;
        self.stopped = true;
    }

    pub fn step(&mut self) {
        self.step += 1;
        let roll = self.rng.below(1000);
        if std::env::var_os("AFS_TRACE_OPS").is_some() {
            eprintln!("{} roll {roll}", self.ctx());
        }
        let has_writer = self.slots.iter().any(|s| matches!(s.r, RHandle::Writer(_)) && !s.close_failed);
        let has_reader = self.slots.iter().any(|s| !matches!(s.r, RHandle::Writer(_)));
        match roll {
            0..=69 if self.slots.len() < 6 => self.op_create(),
            70..=84 if has_writer => self.op_bulk_write(),
            85..=379 if has_writer => self.op_write(),
            380..=429 if has_writer => self.op_close_writer(),
            430..=509 if self.slots.len() < 6 => self.op_open(),
            510..=679 if has_reader => self.op_read(),
            680..=749 if has_reader => self.op_seek(),
            750..=789 if has_reader => self.op_size(),
            790..=819 if has_reader => self.op_position(),
            820..=869 if has_reader => self.op_close_reader(),
            870..=889 => self.op_list_step(),
            890..=914 => self.op_delete(),
            915..=919 if self.slots.is_empty() => self.op_wipe(),
            920..=929 => self.op_prepare(),
            930..=944 => self.op_dump(),
            945..=959 => self.compare_state(),
            960..=969 if self.slots.iter().any(|s| s.close_failed) => self.op_retry_close(),
            970..=999 => {}
            _ => {}
        }
    }

    fn pick_slot(&mut self, want_writer: bool) -> usize {
        let candidates: Vec<usize> = (0..self.slots.len())
            .filter(|&i| matches!(self.slots[i].r, RHandle::Writer(_)) == want_writer && !self.slots[i].close_failed)
            .collect();
        *self.rng.pick(&candidates)
    }

    fn op_create(&mut self) {
        let size = self.pick_buffer_size();
        let Some(buf) = self.take_buffer(size) else { return };
        let (c_id, c_obj) = self.c().create(size);
        let writer = self.r().create(buf).expect("rust create failed");
        assert_eq!(writer.id().get(), c_id, "{}: created object ids differ", self.ctx());
        self.slots.push(Slot {
            c: c_obj,
            r: RHandle::Writer(writer),
            id: c_id,
            stream: 0,
            at_end: false,
            close_failed: false,
        });
        self.check("create");
    }

    fn op_bulk_write(&mut self) {
        let block_size = self.params.config.block_size as u64;
        let total = self.rng.range(block_size / 4, block_size * 2);
        let streams: Vec<u8> = (0..self.rng.range(1, 3)).map(|_| self.random_stream()).collect();
        let max_chunk = self.rng.range(512, 64 * 1024);
        let i = self.pick_slot(true);
        let mut written = 0;
        while written < total && !self.stopped {
            let stream = *self.rng.pick(&streams);
            let length = self.rng.range(1, max_chunk) as usize;
            let mut data = vec![0u8; length];
            self.rng.fill(&mut data);
            if !self.write_slot(i, stream, &data) {
                break;
            }
            written += length as u64;
        }
    }

    fn op_write(&mut self) {
        let i = self.pick_slot(true);
        let stream = self.random_stream();
        let data = self.random_data();
        self.write_slot(i, stream, &data);
    }

    fn write_slot(&mut self, i: usize, stream: u8, data: &[u8]) -> bool {
        let data = data.to_vec();
        let mut slot = self.slots.remove(i);
        let c_ok = self.c().write(&mut slot.c, stream, &data);
        let RHandle::Writer(writer) = &mut slot.r else { unreachable!() };
        let r_result = block_on(writer.write(self.r.as_mut().unwrap(), Stream::from_index(stream).unwrap(), &data));
        self.slots.insert(i, slot);
        match r_result {
            Ok(()) => assert!(c_ok, "{}: write succeeded in rust only", self.ctx()),
            Err(Error::StorageFull) => {
                assert!(!c_ok, "{}: write failed in rust only", self.ctx());
                self.stats.storage_full += 1;
            }
            Err(e) => panic!("{}: rust write error {e:?}", self.ctx()),
        }
        self.check("write");
        c_ok
    }

    fn close_writer_slot(&mut self, i: usize) {
        let mut slot = self.slots.swap_remove(i);
        let c_ok = self.c().close(&mut slot.c);
        let RHandle::Writer(writer) = slot.r else { unreachable!() };
        match block_on(writer.close(self.r.as_mut().unwrap())) {
            Ok(buf) => {
                assert!(c_ok, "{}: close succeeded in rust only", self.ctx());
                self.free.push(buf);
            }
            Err(CloseError { error: Error::StorageFull, writer }) => {
                assert!(!c_ok, "{}: close failed in rust only", self.ctx());
                self.stats.failed_closes += 1;
                slot.r = RHandle::Writer(writer);
                slot.close_failed = true;
                self.slots.push(slot);
            }
            Err(e) => panic!("{}: rust close error {:?}", self.ctx(), e.error),
        }
        self.check("close_writer");
    }

    fn op_close_writer(&mut self) {
        let i = self.pick_slot(true);
        self.close_writer_slot(i);
    }

    fn op_retry_close(&mut self) {
        let i = self.slots.iter().position(|s| s.close_failed).unwrap();
        if self.rng.chance(1, 3) {
            self.close_writer_slot(i);
        }
    }

    fn op_open(&mut self) {
        let list = self.c_list();
        let id = if list.is_empty() || self.rng.chance(1, 20) {
            self.rng.range(1, u16::MAX as u64) as u16
        } else {
            *self.rng.pick(&list)
        };
        let wildcard = self.rng.chance(1, 3);
        let stream = if wildcard { u8::MAX } else { self.random_stream() };
        let size = self.pick_buffer_size();
        let object_id = ObjectId::new(id).unwrap();
        match self.c().open(id, stream, size) {
            None => {
                let mut scratch = vec![0u8; size];
                let r = self.r.as_mut().unwrap();
                let result = if wildcard {
                    block_on(r.open_all(object_id, &mut scratch)).err()
                } else {
                    block_on(r.open(object_id, Stream::from_index(stream).unwrap(), &mut scratch)).err()
                };
                assert_eq!(result, Some(Error::NotFound), "{}: open({id}) should fail", self.ctx());
            }
            Some(c_obj) => {
                let Some(buf) = self.take_buffer(size) else {
                    self.c().close(&mut { c_obj });
                    return;
                };
                let r = self.r.as_mut().unwrap();
                let handle = if wildcard {
                    block_on(r.open_all(object_id, buf)).map(RHandle::Object)
                } else {
                    block_on(r.open(object_id, Stream::from_index(stream).unwrap(), buf)).map(RHandle::Stream)
                };
                let handle = handle.unwrap_or_else(|e| panic!("{}: rust open({id}) failed: {e:?}", self.ctx()));
                self.slots.push(Slot { c: c_obj, r: handle, id, stream, at_end: false, close_failed: false });
            }
        }
        self.check("open");
    }

    fn read_length(&mut self) -> usize {
        let length = match self.rng.below(10) {
            0..=3 => self.rng.range(1, 64),
            4..=7 => self.rng.range(1, 4096),
            _ => self.rng.range(1, self.params.medium_write as u64 * 2),
        };
        length as usize
    }

    fn op_read(&mut self) {
        let i = self.pick_slot(false);
        let length = self.read_length();
        let mut slot = self.slots.swap_remove(i);
        let mut c_buf = vec![0u8; length];
        let mut r_buf = vec![0u8; length];
        let r = self.r.as_mut().unwrap();
        match &mut slot.r {
            RHandle::Stream(reader) => {
                let r_result = block_on(reader.read(r, &mut r_buf));
                let c_len = self.c.as_mut().unwrap().read(&mut slot.c, &mut c_buf);
                let r_len = r_result.unwrap_or_else(|e| panic!("{}: rust read error {e:?}", self.ctx()));
                assert_eq!(c_len, r_len, "{}: read lengths differ", self.ctx());
                assert!(c_buf[..c_len] == r_buf[..r_len], "{}: read data differs", self.ctx());
                slot.at_end |= c_len < length;
                self.stats.bytes_compared += c_len as u64;
            }
            RHandle::Object(reader) => {
                let r_result = block_on(reader.read(r, &mut r_buf));
                let c_result = self.c.as_mut().unwrap().read_chunk(&mut slot.c, &mut c_buf);
                let r_result = r_result.unwrap_or_else(|e| panic!("{}: rust read error {e:?}", self.ctx()));
                let r_result = r_result.map(|(s, n)| (s.index(), n));
                assert_eq!(c_result, r_result, "{}: chunk reads differ", self.ctx());
                if let Some((_, n)) = c_result {
                    assert!(c_buf[..n] == r_buf[..n], "{}: read data differs", self.ctx());
                    self.stats.bytes_compared += n as u64;
                }
                slot.at_end |= c_result.is_none();
            }
            RHandle::Writer(_) => unreachable!(),
        }
        self.slots.push(slot);
        self.check("read");
    }

    fn c_storage_offset(&mut self, slot: usize) -> u64 {
        let pos = self.c.as_mut().unwrap().save_position(&mut self.slots[slot].c);
        u64::from_ne_bytes(pos.0[192..200].try_into().unwrap())
    }

    fn op_seek(&mut self) {
        let i = self.pick_slot(false);
        if self.slots[i].at_end {
            return;
        }
        let storage_offset = self.c_storage_offset(i);
        let block_size = self.params.config.block_size as u64;
        let id = self.slots[i].id;
        let num_blocks = self.c().num_blocks(id) as u64;
        if storage_offset.is_multiple_of(block_size) && storage_offset / block_size >= num_blocks {
            return;
        }
        let offset = match self.rng.below(10) {
            0 => 0,
            1..=4 => self.rng.range(1, 4096),
            5..=7 => self.rng.range(1, self.params.medium_write as u64 * 8),
            _ => self.rng.range(1, self.params.config.block_size as u64 * 4),
        };
        let mut slot = self.slots.swap_remove(i);
        let r = self.r.as_mut().unwrap();
        let r_result = match &mut slot.r {
            RHandle::Stream(reader) => block_on(reader.seek(r, offset)),
            RHandle::Object(reader) => block_on(reader.seek(r, offset)),
            RHandle::Writer(_) => unreachable!(),
        };
        let skipped = match r_result {
            Ok(skipped) => skipped,
            Err(Error::Corrupt) => {
                self.slots.push(slot);
                self.stop();
                return;
            }
            Err(e) => panic!("{}: rust seek error {e:?}", self.ctx()),
        };
        let c_ok = self.c().seek(&mut slot.c, offset);
        assert_eq!(c_ok, skipped == offset, "{}: seek({offset}) results differ (rust skipped {skipped})", self.ctx());
        slot.at_end |= !c_ok;
        self.slots.push(slot);
        self.check("seek");
    }

    fn op_size(&mut self) {
        let i = self.pick_slot(false);
        let mut slot = self.slots.swap_remove(i);
        let r = self.r.as_mut().unwrap();
        let (r_result, mask) = match &mut slot.r {
            RHandle::Stream(reader) => (block_on(reader.size(r)), 0),
            RHandle::Object(reader) => {
                let mask = match self.rng.below(3) {
                    0 => u16::MAX,
                    1 => 1 << self.rng.below(16),
                    _ => (self.rng.below(u16::MAX as u64) + 1) as u16,
                };
                (block_on(reader.size(r, StreamSet::from_bits(mask))), mask)
            }
            RHandle::Writer(_) => unreachable!(),
        };
        let c_size = self.c().size(&mut slot.c, mask);
        let r_size = r_result.unwrap_or_else(|e| panic!("{}: rust size error {e:?}", self.ctx()));
        assert_eq!(c_size, r_size, "{}: sizes differ", self.ctx());
        self.slots.push(slot);
        self.check("size");
    }

    fn op_position(&mut self) {
        let i = self.pick_slot(false);
        let (id, stream) = (self.slots[i].id, self.slots[i].stream);
        let restore =
            self.rng.chance(1, 2) && self.saved.get(&id).is_some_and(|saved| saved.iter().any(|s| s.stream == stream));
        let mut slot = self.slots.swap_remove(i);
        if restore {
            let candidates: Vec<usize> =
                (0..self.saved[&id].len()).filter(|&j| self.saved[&id][j].stream == stream).collect();
            let j = *self.rng.pick(&candidates);
            let (c_pos, r_pos, at_end) = {
                let s = &self.saved[&id][j];
                (s.c, s.r, s.at_end)
            };
            self.c().restore_position(&mut slot.c, c_pos);
            match &mut slot.r {
                RHandle::Stream(reader) => reader.set_position(&r_pos),
                RHandle::Object(reader) => reader.set_position(&r_pos),
                RHandle::Writer(_) => unreachable!(),
            }
            slot.at_end = at_end;
            self.slots.push(slot);
            self.check("restore_position");
        } else {
            let c = self.c().save_position(&mut slot.c);
            let r = match &slot.r {
                RHandle::Stream(reader) => reader.position(),
                RHandle::Object(reader) => reader.position(),
                RHandle::Writer(_) => unreachable!(),
            };
            self.saved.entry(id).or_default().push(Saved { stream, c, r, at_end: slot.at_end });
            self.slots.push(slot);
            self.check("save_position");
        }
    }

    fn op_close_reader(&mut self) {
        let i = self.pick_slot(false);
        let mut slot = self.slots.swap_remove(i);
        assert!(self.c().close(&mut slot.c));
        let r = self.r.as_mut().unwrap();
        let buf = match slot.r {
            RHandle::Stream(reader) => reader.close(r),
            RHandle::Object(reader) => reader.close(r),
            RHandle::Writer(_) => unreachable!(),
        };
        self.free.push(buf);
        self.check("close_reader");
    }

    fn op_list_step(&mut self) {
        let (mut c_entry, mut r_cursor) = self.cursor.take().unwrap_or_default();
        let c_id = self.c().list_next(&mut c_entry);
        let r_id = self.r.as_ref().unwrap().list_next(&mut r_cursor).map(ObjectId::get);
        assert_eq!(c_id, r_id, "{}: list_next differs", self.ctx());
        if c_id.is_some() {
            self.cursor = Some((c_entry, r_cursor));
        }
        self.check("list_next");
    }

    fn op_delete(&mut self) {
        let open = self.open_ids();
        let list: Vec<u16> = self.c_list().into_iter().filter(|id| !open.contains(id)).collect();
        if list.is_empty() {
            return;
        }
        let id = *self.rng.pick(&list);
        self.c().delete(id);
        block_on(self.r().delete(ObjectId::new(id).unwrap())).unwrap_or_else(|e| panic!("rust delete error {e:?}"));
        self.saved.remove(&id);
        self.check("delete");
    }

    fn op_wipe(&mut self) {
        let secure = self.rng.chance(1, 2);
        self.c().wipe(secure);
        block_on(self.r().wipe(secure)).expect("rust wipe failed");
        self.saved.clear();
        self.check("wipe");
    }

    fn op_prepare(&mut self) {
        let n = self.rng.range(1, self.params.config.num_blocks.min(64) as u64) as u16;
        self.c().prepare_storage(n);
        block_on(self.r().prepare_storage(n)).expect("rust prepare_storage failed");
        self.check("prepare_storage");
    }

    fn op_dump(&mut self) {
        let num_blocks = self.params.config.num_blocks;
        let r = self.r.as_mut().unwrap();
        let mut r_out = String::new();
        let (c_out, what) = match self.rng.below(10) {
            0 if self.params.full_dumps
                && !(0..num_blocks).any(|b| c_dump_would_assert(&self.c_storage, b, u32::MAX)) =>
            {
                block_on(r.dump(&mut r_out)).expect("rust dump failed");
                if std::env::var_os("AFS_TRACE_OPS").is_some() {
                    eprintln!("{r_out}");
                }
                (self.c().dump(), "dump")
            }
            0..=5 => {
                let list = self.c.as_mut().unwrap().list();
                let blocks: Vec<u16> = match list.is_empty() {
                    false if self.rng.chance(3, 4) => {
                        let id = *self.rng.pick(&list);
                        let mut text = String::new();
                        r.dump_object(&mut text, ObjectId::new(id).unwrap()).unwrap();
                        text.lines()
                            .filter_map(|l| l.trim_start_matches('[').split(']').next()?.trim().parse().ok())
                            .collect()
                    }
                    _ => Vec::new(),
                };
                let block = match blocks.is_empty() {
                    false => *self.rng.pick(&blocks),
                    true => self.rng.below(num_blocks as u64) as u16,
                };
                let max_chunks = match self.rng.below(3) {
                    0 => u32::MAX,
                    1 => 0,
                    _ => self.rng.range(1, 20) as u32,
                };
                if c_dump_would_assert(&self.c_storage, block, max_chunks) {
                    return;
                }
                if std::env::var_os("AFS_TRACE_OPS").is_some() {
                    eprintln!("{} dump_block({block}, {max_chunks})", self.ctx());
                }
                let r = self.r.as_mut().unwrap();
                block_on(r.dump_block(&mut r_out, block, max_chunks)).expect("rust dump_block failed");
                (self.c().dump_block(block, max_chunks), "dump_block")
            }
            _ => {
                let list = self.c.as_mut().unwrap().list();
                let id = if list.is_empty() { self.rng.range(1, 1000) as u16 } else { *self.rng.pick(&list) };
                r.dump_object(&mut r_out, ObjectId::new(id).unwrap()).expect("rust dump_object failed");
                (self.c().dump_object(id), "dump_object")
            }
        };
        assert_eq!(c_out, r_out, "{}: {what} output differs", self.ctx());
        self.check(what);
    }

    pub fn remount(&mut self, power_cut: bool) {
        if !power_cut {
            assert!(self.slots.is_empty());
            self.c.take().unwrap().deinit();
        }
        let with_callback = self.rng.chance(1, 2);
        self.c = None;
        self.slots.clear();
        self.saved.clear();
        self.cursor = None;
        let (_, rw, lookup) = self.r.take().unwrap().unmount();
        self.mount_both(rw, lookup, with_callback);
    }

    pub fn op_remount(&mut self) {
        let power_cut = !self.slots.is_empty() || self.rng.chance(1, 3);
        self.remount(power_cut);
    }

    pub fn finish(&mut self) {
        assert!(
            self.c_storage.same_image(&self.r_storage),
            "{}: final images differ at {:?}",
            self.ctx(),
            self.c_storage.first_difference(&self.r_storage)
        );
    }
}

pub fn c_dump_would_assert(storage: &SimStorage, block: u16, max_chunks: u32) -> bool {
    let config = storage.inner().config;
    let mut offset = 0u32;
    let mut length = 0u32;
    let mut remaining = max_chunks;
    loop {
        offset = if offset == 0 { 8 } else { offset + 4 + length };
        if offset as u64 + 5 >= config.block_size as u64 {
            return false;
        }
        let mut raw = [0u8; 4];
        storage.raw_read(block, offset, &mut raw);
        let tag = u32::from_le_bytes(raw);
        let chunk_type = (tag >> 24) as u8;
        length = tag & 0xff_ffff;
        match chunk_type {
            0x5e if length.min(32) / 4 >= 8 && length.is_multiple_of(4) => return true,
            0xd0..=0xdf | 0x3e | 0x5e | 0xed => {}
            _ => return false,
        }
        remaining = remaining.wrapping_sub(1);
        if remaining == 0 {
            return false;
        }
    }
}

fn static_name(what: &str) -> &'static str {
    const NAMES: &[&str] = &[
        "mount",
        "state",
        "create",
        "write",
        "close_writer",
        "open",
        "read",
        "seek",
        "size",
        "restore_position",
        "save_position",
        "close_reader",
        "list_next",
        "delete",
        "wipe",
        "prepare_storage",
        "dump",
        "dump_block",
        "dump_object",
    ];
    NAMES.iter().find(|n| **n == what).copied().unwrap_or("other")
}

pub fn run_seed(params: &Params, seed: u64) -> Stats {
    let c_storage = SimStorage::new(params.config);
    let mut rng = Rng::new(seed ^ 0x0001_0000_0000);
    if params.v1_objects > 0 && rng.chance(1, 2) {
        let count = rng.range(1, params.v1_objects as u64) as usize;
        v1::populate(&c_storage, &mut rng, count, params.v1_max_size, params.streams);
    }
    let r_storage = c_storage.deep_clone();
    let mut buffers = Buffers::new(params, 64);
    let mut pair = Pair::new(params, seed, &mut buffers, c_storage, r_storage);
    pair.compare_state();
    for _ in 0..params.ops {
        if pair.rng.chance(15, 1000) {
            pair.op_remount();
        } else {
            pair.step();
        }
        if pair.stopped {
            break;
        }
    }
    pair.compare_state();
    pair.finish();
    pair.stats.clone()
}

impl<'a> Pair<'a> {
    pub fn read_everything(&mut self) {
        let list = self.c_list();
        let size = *self.params.buffer_sizes.first().unwrap();
        for id in list {
            if self.slots.iter().any(|s| s.id == id) {
                continue;
            }
            let object_id = ObjectId::new(id).unwrap();
            let mut c_obj = self.c().open(id, u8::MAX, size).unwrap();
            let mut scratch = vec![0u8; size];
            let mut reader = block_on(self.r().open_all(object_id, &mut scratch)).unwrap();
            let mut c_buf = vec![0u8; 3000];
            let mut r_buf = vec![0u8; 3000];
            let r_size = block_on(reader.size(self.r.as_mut().unwrap(), StreamSet::ALL)).unwrap();
            assert_eq!(self.c().size(&mut c_obj, u16::MAX), r_size, "{}: size({id})", self.ctx());
            loop {
                let c_result = self.c.as_mut().unwrap().read_chunk(&mut c_obj, &mut c_buf);
                let r_result = block_on(reader.read(self.r.as_mut().unwrap(), &mut r_buf))
                    .unwrap_or_else(|e| panic!("{}: rust read error {e:?}", self.ctx()));
                assert_eq!(c_result, r_result.map(|(s, n)| (s.index(), n)), "{}: object {id} chunk", self.ctx());
                let Some((_, n)) = c_result else { break };
                assert!(c_buf[..n] == r_buf[..n]);
                self.stats.bytes_compared += n as u64;
            }
            self.c().close(&mut c_obj);
            reader.close(self.r.as_mut().unwrap());
            self.check("read");
            for stream in Stream::ALL {
                let mut c_obj = self.c().open(id, stream.index(), size).unwrap();
                let mut reader = block_on(self.r().open(object_id, stream, &mut scratch)).unwrap();
                let r_size = block_on(reader.size(self.r.as_mut().unwrap())).unwrap();
                assert_eq!(self.c().size(&mut c_obj, 0), r_size, "{}: size({id}, {stream:?})", self.ctx());
                loop {
                    let c_n = self.c.as_mut().unwrap().read(&mut c_obj, &mut c_buf);
                    let r_n = block_on(reader.read(self.r.as_mut().unwrap(), &mut r_buf)).unwrap();
                    assert_eq!(c_n, r_n, "{}: object {id} stream {stream:?} read", self.ctx());
                    assert!(c_buf[..c_n] == r_buf[..r_n]);
                    self.stats.bytes_compared += c_n as u64;
                    if c_n == 0 {
                        break;
                    }
                }
                self.c().close(&mut c_obj);
                reader.close(self.r.as_mut().unwrap());
                self.check("read");
            }
        }
    }
}
