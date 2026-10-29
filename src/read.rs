use crate::cache::{self, Cache, Position};
use crate::format::{
    BLOCK_FOOTER_LENGTH, BLOCK_HEADER_LENGTH, BlockHeader, CHUNK_HEADER_LENGTH, CHUNK_TYPE_END, CHUNK_TYPE_INVALID_ONE,
    CHUNK_TYPE_INVALID_ZERO, CHUNK_TYPE_OFFSET, CHUNK_TYPE_SEEK, MIN_CHUNK_SIZE, OFFSET_CHUNK_MAX_LENGTH,
    SEEK_CHUNK_MAX_LENGTH, WILDCARD_STREAM, align_up_u64, is_data_chunk, tag_length, tag_type,
};
use crate::fs::OpenKind;
use crate::seek;
use crate::{Afs, Error, NUM_STREAMS, ObjectId, Storage, Stream, StreamSet};

/// Read position used by `position()` and `set_position()` of the readers
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct ReadPosition {
    object_offset: [u64; NUM_STREAMS],
    block_offset: [u32; NUM_STREAMS],
    storage_offset: u64,
    data_chunk_length: u32,
    current_stream: u8,
}

pub(crate) struct ReadState<'b> {
    pub id: ObjectId,
    pub stream: u8,
    pub object_offset: [u64; NUM_STREAMS],
    pub block_offset: [u32; NUM_STREAMS],
    pub storage_offset: u64,
    pub data_chunk_length: u32,
    pub current_stream: u8,
    pub cache: Cache<'b>,
}

enum Step {
    Done,
    Continue(u32),
}

impl<'b> ReadState<'b> {
    pub(crate) fn new(id: ObjectId, stream: u8, cache: Cache<'b>) -> Self {
        Self {
            id,
            stream,
            object_offset: [0; NUM_STREAMS],
            block_offset: [0; NUM_STREAMS],
            storage_offset: 0,
            data_chunk_length: 0,
            current_stream: 0,
            cache,
        }
    }

    pub(crate) fn position(&self) -> ReadPosition {
        ReadPosition {
            object_offset: self.object_offset,
            block_offset: self.block_offset,
            storage_offset: self.storage_offset,
            data_chunk_length: self.data_chunk_length,
            current_stream: self.current_stream,
        }
    }

    pub(crate) fn set_position(&mut self, position: &ReadPosition) {
        self.object_offset = position.object_offset;
        self.block_offset = position.block_offset;
        self.storage_offset = position.storage_offset;
        self.data_chunk_length = position.data_chunk_length;
        self.current_stream = position.current_stream;
    }

    fn selected_stream(&self) -> usize {
        (if self.stream == WILDCARD_STREAM { self.current_stream } else { self.stream }) as usize
    }

    async fn process_block_header<S: Storage, const N: usize>(
        &mut self,
        afs: &mut Afs<'_, S, N>,
        pos: &mut Position,
    ) -> Result<(), Error<S::Error>> {
        let mut raw = [0u8; BLOCK_HEADER_LENGTH as usize];
        cache::read_data(&mut afs.storage, &afs.config, &mut self.cache, pos, &mut raw).await?;
        let header = BlockHeader::parse(&raw).ok_or(Error::Corrupt)?;
        if header.object_id != self.id.get()
            || header.object_block_index as u64 != self.storage_offset / afs.config.block_size as u64
        {
            return Err(Error::Corrupt);
        }
        self.storage_offset += BLOCK_HEADER_LENGTH as u64;
        Ok(())
    }

    fn process_read_data(&mut self, max_length: u32) -> u32 {
        let length = self.data_chunk_length.min(max_length);
        self.data_chunk_length -= length;
        self.storage_offset += length as u64;
        let stream = self.selected_stream();
        self.object_offset[stream] = self.object_offset[stream].wrapping_add(length as u64);
        self.block_offset[stream] = self.block_offset[stream].wrapping_add(length);
        length
    }

    async fn process_new_chunk<S: Storage, const N: usize>(
        &mut self,
        afs: &mut Afs<'_, S, N>,
        pos: &mut Position,
        block_end: u32,
    ) -> Result<Option<bool>, Error<S::Error>> {
        let header = cache::read_u32(&mut afs.storage, &afs.config, &mut self.cache, pos).await?;
        let chunk_type = tag_type(header);
        let length = tag_length(header);
        let length_invalid = match chunk_type {
            t if is_data_chunk(t) => pos.offset + length > block_end,
            CHUNK_TYPE_OFFSET => length > OFFSET_CHUNK_MAX_LENGTH,
            CHUNK_TYPE_SEEK => length > SEEK_CHUNK_MAX_LENGTH,
            CHUNK_TYPE_END => length > 0,
            _ => false,
        };
        if length_invalid {
            return Err(Error::Corrupt);
        }
        match chunk_type {
            t if is_data_chunk(t) => {
                self.storage_offset += CHUNK_HEADER_LENGTH as u64;
                let chunk_stream = t & 0xf;
                if self.stream == WILDCARD_STREAM || chunk_stream == self.stream {
                    self.data_chunk_length = length;
                    self.current_stream = chunk_stream;
                } else {
                    self.storage_offset += length as u64;
                }
                Ok(None)
            }
            CHUNK_TYPE_OFFSET | CHUNK_TYPE_SEEK => {
                self.storage_offset += (CHUNK_HEADER_LENGTH + length) as u64;
                Ok(None)
            }
            CHUNK_TYPE_INVALID_ZERO | CHUNK_TYPE_INVALID_ONE => {
                self.storage_offset = align_up_u64(self.storage_offset, afs.config.block_size as u64);
                Ok(Some(true))
            }
            CHUNK_TYPE_END => Ok(Some(false)),
            _ => Err(Error::Corrupt),
        }
    }

    fn align_storage_offset<S: Storage, const N: usize>(
        &mut self,
        afs: &Afs<'_, S, N>,
        block: u16,
    ) -> Result<(), Error<S::Error>> {
        let block_size = afs.config.block_size;
        let block_offset = (self.storage_offset % block_size as u64) as u32;
        let is_v2 = afs.lookup.is_v2(block);
        let end_offset = block_size - if is_v2 { BLOCK_FOOTER_LENGTH } else { 0 };
        if block_offset > end_offset {
            return Err(Error::Corrupt);
        }
        if end_offset - block_offset < MIN_CHUNK_SIZE {
            self.storage_offset = align_up_u64(self.storage_offset, block_size as u64);
            self.block_offset = [0; NUM_STREAMS];
        }
        if is_v2 {
            let sub_block_size = afs.config.sub_block_size();
            if sub_block_size - block_offset % sub_block_size < MIN_CHUNK_SIZE {
                self.storage_offset = align_up_u64(self.storage_offset, sub_block_size as u64);
            }
        }
        Ok(())
    }

    async fn process<S: Storage, const N: usize>(
        &mut self,
        afs: &mut Afs<'_, S, N>,
        data: Option<&mut [u8]>,
        max_length: u32,
    ) -> Result<Step, Error<S::Error>> {
        let block_size = afs.config.block_size;
        let block_index = (self.storage_offset / block_size as u64) as u16;
        let offset = (self.storage_offset % block_size as u64) as u32;
        let block = match afs.lookup.get_block(self.id.get(), block_index) {
            Some(block) => block,
            None if offset == 0 => return Ok(Step::Done),
            None => return Err(Error::Corrupt),
        };
        let block_end = block_size - if afs.lookup.is_v2(block) { BLOCK_FOOTER_LENGTH } else { 0 };
        if offset >= block_end {
            return Err(Error::Corrupt);
        }
        let mut pos = Position::new(block, offset);

        if offset == 0 {
            self.process_block_header(afs, &mut pos).await?;
            return Ok(Step::Continue(0));
        }
        let mut read_bytes = 0;
        if self.data_chunk_length > 0 {
            let length = self.data_chunk_length.min(max_length);
            if let (Some(data), 1..) = (data, length) {
                let out = data.get_mut(..length as usize).ok_or(Error::Corrupt)?;
                cache::read_data(&mut afs.storage, &afs.config, &mut self.cache, &mut pos, out).await?;
            }
            read_bytes = self.process_read_data(max_length);
        } else if let Some(has_more) = self.process_new_chunk(afs, &mut pos, block_end).await? {
            return Ok(if has_more { Step::Continue(0) } else { Step::Done });
        }

        if self.data_chunk_length == 0 {
            self.align_storage_offset(afs, block)?;
        }
        Ok(Step::Continue(read_bytes))
    }

    async fn read<S: Storage, const N: usize>(
        &mut self,
        afs: &mut Afs<'_, S, N>,
        buf: &mut [u8],
        single_chunk: bool,
    ) -> Result<usize, Error<S::Error>> {
        let mut total = 0usize;
        let mut remaining = u32::try_from(buf.len()).unwrap_or(u32::MAX);
        while remaining > 0 {
            let step = match self.process(afs, Some(&mut buf[total..]), remaining).await {
                Ok(step) => step,
                Err(_) if total > 0 => break,
                Err(e) => return Err(e),
            };
            let Step::Continue(read_bytes) = step else {
                break;
            };
            total += read_bytes as usize;
            remaining -= read_bytes;
            if single_chunk && read_bytes > 0 {
                break;
            }
        }
        Ok(total)
    }

    async fn seek<S: Storage, const N: usize>(
        &mut self,
        afs: &mut Afs<'_, S, N>,
        offset: u64,
    ) -> Result<u64, Error<S::Error>> {
        let remaining = seek::to_block(afs, self, offset).await?;
        let mut remaining = seek::to_sub_block(afs, self, remaining).await?;
        while remaining > 0 {
            let Step::Continue(read_bytes) = self.process(afs, None, remaining.min(u32::MAX as u64) as u32).await?
            else {
                return Ok(offset - remaining);
            };
            remaining -= read_bytes as u64;
        }
        Ok(offset)
    }

    async fn size<S: Storage, const N: usize>(
        &mut self,
        afs: &mut Afs<'_, S, N>,
        streams: u16,
    ) -> Result<u64, Error<S::Error>> {
        if let Some(size) = seek::v2_object_size(afs, self.id, streams).await? {
            return Ok(size);
        }
        let saved = self.position();
        let result = self.size_by_reading(afs, streams).await;
        self.set_position(&saved);
        result
    }

    async fn size_by_reading<S: Storage, const N: usize>(
        &mut self,
        afs: &mut Afs<'_, S, N>,
        streams: u16,
    ) -> Result<u64, Error<S::Error>> {
        seek::to_last_block(afs, self).await?;
        while let Step::Continue(_) = self.process(afs, None, u32::MAX).await? {}
        Ok((0..NUM_STREAMS)
            .filter(|&i| streams & 1 << i != 0)
            .fold(0u64, |size, i| size.wrapping_add(self.object_offset[i])))
    }

    fn close<S: Storage, const N: usize>(self, afs: &mut Afs<'_, S, N>) -> &'b mut [u8] {
        afs.open.remove(self.id.get(), OpenKind::Reading);
        self.cache.into_buffer()
    }
}

/// An object which is open for reading a single stream
pub struct StreamReader<'b> {
    pub(crate) state: ReadState<'b>,
}

impl<'b> StreamReader<'b> {
    /// Gets the object ID
    pub fn id(&self) -> ObjectId {
        self.state.id
    }

    /// Gets the stream being read
    pub fn stream(&self) -> Stream {
        Stream::from_index(self.state.stream).unwrap_or(Stream::S0)
    }

    /// Reads data from the stream and returns the number of bytes read (0 at the end of the stream)
    ///
    /// If a storage error occurs after some data was read, the data read so far is returned.
    pub async fn read<S: Storage, const N: usize>(
        &mut self,
        afs: &mut Afs<'_, S, N>,
        buf: &mut [u8],
    ) -> Result<usize, Error<S::Error>> {
        self.state.read(afs, buf, false).await
    }

    /// Seeks the requested amount further into the object stream and returns the amount actually skipped (less than
    /// `offset` if the end was reached)
    pub async fn seek<S: Storage, const N: usize>(
        &mut self,
        afs: &mut Afs<'_, S, N>,
        offset: u64,
    ) -> Result<u64, Error<S::Error>> {
        self.state.seek(afs, offset).await
    }

    /// Gets the total size of the object stream
    pub async fn size<S: Storage, const N: usize>(&mut self, afs: &mut Afs<'_, S, N>) -> Result<u64, Error<S::Error>> {
        self.state.size(afs, 1 << self.state.stream).await
    }

    /// Saves the current read position
    pub fn position(&self) -> ReadPosition {
        self.state.position()
    }

    /// Restores a previously-saved read position
    pub fn set_position(&mut self, position: &ReadPosition) {
        self.state.set_position(position);
    }

    /// Closes the object, returning its buffer
    pub fn close<S: Storage, const N: usize>(self, afs: &mut Afs<'_, S, N>) -> &'b mut [u8] {
        self.state.close(afs)
    }
}

/// An object which is open for reading all of its streams
pub struct ObjectReader<'b> {
    pub(crate) state: ReadState<'b>,
}

impl<'b> ObjectReader<'b> {
    /// Gets the object ID
    pub fn id(&self) -> ObjectId {
        self.state.id
    }

    /// Reads data from the next data chunk and returns its stream and the number of bytes read (`None` at the end of
    /// the object)
    pub async fn read<S: Storage, const N: usize>(
        &mut self,
        afs: &mut Afs<'_, S, N>,
        buf: &mut [u8],
    ) -> Result<Option<(Stream, usize)>, Error<S::Error>> {
        let length = self.state.read(afs, buf, true).await?;
        Ok(match length {
            0 => None,
            _ => Some((Stream::from_index(self.state.current_stream).ok_or(Error::Corrupt)?, length)),
        })
    }

    /// Seeks the requested amount further into the object stream and returns the amount actually skipped (less than
    /// `offset` if the end was reached)
    pub async fn seek<S: Storage, const N: usize>(
        &mut self,
        afs: &mut Afs<'_, S, N>,
        offset: u64,
    ) -> Result<u64, Error<S::Error>> {
        self.state.seek(afs, offset).await
    }

    /// Gets the total size of the selected streams of the object
    pub async fn size<S: Storage, const N: usize>(
        &mut self,
        afs: &mut Afs<'_, S, N>,
        streams: StreamSet,
    ) -> Result<u64, Error<S::Error>> {
        self.state.size(afs, streams.bits()).await
    }

    /// Saves the current read position
    pub fn position(&self) -> ReadPosition {
        self.state.position()
    }

    /// Restores a previously-saved read position
    pub fn set_position(&mut self, position: &ReadPosition) {
        self.state.set_position(position);
    }

    /// Closes the object, returning its buffer
    pub fn close<S: Storage, const N: usize>(self, afs: &mut Afs<'_, S, N>) -> &'b mut [u8] {
        self.state.close(afs)
    }
}
