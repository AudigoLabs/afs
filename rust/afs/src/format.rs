pub(crate) const CHUNK_TYPE_DATA_FIRST: u8 = 0xd0;
pub(crate) const CHUNK_TYPE_DATA_LAST: u8 = 0xdf;
pub(crate) const CHUNK_TYPE_END: u8 = 0xed;
pub(crate) const CHUNK_TYPE_OFFSET: u8 = 0x3e;
pub(crate) const CHUNK_TYPE_SEEK: u8 = 0x5e;
pub(crate) const CHUNK_TYPE_INVALID_ZERO: u8 = 0x00;
pub(crate) const CHUNK_TYPE_INVALID_ONE: u8 = 0xff;

pub(crate) const CHUNK_MAX_LENGTH: u32 = 0x00ff_ffff;
pub(crate) const CHUNK_HEADER_LENGTH: u32 = 4;
pub(crate) const MIN_CHUNK_SIZE: u32 = CHUNK_HEADER_LENGTH + 1;

pub(crate) const BLOCK_HEADER_LENGTH: u32 = 8;
pub(crate) const BLOCK_FOOTER_LENGTH: u32 = 128;
pub(crate) const MAX_BLOCK_SIZE: u32 = 1 << 28;

pub(crate) const OFFSET_CHUNK_MAX_LENGTH: u32 = 8 * 16;
pub(crate) const SEEK_CHUNK_MAX_LENGTH: u32 = 4 * 16;

pub(crate) const HEADER_MAGIC_V1: [u8; 4] = *b"AFS1";
pub(crate) const HEADER_MAGIC_V2: [u8; 4] = *b"AFS2";
pub(crate) const FOOTER_MAGIC: [u8; 4] = *b"afs2";

pub(crate) const WILDCARD_STREAM: u8 = u8::MAX;

const OFFSET_VALUE_MASK: u64 = 0x0fff_ffff_ffff_ffff;
const SEEK_VALUE_MASK: u32 = 0x0fff_ffff;

pub(crate) const fn tag(chunk_type: u8, length: u32) -> u32 {
    (chunk_type as u32) << 24 | (length & CHUNK_MAX_LENGTH)
}

pub(crate) const fn tag_type(tag: u32) -> u8 {
    (tag >> 24) as u8
}

pub(crate) const fn tag_length(tag: u32) -> u32 {
    tag & CHUNK_MAX_LENGTH
}

pub(crate) const fn is_data_chunk(chunk_type: u8) -> bool {
    chunk_type >= CHUNK_TYPE_DATA_FIRST && chunk_type <= CHUNK_TYPE_DATA_LAST
}

pub(crate) const fn offset_value(stream: usize, offset: u64) -> u64 {
    (stream as u64) << 60 | (offset & OFFSET_VALUE_MASK)
}

pub(crate) const fn offset_value_stream(value: u64) -> usize {
    (value >> 60) as usize
}

pub(crate) const fn offset_value_offset(value: u64) -> u64 {
    value & OFFSET_VALUE_MASK
}

pub(crate) const fn seek_value(stream: usize, offset: u32) -> u32 {
    (stream as u32) << 28 | (offset & SEEK_VALUE_MASK)
}

pub(crate) const fn seek_value_stream(value: u32) -> usize {
    (value >> 28) as usize
}

pub(crate) const fn seek_value_offset(value: u32) -> u32 {
    value & SEEK_VALUE_MASK
}

pub(crate) struct BlockHeader {
    pub object_id: u16,
    pub object_block_index: u16,
    pub is_v2: bool,
}

impl BlockHeader {
    pub(crate) fn parse(raw: &[u8; BLOCK_HEADER_LENGTH as usize]) -> Option<Self> {
        let is_v2 = match [raw[0], raw[1], raw[2], raw[3]] {
            HEADER_MAGIC_V1 => false,
            HEADER_MAGIC_V2 => true,
            _ => return None,
        };
        Some(Self {
            object_id: u16::from_le_bytes([raw[4], raw[5]]),
            object_block_index: u16::from_le_bytes([raw[6], raw[7]]),
            is_v2,
        })
    }

    pub(crate) fn encode_v2(object_id: u16, object_block_index: u16) -> [u8; BLOCK_HEADER_LENGTH as usize] {
        let id = object_id.to_le_bytes();
        let index = object_block_index.to_le_bytes();
        let [m0, m1, m2, m3] = HEADER_MAGIC_V2;
        [m0, m1, m2, m3, id[0], id[1], index[0], index[1]]
    }
}

pub(crate) const fn align_down(value: u32, alignment: u32) -> u32 {
    value - value % alignment
}

pub(crate) const fn align_up(value: u32, alignment: u32) -> u32 {
    let tmp = value.wrapping_add(alignment).wrapping_sub(1);
    tmp - tmp % alignment
}

pub(crate) const fn align_up_u64(value: u64, alignment: u64) -> u64 {
    let tmp = value.wrapping_add(alignment).wrapping_sub(1);
    tmp - tmp % alignment
}

pub(crate) fn stream_offset(offsets: &[u64; 16], stream: u8) -> u64 {
    match offsets.get(stream as usize) {
        Some(&offset) => offset,
        None => offsets.iter().fold(0u64, |sum, &offset| sum.wrapping_add(offset)),
    }
}

pub(crate) fn block_offset(offsets: &[u32; 16], stream: u8) -> u32 {
    match offsets.get(stream as usize) {
        Some(&offset) => offset,
        None => offsets.iter().fold(0u64, |sum, &offset| sum + offset as u64) as u32,
    }
}
