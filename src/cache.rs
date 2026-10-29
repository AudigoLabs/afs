use crate::format::{
    BLOCK_FOOTER_LENGTH, BLOCK_HEADER_LENGTH, BlockHeader, CHUNK_TYPE_INVALID_ONE, CHUNK_TYPE_INVALID_ZERO,
    CHUNK_TYPE_OFFSET, CHUNK_TYPE_SEEK, FOOTER_MAGIC, OFFSET_CHUNK_MAX_LENGTH, align_down, align_up,
    offset_value_offset, offset_value_stream, seek_value_offset, seek_value_stream, tag_length, tag_type,
};
use crate::{Config, Error, NUM_STREAMS, Storage};

pub(crate) const INVALID_BLOCK: u16 = u16::MAX;

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub(crate) struct Position {
    pub block: u16,
    pub offset: u32,
}

impl Position {
    pub(crate) const fn new(block: u16, offset: u32) -> Self {
        Self { block, offset }
    }
}

pub(crate) struct Cache<'b> {
    buf: &'b mut [u8],
    pub len: u32,
    pub pos: Position,
}

impl<'b> Cache<'b> {
    pub(crate) fn new(buf: &'b mut [u8], block: u16) -> Self {
        Self { buf, len: 0, pos: Position::new(block, 0) }
    }

    pub(crate) fn size(&self) -> u32 {
        self.buf.len() as u32
    }

    pub(crate) fn data(&self) -> &[u8] {
        self.buf
    }

    pub(crate) fn data_mut(&mut self) -> &mut [u8] {
        self.buf
    }

    pub(crate) fn into_buffer(self) -> &'b mut [u8] {
        self.buf
    }

    pub(crate) fn space(&self) -> u32 {
        self.size() - self.len
    }

    pub(crate) fn is_full(&self) -> bool {
        self.len == self.size()
    }

    fn contains(&self, pos: &Position) -> bool {
        self.len != 0 && self.pos.block == pos.block && self.pos.offset == align_down(pos.offset, self.size())
    }

    fn read(&self, pos: &Position, out: &mut [u8]) -> usize {
        if !self.contains(pos) {
            return 0;
        }
        let index = (pos.offset - self.pos.offset) as usize;
        let available = (self.len as usize).saturating_sub(index);
        let length = out.len().min(available);
        out[..length].copy_from_slice(&self.buf[index..index + length]);
        length
    }

    pub(crate) fn append(&mut self, data: &[u8]) -> bool {
        let start = self.len as usize;
        let Some(dst) = self.buf.get_mut(start..start + data.len()) else {
            return false;
        };
        dst.copy_from_slice(data);
        self.len += data.len() as u32;
        true
    }

    pub(crate) fn append_zeros(&mut self, length: u32) -> bool {
        let start = self.len as usize;
        let Some(dst) = self.buf.get_mut(start..start + length as usize) else {
            return false;
        };
        dst.fill(0);
        self.len += length;
        true
    }

    fn invalidate(&mut self, pos: &Position, length: u32) {
        if pos.block != self.pos.block
            || pos.offset > self.pos.offset.wrapping_add(self.size())
            || pos.offset.wrapping_add(length) <= self.pos.offset
        {
            return;
        }
        self.len = 0;
    }
}

pub(crate) async fn read_data<S: Storage>(
    storage: &mut S,
    config: &Config,
    cache: &mut Cache<'_>,
    pos: &mut Position,
    mut out: &mut [u8],
) -> Result<(), Error<S::Error>> {
    if pos.block == INVALID_BLOCK || pos.offset as u64 + out.len() as u64 > config.block_size as u64 {
        return Err(Error::Corrupt);
    }
    while !out.is_empty() {
        if !cache.contains(pos) {
            cache.pos = Position::new(pos.block, align_down(pos.offset, cache.size()));
            cache.len = 0;
            storage.read(cache.pos.block, cache.pos.offset, cache.buf).await.map_err(Error::Storage)?;
            cache.len = cache.size();
        }
        let length = cache.read(pos, out);
        if length == 0 {
            return Err(Error::Corrupt);
        }
        pos.offset += length as u32;
        out = &mut out[length..];
    }
    Ok(())
}

pub(crate) async fn read_u32<S: Storage>(
    storage: &mut S,
    config: &Config,
    cache: &mut Cache<'_>,
    pos: &mut Position,
) -> Result<u32, Error<S::Error>> {
    let mut raw = [0u8; 4];
    read_data(storage, config, cache, pos, &mut raw).await?;
    Ok(u32::from_le_bytes(raw))
}

pub(crate) async fn write_cache<S: Storage>(
    storage: &mut S,
    config: &Config,
    cache: &mut Cache<'_>,
    pad: bool,
) -> Result<(), Error<S::Error>> {
    let aligned_length = align_up(cache.len, config.min_read_write_size);
    if aligned_length > cache.len {
        if !pad || aligned_length > cache.size() {
            return Err(Error::Corrupt);
        }
        cache.buf[cache.len as usize..aligned_length as usize].fill(0);
    }
    if cache.pos.offset as u64 + aligned_length as u64 > config.block_size as u64 {
        return Err(Error::Corrupt);
    }
    storage
        .write(cache.pos.block, cache.pos.offset, &cache.buf[..aligned_length as usize])
        .await
        .map_err(Error::Storage)?;
    cache.pos.offset += aligned_length;
    cache.len = 0;
    if cache.pos.offset == config.block_size {
        cache.pos = Position::new(INVALID_BLOCK, 0);
    }
    Ok(())
}

pub(crate) async fn erase<S: Storage>(
    storage: &mut S,
    config: &Config,
    cache: &mut Cache<'_>,
    block: u16,
) -> Result<(), Error<S::Error>> {
    let result = storage.erase(block).await;
    cache.invalidate(&Position::new(block, 0), config.block_size);
    result.map_err(Error::Storage)
}

pub(crate) async fn wipe_block_header<S: Storage>(
    storage: &mut S,
    cache: &mut Cache<'_>,
    block: u16,
) -> Result<(), Error<S::Error>> {
    cache.buf.fill(0);
    cache.len = 0;
    storage.write(block, 0, cache.buf).await.map_err(Error::Storage)?;
    cache.pos = Position::new(block, 0);
    cache.len = cache.size();
    Ok(())
}

async fn read_seek_chunk<S: Storage>(
    storage: &mut S,
    config: &Config,
    cache: &mut Cache<'_>,
    pos: &mut Position,
    data: &mut [u32; NUM_STREAMS],
) -> Result<bool, Error<S::Error>> {
    let header = read_u32(storage, config, cache, pos).await?;
    let chunk_type = tag_type(header);
    let length = tag_length(header);
    let num_entries = length / 4;
    if chunk_type == CHUNK_TYPE_INVALID_ZERO || chunk_type == CHUNK_TYPE_INVALID_ONE {
        return Ok(false);
    }
    if chunk_type != CHUNK_TYPE_SEEK
        || length > config.block_size - pos.offset
        || length % 4 != 0
        || num_entries as usize > NUM_STREAMS
    {
        return Err(Error::Corrupt);
    }
    for _ in 0..num_entries {
        let value = read_u32(storage, config, cache, pos).await?;
        let stream = seek_value_stream(value);
        if data[stream] != 0 {
            return Err(Error::Corrupt);
        }
        data[stream] = seek_value_offset(value);
    }
    Ok(true)
}

pub(crate) async fn read_offset_data<S: Storage>(
    storage: &mut S,
    config: &Config,
    cache: &mut Cache<'_>,
    block: u16,
    data: &mut [u64; NUM_STREAMS],
) -> Result<bool, Error<S::Error>> {
    let mut pos = Position::new(block, 0);
    let mut header = [0u8; BLOCK_HEADER_LENGTH as usize];
    read_data(storage, config, cache, &mut pos, &mut header).await?;
    if BlockHeader::parse(&header).is_none() {
        return Err(Error::Corrupt);
    }
    let chunk_header = read_u32(storage, config, cache, &mut pos).await?;
    match tag_type(chunk_header) {
        CHUNK_TYPE_OFFSET => {}
        CHUNK_TYPE_INVALID_ZERO | CHUNK_TYPE_INVALID_ONE => return Ok(false),
        _ => return Err(Error::Corrupt),
    }
    let length = tag_length(chunk_header);
    if length % 8 != 0 || length > OFFSET_CHUNK_MAX_LENGTH {
        return Err(Error::Corrupt);
    }
    let num_streams = length / 8;
    for _ in 0..num_streams {
        let mut raw = [0u8; 8];
        read_data(storage, config, cache, &mut pos, &mut raw).await?;
        let value = u64::from_le_bytes(raw);
        data[offset_value_stream(value)] = offset_value_offset(value);
    }
    Ok(true)
}

pub(crate) async fn read_footer_seek_data<S: Storage>(
    storage: &mut S,
    config: &Config,
    cache: &mut Cache<'_>,
    block: u16,
    data: &mut [u32; NUM_STREAMS],
) -> Result<bool, Error<S::Error>> {
    let mut pos = Position::new(block, config.block_size - BLOCK_FOOTER_LENGTH);
    let mut magic = [0u8; 4];
    read_data(storage, config, cache, &mut pos, &mut magic).await?;
    if magic != FOOTER_MAGIC {
        return Ok(false);
    }
    read_seek_chunk(storage, config, cache, &mut pos, data).await
}

pub(crate) async fn read_seek_data<S: Storage>(
    storage: &mut S,
    config: &Config,
    cache: &mut Cache<'_>,
    block: u16,
    sub_block_index: u32,
    data: &mut [u32; NUM_STREAMS],
) -> Result<bool, Error<S::Error>> {
    if sub_block_index == 0 {
        *data = [0; NUM_STREAMS];
        return Ok(true);
    }
    if sub_block_index == config.sub_blocks_per_block - 1 {
        return read_footer_seek_data(storage, config, cache, block, data).await;
    }
    let mut pos = Position::new(block, sub_block_index.wrapping_mul(config.sub_block_size()));
    read_seek_chunk(storage, config, cache, &mut pos, data).await
}
