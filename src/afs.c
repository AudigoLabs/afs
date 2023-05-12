#include "afs/afs.h"

#include "afs_cache.h"
#include "afs_impl_types.h"
#include "afs_storage.h"
#include "afs_storage_types.h"
#include "afs_util.h"

#include "afs_config.h"

#include <string.h>

_Static_assert(sizeof(afs_impl_t) == sizeof(((afs_handle_def_t*)0)->priv), "Invalid private buffer size");
_Static_assert(sizeof(afs_obj_impl_t) == sizeof(((afs_object_handle_def_t*)0)->priv), "Invalid private buffer size");
_Static_assert(sizeof(afs_read_pos_impl_t) == sizeof(((afs_read_position_t*)0)->priv), "Invalid private buffer size");
_Static_assert(sizeof(afs_object_list_entry_impl_t) == sizeof(((afs_object_list_entry_t*)0)->priv), "Invalid private buffer size");
_Static_assert(sizeof(MAGIC_VALUE) == sizeof(((block_header_t*)0)->magic), "Invalid magic size");

//! Gets an unused, psuedo-random object ID
static uint16_t get_next_object_id(afs_impl_t* afs) {
    // In the worst case, this loop is O(num_blocks), but statistically, there is a num_blocks / 2^16 chance that we
    // find a valid object ID with each loop, so in practice it should be very fast.
    while (true) {
        // Very simple psuedo-random number generator which uniformly generates 16-bit values
        afs->object_id_seed = afs->object_id_seed * 1664525 + 1013904223;
        const uint16_t object_id = (uint16_t)afs->object_id_seed;
        if (object_id == INVALID_OBJECT_ID) {
            continue;
        }
        bool in_use = false;
        for (uint16_t j = 0; j < afs->num_blocks; j++) {
            if (object_id == LOOKUP_TABLE_GET_OBJECT_ID(afs->lookup_table[j])) {
                in_use = true;
                break;
            }
        }
        if (!in_use) {
            return object_id;
        }
    }
}

//! Sets a value in the lookup table from a block header
static void lookup_table_set(afs_impl_t* afs, uint16_t block, const block_header_t* header) {
    AFS_ASSERT(header);
    afs->lookup_table[block] = LOOKUP_TABLE_VALUE(header->object_id, header->object_block_index);
}

//! Marks a block free in the specified state in the lookup table
static void lookup_table_set_free(afs_impl_t* afs, uint16_t block, uint16_t state) {
    afs->lookup_table[block] = LOOKUP_TABLE_FREE_BLOCK_VALUE(state);
}

//! Gets the block for a given object_id and object_block_index
static uint32_t lookup_get_block(const afs_impl_t* afs, uint16_t object_id, uint16_t object_block_index) {
    const afs_lookup_table_entry_t expected_lookup_value = LOOKUP_TABLE_VALUE(object_id, object_block_index);
    for (uint16_t i = 0; i < afs->num_blocks; i++) {
        if (afs->lookup_table[i] == expected_lookup_value) {
            return i;
        }
    }
    return INVALID_BLOCK;
}

//! Gets the number of blocks for a given object_id
static uint16_t lookup_get_num_blocks(const afs_impl_t* afs, uint16_t object_id) {
    uint16_t num_blocks = 0;
    for (uint16_t i = 0; i < afs->num_blocks; i++) {
        const afs_lookup_table_entry_t value = afs->lookup_table[i];
        if (LOOKUP_TABLE_GET_OBJECT_ID(value) == object_id) {
            num_blocks = MAX_VAL(num_blocks, LOOKUP_TABLE_GET_OBJECT_BLOCK_INDEX(value) + 1);
        }
    }
    return num_blocks;
}

//! Adds an open object to the list
static void open_object_list_add(afs_impl_t* afs, afs_obj_impl_t* obj) {
    AFS_ASSERT_EQ(obj->next_open_object, NULL);
    // Add to the head of the list since that's easiest
    obj->next_open_object = afs->open_object_list_head;
    afs->open_object_list_head = obj;
}

//! Removes an open object from the list
static void open_object_list_remove(afs_impl_t* afs, afs_obj_impl_t* obj) {
    afs_obj_impl_t* prev = NULL;
    FOREACH_OPEN_OBJECT(afs, cur) {
        if (cur == obj) {
            if (prev) {
                prev->next_open_object = obj->next_open_object;
            } else {
                afs->open_object_list_head = obj->next_open_object;
            }
            obj->next_open_object = NULL;
            return;
        }
        prev = cur;
    }
    AFS_FAIL("Did not find object in list");
}

// Flushes the current write buffer
static bool flush_write_buffer(afs_impl_t* afs, afs_obj_impl_t* obj, bool pad) {
    if (obj->cache.offset == 0) {
        // We are writing at the start of the block, so we need to find a block to write to
        AFS_ASSERT_EQ(obj->cache.block, INVALID_BLOCK);
        // Look for a block which is ideally already erased
        // Find the first free / best block from our lookup table (the underlying storage handles wear leveling for us)
        uint16_t free_block_state = UINT16_MAX;
        for (uint16_t i = 0; i < afs->num_blocks; i++) {
            const afs_lookup_table_entry_t value = afs->lookup_table[i];
            if (LOOKUP_TABLE_GET_OBJECT_ID(value) == INVALID_OBJECT_ID) {
                const uint16_t state = LOOKUP_TABLE_GET_BLOCK_STATE(value);
                if (state < free_block_state) {
                    obj->cache.block = i;
                    free_block_state = state;
                    if (state == LOOKUP_TABLE_BLOCK_STATE_ERASED) {
                        break;
                    }
                }
            }
        }
        if (obj->cache.block == INVALID_BLOCK) {
            AFS_LOG_ERROR("Could not find free block");
            return false;
        }
        if (free_block_state != LOOKUP_TABLE_BLOCK_STATE_ERASED) {
            afs_storage_erase(afs, obj->cache.block);
        }
        lookup_table_set(afs, obj->cache.block, (const block_header_t*)obj->cache.buffer);
    } else {
        AFS_ASSERT_NOT_EQ(obj->cache.block, INVALID_BLOCK);
    }
    AFS_LOG_DEBUG("Flushing cache (block=%u, offset=0x%"PRIx32", length=%"PRIu32")", obj->cache.block,
        obj->cache.offset, obj->cache.length);
    afs_storage_write_cache(afs, &obj->cache, pad);
    return true;
}

//! Helper function to write data for an object
static bool write_data(afs_impl_t* afs, afs_obj_impl_t* obj, const uint8_t* data, uint32_t length) {
    AFS_LOG_DEBUG("Writing data (length=%"PRIu32", cache.offset=0x%"PRIx32", cache.length=%"PRIu32")", length,
        obj->cache.offset, obj->cache.length);
    while (length) {
        // Write as much as we can into the buffer
        const uint32_t buffer_space = obj->cache.size - obj->cache.length;
        const uint32_t write_size = MIN_VAL(length, buffer_space);
        memcpy(&obj->cache.buffer[obj->cache.length], data, write_size);
        obj->cache.length += write_size;
        AFS_LOG_DEBUG("Copied %"PRIu32" bytes into the cache (offset=0x%"PRIx32", length=%"PRIu32")", write_size,
            obj->cache.offset, obj->cache.length);
        data += write_size;
        length -= write_size;
        if (obj->cache.length == obj->cache.size) {
            // The buffer is full so flush it to disk
            if (!flush_write_buffer(afs, obj, false)) {
                AFS_LOG_ERROR("Error flushing write buffer");
                return false;
            }
        }
    }
    return true;
}

//! Helper function to prepare for writing `length` bytes of data
static bool prepare_for_write(afs_impl_t* afs, afs_obj_impl_t* obj, uint32_t length) {
    const uint32_t max_write_length = afs->block_size - obj->cache.offset - obj->cache.length;
    AFS_LOG_DEBUG("Preparing for write (length=%"PRIu32", max_write_length=%"PRIu32")", length, max_write_length);
    if (max_write_length <= length) {
        // Not enough room for the write, so flush the buffer (if necessary) and advance to the next block
        if (obj->cache.length > 0) {
            if (!flush_write_buffer(afs, obj, true)) {
                AFS_LOG_ERROR("Error flushing write buffer");
                return false;
            }
        }
        // Reset the cache for the start of next block
        obj->cache.length = 0;
        obj->cache.block = INVALID_BLOCK;
        obj->cache.offset = 0;
    }

    if (obj->cache.offset == 0 && obj->cache.length == 0) {
        // We are at the start of a block, so write the block header
        AFS_ASSERT_NOT_EQ(obj->object_id, INVALID_OBJECT_ID);
        block_header_t block_header = {
            .object_id = obj->object_id,
            .object_block_index = obj->write.next_block_index++,
        };
        memcpy(block_header.magic, MAGIC_VALUE, sizeof(MAGIC_VALUE));
        AFS_LOG_DEBUG("Writing block header (object_id=%u, object_block_index=%u)", block_header.object_id, block_header.object_block_index);
        if (!write_data(afs, obj, (const uint8_t*)&block_header, sizeof(block_header))) {
            AFS_LOG_ERROR("Error writing block header");
            return false;
        }

        if (block_header.object_block_index > 0) {
            // This is not the first block, so write the offset chunk
            AFS_LOG_DEBUG("Writing offset chunk");
            uint8_t offset_chunk_buffer[sizeof(chunk_header_t) + sizeof(uint64_t) * AFS_NUM_STREAMS];
            uint8_t num_streams = 0;
            for (uint8_t i = 0; i < AFS_NUM_STREAMS; i++) {
                uint64_t offset = obj->offset[i];
                if (offset) {
                    offset |= ((uint64_t)i) << 60;
                    memcpy(&offset_chunk_buffer[sizeof(chunk_header_t) + num_streams * sizeof(offset)], &offset, sizeof(offset));
                    num_streams++;
                }
            }
            const chunk_header_t chunk_header = {
                .tag = CHUNK_TAG_VALUE(CHUNK_TYPE_OFFSET, num_streams * sizeof(uint64_t)),
            };
            memcpy(offset_chunk_buffer, &chunk_header, sizeof(chunk_header));
            if (!write_data(afs, obj, offset_chunk_buffer, sizeof(chunk_header) + num_streams * sizeof(uint64_t))) {
                AFS_LOG_ERROR("Error writing chunk header");
                return false;
            }
        }
    }

    return true;
}

static bool read_seek_helper(afs_impl_t* afs, afs_obj_impl_t* obj, uint8_t* data, uint32_t max_length, uint32_t* read_bytes) {
    const uint16_t block_index = obj->read.storage_offset / afs->block_size;
    const uint32_t block_offset = obj->read.storage_offset % afs->block_size;
    const uint16_t block = lookup_get_block(afs, obj->object_id, block_index);
    AFS_LOG_DEBUG("Reading/seeking (block=%u, index=%u, offset=0x%"PRIx32")", block, block_index, block_offset);
    if (block == INVALID_BLOCK && block_offset == 0) {
        // Writing got interrupted in the middle of the previous block, so just bail
        return false;
    }
    AFS_ASSERT_NOT_EQ(block, INVALID_BLOCK);
    *read_bytes = 0;
    if (block_offset == 0) {
        // Read the block header
        block_header_t header;
        AFS_ASSERT_EQ(afs_storage_read(afs, &obj->cache, block, block_offset, (uint8_t*)&header, sizeof(header)), sizeof(header));
        // Validate the block header as a sanity check
        AFS_ASSERT(afs_util_is_block_header_valid(&header));
        AFS_ASSERT_EQ(header.object_id, obj->object_id);
        AFS_ASSERT_EQ(header.object_block_index, block_index);
        obj->read.storage_offset += sizeof(header);
        AFS_LOG_DEBUG("Read block header");
    } else if (block_index > 0 && block_offset == sizeof(block_header_t)) {
        // Read the offset chunk
        chunk_header_t header;
        AFS_ASSERT_EQ(afs_storage_read(afs, &obj->cache, block, block_offset, (uint8_t*)&header, sizeof(header)), sizeof(header));
        if (CHUNK_TAG_GET_TYPE(header.tag) != CHUNK_TYPE_OFFSET) {
            AFS_LOG_WARN("Invalid offset chunk, assuming end of object");
            return false;
        }
        obj->read.storage_offset += sizeof(header) + CHUNK_TAG_GET_LENGTH(header.tag);
        AFS_LOG_DEBUG("Read offset chunk");
    } else if (obj->read.data_chunk_length > 0) {
        // We are within a data chunk, so read as much data as possible from it
        uint32_t chunk_read_length = MIN_VAL(obj->read.data_chunk_length, max_length);
        if (data) {
            chunk_read_length = afs_storage_read(afs, &obj->cache, block, block_offset, data, chunk_read_length);
        }
        obj->read.data_chunk_length -= chunk_read_length;
        *read_bytes = chunk_read_length;
        obj->read.storage_offset += chunk_read_length;
        if (obj->read.stream == AFS_WILDCARD_STREAM) {
            obj->offset[obj->read.current_stream] += chunk_read_length;
        } else {
            obj->offset[obj->read.stream] += chunk_read_length;
        }
        AFS_LOG_DEBUG("Read %"PRIu32" bytes of data", chunk_read_length);
    } else {
        // We need to read a new chunk
        chunk_header_t header;
        uint32_t header_read_length = afs_storage_read(afs, &obj->cache, block, block_offset, (uint8_t*)&header, sizeof(header));
        AFS_ASSERT(header_read_length > 0);
        if (header_read_length != sizeof(header)) {
            // Couldn't read the full header from the current cache, so read the rest as a second call
            header_read_length += afs_storage_read(afs, &obj->cache, block, block_offset + header_read_length, ((uint8_t*)&header) + header_read_length, sizeof(header) - header_read_length);
            AFS_ASSERT_EQ(header_read_length, sizeof(header));
        }
        const uint8_t chunk_type = CHUNK_TAG_GET_TYPE(header.tag);
        const uint32_t chunk_length = CHUNK_TAG_GET_LENGTH(header.tag);
        AFS_LOG_DEBUG("Read chunk header (type=0x%x, length=%"PRIu32")", chunk_type, chunk_length);
        switch (chunk_type) {
            case CHUNK_TYPE_DATA_FIRST ... CHUNK_TYPE_DATA_LAST:
                if (obj->read.stream == AFS_WILDCARD_STREAM || (chunk_type & 0xf) == obj->read.stream) {
                    obj->read.data_chunk_length = chunk_length;
                    obj->read.current_stream = chunk_type & 0xf;
                } else {
                    // Skip over this chunk since it's a different stream
                    obj->read.storage_offset += chunk_length;
                }
                break;
            case CHUNK_TYPE_OFFSET:
                // Skip over this chunk
                obj->read.storage_offset += chunk_length;
                break;
            case CHUNK_TYPE_END:
                // Reached the end of the file - keep the object impl in this state in case we try to read again
                return false;
            case CHUNK_TYPE_INVALID_ZERO:
            case CHUNK_TYPE_INVALID_ONE:
                // No more chunks in this block, so move to the next block
                obj->read.storage_offset = ALIGN_UP(obj->read.storage_offset, afs->block_size);
                return true;
            default:
                AFS_LOG_ERROR("Unexpected chunk type (0x%x)", chunk_type);
                // Assume the next part of flash got corrupted, so just bail
                return false;
        }
        obj->read.storage_offset += sizeof(header);
    }

    const uint32_t new_block_offset = obj->read.storage_offset % afs->block_size;
    AFS_ASSERT(new_block_offset <= afs->block_size);
    if (afs->block_size - new_block_offset < sizeof(chunk_header_t) + 1 && obj->read.data_chunk_length == 0) {
        // No more chunks or data in this block, so move to the next block
        AFS_LOG_DEBUG("No more chunks in current block");
        obj->read.storage_offset = ALIGN_UP(obj->read.storage_offset, afs->block_size);
    }

    return true;
}

static bool get_offsets_from_block_index(afs_impl_t* afs, uint16_t object_id, uint16_t block_index, uint64_t* offsets) {
    const uint16_t block = lookup_get_block(afs, object_id, block_index);
    AFS_ASSERT_NOT_EQ(block, INVALID_BLOCK);
    uint8_t offset_chunk_buffer[sizeof(chunk_header_t) + sizeof(uint64_t) * AFS_NUM_STREAMS];
    AFS_ASSERT_EQ(afs_storage_read(afs, &afs->cache, block, sizeof(block_header_t), offset_chunk_buffer, sizeof(offset_chunk_buffer)), sizeof(offset_chunk_buffer));
    chunk_header_t header;
    memcpy(&header, offset_chunk_buffer, sizeof(header));
    if (CHUNK_TAG_GET_TYPE(header.tag) != CHUNK_TYPE_OFFSET) {
        // There must not be any data in this block since the offset chunk wasn't written
        AFS_LOG_WARN("Invalid offset chunk (0x%"PRIx32")", header.tag);
        return false;
    }
    const uint8_t num_streams = CHUNK_TAG_GET_LENGTH(header.tag) / sizeof(uint64_t);
    AFS_ASSERT(num_streams <= AFS_NUM_STREAMS);
    for (uint8_t i = 0; i < num_streams; i++) {
        uint64_t value;
        memcpy(&value, &offset_chunk_buffer[sizeof(header) + i * sizeof(value)], sizeof(value));
        const uint8_t stream = value >> 60;
        if (stream >= AFS_NUM_STREAMS) {
            AFS_LOG_ERROR("Invalid stream (%u)", stream);
            return false;
        }
        offsets[stream] = value & 0x00ffffffffffffff;
    }
    return true;
}

void afs_init(afs_handle_t afs_handle, const afs_init_t* init) {
    afs_impl_t* afs = get_afs_impl(afs_handle);
    AFS_ASSERT(afs && init);
    AFS_ASSERT(init->num_blocks && init->num_blocks < INVALID_BLOCK);
    AFS_ASSERT(init->read_write_buffer && init->lookup_table);
    AFS_ASSERT(init->read_write_size >= sizeof(block_header_t) + sizeof(chunk_header_t));
    AFS_ASSERT(init->block_size > 0 && (init->block_size % init->read_write_size) == 0);
    AFS_ASSERT(init->read_func && init->write_func && init->erase_func);

    // Initialize the impl object
    *afs = (afs_impl_t) {
        .in_use = true,
        .storage_func = {
            .read = init->read_func,
            .write = init->write_func,
            .erase = init->erase_func,
        },
        .block_size = init->block_size,
        .num_blocks = init->num_blocks,
        .cache = (afs_cache_t) {
            .buffer = init->read_write_buffer,
            .size = init->read_write_size,
        },
        .lookup_table = init->lookup_table,
    };

    // Populate our lookup table from the storage
    afs_block_iter_context_t block_iter;
    const block_header_t empty_header = {};
    AFS_UTIL_FOREACH_BLOCK(afs, block_iter, 0) {
        if (afs_util_is_block_header_valid(&block_iter.header)) {
            lookup_table_set(afs, block_iter.block, &block_iter.header);
            if (block_iter.header.object_block_index == 0 && init->mount_callbacks.object_found) {
                AFS_ASSERT_EQ(afs->cache.block, block_iter.block);
                uint8_t stream;
                const uint32_t data_length = afs_util_get_object_data_from_cache(&afs->cache, &stream);
                init->mount_callbacks.object_found(block_iter.header.object_id, stream, afs->cache.buffer, data_length);
            }
        } else if (!memcmp(&block_iter.header, &empty_header, sizeof(empty_header))) {
            lookup_table_set_free(afs, block_iter.block, LOOKUP_TABLE_BLOCK_STATE_MAYBE_ERASED);
        } else {
            lookup_table_set_free(afs, block_iter.block, LOOKUP_TABLE_BLOCK_STATE_UNKNOWN);
        }
        // Use the lookup value to generate some randomness in our seed
        afs->object_id_seed ^= afs->lookup_table[block_iter.block];
    }

    // Remove any entries from our lookup table for deleted objects
    for (uint16_t i = 0; i < afs->num_blocks; i++) {
        const afs_lookup_table_entry_t value = afs->lookup_table[i];
        const uint16_t object_block_index = LOOKUP_TABLE_GET_OBJECT_BLOCK_INDEX(value);
        const uint16_t object_id = LOOKUP_TABLE_GET_OBJECT_ID(value);
        if (object_block_index == 0 || object_id == INVALID_OBJECT_ID) {
            // Either a free block or the first block in an object, so no action
            continue;
        }
        const afs_lookup_table_entry_t search_lookup_value = LOOKUP_TABLE_VALUE(object_id, 0);
        bool object_is_valid = false;
        for (uint32_t j = 0; j < afs->num_blocks; j++) {
            if (afs->lookup_table[j] == search_lookup_value) {
                object_is_valid = true;
                break;
            }
        }
        if (!object_is_valid) {
            AFS_LOG_DEBUG("Removing deleted object from lookup table (object_id=%u, object_block_index=%u)", object_id, object_block_index);
            lookup_table_set_free(afs, i, LOOKUP_TABLE_BLOCK_STATE_GARBAGE);
        }
    }
}

void afs_deinit(afs_handle_t afs_handle) {
    afs_impl_t* afs = get_afs_impl(afs_handle);
    AFS_ASSERT(afs && afs->in_use && !afs->open_object_list_head);
    afs->in_use = false;
}

uint16_t afs_object_create(afs_handle_t afs_handle, afs_object_handle_t object_handle, const afs_object_config_t* config) {
    afs_impl_t* afs = get_afs_impl(afs_handle);
    afs_obj_impl_t* obj = get_obj_impl(object_handle);
    AFS_ASSERT(afs && afs->in_use && obj && config && config->buffer && config->buffer_size);
    AFS_ASSERT(config->buffer_size > 0 && (config->buffer_size % afs->cache.size) == 0);
    AFS_ASSERT(config->buffer_size >= sizeof(block_header_t) + sizeof(chunk_header_t));
    AFS_ASSERT_EQ(obj->state, STATE_INVALID);

    // Initialize the afs_obj_impl_t
    *obj = (afs_obj_impl_t) {
        .state = STATE_WRITING,
        .cache = (afs_cache_t) {
            .buffer = config->buffer,
            .size = config->buffer_size,
            .block = INVALID_BLOCK,
        },
        .object_id = get_next_object_id(afs),
    };

    open_object_list_add(afs, obj);
    return obj->object_id;
}

bool afs_object_write(afs_handle_t afs_handle, afs_object_handle_t object_handle, uint8_t stream, const uint8_t* data, uint32_t length) {
    afs_impl_t* afs = get_afs_impl(afs_handle);
    afs_obj_impl_t* obj = get_obj_impl(object_handle);
    AFS_ASSERT(afs && afs->in_use && obj && data && length);
    AFS_ASSERT_EQ(obj->state, STATE_WRITING);
    AFS_ASSERT(stream < AFS_NUM_STREAMS);

    while (length) {
        // Make sure we can write the chunk header and at least 1 byte of data in the current block
        if (!prepare_for_write(afs, obj, sizeof(chunk_header_t) + 1)) {
            AFS_LOG_ERROR("Error preparing for writing");
            return false;
        }

        // Write the chunk header
        const uint32_t block_bytes_remaining = afs->block_size - obj->cache.offset - obj->cache.length;
        const uint32_t chunk_length = MIN_VAL(MIN_VAL(length, block_bytes_remaining - sizeof(chunk_header_t)), CHUNK_MAX_LENGTH);
        AFS_LOG_DEBUG("Writing data chunk (length=%"PRIu32")", chunk_length);
        chunk_header_t chunk_header = {
            .tag = CHUNK_TAG_VALUE(CHUNK_TYPE_DATA_FIRST | stream, chunk_length),
        };
        if (!write_data(afs, obj, (const uint8_t*)&chunk_header, sizeof(chunk_header))) {
            AFS_LOG_ERROR("Error writing chunk header");
            return false;
        }

        // Write the chunk data
        if (!write_data(afs, obj, data, chunk_length)) {
            AFS_LOG_ERROR("Error writing chunk data");
            return false;
        }
        obj->offset[stream] += chunk_length;
        data += chunk_length;
        length -= chunk_length;
    }

    return true;
}

bool afs_object_open(afs_handle_t afs_handle, afs_object_handle_t object_handle, uint8_t stream, uint16_t object_id, const afs_object_config_t* config) {
    afs_impl_t* afs = get_afs_impl(afs_handle);
    afs_obj_impl_t* obj = get_obj_impl(object_handle);
    AFS_ASSERT(afs && afs->in_use && obj && config && config->buffer && config->buffer_size);
    AFS_ASSERT(config->buffer_size > 0 && (config->buffer_size % afs->cache.size) == 0);
    AFS_ASSERT(config->buffer_size >= sizeof(block_header_t) + sizeof(chunk_header_t));
    AFS_ASSERT_EQ(obj->state, STATE_INVALID);
    AFS_ASSERT(stream < AFS_NUM_STREAMS || stream == AFS_WILDCARD_STREAM);
    AFS_ASSERT(object_id != INVALID_OBJECT_ID);

    // Find the first block from our lookup table
    const uint16_t block = lookup_get_block(afs, object_id, 0);
    if (block == INVALID_BLOCK) {
        AFS_LOG_WARN("Did not find block (object_id=%u)", object_id);
        return false;
    }

    *obj = (afs_obj_impl_t) {
        .state = STATE_READING,
        .cache = (afs_cache_t) {
            .buffer = config->buffer,
            .size = config->buffer_size,
            .block = block,
        },
        .object_id = object_id,
        .read.stream = stream,
    };
    open_object_list_add(afs, obj);
    return true;
}

uint32_t afs_object_read(afs_handle_t afs_handle, afs_object_handle_t object_handle, uint8_t* data, uint32_t max_length, uint8_t* stream) {
    afs_impl_t* afs = get_afs_impl(afs_handle);
    afs_obj_impl_t* obj = get_obj_impl(object_handle);
    AFS_ASSERT(afs && afs->in_use && obj && data && max_length);
    AFS_ASSERT_EQ(obj->state, STATE_READING);
    if (obj->read.stream == AFS_WILDCARD_STREAM) {
        AFS_ASSERT(stream);
        while (true) {
            uint32_t read_bytes;
            if (!read_seek_helper(afs, obj, data, max_length, &read_bytes)) {
                return 0;
            }
            if (read_bytes) {
                *stream = obj->read.current_stream;
                return read_bytes;
            }
        }
    } else {
        AFS_ASSERT(!stream);
        uint32_t total_read_bytes = 0;
        while (max_length) {
            uint32_t read_bytes;
            if (!read_seek_helper(afs, obj, data, max_length, &read_bytes)) {
                break;
            }
            data += read_bytes;
            max_length -= read_bytes;
            total_read_bytes += read_bytes;
        }
        return total_read_bytes;
    }
}

bool afs_object_seek(afs_handle_t afs_handle, afs_object_handle_t object_handle, uint64_t offset) {
    afs_impl_t* afs = get_afs_impl(afs_handle);
    afs_obj_impl_t* obj = get_obj_impl(object_handle);
    AFS_ASSERT(afs && afs->in_use && obj);
    AFS_ASSERT_EQ(obj->state, STATE_READING);

    // If the offset is in block at a much higher index than our current one, it can take a very long time to read
    // through every chunk to get to it. So, as an optimization, we calculate the highest possible block the offset
    // could be in and then do a binary search between the current block and the estimated block.
    const uint16_t current_block_index = obj->read.storage_offset / afs->block_size;
    uint16_t upper_block_index = (obj->read.storage_offset + offset) / afs->block_size;
    upper_block_index = lookup_get_num_blocks(afs, obj->object_id) - 1;
    uint16_t lower_block_index = current_block_index;
    uint64_t prev_stream_offset = 0;
    if (obj->read.stream == AFS_WILDCARD_STREAM) {
        for (uint8_t i = 0; i < AFS_NUM_STREAMS; i++) {
            prev_stream_offset += obj->offset[i];
        }
    } else {
        prev_stream_offset = obj->offset[obj->read.stream];
    }
    const uint64_t target_stream_offset = prev_stream_offset + offset;
    uint64_t new_stream_offsets[AFS_NUM_STREAMS];
    while (upper_block_index > lower_block_index) {
        const uint16_t mid_block_index = (upper_block_index + lower_block_index + 1) / 2;
        uint64_t stream_offsets[AFS_NUM_STREAMS] = {};
        if (obj->read.stream != AFS_WILDCARD_STREAM) {
            stream_offsets[obj->read.stream] = UINT64_MAX;
        }
        if (!get_offsets_from_block_index(afs, obj->object_id, mid_block_index, stream_offsets)) {
            // There must not be any data in this block since the offset chunk wasn't written
            upper_block_index = mid_block_index - 1;
            continue;
        }
        uint64_t mid_stream_offset = 0;
        if (obj->read.stream == AFS_WILDCARD_STREAM) {
            for (uint8_t i = 0; i < AFS_NUM_STREAMS; i++) {
                mid_stream_offset += stream_offsets[i];
            }
        } else {
            mid_stream_offset = stream_offsets[obj->read.stream];
            AFS_ASSERT_NOT_EQ(mid_stream_offset, UINT64_MAX);
        }
        if (mid_stream_offset > target_stream_offset) {
            upper_block_index = mid_block_index - 1;
        } else {
            lower_block_index = mid_block_index;
            memcpy(new_stream_offsets, stream_offsets, sizeof(stream_offsets));
        }
    }
    AFS_ASSERT(lower_block_index == upper_block_index);
    AFS_ASSERT(current_block_index >= current_block_index);
    if (current_block_index != lower_block_index) {
        // Advance to the new block
        obj->read.storage_offset = lower_block_index * afs->block_size;
        obj->read.data_chunk_length = 0;
        memcpy(obj->offset, new_stream_offsets, sizeof(new_stream_offsets));
        if (obj->read.stream == AFS_WILDCARD_STREAM) {
            uint64_t new_offset = 0;
            for (uint8_t i = 0; i < AFS_NUM_STREAMS; i++) {
                new_offset += new_stream_offsets[i];
            }
            offset -= new_offset - prev_stream_offset;
        } else {
            offset -= new_stream_offsets[obj->read.stream] - prev_stream_offset;
        }
    }

    while (offset) {
        uint32_t read_bytes;
        if (!read_seek_helper(afs, obj, NULL, MIN_VAL(offset, UINT32_MAX), &read_bytes)) {
            return false;
        }
        offset -= read_bytes;
    }
    return true;
}

uint64_t afs_object_size(afs_handle_t afs_handle, afs_object_handle_t object_handle, afs_stream_bitmask_t stream_bitmask) {
    afs_impl_t* afs = get_afs_impl(afs_handle);
    afs_obj_impl_t* obj = get_obj_impl(object_handle);
    AFS_ASSERT(afs && afs->in_use && obj);
    AFS_ASSERT_EQ(obj->state, STATE_READING);
    if (obj->read.stream == AFS_WILDCARD_STREAM) {
        AFS_ASSERT_NOT_EQ(stream_bitmask, 0);
    } else {
        AFS_ASSERT_EQ(stream_bitmask, 0);
    }

    // Save the current read position
    afs_read_position_t prev_pos;
    afs_object_save_read_position(afs_handle, object_handle, &prev_pos);

    // Advance to the last block
    const uint16_t current_block_index = obj->read.storage_offset / afs->block_size;
    uint16_t last_block_index = lookup_get_num_blocks(afs, obj->object_id) - 1;
    while (last_block_index > current_block_index) {
        uint64_t new_offsets[AFS_NUM_STREAMS] = {};
        if (!get_offsets_from_block_index(afs, obj->object_id, last_block_index, new_offsets)) {
            // Must not have written the offsets to this block, so ignore it and try the previous one
            last_block_index--;
            continue;
        }
        obj->read.storage_offset = last_block_index * afs->block_size;
        obj->read.data_chunk_length = 0;
        memcpy(obj->offset, new_offsets, sizeof(new_offsets));
        break;
    }

    // Seek until the end of the object (should return false once we hit the end)
    uint32_t read_bytes;
    while (read_seek_helper(afs, obj, NULL, UINT32_MAX, &read_bytes)) {
        // Keep seeking
    }
    uint64_t size = 0;
    if (obj->read.stream == AFS_WILDCARD_STREAM) {
        for (uint8_t i = 0; i < AFS_NUM_STREAMS; i++) {
            if (stream_bitmask & (1 << i)) {
                size += obj->offset[i];
            }
        }
    } else {
        size = obj->offset[obj->read.stream];
    }

    // Restore the previous read position
    afs_object_restore_read_position(afs_handle, object_handle, &prev_pos);

    return size;
}

void afs_object_save_read_position(afs_handle_t afs_handle, afs_object_handle_t object_handle, afs_read_position_t* read_position) {
    afs_impl_t* afs = get_afs_impl(afs_handle);
    afs_obj_impl_t* obj = get_obj_impl(object_handle);
    afs_read_pos_impl_t* pos = get_read_pos_impl(read_position);
    AFS_ASSERT(afs && afs->in_use && obj && pos);
    AFS_ASSERT_EQ(obj->state, STATE_READING);

    *pos = (afs_read_pos_impl_t) {
        .storage_offset = obj->read.storage_offset,
        .data_chunk_length = obj->read.data_chunk_length,
        .current_stream = obj->read.current_stream,
    };
    _Static_assert(sizeof(pos->offset) == sizeof(obj->offset), "Invalid offset sizes");
    memcpy(pos->offset, obj->offset, sizeof(pos->offset));
}

void afs_object_restore_read_position(afs_handle_t afs_handle, afs_object_handle_t object_handle, afs_read_position_t* read_position) {
    afs_impl_t* afs = get_afs_impl(afs_handle);
    afs_obj_impl_t* obj = get_obj_impl(object_handle);
    afs_read_pos_impl_t* pos = get_read_pos_impl(read_position);
    AFS_ASSERT(afs && afs->in_use && obj && pos);
    AFS_ASSERT_EQ(obj->state, STATE_READING);

    _Static_assert(sizeof(pos->offset) == sizeof(obj->offset), "Invalid offset sizes");
    memcpy(obj->offset, pos->offset, sizeof(pos->offset));
    obj->read.data_chunk_length = pos->data_chunk_length;
    obj->read.storage_offset = pos->storage_offset;
    obj->read.current_stream = pos->current_stream;
}

bool afs_object_close(afs_handle_t afs_handle, afs_object_handle_t object_handle) {
    afs_impl_t* afs = get_afs_impl(afs_handle);
    afs_obj_impl_t* obj = get_obj_impl(object_handle);
    AFS_ASSERT(afs && afs->in_use && obj);
    AFS_ASSERT_NOT_EQ(obj->state, STATE_INVALID);

    if (obj->state == STATE_WRITING) {
        // Make sure we can write the chunk header in the current block
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
        if (obj->cache.length > 0) {
            if (!flush_write_buffer(afs, obj, true)) {
                AFS_LOG_ERROR("Error flushing write buffer");
                return false;
            }
        }
    }

    open_object_list_remove(afs, obj);
    obj->state = STATE_INVALID;
    return true;
}

bool afs_object_list(afs_handle_t afs_handle, afs_object_list_entry_t* entry) {
    afs_impl_t* afs = get_afs_impl(afs_handle);
    afs_object_list_entry_impl_t* context = get_object_list_entry_impl(entry);
    AFS_ASSERT(afs && afs->in_use && entry && context);

    // Find the next block which contains the first block of an object
    for (uint16_t i = context->block; i < afs->num_blocks; i++) {
        const afs_lookup_table_entry_t lookup_value = afs->lookup_table[i];
        const uint16_t object_id = LOOKUP_TABLE_GET_OBJECT_ID(lookup_value);
        const uint16_t object_block_index = LOOKUP_TABLE_GET_OBJECT_BLOCK_INDEX(lookup_value);
        if (object_id == INVALID_OBJECT_ID || object_block_index != 0) {
            // This block is free or not the first block in the object
            continue;
        }
        context->block = i + 1;
        entry->object_id = object_id;
        return true;
    }

    // Check the objects which are open for writing and haven't written to the storage yet
    uint16_t open_index = 0;
    FOREACH_OPEN_OBJECT(afs, open_obj) {
        if (open_obj->state != STATE_WRITING) {
            continue;
        }
        if (lookup_get_num_blocks(afs, open_obj->object_id)) {
            // This object has been written to storage, so was already checked above
            continue;
        }
        if (open_index++ < context->open_index) {
            continue;
        }
        context->open_index++;
        entry->object_id = open_obj->object_id;
        return true;
    }
    // No more entries
    return false;
}

uint16_t afs_object_get_num_blocks(afs_handle_t afs_handle, uint16_t object_id) {
    afs_impl_t* afs = get_afs_impl(afs_handle);
    AFS_ASSERT(afs && afs->in_use);
    AFS_ASSERT_NOT_EQ(object_id, INVALID_OBJECT_ID);
    return lookup_get_num_blocks(afs, object_id);
}

void afs_object_delete(afs_handle_t afs_handle, uint16_t object_id) {
    afs_impl_t* afs = get_afs_impl(afs_handle);
    AFS_ASSERT(afs && afs->in_use);
    AFS_ASSERT_NOT_EQ(object_id, INVALID_OBJECT_ID);

    // Make sure the object isn't open
    FOREACH_OPEN_OBJECT(afs, open_obj) {
        AFS_ASSERT_NOT_EQ(open_obj->object_id, object_id);
    }

    // Remove the object from our lookup table
    AFS_LOG_DEBUG("Deleting object (%u)", object_id);
    bool found = false;
    for (uint16_t i = 0; i < afs->num_blocks; i++) {
        const afs_lookup_table_entry_t value = afs->lookup_table[i];
        if (LOOKUP_TABLE_GET_OBJECT_ID(value) != object_id) {
            continue;
        }
        const uint16_t object_block_index = LOOKUP_TABLE_GET_OBJECT_BLOCK_INDEX(value);
        if (object_block_index == 0) {
            // Erase the first block to delete the object on the underlying storage
            afs_storage_erase(afs, i);
            found = true;
        }
        AFS_LOG_DEBUG("Clearing lookup table for block (block=%u, object_block_index=%u)", object_id, object_block_index);
        lookup_table_set_free(afs, i, object_block_index == 0 ? LOOKUP_TABLE_BLOCK_STATE_ERASED : LOOKUP_TABLE_BLOCK_STATE_GARBAGE);
    }
    AFS_ASSERT(found);
}

void afs_wipe(afs_handle_t afs_handle, bool secure) {
    afs_impl_t* afs = get_afs_impl(afs_handle);
    AFS_ASSERT(afs && afs->in_use);
    AFS_ASSERT(!afs->open_object_list_head);

    for (uint16_t i = 0; i < afs->num_blocks; i++) {
        const afs_lookup_table_entry_t lookup_value = afs->lookup_table[i];
        const uint16_t object_id = LOOKUP_TABLE_GET_OBJECT_ID(lookup_value);
        if (object_id == INVALID_OBJECT_ID) {
            // This block is free
            continue;
        }
        const uint16_t object_block_index = LOOKUP_TABLE_GET_OBJECT_BLOCK_INDEX(lookup_value);
        // Only delete the first block for non-secure erase
        if (object_block_index == 0 || secure) {
            AFS_LOG_DEBUG("Erasing block (block=%u, object_id=%u, object_block_index%u)", i, object_id, object_block_index);
            afs_storage_erase(afs, i);
        }
        lookup_table_set_free(afs, i, object_block_index == 0 || secure ? LOOKUP_TABLE_BLOCK_STATE_ERASED : LOOKUP_TABLE_BLOCK_STATE_GARBAGE);
    }
}

uint16_t afs_size(afs_handle_t afs_handle) {
    afs_impl_t* afs = get_afs_impl(afs_handle);
    AFS_ASSERT(afs && afs->in_use);
    uint16_t blocks_used = 0;
    for (uint16_t i = 0; i < afs->num_blocks; i++) {
        if (LOOKUP_TABLE_GET_OBJECT_ID(afs->lookup_table[i]) != INVALID_OBJECT_ID) {
            blocks_used++;
        }
    }
    return blocks_used;
}

bool afs_is_storage_full(afs_handle_t afs_handle) {
    afs_impl_t* afs = get_afs_impl(afs_handle);
    AFS_ASSERT(afs && afs->in_use);
    for (uint16_t i = 0; i < afs->num_blocks; i++) {
        if (LOOKUP_TABLE_GET_OBJECT_ID(afs->lookup_table[i]) == INVALID_OBJECT_ID) {
            return false;
        }
    }
    return true;
}

void afs_prepare_storage(afs_handle_t afs_handle, uint16_t num_blocks) {
    afs_impl_t* afs = get_afs_impl(afs_handle);
    AFS_ASSERT(afs && afs->in_use);
    AFS_ASSERT(num_blocks > 0);
    for (uint16_t i = 0; i < afs->num_blocks; i++) {
        if (afs->lookup_table[i] == LOOKUP_TABLE_FREE_BLOCK_VALUE(LOOKUP_TABLE_BLOCK_STATE_ERASED)) {
            if (--num_blocks == 0) {
                return;
            }
        }
    }
    for (uint16_t i = 0; i < afs->num_blocks; i++) {
        const afs_lookup_table_entry_t value = afs->lookup_table[i];
        if (LOOKUP_TABLE_GET_OBJECT_ID(value) == INVALID_OBJECT_ID && LOOKUP_TABLE_GET_BLOCK_STATE(value) != LOOKUP_TABLE_BLOCK_STATE_ERASED) {
            afs_storage_erase(afs, i);
            lookup_table_set_free(afs, i, LOOKUP_TABLE_BLOCK_STATE_ERASED);
            if (--num_blocks == 0) {
                return;
            }
        }
    }
}
