use std::boxed::Box;
use std::cell::RefCell;
use std::collections::BTreeMap;
use std::fmt;
use std::pin::pin;
use std::rc::Rc;
use std::task::{Context, Poll, Waker};
use std::vec;
use std::vec::Vec;

use crate::{Afs, Config, Storage, lookup_table_len};

pub const READ_WRITE_SIZE: u32 = 512;
pub const BLOCK_SIZE: u32 = 4 * 1024 * 1024;
pub const STORAGE_SIZE: u64 = 1024 * 1024 * 1024;
pub const NUM_BLOCKS: u16 = (STORAGE_SIZE / BLOCK_SIZE as u64) as u16;
pub const SUB_BLOCKS_PER_BLOCK: u32 = 8;

pub const CONFIG: Config = Config {
    block_size: BLOCK_SIZE,
    num_blocks: NUM_BLOCKS,
    sub_blocks_per_block: SUB_BLOCKS_PER_BLOCK,
    min_read_write_size: READ_WRITE_SIZE,
};

pub fn block_on<F: Future>(future: F) -> F::Output {
    let mut future = pin!(future);
    let mut cx = Context::from_waker(Waker::noop());
    loop {
        if let Poll::Ready(output) = future.as_mut().poll(&mut cx) {
            return output;
        }
    }
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct Fault;

impl fmt::Display for Fault {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        f.write_str("fault")
    }
}

#[derive(Default)]
pub struct Inner {
    pub config: Option<Config>,
    sectors: BTreeMap<u64, Box<[u8]>>,
    pub read_calls: u64,
    pub read_bytes: u64,
    pub fail_after: Option<u64>,
    pub ops: u64,
}

#[derive(Clone, Default)]
pub struct MemStorage(pub Rc<RefCell<Inner>>);

impl MemStorage {
    pub fn new(config: Config) -> Self {
        let storage = Self::default();
        storage.0.borrow_mut().config = Some(config);
        storage
    }

    fn config(&self) -> Config {
        self.0.borrow().config.unwrap()
    }

    fn sector_size(&self) -> u64 {
        self.config().min_read_write_size as u64
    }

    fn base(&self, block: u16, offset: u32) -> u64 {
        block as u64 * self.config().block_size as u64 + offset as u64
    }

    pub fn reset_read_stats(&self) {
        let mut inner = self.0.borrow_mut();
        inner.read_calls = 0;
        inner.read_bytes = 0;
    }

    pub fn read_stats(&self) -> (u64, u64) {
        let inner = self.0.borrow();
        (inner.read_calls, inner.read_bytes)
    }

    pub fn byte(&self, address: u64) -> u8 {
        let size = self.sector_size();
        self.0.borrow().sectors.get(&(address / size)).map_or(0, |s| s[(address % size) as usize])
    }

    pub fn bytes(&self, address: u64, length: usize) -> Vec<u8> {
        (0..length as u64).map(|i| self.byte(address + i)).collect()
    }

    pub fn first_nonzero(&self, start: u64, end: u64) -> Option<u64> {
        let size = self.sector_size();
        let inner = self.0.borrow();
        for (&index, data) in inner.sectors.range(start / size..end.div_ceil(size)) {
            for (i, &b) in data.iter().enumerate() {
                let address = index * size + i as u64;
                if b != 0 && address >= start && address < end {
                    return Some(address);
                }
            }
        }
        None
    }

    pub fn is_empty(&self) -> bool {
        self.0.borrow().sectors.is_empty()
    }

    pub fn raw_write(&self, address: u64, data: &[u8]) {
        let size = self.sector_size();
        let mut inner = self.0.borrow_mut();
        for (i, &b) in data.iter().enumerate() {
            let a = address + i as u64;
            let sector = inner.sectors.entry(a / size).or_insert_with(|| vec![0; size as usize].into_boxed_slice());
            sector[(a % size) as usize] = b;
        }
    }

    pub fn wipe_block_header(&self, block: u16) {
        let base = self.base(block, 0);
        self.raw_write(base, &vec![0; READ_WRITE_SIZE as usize]);
    }

    fn check(&self, block: u16, offset: u32, length: usize) -> Result<(), Fault> {
        let config = self.config();
        assert!(block < config.num_blocks);
        assert!(offset as u64 + length as u64 <= config.block_size as u64);
        assert_eq!(offset % config.min_read_write_size, 0);
        assert_eq!(length as u32 % config.min_read_write_size, 0);
        let mut inner = self.0.borrow_mut();
        inner.ops += 1;
        match inner.fail_after {
            Some(limit) if inner.ops > limit => Err(Fault),
            _ => Ok(()),
        }
    }
}

impl Storage for MemStorage {
    type Error = Fault;

    async fn read(&mut self, block: u16, offset: u32, buf: &mut [u8]) -> Result<(), Fault> {
        self.check(block, offset, buf.len())?;
        let base = self.base(block, offset);
        let size = self.sector_size();
        {
            let mut inner = self.0.borrow_mut();
            inner.read_calls += 1;
            inner.read_bytes += buf.len() as u64;
        }
        let inner = self.0.borrow();
        for (i, chunk) in buf.chunks_mut(size as usize).enumerate() {
            match inner.sectors.get(&(base / size + i as u64)) {
                Some(data) => chunk.copy_from_slice(&data[..chunk.len()]),
                None => chunk.fill(0),
            }
        }
        Ok(())
    }

    async fn write(&mut self, block: u16, offset: u32, data: &[u8]) -> Result<(), Fault> {
        self.check(block, offset, data.len())?;
        let base = self.base(block, offset);
        let size = self.sector_size();
        let mut inner = self.0.borrow_mut();
        for (i, chunk) in data.chunks(size as usize).enumerate() {
            let index = base / size + i as u64;
            if chunk.iter().all(|&b| b == 0) {
                inner.sectors.remove(&index);
            } else {
                inner.sectors.insert(index, chunk.to_vec().into_boxed_slice());
            }
        }
        Ok(())
    }

    async fn erase(&mut self, block: u16) -> Result<(), Fault> {
        self.check(block, 0, 0)?;
        let size = self.sector_size();
        let start = self.base(block, 0) / size;
        let end = self.base(block + 1, 0) / size;
        let mut inner = self.0.borrow_mut();
        let keys: Vec<u64> = inner.sectors.range(start..end).map(|(&k, _)| k).collect();
        for key in keys {
            inner.sectors.remove(&key);
        }
        Ok(())
    }
}

pub fn leak<T: Copy>(value: T, length: usize) -> &'static mut [T] {
    Box::leak(vec![value; length].into_boxed_slice())
}

pub type TestAfs = Afs<'static, MemStorage>;

pub fn mount(storage: &MemStorage) -> TestAfs {
    let config = storage.config();
    let rw = leak(0u8, config.min_read_write_size as usize);
    let lookup = leak(0u32, lookup_table_len(config.num_blocks));
    block_on(Afs::mount(storage.clone(), config, rw, lookup)).unwrap()
}

pub fn buffer(size: usize) -> &'static mut [u8] {
    leak(0u8, size)
}

pub fn random_data(length: usize, seed: u64) -> Vec<u8> {
    let mut state = seed ^ 0x9e37_79b9_7f4a_7c15;
    (0..length)
        .map(|_| {
            state ^= state << 13;
            state ^= state >> 7;
            state ^= state << 17;
            state as u8
        })
        .collect()
}
