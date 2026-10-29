use crate::format::{BLOCK_FOOTER_LENGTH, MAX_BLOCK_SIZE};

/// The storage configuration
#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash)]
pub struct Config {
    /// The size of a block (should match the AU size of the storage - typically 4MB)
    pub block_size: u32,
    /// The total number of blocks
    pub num_blocks: u16,
    /// The number of sub-blocks per block (block_size must be evenly divisible by this value - typically 256)
    pub sub_blocks_per_block: u32,
    /// The minimum read/write size (should match the block size of the storage - typically 512 bytes)
    pub min_read_write_size: u32,
}

impl Config {
    pub(crate) fn is_valid(&self) -> bool {
        self.num_blocks != 0
            && self.num_blocks != u16::MAX
            && self.min_read_write_size >= BLOCK_FOOTER_LENGTH
            && self.block_size != 0
            && self.block_size <= MAX_BLOCK_SIZE
            && self.block_size % self.min_read_write_size == 0
            && self.sub_blocks_per_block != 0
            && self.block_size % self.sub_blocks_per_block == 0
            && self.block_size / self.sub_blocks_per_block >= BLOCK_FOOTER_LENGTH
    }

    pub(crate) fn sub_block_size(&self) -> u32 {
        self.block_size / self.sub_blocks_per_block
    }

    pub(crate) fn is_valid_object_buffer(&self, size: usize) -> bool {
        let Ok(size) = u32::try_from(size) else {
            return false;
        };
        let sub_block_size = self.sub_block_size();
        size >= BLOCK_FOOTER_LENGTH
            && size >= self.min_read_write_size
            && size % self.min_read_write_size == 0
            && if size > sub_block_size { size % sub_block_size == 0 } else { sub_block_size % size == 0 }
    }
}

/// The interface to the underlying storage device
///
/// Reads and writes are always aligned to, and a multiple of, `min_read_write_size`.
#[allow(async_fn_in_trait)]
pub trait Storage {
    /// The error type returned by the storage device
    type Error;

    /// Reads data from the underlying storage device
    async fn read(&mut self, block: u16, offset: u32, buf: &mut [u8]) -> Result<(), Self::Error>;

    /// Writes data to the underlying storage device
    async fn write(&mut self, block: u16, offset: u32, data: &[u8]) -> Result<(), Self::Error>;

    /// Erases a block on the underlying storage device
    async fn erase(&mut self, block: u16) -> Result<(), Self::Error>;
}

impl<T: Storage + ?Sized> Storage for &mut T {
    type Error = T::Error;

    async fn read(&mut self, block: u16, offset: u32, buf: &mut [u8]) -> Result<(), Self::Error> {
        T::read(self, block, offset, buf).await
    }

    async fn write(&mut self, block: u16, offset: u32, data: &[u8]) -> Result<(), Self::Error> {
        T::write(self, block, offset, data).await
    }

    async fn erase(&mut self, block: u16) -> Result<(), Self::Error> {
        T::erase(self, block).await
    }
}
