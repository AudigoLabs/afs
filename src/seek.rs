use crate::cache;
use crate::format::{block_offset, stream_offset};
use crate::read::ReadState;
use crate::{Afs, Error, NUM_STREAMS, ObjectId, Storage};

const MIN_DATA_OFFSET_FOR_DENSITY: u64 = 1024;
const DENSITY_MULTIPLIER: u64 = 1_000_000;
const DEFAULT_DENSITY: u64 = 980_000;
const MIN_DENSITY: u64 = 1000;

fn clamp(value: u32, lower: u32, upper: u32) -> u32 {
    if value > upper {
        upper
    } else if value < lower {
        lower
    } else {
        value
    }
}

fn estimate_density(data_offset: u64, storage_offset: u64) -> u64 {
    if data_offset < MIN_DATA_OFFSET_FOR_DENSITY {
        return DEFAULT_DENSITY;
    }
    let density = data_offset.wrapping_mul(DENSITY_MULTIPLIER).checked_div(storage_offset).unwrap_or(u64::MAX);
    density.clamp(MIN_DENSITY, DENSITY_MULTIPLIER)
}

fn estimate_index(density: u64, target_offset: u64, region_size: u32) -> u16 {
    (target_offset.wrapping_mul(DENSITY_MULTIPLIER) / density / region_size as u64) as u16
}

fn current_block_index(state: &ReadState<'_>, block_size: u32) -> u16 {
    (state.storage_offset / block_size as u64) as u16
}

struct Search {
    low: u32,
    high: u32,
    probe: u32,
    step: u32,
    start: u32,
    end: u32,
}

impl Search {
    fn new(start: u32, end: u32, estimate: u16) -> Self {
        let probe = clamp((estimate as u32).wrapping_add(1), start.wrapping_add(1), end.wrapping_sub(1));
        Self { low: start, high: end, probe, step: 1, start, end }
    }

    fn is_searching(&self) -> bool {
        self.high.wrapping_sub(self.low) > 1
    }

    fn update(&mut self, is_above: bool) {
        if is_above {
            self.high = self.probe;
        } else {
            self.low = self.probe;
        }
        if self.low != self.start && self.high != self.end {
            self.probe = self.low.wrapping_add(self.high.wrapping_sub(self.low) / 2);
        } else if is_above {
            self.probe = if self.probe > self.low.wrapping_add(self.step) {
                self.probe - self.step
            } else {
                self.low.wrapping_add(1)
            };
            self.step = self.step.wrapping_mul(2);
        } else {
            self.probe = self.probe.wrapping_add(self.step).min(self.high.wrapping_sub(1));
            self.step = self.step.wrapping_mul(2);
        }
    }

    fn result(&self) -> Option<u16> {
        (self.low != self.start).then_some(self.low as u16)
    }
}

async fn offset_chunk_data<S: Storage, const N: usize>(
    afs: &mut Afs<'_, S, N>,
    object_id: ObjectId,
    block_index: u16,
    data: &mut [u64; NUM_STREAMS],
) -> Result<bool, Error<S::Error>> {
    let Some(block) = afs.lookup.get_block(object_id.get(), block_index) else {
        return Ok(false);
    };
    cache::read_offset_data(&mut afs.storage, &afs.config, &mut afs.cache, block, data).await
}

async fn search_block_index<S: Storage, const N: usize>(
    afs: &mut Afs<'_, S, N>,
    state: &ReadState<'_>,
    target_offset: u64,
    new_offsets: &mut [u64; NUM_STREAMS],
) -> Result<Option<u16>, Error<S::Error>> {
    let block_size = afs.config.block_size;
    let current_index = current_block_index(state, block_size);
    let max_index = afs.lookup.object_num_blocks(state.id.get()).wrapping_sub(1);
    if current_index == max_index {
        return Ok(None);
    }
    let density = estimate_density(stream_offset(&state.object_offset, state.stream), state.storage_offset);
    let mut search =
        Search::new(current_index as u32, max_index as u32 + 1, estimate_index(density, target_offset, block_size));
    while search.is_searching() {
        let mut data = [0u64; NUM_STREAMS];
        let offset = match offset_chunk_data(afs, state.id, search.probe as u16, &mut data).await? {
            true => stream_offset(&data, state.stream),
            false => u64::MAX,
        };
        let is_above = offset > target_offset;
        if !is_above {
            *new_offsets = data;
        }
        search.update(is_above);
    }
    Ok(search.result())
}

async fn search_sub_block_index<S: Storage, const N: usize>(
    afs: &mut Afs<'_, S, N>,
    state: &ReadState<'_>,
    block: u16,
    target_offset: u64,
    new_offsets: &mut [u32; NUM_STREAMS],
) -> Result<Option<u16>, Error<S::Error>> {
    let sub_block_size = afs.config.sub_block_size();
    let current_index = ((state.storage_offset % afs.config.block_size as u64) / sub_block_size as u64) as u16;
    let max_index = afs.config.sub_blocks_per_block.wrapping_sub(1) as u16;
    if current_index as u32 + 1 >= max_index as u32 {
        return Ok(None);
    }
    let density = estimate_density(stream_offset(&state.object_offset, state.stream), state.storage_offset);
    let mut search =
        Search::new(current_index as u32, max_index as u32, estimate_index(density, target_offset, sub_block_size));
    while search.is_searching() {
        let mut data = [0u32; NUM_STREAMS];
        let found = cache::read_seek_data(
            &mut afs.storage,
            &afs.config,
            &mut afs.cache,
            block,
            search.probe as u16 as u32,
            &mut data,
        )
        .await?;
        let offset = match found {
            true => block_offset(&data, state.stream) as u64,
            false => u64::MAX,
        };
        let is_above = offset > target_offset;
        if !is_above {
            *new_offsets = data;
        }
        search.update(is_above);
    }
    Ok(search.result())
}

pub(crate) async fn to_block<S: Storage, const N: usize>(
    afs: &mut Afs<'_, S, N>,
    state: &mut ReadState<'_>,
    offset: u64,
) -> Result<u64, Error<S::Error>> {
    let prev_offset = stream_offset(&state.object_offset, state.stream);
    let mut new_offsets = [0u64; NUM_STREAMS];
    let Some(index) = search_block_index(afs, state, prev_offset.wrapping_add(offset), &mut new_offsets).await? else {
        return Ok(offset);
    };
    state.storage_offset = index as u64 * afs.config.block_size as u64;
    state.data_chunk_length = 0;
    state.object_offset = new_offsets;
    state.block_offset = [0; NUM_STREAMS];
    let moved = stream_offset(&new_offsets, state.stream).wrapping_sub(prev_offset);
    offset.checked_sub(moved).ok_or(Error::Corrupt)
}

pub(crate) async fn to_sub_block<S: Storage, const N: usize>(
    afs: &mut Afs<'_, S, N>,
    state: &mut ReadState<'_>,
    offset: u64,
) -> Result<u64, Error<S::Error>> {
    let block_index = current_block_index(state, afs.config.block_size);
    let block = afs.lookup.get_block(state.id.get(), block_index).ok_or(Error::Corrupt)?;
    if !afs.lookup.is_v2(block) {
        return Ok(offset);
    }
    let prev_offset = block_offset(&state.block_offset, state.stream) as u64;
    let mut new_offsets = [0u32; NUM_STREAMS];
    let Some(index) =
        search_sub_block_index(afs, state, block, prev_offset.wrapping_add(offset), &mut new_offsets).await?
    else {
        return Ok(offset);
    };
    state.storage_offset =
        block_index as u64 * afs.config.block_size as u64 + index as u64 * afs.config.sub_block_size() as u64;
    state.data_chunk_length = 0;
    for ((object_offset, &new), &old) in state.object_offset.iter_mut().zip(&new_offsets).zip(&state.block_offset) {
        *object_offset = object_offset.wrapping_add(new.wrapping_sub(old) as u64);
    }
    state.block_offset = new_offsets;
    let moved = (block_offset(&new_offsets, state.stream) as u64).wrapping_sub(prev_offset);
    offset.checked_sub(moved).ok_or(Error::Corrupt)
}

pub(crate) async fn to_last_block<S: Storage, const N: usize>(
    afs: &mut Afs<'_, S, N>,
    state: &mut ReadState<'_>,
) -> Result<(), Error<S::Error>> {
    let current_index = current_block_index(state, afs.config.block_size);
    let mut last_index = afs.lookup.object_num_blocks(state.id.get()).wrapping_sub(1);
    while last_index > current_index {
        let mut data = [0u64; NUM_STREAMS];
        if !offset_chunk_data(afs, state.id, last_index, &mut data).await? {
            last_index -= 1;
            continue;
        }
        state.storage_offset = last_index as u64 * afs.config.block_size as u64;
        state.data_chunk_length = 0;
        state.object_offset = data;
        break;
    }
    Ok(())
}

pub(crate) async fn v2_object_size<S: Storage, const N: usize>(
    afs: &mut Afs<'_, S, N>,
    object_id: ObjectId,
    streams: u16,
) -> Result<Option<u64>, Error<S::Error>> {
    let Some(last_block) = afs.lookup.last_block(object_id.get()) else {
        return Ok(None);
    };
    if !afs.lookup.is_v2(last_block) {
        return Ok(None);
    }
    let mut seek_data = [0u32; NUM_STREAMS];
    if !cache::read_footer_seek_data(&mut afs.storage, &afs.config, &mut afs.cache, last_block, &mut seek_data).await? {
        return Ok(None);
    }
    let mut offset_data = [0u64; NUM_STREAMS];
    if afs.lookup.object_num_blocks(object_id.get()) > 1
        && !cache::read_offset_data(&mut afs.storage, &afs.config, &mut afs.cache, last_block, &mut offset_data).await?
    {
        return Ok(None);
    }
    Ok(Some(
        (0..NUM_STREAMS)
            .filter(|&i| streams & 1 << i != 0)
            .fold(0u64, |size, i| size.wrapping_add(offset_data[i]).wrapping_add(seek_data[i] as u64)),
    ))
}
