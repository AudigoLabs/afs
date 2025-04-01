#include "object_write.h"

#include "afs_config.h"
#include "cache.h"
#include "lookup_table.h"
#include "storage.h"
#include "util.h"

#include <string.h>

//! Gets the current cache write position
static inline uint32_t cache_write_position(const cache_t* cache) {
    return cache->position.offset + cache->length;
}

//! Calculates the remaining space within the block
static inline uint32_t remaining_block_space(afs_obj_impl_t* obj) {
    return obj->storage.config->block_size - BLOCK_FOOTER_LENGTH - cache_write_position(&obj->storage.cache);
}

//! Calculates the remaining spae within the sub-block
static inline uint32_t remaining_sub_block_space(afs_obj_impl_t* obj) {
    const uint32_t write_pos = cache_write_position(&obj->storage.cache);
    const uint32_t sub_block_size = obj->storage.config->block_size / obj->storage.config->sub_blocks_per_block;
    return ALIGN_UP(write_pos, sub_block_size) - write_pos;
}

//! Flushes the current write buffer
static bool flush_write_buffer(afs_impl_t* afs, afs_obj_impl_t* obj, bool pad) {
    cache_t* cache = &obj->storage.cache;
    if (cache->position.offset == 0) {
        // We are writing at the start of the block, so we need to find a block to write to
        AFS_ASSERT_EQ(cache->position.block, INVALID_BLOCK);
        AFS_ASSERT(obj->write.next_block_index > 0);
        const uint16_t block_index = obj->write.next_block_index - 1;
        bool is_erased;
        cache->position.block = lookup_table_acquire_block(&afs->lookup_table, obj->object_id, block_index, &is_erased);
        if (cache->position.block == INVALID_BLOCK) {
            AFS_LOG_ERROR("Could not find free block");
            return false;
        }
        if (!is_erased) {
            storage_erase(&afs->storage, cache->position.block);
        }
    } else {
        AFS_ASSERT_NOT_EQ(cache->position.block, INVALID_BLOCK);
    }
    AFS_LOG_DEBUG("Flushing cache (block=%u, offset=0x%"PRIx32", length=%"PRIu32")", cache->position.block,
        cache->position.offset, cache->length);
    storage_write_cache(&obj->storage, pad);
    return true;
}

//! Writes a seek chunk into the cache
static void cache_write_seek_chunk(afs_obj_impl_t* obj) {
    cache_t* cache = &obj->storage.cache;

    // Get the size of the seek chunk
    uint8_t num_offsets = 0;
    for (uint8_t i = 0; i < AFS_NUM_STREAMS; i++) {
        if (obj->block_offset[i]) {
            num_offsets++;
        }
    }

    // Calculate the length of the data that'll be written to the disk.
    const uint32_t data_length = num_offsets * sizeof(uint32_t);

    // Write the seek chunk header
    AFS_LOG_DEBUG("Writing seek chunk header into the cache (offset=0x%"PRIx32")", cache->position.offset);
    const chunk_header_t seek_chunk_header = {
        .tag = CHUNK_TAG_VALUE(CHUNK_TYPE_SEEK, data_length),
    };
    cache_write(cache, &seek_chunk_header, sizeof(seek_chunk_header));

    // Write the seek chunk offsets
    for (uint8_t i = 0; i < AFS_NUM_STREAMS; i++) {
        const uint32_t offset = obj->block_offset[i];
        if (!offset) {
            continue;
        }
        AFS_LOG_DEBUG("Writing seek chuck offset into the cache (offset=0x%"PRIx32")", cache->position.offset);
        AFS_ASSERT_EQ(SEEK_OFFSET_DATA_GET_STREAM(offset), 0);
        const uint32_t value = SEEK_OFFSET_DATA_VALUE(i, offset);
        cache_write(cache, &value, sizeof(value));
    }
}

//! Helper function to write the footer at the end of the current block
static bool write_footer(afs_impl_t* afs, afs_obj_impl_t* obj) {
    cache_t* cache = &obj->storage.cache;
    AFS_LOG_DEBUG("Writing footer (cache.offset=0x%"PRIx32", cache.length=%"PRIu32")", cache->position.offset,
        cache->length);
    const uint32_t footer_offset = afs->storage_config.block_size - BLOCK_FOOTER_LENGTH;
    AFS_ASSERT(cache->position.offset + cache->length <= footer_offset);
    if (cache->position.offset + cache->size < afs->storage_config.block_size) {
        // The current cache doesn't go to the end of the block, so flush it to disk
        AFS_ASSERT(cache->position.offset + cache->size <= footer_offset);
        if (!flush_write_buffer(afs, obj, true)) {
            AFS_LOG_ERROR("Error flushing write buffer");
            return false;
        }
        // Advance to the end of the block
        AFS_ASSERT_NOT_EQ(cache->position.block, INVALID_BLOCK);
        AFS_ASSERT_EQ(cache->length, 0);
        cache->position.offset = ALIGN_DOWN(footer_offset, afs->storage_config.min_read_write_size);
    }

    // Pad the cache with 0's to advance it to the offset of the footer (if necessary)
    const uint32_t cache_buffer_offset = footer_offset - cache->position.offset;
    if (cache->length < cache_buffer_offset) {
        AFS_LOG_DEBUG("Padding cache (cache_buffer_offset=0x%"PRIx32", cache.length=0x%"PRIx32")", cache_buffer_offset,
            cache->length);
        cache_write(&obj->storage.cache, NULL, cache_buffer_offset - cache->length);
    } else {
        AFS_ASSERT_EQ(cache->length, cache_buffer_offset);
    }

    // Write the footer into the cache
    AFS_LOG_DEBUG("Writing block footer into the cache (offset=0x%"PRIx32")", cache->position.offset);
    const block_footer_t footer = {
        .magic.val = FOOTER_MAGIC_VALUE.val,
    };
    cache_write(cache, &footer, sizeof(footer));

    // Write the seek chunk
    cache_write_seek_chunk(obj);

    // Flush the buffer
    if (!flush_write_buffer(afs, obj, true)) {
        AFS_LOG_ERROR("Error flushing write buffer");
        return false;
    }
    return true;
}

//! Helper function to write data for an object
static bool write_data(afs_impl_t* afs, afs_obj_impl_t* obj, const uint8_t* data, uint32_t length) {
    cache_t* cache = &obj->storage.cache;
    AFS_LOG_DEBUG("Writing data (length=%"PRIu32", cache.offset=0x%"PRIx32", cache.length=%"PRIu32")", length,
        cache->position.offset, cache->length);
    while (length) {
        // Write as much as we can into the buffer
        AFS_LOG_DEBUG("Writing data into the cache (offset=0x%"PRIx32")", cache->position.offset);
        const uint32_t buffer_space = cache->size - cache->length;
        const uint32_t write_size = MIN_VAL(length, buffer_space);
        cache_write(&obj->storage.cache, data, write_size);
        data += write_size;
        length -= write_size;
        if (cache->length == cache->size) {
            // The buffer is full so flush it to disk
            if (!flush_write_buffer(afs, obj, false)) {
                AFS_LOG_ERROR("Error flushing write buffer");
                return false;
            }
        }
    }
    return true;
}

static bool write_block_header(afs_impl_t* afs, afs_obj_impl_t* obj) {
    AFS_ASSERT_NOT_EQ(obj->object_id, INVALID_OBJECT_ID);
    cache_t* cache = &obj->storage.cache;

    AFS_LOG_DEBUG("Writing block header (object_id=%u, object_block_index=%u)", obj->object_id, obj->write.next_block_index);
    const block_header_t block_header = {
        .magic.val = HEADER_MAGIC_VALUE_V2.val,
        .object_id = obj->object_id,
        .object_block_index = obj->write.next_block_index++,
    };
    if (!write_data(afs, obj, (const uint8_t*)&block_header, sizeof(block_header))) {
        AFS_LOG_ERROR("Error writing block header");
        return false;
    }

    if (block_header.object_block_index == 0) {
        // This is the first block, so don't need an offset chunk
        return true;
    }

    // Get the size of the offset chunk
    uint8_t num_offsets = 0;
    for (uint8_t i = 0; i < AFS_NUM_STREAMS; i++) {
        const uint64_t offset = obj->object_offset[i];
        if (offset) {
            AFS_ASSERT_EQ(OFFSET_DATA_GET_STREAM(offset), 0);
            num_offsets++;
        }
    }
    const uint32_t offset_data_length = num_offsets * sizeof(uint64_t);

    // Write the offset chunk header
    AFS_LOG_DEBUG("Writing offset chunk header into the cache (offset=0x%"PRIx32", num=%u)", cache->position.offset, num_offsets);
    const chunk_header_t offset_chunk_header = {
        .tag = CHUNK_TAG_VALUE(CHUNK_TYPE_OFFSET, offset_data_length),
    };
    cache_write(cache, &offset_chunk_header, sizeof(offset_chunk_header));

    // Write the offset chunk values
    for (uint8_t i = 0; i < AFS_NUM_STREAMS; i++) {
        const uint64_t offset = obj->object_offset[i];
        if (offset) {
            const uint64_t value = OFFSET_DATA_VALUE(i, offset);
            cache_write(cache, &value, sizeof(value));
        }
    }

    return true;
}

//! Helper function to prepare for writing at least `length` bytes of data
static uint32_t prepare_for_write(afs_impl_t* afs, afs_obj_impl_t* obj, uint32_t length) {
    cache_t* cache = &obj->storage.cache;
    AFS_LOG_DEBUG("Preparing for write (length=%"PRIu32", position=0x%"PRIx32")", length, cache_write_position(cache));

    // Check if we're at the end of the block
    const uint32_t block_space = remaining_block_space(obj);
    if (block_space < length) {
        AFS_LOG_DEBUG("Not enough space left in block (%"PRIu32")", block_space);
        // Not enough room left in this block, so write out the footer and advance to the next block
        if (!write_footer(afs, obj)) {
            AFS_LOG_ERROR("Error writing block footer");
            return 0;
        }
        // Clear our block offsets
        memset(obj->block_offset, 0, sizeof(obj->block_offset));
        // Reset the cache for the start of next block
        cache->length = 0;
        cache->position = (position_t) {
            .block = INVALID_BLOCK,
            .offset = 0,
        };
    }

    // Check if we're at the start of a block
    if (cache_write_position(cache) == 0) {
        // This is the first write in a block, so write the header
        if (!write_block_header(afs, obj)) {
            AFS_LOG_ERROR("Error writing block header");
            return 0;
        }
    }

    // Check if we're at the end of the sub-block
    const uint32_t sub_block_space = remaining_sub_block_space(obj);
    if (sub_block_space < length) {
        AFS_LOG_DEBUG("Not enough space left in sub-block (%"PRIu32")", sub_block_space);
        // Not enough room left in this sub-block, so advance to the next sub-block and write out a seek chunk
        // Pad the rest of the sub-block
        cache_write(cache, NULL, sub_block_space);
        // Check if we're at the end of the cache and need to flush it
        if (cache->length == cache->size) {
            // No space left in the cache, so need to flush it
            if (!flush_write_buffer(afs, obj, false)) {
                AFS_LOG_ERROR("Error flushing write buffer");
                return 0;
            }
        }
        // Write the seek chunk
        cache_write_seek_chunk(obj);
    }

    const uint32_t write_space = MIN_VAL(remaining_block_space(obj), remaining_sub_block_space(obj));
    AFS_ASSERT(write_space > 0);
    return write_space;
}

uint32_t object_write_process(afs_impl_t* afs, afs_obj_impl_t* obj, uint8_t stream, const void* data, uint32_t length) {
    // Make sure we can write the chunk header and at least 1 byte of data in the current block
    const uint32_t write_space = prepare_for_write(afs, obj, sizeof(chunk_header_t) + 1);
    if (!write_space) {
        AFS_LOG_ERROR("Error preparing for writing");
        return 0;
    }

    // Write the chunk header
    const uint32_t chunk_length = MIN_VAL(MIN_VAL(length, write_space - sizeof(chunk_header_t)), CHUNK_MAX_LENGTH);
    AFS_LOG_DEBUG("Writing data chunk (length=%"PRIu32")", chunk_length);
    chunk_header_t chunk_header = {
        .tag = CHUNK_TAG_VALUE(CHUNK_TYPE_DATA_FIRST | stream, chunk_length),
    };
    if (!write_data(afs, obj, (const uint8_t*)&chunk_header, sizeof(chunk_header))) {
        AFS_LOG_ERROR("Error writing chunk header");
        return 0;
    }

    // Write the chunk data
    if (!write_data(afs, obj, data, chunk_length)) {
        AFS_LOG_ERROR("Error writing chunk data");
        return 0;
    }
    obj->object_offset[stream] += chunk_length;
    obj->block_offset[stream] += chunk_length;
    return chunk_length;
}

bool object_write_finish(afs_impl_t* afs, afs_obj_impl_t* obj) {
    // Make sure we can write the end chunk header in the current block
    if (!prepare_for_write(afs, obj, sizeof(chunk_header_t))) {
        AFS_LOG_ERROR("Error preparing for writing");
        return false;
    }

    // Write the end chunk header
    chunk_header_t chunk_header = {
        .tag = CHUNK_TAG_VALUE(CHUNK_TYPE_END, 0),
    };
    if (!write_data(afs, obj, (const uint8_t*)&chunk_header, sizeof(chunk_header))) {
        AFS_LOG_ERROR("Error writing chunk header");
        return false;
    }

    // Write the block footer
    if (!write_footer(afs, obj)) {
        return false;
    }

    return true;
}
