use super::storage::{BLOCK_SIZE, MemStorage, STORAGE_SIZE};

pub struct Expect<'s> {
    storage: &'s MemStorage,
    offset: u64,
}

impl<'s> Expect<'s> {
    pub fn start(storage: &'s MemStorage) -> Self {
        Self { storage, offset: 0 }
    }

    fn u16(&mut self) -> u16 {
        let b = self.storage.bytes(self.offset, 2);
        self.offset += 2;
        u16::from_le_bytes([b[0], b[1]])
    }

    fn u32(&mut self) -> u32 {
        let b = self.storage.bytes(self.offset, 4);
        self.offset += 4;
        u32::from_le_bytes([b[0], b[1], b[2], b[3]])
    }

    fn u64(&mut self) -> u64 {
        let b = self.storage.bytes(self.offset, 8);
        self.offset += 8;
        u64::from_le_bytes(b.try_into().unwrap())
    }

    fn magic(&mut self, expected: &[u8; 4]) {
        let magic = self.storage.bytes(self.offset, 4);
        assert_eq!(magic, expected, "magic at 0x{:x}", self.offset);
        self.offset += 4;
    }

    pub fn block_header(&mut self, object_id: u16, object_block_index: u16) -> &mut Self {
        self.magic(b"AFS2");
        assert_eq!(self.u16(), object_id);
        assert_eq!(self.u16(), object_block_index);
        self
    }

    pub fn block_header_v1(&mut self, object_id: u16, object_block_index: u16) -> &mut Self {
        self.magic(b"AFS1");
        assert_eq!(self.u16(), object_id);
        assert_eq!(self.u16(), object_block_index);
        self
    }

    pub fn data_chunk(&mut self, stream: u8, data: &[u8]) -> &mut Self {
        let at = self.offset;
        assert_eq!(self.u32(), (0xd0 | stream as u32) << 24 | data.len() as u32, "data tag at 0x{at:x}");
        assert!(self.storage.bytes(self.offset, data.len()) == data, "data at 0x{:x}", self.offset);
        self.offset += data.len() as u64;
        self
    }

    pub fn offset_chunk(&mut self, values: &[u64]) -> &mut Self {
        assert_eq!(self.u32(), 0x3e << 24 | (values.len() as u32 * 8));
        for &value in values {
            assert_eq!(self.u64(), value);
        }
        self
    }

    pub fn seek_chunk(&mut self, values: &[u32]) -> &mut Self {
        let at = self.offset;
        assert_eq!(self.u32(), 0x5e << 24 | (values.len() as u32 * 4), "seek tag at 0x{at:x}");
        for &value in values {
            assert_eq!(self.u32(), value, "seek value at 0x{:x}", self.offset);
        }
        self
    }

    pub fn end_chunk(&mut self) -> &mut Self {
        assert_eq!(self.u32(), 0xed << 24);
        self
    }

    pub fn footer(&mut self) -> &mut Self {
        self.magic(b"afs2");
        self
    }

    pub fn unused(&mut self, length: u64) -> &mut Self {
        let end = self.offset + length;
        assert_eq!(self.storage.first_nonzero(self.offset, end), None, "unused 0x{:x}..0x{end:x}", self.offset);
        self.offset = end;
        self
    }

    pub fn unused_until_footer(&mut self) -> &mut Self {
        self.unused(BLOCK_SIZE as u64 - 128 - self.offset % BLOCK_SIZE as u64)
    }

    pub fn unused_until_block_end(&mut self) -> &mut Self {
        self.unused(BLOCK_SIZE as u64 - self.offset % BLOCK_SIZE as u64)
    }

    pub fn end(&mut self) {
        self.unused(STORAGE_SIZE - self.offset);
        assert_eq!(self.offset, STORAGE_SIZE);
    }
}
