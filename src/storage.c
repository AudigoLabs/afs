#include "storage.h"

#include "afs_config.h"
#include "cache.h"
#include "util.h"

#include <string.h>

static void populate_cache(storage_t* storage, const position_t* position) {
    cache_t* cache = &storage->cache;
    cache->position.block = position->block;
    cache->position.offset = ALIGN_DOWN(position->offset, cache->size);
    storage->config->read(cache->buffer, cache->position.block, cache->position.offset, cache->size);
    cache->length = cache->size;
}

static bool read_seek_chunk(storage_t* storage, position_t* position, seek_chunk_data_t* data) {
    // Read the seek chunk header
    chunk_header_t seek_chunk_header;
    storage_read_chunk_header(storage, position, &seek_chunk_header);

    // Validate the seek chunk data length
    const uint32_t seek_chunk_data_length = CHUNK_TAG_GET_LENGTH(seek_chunk_header.tag);
    const uint32_t seek_chunk_num_entries = seek_chunk_data_length / sizeof(uint32_t);
    if (CHUNK_TAG_GET_TYPE(seek_chunk_header.tag) != CHUNK_TYPE_SEEK) {
        AFS_LOG_ERROR("Invalid seek chunk (0x%"PRIx32")", seek_chunk_header.tag);
        return false;
    } else if (seek_chunk_data_length > storage->config->block_size - position->offset) {
        // Seek chunk can't be bigger than the remaining space in the block
        AFS_LOG_ERROR("Invalid seek chunk (0x%"PRIx32")", seek_chunk_header.tag);
        return false;
    } else if (seek_chunk_data_length % sizeof(uint32_t)) {
        // Length should be a multiple of the size of the entries (4 bytes)
        AFS_LOG_ERROR("Invalid seek chunk (0x%"PRIx32")", seek_chunk_header.tag);
        return false;
    } else if (seek_chunk_num_entries > AFS_NUM_STREAMS) {
        // Invalid number of entries
        AFS_LOG_ERROR("Invalid seek chunk (0x%"PRIx32")", seek_chunk_header.tag);
        return false;
    }

    // Read the data one value at a time into the result offsets. The reading is cached (doesn't actually hit the disk)
    // in practice so this isn't as inefficient as it might seem and makes the logic a bit simpler.
    for (uint32_t i = 0; i < seek_chunk_num_entries; i++) {
        uint32_t value;
        storage_read_data(storage, position, &value, sizeof(value));
        // This is an offset value
        const uint8_t stream = SEEK_OFFSET_DATA_GET_STREAM(value);
        if (stream >= AFS_NUM_STREAMS) {
            AFS_LOG_ERROR("Invalid stream (%u)", stream);
            return false;
        } else if (data->offsets[stream]) {
            AFS_LOG_ERROR("Duplicate stream (%u)", stream);
            return false;
        }
        data->offsets[stream] = SEEK_OFFSET_DATA_GET_OFFSET(value);
    }

    return true;
}

void storage_read_data(storage_t* storage, position_t* position, void* buf, uint32_t length) {
    AFS_ASSERT_NOT_EQ(position->block, INVALID_BLOCK);
    AFS_ASSERT(position->offset + length <= storage->config->block_size);
    bool is_first = true;
    while (length > 0) {
        if (cache_contains(&storage->cache, position)) {
            // We shouldn't get here after the first loop since we should have read all the way to the end of the cache
            // in the prior loop
            AFS_ASSERT(is_first);
        } else {
            // Populate the cache for the requested position
            populate_cache(storage, position);
        }

        // Read what we can from the cache
        const uint32_t read_length = cache_read(&storage->cache, position, buf, length);
        AFS_ASSERT(read_length > 0 && read_length <= length);

        // Advance our pointers
        position->offset += read_length;
        buf += read_length;
        length -= read_length;
        is_first = false;
    }
}

bool storage_read_block_header_offset_data(storage_t* storage, uint16_t block, offset_chunk_data_t* data) {
    // Create a read pointer
    position_t position = {
        .block = block,
        .offset = 0,
    };

    // Read the block header for validation
    block_header_t block_header;
    storage_read_data(storage, &position, &block_header, sizeof(block_header));
    AFS_ASSERT(util_is_block_header_valid(&block_header, NULL));

    // Read the offset chunk header
    chunk_header_t offset_chunk_header;
    storage_read_chunk_header(storage, &position, &offset_chunk_header);

    // Validate the offset chunk header
    if (CHUNK_TAG_GET_TYPE(offset_chunk_header.tag) != CHUNK_TYPE_OFFSET) {
        // There must not be any data in this block since the offset chunk wasn't written
        AFS_LOG_WARN("Invalid offset chunk (0x%"PRIx32")", offset_chunk_header.tag);
        return false;
    }
    const uint8_t num_streams = CHUNK_TAG_GET_LENGTH(offset_chunk_header.tag) / sizeof(uint64_t);
    if (num_streams > AFS_NUM_STREAMS) {
        AFS_LOG_ERROR("Invalid number of streams (%u)", num_streams);
        return false;
    }

    // Read the data one value at a time into the result offsets. The reading is cached (doesn't actually hit the disk
    // in practice) so this isn't as inefficient as it might seem and saves us some stack space.
    for (uint8_t i = 0; i < num_streams; i++) {
        uint64_t value;
        storage_read_data(storage, &position, &value, sizeof(value));
        const uint8_t stream = OFFSET_DATA_GET_STREAM(value);
        if (stream >= AFS_NUM_STREAMS) {
            AFS_LOG_ERROR("Invalid stream (%u)", stream);
            return false;
        }
        data->offsets[stream] = OFFSET_DATA_GET_OFFSET(value);
    }
    return true;
}

bool storage_read_block_footer_seek_data(storage_t* storage, uint16_t block, seek_chunk_data_t* data) {
    // Create a read pointer
    position_t position = {
        .block = block,
        .offset = storage->config->block_size - BLOCK_FOOTER_LENGTH,
    };

    // Read the footer for validation
    block_footer_t footer;
    storage_read_data(storage, &position, &footer, sizeof(footer));
    if (footer.magic.val != FOOTER_MAGIC_VALUE.val) {
        return false;
    }

    // Read the seek chunk
    return read_seek_chunk(storage, &position, data);
}

bool storage_read_seek_data(storage_t* storage, uint16_t block, uint32_t sub_block_index, seek_chunk_data_t* data) {
    if (sub_block_index == 0) {
        // The first sub-block has all offsets of 0
        memset(data, 0, sizeof(*data));
        return true;
    } else if (sub_block_index == storage->config->sub_blocks_per_block - 1) {
        // The last sub-block has the offsets within the footer
        return storage_read_block_footer_seek_data(storage, block, data);
    }
    position_t position = {
        .block = block,
        .offset = sub_block_index * (storage->config->block_size / storage->config->sub_blocks_per_block),
    };
    return read_seek_chunk(storage, &position, data);
}

void storage_write_cache(storage_t* storage, bool pad) {
    cache_t* cache = &storage->cache;

    // Pad what we're writing up to the minimum write size
    const uint32_t aligned_length = ALIGN_UP(cache->length, storage->config->min_read_write_size);
    if (aligned_length > cache->length) {
        AFS_ASSERT(pad);
        AFS_ASSERT(aligned_length <= cache->size);
        memset(&cache->buffer[cache->length], 0, aligned_length - cache->length);
    }
    AFS_ASSERT(cache->position.offset + aligned_length <= storage->config->block_size);

    // Write the data
    storage->config->write(cache->buffer, cache->position.block, cache->position.offset, aligned_length);

    // Invalidate the file system cache
    const position_t position = {
        .block = cache->position.block,
        .offset = cache->position.block,
    };
    cache_invalidate(cache, &position, aligned_length);

    // Advance the cache forward
    cache->position.offset += aligned_length;
    cache->length = 0;
    AFS_ASSERT(cache->position.offset <= storage->config->block_size);
    if (cache->position.offset == storage->config->block_size) {
        // No more space in the current block or we added padding, so advance to the next once
        cache->position = (position_t) {
            .block = INVALID_BLOCK,
            .offset = 0,
        };
    }
}

void storage_erase(storage_t* storage, uint16_t block) {
    storage->config->erase(block);
    const position_t position = {
        .block = block,
        .offset = 0,
    };
    cache_invalidate(&storage->cache, &position, storage->config->block_size);
}
