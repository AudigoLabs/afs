use crate::cache::{self, Cache, Position};
use crate::format::WILDCARD_STREAM;
use crate::format::{BLOCK_HEADER_LENGTH, BlockHeader, CHUNK_HEADER_LENGTH, is_data_chunk, tag_length, tag_type};
pub use crate::lookup::lookup_table_len;
use crate::lookup::{LookupTable, STATE_GARBAGE, STATE_MAYBE_ERASED, STATE_UNKNOWN};
use crate::read::{ObjectReader, ReadState, StreamReader};
use crate::seek;
use crate::{Config, Error, ObjectId, ObjectWriter, Storage, Stream};

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub(crate) enum OpenKind {
    Reading,
    Writing,
}

pub(crate) struct OpenList<const N: usize> {
    entries: [(u16, OpenKind); N],
    len: usize,
}

impl<const N: usize> OpenList<N> {
    const fn new() -> Self {
        Self { entries: [(0, OpenKind::Reading); N], len: 0 }
    }

    fn is_empty(&self) -> bool {
        self.len == 0
    }

    fn is_full(&self) -> bool {
        self.len == N
    }

    fn entries(&self) -> &[(u16, OpenKind)] {
        &self.entries[..self.len]
    }

    fn add(&mut self, object_id: u16, kind: OpenKind) {
        self.entries.copy_within(..self.len, 1);
        self.entries[0] = (object_id, kind);
        self.len += 1;
    }

    pub(crate) fn remove(&mut self, object_id: u16, kind: OpenKind) {
        if let Some(index) = self.entries().iter().position(|&entry| entry == (object_id, kind)) {
            self.entries.copy_within(index + 1..self.len, index);
            self.len -= 1;
        }
    }

    fn contains(&self, object_id: u16) -> bool {
        self.entries().iter().any(|&(id, _)| id == object_id)
    }

    fn writing_without_storage(&self, lookup: &LookupTable<'_>, skip: u16) -> Option<u16> {
        self.entries()
            .iter()
            .filter(|&&(id, kind)| kind == OpenKind::Writing && lookup.object_num_blocks(id) == 0)
            .nth(skip as usize)
            .map(|&(id, _)| id)
    }
}

/// Iterator context used by [`Afs::list_next`]
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct ListCursor {
    block: u16,
    open_index: u16,
}

impl ListCursor {
    /// Creates a cursor which starts at the first object
    pub const fn new() -> Self {
        Self { block: 0, open_index: 0 }
    }
}

type FoundCallback<'f> = &'f mut dyn FnMut(ObjectId, Option<Stream>, &[u8]);

/// An AFS instance, which owns its storage
///
/// Up to `MAX_OPEN` objects can be open at once.
pub struct Afs<'a, S, const MAX_OPEN: usize = 8> {
    pub(crate) storage: S,
    pub(crate) config: Config,
    pub(crate) lookup: LookupTable<'a>,
    pub(crate) cache: Cache<'a>,
    pub(crate) open: OpenList<MAX_OPEN>,
}

fn object_data_from_cache(cache: &mut Cache<'_>) -> (Option<Stream>, usize) {
    let length = cache.len as usize;
    let buf = cache.data_mut();
    let mut read_offset = BLOCK_HEADER_LENGTH as usize;
    let mut stream = None;
    let mut data_length = 0;
    while length - read_offset >= CHUNK_HEADER_LENGTH as usize {
        let tag =
            u32::from_le_bytes([buf[read_offset], buf[read_offset + 1], buf[read_offset + 2], buf[read_offset + 3]]);
        read_offset += CHUNK_HEADER_LENGTH as usize;
        let chunk_type = tag_type(tag);
        let chunk_stream = chunk_type & 0xf;
        if !is_data_chunk(chunk_type) || *stream.get_or_insert(chunk_stream) != chunk_stream {
            break;
        }
        let chunk_length = (tag_length(tag) as usize).min(length - read_offset);
        buf.copy_within(read_offset..read_offset + chunk_length, data_length);
        read_offset += chunk_length;
        data_length += chunk_length;
    }
    cache.len = 0;
    (stream.and_then(Stream::from_index), data_length)
}

impl<'a, S: Storage, const MAX_OPEN: usize> Afs<'a, S, MAX_OPEN> {
    /// Initializes and mounts the file system
    ///
    /// `read_write_buffer` must hold at least `min_read_write_size` bytes and `lookup_table` at least
    /// [`lookup_table_len`] words.
    pub async fn mount(
        storage: S,
        config: Config,
        read_write_buffer: &'a mut [u8],
        lookup_table: &'a mut [u32],
    ) -> Result<Self, Error<S::Error>> {
        Self::mount_inner(storage, config, read_write_buffer, lookup_table, None).await
    }

    /// Initializes and mounts the file system, calling `on_object_found` as objects are found
    ///
    /// The callback receives the object ID, the stream of the first data chunk (if any) and the data from the start of
    /// the object which fits within the first `min_read_write_size` bytes of its first block.
    pub async fn mount_with<F: FnMut(ObjectId, Option<Stream>, &[u8])>(
        storage: S,
        config: Config,
        read_write_buffer: &'a mut [u8],
        lookup_table: &'a mut [u32],
        mut on_object_found: F,
    ) -> Result<Self, Error<S::Error>> {
        Self::mount_inner(storage, config, read_write_buffer, lookup_table, Some(&mut on_object_found)).await
    }

    async fn mount_inner(
        storage: S,
        config: Config,
        read_write_buffer: &'a mut [u8],
        lookup_table: &'a mut [u32],
        on_object_found: Option<FoundCallback<'_>>,
    ) -> Result<Self, Error<S::Error>> {
        if !config.is_valid() {
            return Err(Error::InvalidConfig);
        }
        let read_write_buffer =
            read_write_buffer.get_mut(..config.min_read_write_size as usize).ok_or(Error::InvalidBuffer)?;
        let lookup = LookupTable::new(lookup_table, config.num_blocks).ok_or(Error::InvalidBuffer)?;
        let mut afs = Self { storage, config, lookup, cache: Cache::new(read_write_buffer, 0), open: OpenList::new() };
        afs.populate(on_object_found).await?;
        Ok(afs)
    }

    async fn populate(&mut self, mut on_object_found: Option<FoundCallback<'_>>) -> Result<(), Error<S::Error>> {
        for block in 0..self.config.num_blocks {
            let mut pos = Position::new(block, 0);
            let mut raw = [0u8; BLOCK_HEADER_LENGTH as usize];
            cache::read_data(&mut self.storage, &self.config, &mut self.cache, &mut pos, &mut raw).await?;
            let header = BlockHeader::parse(&raw);
            match &header {
                Some(header) => {
                    self.lookup.set_value(block, header.object_id, header.object_block_index);
                    if let (0, Some(callback)) = (header.object_block_index, on_object_found.as_mut()) {
                        let (stream, length) = object_data_from_cache(&mut self.cache);
                        if let Some(object_id) = ObjectId::new(header.object_id) {
                            callback(object_id, stream, &self.cache.data()[..length]);
                        }
                    }
                }
                None => {
                    let state =
                        if raw == [0; BLOCK_HEADER_LENGTH as usize] { STATE_MAYBE_ERASED } else { STATE_UNKNOWN };
                    self.lookup.set_free(block, state);
                }
            }
            self.lookup.set_is_v2(block, header.is_some_and(|header| header.is_v2));
            self.lookup.mix_seed(block);
        }

        for block in 0..self.config.num_blocks {
            let value = self.lookup.raw_value(block);
            let object_id = (value >> 16) as u16;
            if object_id == 0 || value as u16 == 0 || self.lookup.contains_first_block(object_id) {
                continue;
            }
            self.lookup.set_free(block, STATE_GARBAGE);
            cache::wipe_block_header(&mut self.storage, &mut self.cache, block).await?;
        }
        Ok(())
    }

    /// De-initializes the file system, returning the storage and buffers
    pub fn unmount(self) -> (S, &'a mut [u8], &'a mut [u32]) {
        (self.storage, self.cache.into_buffer(), self.lookup.into_buffer())
    }

    /// Gets the storage configuration
    pub fn config(&self) -> &Config {
        &self.config
    }

    /// Gets the underlying storage
    pub fn storage(&self) -> &S {
        &self.storage
    }

    /// Returns whether or not any objects are open
    pub fn has_open_objects(&self) -> bool {
        !self.open.is_empty()
    }

    fn check_object_buffer(&self, buffer: &[u8]) -> Result<(), Error<S::Error>> {
        if !self.config.is_valid_object_buffer(buffer.len()) {
            return Err(Error::InvalidBuffer);
        }
        if self.open.is_full() {
            return Err(Error::TooManyOpen);
        }
        Ok(())
    }

    /// Creates a new object for writing
    ///
    /// `buffer` must be a multiple of `min_read_write_size`, either a multiple or a divisor of the sub-block size, and a
    /// divisor of the block size.
    pub fn create<'b>(&mut self, buffer: &'b mut [u8]) -> Result<ObjectWriter<'b>, Error<S::Error>> {
        self.check_object_buffer(buffer)?;
        let object_id = self.lookup.next_object_id();
        self.open.add(object_id, OpenKind::Writing);
        Ok(ObjectWriter::new(ObjectId::new(object_id).ok_or(Error::Corrupt)?, buffer))
    }

    fn open_state<'b>(
        &mut self,
        object_id: ObjectId,
        stream: u8,
        buffer: &'b mut [u8],
    ) -> Result<ReadState<'b>, Error<S::Error>> {
        self.check_object_buffer(buffer)?;
        let block = self.lookup.get_block(object_id.get(), 0).ok_or(Error::NotFound)?;
        self.open.add(object_id.get(), OpenKind::Reading);
        Ok(ReadState::new(object_id, stream, Cache::new(buffer, block)))
    }

    /// Opens an existing object for reading a single stream
    pub async fn open<'b>(
        &mut self,
        object_id: ObjectId,
        stream: Stream,
        buffer: &'b mut [u8],
    ) -> Result<StreamReader<'b>, Error<S::Error>> {
        let mut state = self.open_state(object_id, stream.index(), buffer)?;
        let result = match seek::to_block(self, &mut state, 0).await {
            Ok(_) => seek::to_sub_block(self, &mut state, 0).await,
            Err(e) => Err(e),
        };
        if let Err(e) = result {
            self.open.remove(object_id.get(), OpenKind::Reading);
            return Err(e);
        }
        Ok(StreamReader { state })
    }

    /// Opens an existing object for reading all of its streams
    pub async fn open_all<'b>(
        &mut self,
        object_id: ObjectId,
        buffer: &'b mut [u8],
    ) -> Result<ObjectReader<'b>, Error<S::Error>> {
        Ok(ObjectReader { state: self.open_state(object_id, WILDCARD_STREAM, buffer)? })
    }

    /// Lists all objects in the file system (returns `None` if there are no more)
    ///
    /// The cursor should initially be [`ListCursor::new`] and then continually passed to retrieve the next object.
    pub fn list_next(&self, cursor: &mut ListCursor) -> Option<ObjectId> {
        if let Some(object_id) = self.lookup.next_object(&mut cursor.block) {
            return ObjectId::new(object_id);
        }
        let object_id = self.open.writing_without_storage(&self.lookup, cursor.open_index)?;
        cursor.open_index += 1;
        ObjectId::new(object_id)
    }

    /// Gets the number of blocks used by an object (will be larger than the actual object data size)
    pub fn num_blocks(&self, object_id: ObjectId) -> u16 {
        self.lookup.object_num_blocks(object_id.get())
    }

    /// Deletes an object from the file system
    pub async fn delete(&mut self, object_id: ObjectId) -> Result<(), Error<S::Error>> {
        let id = object_id.get();
        if self.open.contains(id) {
            return Err(Error::ObjectOpen);
        }
        let first_block = self.lookup.get_block(id, 0).ok_or(Error::NotFound)?;
        cache::erase(&mut self.storage, &self.config, &mut self.cache, first_block).await?;
        for index in 1..self.lookup.object_num_blocks(id) {
            if let Some(block) = self.lookup.get_block(id, index) {
                cache::wipe_block_header(&mut self.storage, &mut self.cache, block).await?;
            }
        }
        self.lookup.delete_object(id);
        Ok(())
    }

    /// Deletes all objects from the file system
    ///
    /// With `secure`, every block used by an object is erased; otherwise only the first block of each object is erased
    /// and the headers of the others are wiped.
    pub async fn wipe(&mut self, secure: bool) -> Result<(), Error<S::Error>> {
        if !self.open.is_empty() {
            return Err(Error::ObjectOpen);
        }
        let mut block = 0;
        while let Some((next, should_erase)) = self.lookup.next_in_use(block, secure) {
            block = next;
            if should_erase {
                cache::erase(&mut self.storage, &self.config, &mut self.cache, block).await?;
            } else {
                cache::wipe_block_header(&mut self.storage, &mut self.cache, block).await?;
            }
            self.lookup.set_wiped(block, should_erase);
        }
        Ok(())
    }

    /// Gets the total size of the file system as a total number of blocks being used
    pub fn size(&self) -> u16 {
        self.lookup.used_blocks()
    }

    /// Returns whether or not the store is full (which causes writes to fail)
    pub fn is_full(&self) -> bool {
        self.lookup.is_full()
    }

    /// Prepares the backing storage for writing to the specified number of blocks
    pub async fn prepare_storage(&mut self, num_blocks: u16) -> Result<(), Error<S::Error>> {
        let num_erased = self.lookup.num_erased();
        if num_erased >= num_blocks {
            return Ok(());
        }
        let mut block = 0;
        for _ in num_erased..num_blocks {
            let Some(next) = self.lookup.next_pending_erase(block) else {
                break;
            };
            block = next;
            cache::erase(&mut self.storage, &self.config, &mut self.cache, block).await?;
            self.lookup.set_wiped(block, true);
        }
        Ok(())
    }
}
