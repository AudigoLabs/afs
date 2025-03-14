#include "object_read.h"

#include "afs_config.h"
#include "lookup_table.h"
#include "storage.h"
#include "util.h"

#include <string.h>

static void process_block_header(position_t* position, afs_obj_impl_t* obj) {
    // Read the block header
    block_header_t header;
    storage_read_block_header(&obj->storage, position, &header);

    // Validate the header as a sanity check
    AFS_ASSERT(util_is_block_header_valid(&header, NULL));
    AFS_ASSERT_EQ(header.object_id, obj->object_id);
    AFS_ASSERT_EQ(header.object_block_index, obj->read.storage_offset / obj->storage.config->block_size);

    // Advance past the header
    obj->read.storage_offset += sizeof(header);
    AFS_LOG_DEBUG("Read block header");
}

static uint32_t process_read_data(afs_obj_impl_t* obj, uint32_t max_length) {
    const uint32_t chunk_read_length = MIN_VAL(obj->read.data_chunk_length, max_length);
    obj->read.data_chunk_length -= chunk_read_length;
    obj->read.storage_offset += chunk_read_length;
    const uint8_t stream = obj->read.stream == AFS_WILDCARD_STREAM ? obj->read.current_stream : obj->read.stream;
    obj->object_offset[stream] += chunk_read_length;
    obj->block_offset[stream] += chunk_read_length;
    AFS_LOG_DEBUG("Read %"PRIu32" bytes of data", chunk_read_length);
    return chunk_read_length;
}

static bool process_new_chunk(afs_obj_impl_t* obj, position_t* position, uint32_t block_end, bool* has_more_data) {
    chunk_header_t header;
    storage_read_chunk_header(&obj->storage, position, &header);
    const uint8_t chunk_type = CHUNK_TAG_GET_TYPE(header.tag);
    const uint32_t chunk_length = CHUNK_TAG_GET_LENGTH(header.tag);
    AFS_LOG_DEBUG("Read chunk header (type=0x%x, length=%"PRIu32")", chunk_type, chunk_length);
    // Check the chunk length
    bool length_invalid;
    switch (chunk_type) {
        case CHUNK_TYPE_DATA_FIRST ... CHUNK_TYPE_DATA_LAST:
            length_invalid = (position->offset + chunk_length) > block_end;
            break;
        case CHUNK_TYPE_OFFSET:
            length_invalid = chunk_length > sizeof(offset_chunk_data_t);
            break;
        case CHUNK_TYPE_SEEK:
            length_invalid = chunk_length > sizeof(seek_chunk_data_t);
            break;
        case CHUNK_TYPE_END:
            length_invalid = chunk_length > 0;
            break;
        default:
            length_invalid = false;
            break;
    }
    if (length_invalid) {
        AFS_LOG_ERROR("Invalid length (type=0x%x, length=%"PRIu32")", chunk_type, chunk_length);
        // Assume the storage got corrupted, so just bail
        *has_more_data = false;
        return false;
    }
    // Process the chunk
    switch (chunk_type) {
        case CHUNK_TYPE_DATA_FIRST ... CHUNK_TYPE_DATA_LAST:
            obj->read.storage_offset += sizeof(header);
            if (obj->read.stream == AFS_WILDCARD_STREAM || (chunk_type & 0xf) == obj->read.stream) {
                obj->read.data_chunk_length = chunk_length;
                obj->read.current_stream = chunk_type & 0xf;
            } else {
                // Skip over this chunk since it's a different stream
                obj->read.storage_offset += chunk_length;
            }
            *has_more_data = true;
            return true;
        case CHUNK_TYPE_OFFSET:
        case CHUNK_TYPE_SEEK:
            // Skip over this chunk
            obj->read.storage_offset += sizeof(header) + chunk_length;
            *has_more_data = true;
            return true;
        case CHUNK_TYPE_END:
            // Reached the end of the file - keep the object impl in this state in case we try to read again
            if (chunk_length > 0) {
                AFS_LOG_WARN("Invalid end chunk length (%"PRIu32")", chunk_length);
            }
            *has_more_data = false;
            return false;
        case CHUNK_TYPE_INVALID_ZERO:
        case CHUNK_TYPE_INVALID_ONE:
            // No more chunks in this block, so move to the next block
            obj->read.storage_offset = ALIGN_UP(obj->read.storage_offset, obj->storage.config->block_size);
            *has_more_data = true;
            return false;
        default:
            AFS_LOG_ERROR("Unexpected chunk type (0x%x)", chunk_type);
            // Assume the storage got corrupted, so just bail
            *has_more_data = false;
            return false;
    }
}

static void align_storage_offset(const afs_impl_t* afs, afs_obj_impl_t* obj, const position_t* position) {
    const uint32_t block_size = obj->storage.config->block_size;
    const uint32_t block_offset = obj->read.storage_offset % block_size;
    AFS_ASSERT(block_offset <= block_size);
    if (lookup_table_get_is_v2(&afs->lookup_table, position->block)) {
        if (block_size - BLOCK_FOOTER_LENGTH - block_offset < sizeof(chunk_header_t) + 1) {
            // No more chunks or data in this block, so move to the next block
            AFS_LOG_DEBUG("No more chunks in current block");
            obj->read.storage_offset = ALIGN_UP(obj->read.storage_offset, block_size);
            memset(obj->block_offset, 0, sizeof(obj->block_offset));
        } else {
            const uint32_t sub_block_size = block_size / obj->storage.config->sub_blocks_per_block;
            const uint32_t sub_block_offset = block_offset % sub_block_size;
            if (sub_block_size - sub_block_offset < sizeof(chunk_header_t) + 1) {
                // No more chunks or data in this sub-block, so align up to the next sub-block
                AFS_LOG_DEBUG("No more chunks in current sub-block");
                obj->read.storage_offset = ALIGN_UP(obj->read.storage_offset, sub_block_size);
                memset(obj->block_offset, 0, sizeof(obj->block_offset));
            }
        }
    } else {
        if (block_size - block_offset < sizeof(chunk_header_t) + 1) {
            // No more chunks or data in this block, so move to the next block
            AFS_LOG_DEBUG("No more chunks in current block");
            obj->read.storage_offset = ALIGN_UP(obj->read.storage_offset, block_size);
            memset(obj->block_offset, 0, sizeof(obj->block_offset));
        }
    }
}

bool object_read_process(const afs_impl_t* afs, afs_obj_impl_t* obj, uint8_t* data, uint32_t max_length, uint32_t* read_bytes) {
    *read_bytes = 0;
    const uint32_t block_size = obj->storage.config->block_size;
    const uint16_t block_index = obj->read.storage_offset / block_size;
    position_t position = {
        .block = lookup_table_get_block(&afs->lookup_table, obj->object_id, block_index),
        .offset = obj->read.storage_offset % block_size,
    };
    AFS_LOG_DEBUG("Reading/seeking (index=%u, block=%u, offset=0x%"PRIx32")", block_index, position.block, position.offset);

    if (position.block == INVALID_BLOCK && position.offset == 0) {
        // Writing got interrupted in the middle of the previous block, so just bail
        return false;
    }
    AFS_ASSERT_NOT_EQ(position.block, INVALID_BLOCK);
    const bool is_v2 = lookup_table_get_is_v2(&afs->lookup_table, position.block);
    const uint32_t block_end = block_size - (is_v2 ? BLOCK_FOOTER_LENGTH : 0);
    AFS_ASSERT(position.offset < block_end);

    if (position.offset == 0) {
        // Process the block header
        process_block_header(&position, obj);
        return true;
    } else if (obj->read.data_chunk_length > 0) {
        // We are within a data chunk, so read as much data as possible from it
        *read_bytes = process_read_data(obj, max_length);
        if (data && *read_bytes) {
            storage_read_data(&obj->storage, &position, data, *read_bytes);
        }
    } else {
        // We need to read a new chunk
        bool has_more_data;
        if (!process_new_chunk(obj, &position, block_end, &has_more_data)) {
            return has_more_data;
        }
    }

    if (obj->read.data_chunk_length > 0) {
        // More data to read in the current data chunk
        return true;
    }

    align_storage_offset(afs, obj, &position);
    return true;
}
