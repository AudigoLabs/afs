use core::fmt::Write;

use crate::cache::{self, Position};
use crate::format::{
    BLOCK_HEADER_LENGTH, CHUNK_HEADER_LENGTH, CHUNK_TYPE_END, CHUNK_TYPE_INVALID_ONE, CHUNK_TYPE_INVALID_ZERO,
    CHUNK_TYPE_OFFSET, CHUNK_TYPE_SEEK, is_data_chunk, offset_value_offset, offset_value_stream, seek_value_offset,
    seek_value_stream, tag_length, tag_type,
};
use crate::{Afs, Error, ObjectId, Storage};

const MAX_DUMP_DATA: usize = 32;

fn write_data_hex(w: &mut impl Write, data: &[u8]) -> core::fmt::Result {
    data.iter().try_for_each(|byte| write!(w, "{byte:02x}"))
}

fn write_offset_entries(w: &mut impl Write, length: u32, data: &[u8]) -> core::fmt::Result {
    if length % 8 != 0 {
        return write!(w, "<invalid length ({length})>");
    }
    for (i, raw) in data.chunks_exact(8).enumerate() {
        let value = u64::from_le_bytes([raw[0], raw[1], raw[2], raw[3], raw[4], raw[5], raw[6], raw[7]]);
        let offset = offset_value_offset(value);
        if i > 0 {
            w.write_char(',')?;
        }
        write!(w, "{{0x{:x},0x{:02x}{:08x}}}", offset_value_stream(value), offset >> 32, offset as u32)?;
    }
    Ok(())
}

fn write_seek_entries(w: &mut impl Write, length: u32, data: &[u8]) -> core::fmt::Result {
    if length % 4 != 0 {
        return write!(w, "<invalid length ({length})>");
    }
    for (i, raw) in data.chunks_exact(4).enumerate() {
        let value = u32::from_le_bytes([raw[0], raw[1], raw[2], raw[3]]);
        if i > 0 {
            w.write_char(',')?;
        }
        write!(w, "{{0x{:x},0x{:08x}}}", seek_value_stream(value), seek_value_offset(value))?;
    }
    Ok(())
}

impl<S: Storage, const N: usize> Afs<'_, S, N> {
    /// Dumps the contents of the file system
    pub async fn dump(&mut self, w: &mut impl Write) -> Result<(), Error<S::Error>> {
        for block in 0..self.config.num_blocks {
            self.dump_block(w, block, u32::MAX).await?;
        }
        Ok(())
    }

    /// Dumps the contents of a single block of the file system (a `max_chunks` of 0 means no limit)
    pub async fn dump_block(&mut self, w: &mut impl Write, block: u16, max_chunks: u32) -> Result<(), Error<S::Error>> {
        if block >= self.config.num_blocks {
            return Ok(());
        }
        let value = self.lookup.raw_value(block);
        if value == 0 {
            return Ok(());
        }
        writeln!(w, "[{:3}]={{object_id={}, object_block_index={}}}", block, value >> 16, value & 0xffff)?;

        let block_size = self.config.block_size;
        let mut offset = 0u32;
        let mut header = 0u32;
        let mut remaining = max_chunks;
        let mut data = [0u8; MAX_DUMP_DATA];
        loop {
            offset = match offset {
                0 => BLOCK_HEADER_LENGTH,
                _ => offset.wrapping_add(CHUNK_HEADER_LENGTH).wrapping_add(tag_length(header)),
            };
            if offset as u64 + CHUNK_HEADER_LENGTH as u64 + 1 >= block_size as u64 {
                break;
            }
            let mut pos = Position::new(block, offset);
            header = cache::read_u32(&mut self.storage, &self.config, &mut self.cache, &mut pos).await?;
            let chunk_type = tag_type(header);
            let length = tag_length(header);
            let data_length = (length as usize).min(MAX_DUMP_DATA);
            match chunk_type {
                t if is_data_chunk(t) || t == CHUNK_TYPE_OFFSET || t == CHUNK_TYPE_SEEK => {
                    let out = &mut data[..data_length];
                    cache::read_data(&mut self.storage, &self.config, &mut self.cache, &mut pos, out).await?;
                }
                CHUNK_TYPE_END => {}
                CHUNK_TYPE_INVALID_ZERO | CHUNK_TYPE_INVALID_ONE => break,
                t => {
                    writeln!(w, "Unexpected chunk type (0x{t:x})")?;
                    break;
                }
            }
            match chunk_type {
                CHUNK_TYPE_END => writeln!(w, "  [0x{offset:06x}]=End()")?,
                CHUNK_TYPE_OFFSET => {
                    write!(w, "  [0x{offset:06x}]=Offset(num={}, data=", (length / 8) as u8)?;
                    write_offset_entries(w, length, &data[..data_length / 8 * 8])?;
                    writeln!(w, ")")?;
                }
                CHUNK_TYPE_SEEK => {
                    write!(w, "  [0x{offset:06x}]=Seek(num={}, data=", (length / 4) as u8)?;
                    write_seek_entries(w, length, &data[..data_length / 4 * 4])?;
                    writeln!(w, ")")?;
                }
                t => {
                    write!(w, "  [0x{offset:06x}]=Data(stream=0x{:x}, length={length}, data=", t & 0xf)?;
                    write_data_hex(w, &data[..data_length])?;
                    writeln!(w, ")")?;
                }
            }
            remaining = remaining.wrapping_sub(1);
            if remaining == 0 {
                break;
            }
        }
        Ok(())
    }

    /// Dumps the blocks used by an object
    pub fn dump_object(&self, w: &mut impl Write, object_id: ObjectId) -> Result<(), Error<S::Error>> {
        for (block, object_block_index) in self.lookup.object_blocks(object_id.get()) {
            writeln!(w, "[{block:3}]={{object_block_index={object_block_index}}}")?;
        }
        Ok(())
    }
}
