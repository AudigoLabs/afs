const STATE_ERASED: u16 = 0x0000;
pub(crate) const STATE_MAYBE_ERASED: u16 = 0x0001;
pub(crate) const STATE_UNKNOWN: u16 = 0x0002;
pub(crate) const STATE_GARBAGE: u16 = 0x0003;

const fn value(object_id: u16, object_block_index: u16) -> u32 {
    (object_id as u32) << 16 | object_block_index as u32
}

const fn value_object_id(value: u32) -> u16 {
    (value >> 16) as u16
}

const fn value_object_block_index(value: u32) -> u16 {
    value as u16
}

/// Calculates the required length (in words) of the lookup table buffer
pub const fn lookup_table_len(num_blocks: u16) -> usize {
    num_blocks as usize + (num_blocks as usize).div_ceil(32)
}

pub(crate) struct Acquired {
    pub block: u16,
    state: u16,
    was_v2: bool,
}

impl Acquired {
    pub(crate) fn is_erased(&self) -> bool {
        self.state == STATE_ERASED
    }
}

pub(crate) struct LookupTable<'a> {
    buffer: &'a mut [u32],
    num_blocks: usize,
    pub seed: u32,
}

impl<'a> LookupTable<'a> {
    pub(crate) fn new(buf: &'a mut [u32], num_blocks: u16) -> Option<Self> {
        let buffer = buf.get_mut(..lookup_table_len(num_blocks))?;
        buffer.fill(0);
        Some(Self { buffer, num_blocks: num_blocks as usize, seed: 0 })
    }

    pub(crate) fn into_buffer(self) -> &'a mut [u32] {
        self.buffer
    }

    fn values(&self) -> &[u32] {
        &self.buffer[..self.num_blocks]
    }

    fn values_mut(&mut self) -> &mut [u32] {
        &mut self.buffer[..self.num_blocks]
    }

    pub(crate) fn raw_value(&self, block: u16) -> u32 {
        self.buffer[block as usize]
    }

    fn blocks(&self) -> impl Iterator<Item = (u16, u32)> + '_ {
        self.values().iter().enumerate().map(|(block, &value)| (block as u16, value))
    }

    pub(crate) fn set_value(&mut self, block: u16, object_id: u16, object_block_index: u16) {
        self.buffer[block as usize] = value(object_id, object_block_index);
    }

    pub(crate) fn set_free(&mut self, block: u16, state: u16) {
        self.set_value(block, 0, state);
    }

    pub(crate) fn set_is_v2(&mut self, block: u16, is_v2: bool) {
        let mask = 1 << (block % 32);
        let word = &mut self.buffer[self.num_blocks + block as usize / 32];
        if is_v2 {
            *word |= mask;
        } else {
            *word &= !mask;
        }
    }

    pub(crate) fn is_v2(&self, block: u16) -> bool {
        self.buffer[self.num_blocks + block as usize / 32] & 1 << (block % 32) != 0
    }

    pub(crate) fn mix_seed(&mut self, block: u16) {
        self.seed ^= self.buffer[block as usize];
    }

    pub(crate) fn contains_first_block(&self, object_id: u16) -> bool {
        let search = value(object_id, 0);
        self.values().contains(&search)
    }

    pub(crate) fn get_block(&self, object_id: u16, object_block_index: u16) -> Option<u16> {
        let search = value(object_id, object_block_index);
        self.values().iter().position(|&v| v == search).map(|block| block as u16)
    }

    pub(crate) fn object_num_blocks(&self, object_id: u16) -> u16 {
        self.blocks()
            .filter(|&(_, v)| value_object_id(v) == object_id)
            .fold(0u16, |num, (_, v)| (num as u32).max(value_object_block_index(v) as u32 + 1) as u16)
    }

    pub(crate) fn last_block(&self, object_id: u16) -> Option<u16> {
        let mut last = None;
        let mut max_index = 0;
        for (block, v) in self.blocks() {
            if value_object_id(v) != object_id {
                continue;
            }
            let index = value_object_block_index(v);
            if last.is_none() || index > max_index {
                last = Some(block);
                max_index = index;
            }
        }
        last
    }

    pub(crate) fn next_object_id(&mut self) -> u16 {
        loop {
            self.seed = self.seed.wrapping_mul(1_664_525).wrapping_add(1_013_904_223);
            let object_id = self.seed as u16;
            if object_id != 0 && !self.values().iter().any(|&v| value_object_id(v) == object_id) {
                return object_id;
            }
        }
    }

    pub(crate) fn next_object(&self, block: &mut u16) -> Option<u16> {
        for (i, v) in self.blocks().skip(*block as usize) {
            let object_id = value_object_id(v);
            if object_id == 0 || value_object_block_index(v) != 0 {
                continue;
            }
            *block = i + 1;
            return Some(object_id);
        }
        None
    }

    pub(crate) fn delete_object(&mut self, object_id: u16) {
        for v in self.values_mut().iter_mut() {
            if value_object_id(*v) != object_id {
                continue;
            }
            let state = if value_object_block_index(*v) == 0 { STATE_ERASED } else { STATE_GARBAGE };
            *v = value(0, state);
        }
    }

    pub(crate) fn used_blocks(&self) -> u16 {
        self.values().iter().filter(|&&v| value_object_id(v) != 0).count() as u16
    }

    pub(crate) fn is_full(&self) -> bool {
        self.values().iter().all(|&v| value_object_id(v) != 0)
    }

    pub(crate) fn acquire_block(&mut self, object_id: u16, object_block_index: u16) -> Option<Acquired> {
        let mut best: Option<(u16, u16)> = None;
        for (block, v) in self.blocks() {
            if value_object_id(v) != 0 {
                continue;
            }
            let state = value_object_block_index(v);
            if best.is_none_or(|(_, best_state)| state < best_state) {
                best = Some((block, state));
            }
            if state == STATE_ERASED {
                break;
            }
        }
        let (block, state) = best?;
        let was_v2 = self.is_v2(block);
        self.set_value(block, object_id, object_block_index);
        self.set_is_v2(block, true);
        Some(Acquired { block, state, was_v2 })
    }

    pub(crate) fn release_block(&mut self, acquired: &Acquired) {
        self.set_free(acquired.block, acquired.state);
        self.set_is_v2(acquired.block, acquired.was_v2);
    }

    pub(crate) fn next_in_use(&self, start_block: u16, secure: bool) -> Option<(u16, bool)> {
        let (block, v) = self.blocks().skip(start_block as usize).find(|&(_, v)| value_object_id(v) != 0)?;
        Some((block, secure || value_object_block_index(v) == 0))
    }

    pub(crate) fn set_wiped(&mut self, block: u16, erased: bool) {
        self.set_free(block, if erased { STATE_ERASED } else { STATE_GARBAGE });
    }

    pub(crate) fn num_erased(&self) -> u16 {
        self.values().iter().filter(|&&v| v == value(0, STATE_ERASED)).count() as u16
    }

    pub(crate) fn next_pending_erase(&self, start_block: u16) -> Option<u16> {
        let (block, _) = self
            .blocks()
            .skip(start_block as usize)
            .find(|&(_, v)| value_object_id(v) == 0 && value_object_block_index(v) != STATE_ERASED)?;
        Some(block)
    }

    pub(crate) fn object_blocks(&self, object_id: u16) -> impl Iterator<Item = (u16, u16)> + '_ {
        self.blocks()
            .filter(move |&(_, v)| value_object_id(v) == object_id)
            .map(|(block, v)| (block, value_object_block_index(v)))
    }
}
