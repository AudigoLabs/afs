use afs::Config;

use crate::sim::{Rng, SimStorage};

pub struct V1Object {
    pub id: u16,
    pub chunks: Vec<(u8, Vec<u8>)>,
    pub blocks: Vec<u16>,
}

impl V1Object {
    pub fn stream_data(&self, stream: u8) -> Vec<u8> {
        self.chunks.iter().filter(|(s, _)| *s == stream).flat_map(|(_, d)| d.iter().copied()).collect()
    }
}

struct BlockWriter {
    data: Vec<u8>,
    block: u16,
}

pub fn write_object(
    storage: &SimStorage,
    rng: &mut Rng,
    config: Config,
    free_blocks: &mut Vec<u16>,
    id: u16,
    total: usize,
    streams: u8,
) -> Option<V1Object> {
    let block_size = config.block_size as usize;
    let mut object = V1Object { id, chunks: Vec::new(), blocks: Vec::new() };
    let mut offsets = [0u64; 16];
    let mut current: Option<BlockWriter> = None;
    let mut remaining = total;
    let flush = |w: BlockWriter| storage.raw_write(w.block, 0, &w.data);
    loop {
        let need = if remaining > 0 { 5 } else { 4 };
        let leave_early = rng.chance(1, 40);
        let has_room = current.as_ref().is_some_and(|w| block_size - w.data.len() >= 5.max(need) && !leave_early);
        if !has_room {
            if let Some(w) = current.take() {
                flush(w);
            }
            if free_blocks.is_empty() {
                for &block in &object.blocks {
                    storage.raw_erase(block);
                }
                return None;
            }
            let block = free_blocks.remove(rng.below(free_blocks.len() as u64) as usize);
            let index = object.blocks.len() as u16;
            object.blocks.push(block);
            let mut data = Vec::with_capacity(64);
            data.extend_from_slice(b"AFS1");
            data.extend_from_slice(&id.to_le_bytes());
            data.extend_from_slice(&index.to_le_bytes());
            if index > 0 {
                let entries: Vec<u64> =
                    (0..16).filter(|&s| offsets[s] != 0).map(|s| (s as u64) << 60 | offsets[s]).collect();
                data.extend_from_slice(&(0x3e00_0000u32 | (entries.len() as u32 * 8)).to_le_bytes());
                for entry in entries {
                    data.extend_from_slice(&entry.to_le_bytes());
                }
            }
            current = Some(BlockWriter { data, block });
            continue;
        }
        let w = current.as_mut().unwrap();
        if remaining == 0 {
            w.data.extend_from_slice(&0xed00_0000u32.to_le_bytes());
            let w = current.take().unwrap();
            flush(w);
            return Some(object);
        }
        let space = block_size - w.data.len() - 4;
        let stream = rng.below(streams as u64) as u8;
        let length = remaining.min(space).min(rng.range(1, 3000) as usize).min(0xff_ffff);
        let mut chunk = vec![0u8; length];
        rng.fill(&mut chunk);
        w.data.extend_from_slice(&((0xd0 | stream as u32) << 24 | length as u32).to_le_bytes());
        w.data.extend_from_slice(&chunk);
        offsets[stream as usize] += length as u64;
        remaining -= length;
        object.chunks.push((stream, chunk));
    }
}

pub fn populate(storage: &SimStorage, rng: &mut Rng, count: usize, max_size: usize, streams: u8) -> Vec<V1Object> {
    let config = storage.inner().config;
    let mut free_blocks: Vec<u16> = (0..config.num_blocks).collect();
    let mut objects = Vec::new();
    let mut next_id = rng.range(1, 60000) as u16;
    for _ in 0..count {
        let total = match rng.below(10) {
            0 => 0,
            1..=6 => rng.range(1, 4096) as usize,
            _ => rng.range(1, max_size as u64) as usize,
        };
        if free_blocks.len() < total / (config.block_size as usize / 2) + 2 {
            break;
        }
        let id = next_id;
        next_id = next_id.wrapping_add(rng.range(1, 500) as u16).max(1);
        match write_object(storage, rng, config, &mut free_blocks, id, total, streams) {
            Some(object) => objects.push(object),
            None => break,
        }
    }
    objects
}
