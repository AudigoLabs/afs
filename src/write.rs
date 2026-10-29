use core::fmt;

use crate::cache::{self, Cache, INVALID_BLOCK, Position};
use crate::format::{
    BLOCK_FOOTER_LENGTH, BlockHeader, CHUNK_HEADER_LENGTH, CHUNK_MAX_LENGTH, CHUNK_TYPE_DATA_FIRST, CHUNK_TYPE_END,
    CHUNK_TYPE_OFFSET, CHUNK_TYPE_SEEK, FOOTER_MAGIC, MIN_CHUNK_SIZE, align_down, align_up, offset_value, seek_value,
    tag,
};
use crate::fs::OpenKind;
use crate::{Afs, Error, NUM_STREAMS, ObjectId, Storage, Stream};

/// An object which is open for writing
pub struct ObjectWriter<'b> {
    id: ObjectId,
    object_offset: [u64; NUM_STREAMS],
    block_offset: [u32; NUM_STREAMS],
    next_block_index: u16,
    cache: Cache<'b>,
}

/// The error returned when closing an [`ObjectWriter`] fails
///
/// The object stays open, so the writer is returned along with the error.
pub struct CloseError<'b, E> {
    /// The cause of the failure
    pub error: Error<E>,
    /// The writer, which is still open
    pub writer: ObjectWriter<'b>,
}

impl fmt::Debug for ObjectWriter<'_> {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        f.debug_struct("ObjectWriter").field("id", &self.id).finish_non_exhaustive()
    }
}

impl<E: fmt::Debug> fmt::Debug for CloseError<'_, E> {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        f.debug_struct("CloseError").field("error", &self.error).field("writer", &self.writer).finish()
    }
}

impl<E: fmt::Display> fmt::Display for CloseError<'_, E> {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        self.error.fmt(f)
    }
}

impl<E: fmt::Debug + fmt::Display> core::error::Error for CloseError<'_, E> {}

impl<'b> ObjectWriter<'b> {
    pub(crate) fn new(id: ObjectId, buffer: &'b mut [u8]) -> Self {
        Self {
            id,
            object_offset: [0; NUM_STREAMS],
            block_offset: [0; NUM_STREAMS],
            next_block_index: 0,
            cache: Cache::new(buffer, INVALID_BLOCK),
        }
    }

    /// Gets the object ID
    pub fn id(&self) -> ObjectId {
        self.id
    }

    fn write_position(&self) -> u32 {
        self.cache.pos.offset.wrapping_add(self.cache.len)
    }

    fn remaining_block_space(&self, block_size: u32) -> u32 {
        block_size.wrapping_sub(BLOCK_FOOTER_LENGTH).wrapping_sub(self.write_position())
    }

    fn remaining_sub_block_space(&self, sub_block_size: u32) -> u32 {
        let position = self.write_position();
        align_up(position, sub_block_size).wrapping_sub(position)
    }

    fn append(&mut self, data: &[u8]) -> Result<(), ()> {
        if self.cache.append(data) { Ok(()) } else { Err(()) }
    }

    async fn flush<S: Storage, const N: usize>(
        &mut self,
        afs: &mut Afs<'_, S, N>,
        pad: bool,
    ) -> Result<(), Error<S::Error>> {
        if self.cache.pos.offset == 0 {
            let block_index = self.next_block_index.wrapping_sub(1);
            let (block, is_erased) = afs.lookup.acquire_block(self.id.get(), block_index).ok_or(Error::StorageFull)?;
            self.cache.pos.block = block;
            if !is_erased {
                cache::erase(&mut afs.storage, &afs.config, &mut afs.cache, block).await?;
            }
        }
        cache::write_cache(&mut afs.storage, &afs.config, &mut self.cache, pad).await
    }

    fn write_seek_chunk<E>(&mut self) -> Result<(), Error<E>> {
        let num_offsets = self.block_offset.iter().filter(|&&offset| offset != 0).count() as u32;
        let header = tag(CHUNK_TYPE_SEEK, num_offsets * 4);
        self.append(&header.to_le_bytes()).map_err(|()| Error::InvalidBuffer)?;
        for stream in 0..NUM_STREAMS {
            let offset = self.block_offset[stream];
            if offset != 0 {
                self.append(&seek_value(stream, offset).to_le_bytes()).map_err(|()| Error::InvalidBuffer)?;
            }
        }
        Ok(())
    }

    async fn write_footer<S: Storage, const N: usize>(
        &mut self,
        afs: &mut Afs<'_, S, N>,
    ) -> Result<(), Error<S::Error>> {
        let block_size = afs.config.block_size;
        let footer_offset = block_size - BLOCK_FOOTER_LENGTH;
        if self.write_position() > footer_offset {
            return Err(Error::Corrupt);
        }
        if self.cache.pos.offset.wrapping_add(self.cache.size()) < block_size {
            if self.cache.pos.offset.wrapping_add(self.cache.size()) > footer_offset {
                return Err(Error::Corrupt);
            }
            self.flush(afs, true).await?;
            if self.cache.pos.block == INVALID_BLOCK || self.cache.len != 0 {
                return Err(Error::Corrupt);
            }
            self.cache.pos.offset = align_down(footer_offset, afs.config.min_read_write_size);
        }
        let footer_buffer_offset = footer_offset - self.cache.pos.offset;
        if self.cache.len < footer_buffer_offset {
            if !self.cache.append_zeros(footer_buffer_offset - self.cache.len) {
                return Err(Error::Corrupt);
            }
        } else if self.cache.len != footer_buffer_offset {
            return Err(Error::Corrupt);
        }
        self.append(&FOOTER_MAGIC).map_err(|()| Error::Corrupt)?;
        self.write_seek_chunk()?;
        self.flush(afs, true).await
    }

    async fn write_data<S: Storage, const N: usize>(
        &mut self,
        afs: &mut Afs<'_, S, N>,
        mut data: &[u8],
    ) -> Result<(), Error<S::Error>> {
        while !data.is_empty() {
            let length = data.len().min(self.cache.space() as usize);
            let (now, rest) = data.split_at(length);
            self.append(now).map_err(|()| Error::Corrupt)?;
            data = rest;
            if self.cache.is_full() {
                self.flush(afs, false).await?;
            }
        }
        Ok(())
    }

    async fn write_block_header<S: Storage, const N: usize>(
        &mut self,
        afs: &mut Afs<'_, S, N>,
    ) -> Result<(), Error<S::Error>> {
        let block_index = self.next_block_index;
        self.next_block_index = block_index.wrapping_add(1);
        self.write_data(afs, &BlockHeader::encode_v2(self.id.get(), block_index)).await?;
        if block_index == 0 {
            return Ok(());
        }
        let num_offsets = self.object_offset.iter().filter(|&&offset| offset != 0).count() as u32;
        self.append(&tag(CHUNK_TYPE_OFFSET, num_offsets * 8).to_le_bytes()).map_err(|()| Error::InvalidBuffer)?;
        for stream in 0..NUM_STREAMS {
            let offset = self.object_offset[stream];
            if offset != 0 {
                self.append(&offset_value(stream, offset).to_le_bytes()).map_err(|()| Error::InvalidBuffer)?;
            }
        }
        Ok(())
    }

    async fn prepare_for_write<S: Storage, const N: usize>(
        &mut self,
        afs: &mut Afs<'_, S, N>,
        length: u32,
    ) -> Result<u32, Error<S::Error>> {
        let block_size = afs.config.block_size;
        let sub_block_size = afs.config.sub_block_size();
        if self.remaining_block_space(block_size) < length {
            self.write_footer(afs).await?;
            self.block_offset = [0; NUM_STREAMS];
            self.cache.len = 0;
            self.cache.pos = Position::new(INVALID_BLOCK, 0);
        }
        if self.write_position() == 0 {
            self.write_block_header(afs).await?;
        }
        let sub_block_space = self.remaining_sub_block_space(sub_block_size);
        if sub_block_space < length {
            if !self.cache.append_zeros(sub_block_space) {
                return Err(Error::Corrupt);
            }
            if self.cache.is_full() {
                self.flush(afs, false).await?;
            }
            self.write_seek_chunk()?;
        }
        match self.remaining_block_space(block_size).min(self.remaining_sub_block_space(sub_block_size)) {
            0 => Err(Error::Corrupt),
            space => Ok(space),
        }
    }

    async fn write_chunk<S: Storage, const N: usize>(
        &mut self,
        afs: &mut Afs<'_, S, N>,
        stream: Stream,
        data: &[u8],
    ) -> Result<usize, Error<S::Error>> {
        let space = self.prepare_for_write(afs, MIN_CHUNK_SIZE).await?;
        let length = u32::try_from(data.len())
            .unwrap_or(u32::MAX)
            .min(space.wrapping_sub(CHUNK_HEADER_LENGTH))
            .min(CHUNK_MAX_LENGTH);
        let header = tag(CHUNK_TYPE_DATA_FIRST | stream.index(), length);
        self.write_data(afs, &header.to_le_bytes()).await?;
        self.write_data(afs, &data[..length as usize]).await?;
        let index = stream.index() as usize;
        self.object_offset[index] = self.object_offset[index].wrapping_add(length as u64);
        self.block_offset[index] = self.block_offset[index].wrapping_add(length);
        Ok(length as usize)
    }

    /// Writes data to a stream of the object
    ///
    /// Returns [`Error::StorageFull`] if the storage is full (see [`Afs::is_full`]).
    pub async fn write<S: Storage, const N: usize>(
        &mut self,
        afs: &mut Afs<'_, S, N>,
        stream: Stream,
        mut data: &[u8],
    ) -> Result<(), Error<S::Error>> {
        while !data.is_empty() {
            let length = self.write_chunk(afs, stream, data).await?;
            data = &data[length..];
        }
        Ok(())
    }

    async fn finish<S: Storage, const N: usize>(&mut self, afs: &mut Afs<'_, S, N>) -> Result<(), Error<S::Error>> {
        self.prepare_for_write(afs, CHUNK_HEADER_LENGTH).await?;
        self.write_data(afs, &tag(CHUNK_TYPE_END, 0).to_le_bytes()).await?;
        self.write_footer(afs).await
    }

    /// Closes the object, returning its buffer
    ///
    /// Fails if writing the end chunk failed due to insufficient space.
    #[allow(clippy::result_large_err)]
    pub async fn close<S: Storage, const N: usize>(
        mut self,
        afs: &mut Afs<'_, S, N>,
    ) -> Result<&'b mut [u8], CloseError<'b, S::Error>> {
        match self.finish(afs).await {
            Ok(()) => {
                afs.open.remove(self.id.get(), OpenKind::Writing);
                Ok(self.cache.into_buffer())
            }
            Err(error) => Err(CloseError { error, writer: self }),
        }
    }
}
