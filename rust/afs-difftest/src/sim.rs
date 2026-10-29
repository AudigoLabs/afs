use std::cell::RefCell;
use std::collections::BTreeMap;
use std::fmt;
use std::pin::pin;
use std::rc::Rc;
use std::task::{Context, Poll, Waker};

use afs::{Config, Storage};

pub fn block_on<F: Future>(future: F) -> F::Output {
    let mut future = pin!(future);
    let mut cx = Context::from_waker(Waker::noop());
    loop {
        if let Poll::Ready(output) = future.as_mut().poll(&mut cx) {
            return output;
        }
    }
}

pub struct Rng(u64);

impl Rng {
    pub fn new(seed: u64) -> Self {
        Self(seed ^ 0x9e37_79b9_7f4a_7c15)
    }

    pub fn next_u64(&mut self) -> u64 {
        self.0 = self.0.wrapping_add(0x9e37_79b9_7f4a_7c15);
        let mut z = self.0;
        z = (z ^ (z >> 30)).wrapping_mul(0xbf58_476d_1ce4_e5b9);
        z = (z ^ (z >> 27)).wrapping_mul(0x94d0_49bb_1331_11eb);
        z ^ (z >> 31)
    }

    pub fn below(&mut self, bound: u64) -> u64 {
        if bound == 0 { 0 } else { self.next_u64() % bound }
    }

    pub fn range(&mut self, low: u64, high: u64) -> u64 {
        low + self.below(high - low + 1)
    }

    pub fn chance(&mut self, numerator: u64, denominator: u64) -> bool {
        self.below(denominator) < numerator
    }

    pub fn pick<'t, T>(&mut self, items: &'t [T]) -> &'t T {
        &items[self.below(items.len() as u64) as usize]
    }

    pub fn fill(&mut self, buf: &mut [u8]) {
        for chunk in buf.chunks_mut(8) {
            let bytes = self.next_u64().to_le_bytes();
            chunk.copy_from_slice(&bytes[..chunk.len()]);
        }
    }
}

pub fn hash(data: &[u8]) -> u64 {
    let (words, rest) = data.as_chunks::<8>();
    let h =
        words.iter().fold(0xcbf2_9ce4_8422_2325u64, |h, w| (h ^ u64::from_le_bytes(*w)).wrapping_mul(0x0100_0000_01b3));
    rest.iter().fold(h, |h, &b| (h ^ b as u64).wrapping_mul(0x0100_0000_01b3)).wrapping_add(data.len() as u64)
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Op {
    Read { block: u16, offset: u32, length: u32 },
    Write { block: u16, offset: u32, length: u32, hash: u64 },
    Erase { block: u16 },
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct PowerLoss;

impl fmt::Display for PowerLoss {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        f.write_str("power loss")
    }
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum FailMode {
    Error,
    Ignore,
}

pub struct SimInner {
    pub config: Config,
    blocks: Vec<BTreeMap<u32, Box<[u8]>>>,
    pub trace: Vec<Op>,
    pub num_ops: u64,
    pub reads: u64,
    pub read_bytes: u64,
    pub fail_at: Option<(u64, FailMode, u32)>,
    pub failed: bool,
}

#[derive(Clone)]
pub struct SimStorage(Rc<RefCell<SimInner>>);

impl SimStorage {
    pub fn new(config: Config) -> Self {
        Self(Rc::new(RefCell::new(SimInner {
            config,
            blocks: (0..config.num_blocks).map(|_| BTreeMap::new()).collect(),
            trace: Vec::new(),
            num_ops: 0,
            reads: 0,
            read_bytes: 0,
            fail_at: None,
            failed: false,
        })))
    }

    pub fn inner(&self) -> std::cell::RefMut<'_, SimInner> {
        self.0.borrow_mut()
    }

    pub fn deep_clone(&self) -> Self {
        let inner = self.0.borrow();
        let copy = Self::new(inner.config);
        copy.inner().blocks = inner.blocks.clone();
        copy
    }

    pub fn take_trace(&self) -> Vec<Op> {
        std::mem::take(&mut self.inner().trace)
    }

    pub fn reset_stats(&self) {
        let mut inner = self.inner();
        inner.reads = 0;
        inner.read_bytes = 0;
    }

    pub fn read_stats(&self) -> (u64, u64) {
        let inner = self.0.borrow();
        (inner.reads, inner.read_bytes)
    }

    pub fn same_image(&self, other: &SimStorage) -> bool {
        self.0.borrow().blocks == other.0.borrow().blocks
    }

    pub fn first_difference(&self, other: &SimStorage) -> Option<(u16, u32)> {
        let a = self.0.borrow();
        let b = other.0.borrow();
        for (block, (x, y)) in a.blocks.iter().zip(&b.blocks).enumerate() {
            if x != y {
                let sector = x.keys().chain(y.keys()).copied().find(|k| x.get(k) != y.get(k)).unwrap_or(0);
                return Some((block as u16, sector * a.config.min_read_write_size));
            }
        }
        None
    }

    pub fn fail_at(&self, op: u64, mode: FailMode, torn_sectors: u32) {
        self.inner().fail_at = Some((op, mode, torn_sectors));
    }

    pub fn raw_read(&self, block: u16, offset: u32, buf: &mut [u8]) {
        let inner = self.0.borrow();
        let sector = inner.config.min_read_write_size as usize;
        let start = offset as usize / sector * sector;
        let end = (offset as usize + buf.len()).div_ceil(sector) * sector;
        let mut tmp = vec![0u8; end - start];
        inner.copy_out(block, start as u32, &mut tmp);
        let skip = offset as usize - start;
        buf.copy_from_slice(&tmp[skip..skip + buf.len()]);
    }

    pub fn raw_write(&self, block: u16, offset: u32, data: &[u8]) {
        let mut inner = self.inner();
        let sector = inner.config.min_read_write_size as usize;
        let start = offset as usize / sector * sector;
        let end = (offset as usize + data.len()).div_ceil(sector) * sector;
        let mut buf = vec![0u8; end - start];
        inner.copy_out(block, start as u32, &mut buf);
        buf[offset as usize - start..offset as usize - start + data.len()].copy_from_slice(data);
        inner.copy_in(block, start as u32, &buf);
    }

    pub fn raw_erase(&self, block: u16) {
        self.inner().blocks[block as usize].clear();
    }

    pub fn used_sectors(&self) -> usize {
        self.0.borrow().blocks.iter().map(BTreeMap::len).sum()
    }

    pub fn is_blank(&self) -> bool {
        self.used_sectors() == 0
    }

    pub fn sectors(&self) -> Vec<(u16, u32, Box<[u8]>)> {
        let inner = self.0.borrow();
        let size = inner.config.min_read_write_size;
        inner
            .blocks
            .iter()
            .enumerate()
            .flat_map(|(block, map)| map.iter().map(move |(&s, d)| (block as u16, s * size, d.clone())))
            .collect()
    }

    fn begin_op(&self) -> Result<Option<u32>, ()> {
        let mut inner = self.inner();
        let op = inner.num_ops;
        inner.num_ops += 1;
        if inner.failed {
            return Err(());
        }
        if let Some((fail_op, _, torn)) = inner.fail_at
            && op == fail_op
        {
            inner.failed = true;
            return Ok(Some(torn));
        }
        Ok(None)
    }

    fn fail_mode(&self) -> FailMode {
        self.0.borrow().fail_at.map_or(FailMode::Error, |(_, mode, _)| mode)
    }

    pub fn c_read(&self, block: u16, offset: u32, buf: &mut [u8]) {
        let _ = self.do_read(block, offset, buf);
    }

    pub fn c_write(&self, block: u16, offset: u32, data: &[u8]) {
        let _ = self.do_write(block, offset, data);
    }

    pub fn c_erase(&self, block: u16) {
        let _ = self.do_erase(block);
    }

    fn check(&self, block: u16, offset: u32, length: usize) {
        let inner = self.0.borrow();
        let config = inner.config;
        assert!(block < config.num_blocks, "block {block} out of range");
        assert!(offset as u64 + length as u64 <= config.block_size as u64, "access past block end");
        assert_eq!(offset % config.min_read_write_size, 0, "unaligned offset");
        assert_eq!(length as u32 % config.min_read_write_size, 0, "unaligned length");
    }

    fn do_read(&self, block: u16, offset: u32, buf: &mut [u8]) -> Result<(), PowerLoss> {
        self.check(block, offset, buf.len());
        match self.begin_op() {
            Err(()) | Ok(Some(_)) if self.fail_mode() == FailMode::Error => return Err(PowerLoss),
            _ => {}
        }
        let mut inner = self.inner();
        inner.trace.push(Op::Read { block, offset, length: buf.len() as u32 });
        inner.reads += 1;
        inner.read_bytes += buf.len() as u64;
        inner.copy_out(block, offset, buf);
        Ok(())
    }

    fn do_write(&self, block: u16, offset: u32, data: &[u8]) -> Result<(), PowerLoss> {
        self.check(block, offset, data.len());
        let status = self.begin_op();
        let mode = self.fail_mode();
        let mut inner = self.inner();
        match status {
            Err(()) => {
                return if mode == FailMode::Error { Err(PowerLoss) } else { Ok(()) };
            }
            Ok(Some(torn)) => {
                let sector = inner.config.min_read_write_size as usize;
                let length = data.len().min(torn as usize * sector);
                inner.copy_in(block, offset, &data[..length]);
                return if mode == FailMode::Error { Err(PowerLoss) } else { Ok(()) };
            }
            Ok(None) => {}
        }
        inner.trace.push(Op::Write { block, offset, length: data.len() as u32, hash: hash(data) });
        inner.copy_in(block, offset, data);
        Ok(())
    }

    fn do_erase(&self, block: u16) -> Result<(), PowerLoss> {
        assert!(block < self.0.borrow().config.num_blocks);
        let status = self.begin_op();
        let mode = self.fail_mode();
        let mut inner = self.inner();
        match status {
            Err(()) | Ok(Some(_)) => {
                return if mode == FailMode::Error { Err(PowerLoss) } else { Ok(()) };
            }
            Ok(None) => {}
        }
        inner.trace.push(Op::Erase { block });
        inner.blocks[block as usize].clear();
        Ok(())
    }
}

impl SimInner {
    fn copy_out(&self, block: u16, offset: u32, buf: &mut [u8]) {
        let sector = self.config.min_read_write_size as usize;
        let map = &self.blocks[block as usize];
        for (i, chunk) in buf.chunks_mut(sector).enumerate() {
            let index = offset / sector as u32 + i as u32;
            match map.get(&index) {
                Some(data) => chunk.copy_from_slice(&data[..chunk.len()]),
                None => chunk.fill(0),
            }
        }
    }

    fn copy_in(&mut self, block: u16, offset: u32, data: &[u8]) {
        let sector = self.config.min_read_write_size as usize;
        let map = &mut self.blocks[block as usize];
        for (i, chunk) in data.chunks(sector).enumerate() {
            let index = offset / sector as u32 + i as u32;
            if chunk.iter().all(|&b| b == 0) {
                map.remove(&index);
            } else {
                let mut full = vec![0u8; sector].into_boxed_slice();
                full[..chunk.len()].copy_from_slice(chunk);
                map.insert(index, full);
            }
        }
    }
}

impl Storage for SimStorage {
    type Error = PowerLoss;

    async fn read(&mut self, block: u16, offset: u32, buf: &mut [u8]) -> Result<(), PowerLoss> {
        self.do_read(block, offset, buf)
    }

    async fn write(&mut self, block: u16, offset: u32, data: &[u8]) -> Result<(), PowerLoss> {
        self.do_write(block, offset, data)
    }

    async fn erase(&mut self, block: u16) -> Result<(), PowerLoss> {
        self.do_erase(block)
    }
}
