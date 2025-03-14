#include "afs/afs.h"

#include "afs_config.h"
#include "impl_types.h"
#include "lookup_table.h"
#include "open_object_list.h"
#include "object_read.h"
#include "object_seek.h"
#include "object_write.h"
#include "storage.h"
#include "util.h"

#include <string.h>

#define GET_IMPL(TYPE, HANDLE) ({ \
        AFS_ASSERT(HANDLE); \
        TYPE* impl = (TYPE*)HANDLE->priv; \
        AFS_ASSERT(impl); \
        impl; \
    })
#define GET_AFS_IMPL_IN_USE(HANDLE) ({ \
        afs_impl_t* impl = GET_IMPL(afs_impl_t, HANDLE); \
        AFS_ASSERT(impl->in_use); \
        impl; \
    })

static void validate_object_buffer_size(const afs_storage_config_t* storage_config, uint32_t buffer_size) {
    AFS_ASSERT(buffer_size >= sizeof(block_header_t) + sizeof(chunk_header_t));
    AFS_ASSERT(buffer_size >= BLOCK_FOOTER_LENGTH);
    AFS_ASSERT(buffer_size >= storage_config->min_read_write_size);
    const uint32_t sub_block_size = storage_config->block_size / storage_config->sub_blocks_per_block;
    if (buffer_size > sub_block_size) {
        AFS_ASSERT_EQ(buffer_size % sub_block_size, 0);
    } else {
        AFS_ASSERT_EQ(sub_block_size % buffer_size, 0);
    }
    AFS_ASSERT_EQ(buffer_size % storage_config->min_read_write_size, 0);
}

void afs_init(afs_handle_t afs_handle, const afs_init_t* init) {
    AFS_ASSERT(init);
    const afs_storage_config_t* storage_config = &init->storage_config;
    AFS_ASSERT(storage_config->num_blocks && storage_config->num_blocks < INVALID_BLOCK);
    AFS_ASSERT(init->read_write_buffer && init->lookup_table_buffer);
    AFS_ASSERT(storage_config->min_read_write_size >= BLOCK_FOOTER_LENGTH);
    AFS_ASSERT(storage_config->block_size > 0 && (storage_config->block_size % storage_config->min_read_write_size) == 0);
    AFS_ASSERT(storage_config->sub_blocks_per_block > 0 && (storage_config->block_size % storage_config->sub_blocks_per_block) == 0);
    AFS_ASSERT(storage_config->block_size / storage_config->sub_blocks_per_block >= BLOCK_FOOTER_LENGTH);
    AFS_ASSERT(storage_config->read && storage_config->write && storage_config->erase);

    // Initialize the impl object and populate the lookup table from the storage
    afs_impl_t* afs = GET_IMPL(afs_impl_t, afs_handle);
    *afs = (afs_impl_t) {
        .in_use = true,
        .storage_config = *storage_config,
        .lookup_table = {
            .num_blocks = storage_config->num_blocks,
            .values = init->lookup_table_buffer,
            .version_bitmap = init->lookup_table_buffer + storage_config->num_blocks * sizeof(uint32_t),
        },
        .storage = {
            .config = &afs->storage_config,
            .cache = {
                .buffer = init->read_write_buffer,
                .size = storage_config->min_read_write_size,
            },
        },
    };
    lookup_table_populate(afs, init->mount_callbacks.object_found);
}

void afs_deinit(afs_handle_t afs_handle) {
    afs_impl_t* afs = GET_AFS_IMPL_IN_USE(afs_handle);
    AFS_ASSERT(open_object_list_is_empty(afs));
    afs->in_use = false;
}

uint16_t afs_object_create(afs_handle_t afs_handle, afs_object_handle_t object_handle, const afs_object_config_t* config) {
    afs_impl_t* afs = GET_AFS_IMPL_IN_USE(afs_handle);
    afs_obj_impl_t* obj = GET_IMPL(afs_obj_impl_t, object_handle);
    AFS_ASSERT(config && config->buffer);
    AFS_ASSERT_EQ(obj->state, OBJ_STATE_INVALID);
    validate_object_buffer_size(afs->storage.config, config->buffer_size);

    // Initialize the afs_obj_impl_t and add it to the open object list
    *obj = (afs_obj_impl_t) {
        .state = OBJ_STATE_WRITING,
        .object_id = lookup_table_get_next_object_id(&afs->lookup_table),
        .storage = {
            .config = afs->storage.config,
            .cache = {
                .buffer = config->buffer,
                .size = config->buffer_size,
                .position = {
                    .block = INVALID_BLOCK,
                },
            },
        },
    };
    open_object_list_add(afs, obj);
    return obj->object_id;
}

bool afs_object_write(afs_handle_t afs_handle, afs_object_handle_t object_handle, uint8_t stream, const uint8_t* data, uint32_t length) {
    AFS_ASSERT(data && length);
    afs_impl_t* afs = GET_AFS_IMPL_IN_USE(afs_handle);
    afs_obj_impl_t* obj = GET_IMPL(afs_obj_impl_t, object_handle);
    AFS_ASSERT_EQ(obj->state, OBJ_STATE_WRITING);
    AFS_ASSERT(stream < AFS_NUM_STREAMS);
    while (length) {
        const uint32_t write_length = object_write_process(afs, obj, stream, data, length);
        if (!write_length) {
            return false;
        }
        data += write_length;
        length -= write_length;
    }
    return true;
}

bool afs_object_open(afs_handle_t afs_handle, afs_object_handle_t object_handle, uint8_t stream, uint16_t object_id, const afs_object_config_t* config) {
    afs_impl_t* afs = GET_AFS_IMPL_IN_USE(afs_handle);
    afs_obj_impl_t* obj = GET_IMPL(afs_obj_impl_t, object_handle);
    AFS_ASSERT(config && config->buffer);
    AFS_ASSERT_EQ(obj->state, OBJ_STATE_INVALID);
    AFS_ASSERT(stream < AFS_NUM_STREAMS || stream == AFS_WILDCARD_STREAM);
    AFS_ASSERT(object_id != INVALID_OBJECT_ID);
    validate_object_buffer_size(afs->storage.config, config->buffer_size);

    // Find the first block from our lookup table
    const uint16_t block = lookup_table_get_block(&afs->lookup_table, object_id, 0);
    if (block == INVALID_BLOCK) {
        AFS_LOG_WARN("Did not find block (object_id=%u)", object_id);
        return false;
    }

    *obj = (afs_obj_impl_t) {
        .state = OBJ_STATE_READING,
        .object_id = object_id,
        .read.stream = stream,
        .storage = {
            .config = afs->storage.config,
            .cache = {
                .buffer = config->buffer,
                .size = config->buffer_size,
                .position = {
                    .block = block,
                },
            },
        },
    };
    open_object_list_add(afs, obj);
    return true;
}

uint32_t afs_object_read(afs_handle_t afs_handle, afs_object_handle_t object_handle, uint8_t* data, uint32_t max_length, uint8_t* stream) {
    AFS_ASSERT(data && max_length);
    afs_impl_t* afs = GET_AFS_IMPL_IN_USE(afs_handle);
    afs_obj_impl_t* obj = GET_IMPL(afs_obj_impl_t, object_handle);
    AFS_ASSERT_EQ(obj->state, OBJ_STATE_READING);
    if (obj->read.stream == AFS_WILDCARD_STREAM) {
        // Must pass a stream pointer when the object is opened with a wildcard stream specified
        AFS_ASSERT(stream);
    } else {
        // Shouldn't pass a stream pointer when the object is opened with one specified
        AFS_ASSERT(!stream);
    }

    uint32_t total_read_bytes = 0;
    while (max_length) {
        uint32_t read_bytes;
        if (!object_read_process(afs, obj, data, max_length, &read_bytes)) {
            break;
        }
        data += read_bytes;
        max_length -= read_bytes;
        total_read_bytes += read_bytes;
        if (read_bytes && stream) {
            // There is a stream pointer passed, so we can only read a single chunk of data
            *stream = obj->read.current_stream;
            break;
        }
    }
    return total_read_bytes;
}

bool afs_object_seek(afs_handle_t afs_handle, afs_object_handle_t object_handle, uint64_t offset) {
    afs_impl_t* afs = GET_AFS_IMPL_IN_USE(afs_handle);
    afs_obj_impl_t* obj = GET_IMPL(afs_obj_impl_t, object_handle);
    AFS_ASSERT_EQ(obj->state, OBJ_STATE_READING);

    // Try to seek directly to the block and sub-block containing the offset as an optimization
    offset = object_seek_to_block(afs, obj, offset);
    offset = object_seek_to_sub_block(afs, obj, offset);

    // Read the remaining bytes through the object
    while (offset) {
        uint32_t read_bytes;
        if (!object_read_process(afs, obj, NULL, MIN_VAL(offset, UINT32_MAX), &read_bytes)) {
            return false;
        }
        offset -= read_bytes;
    }
    return true;
}

uint64_t afs_object_size(afs_handle_t afs_handle, afs_object_handle_t object_handle, afs_stream_bitmask_t stream_bitmask) {
    afs_impl_t* afs = GET_AFS_IMPL_IN_USE(afs_handle);
    afs_obj_impl_t* obj = GET_IMPL(afs_obj_impl_t, object_handle);
    AFS_ASSERT_EQ(obj->state, OBJ_STATE_READING);
    if (obj->read.stream == AFS_WILDCARD_STREAM) {
        AFS_ASSERT_NOT_EQ(stream_bitmask, 0);
    } else {
        AFS_ASSERT_EQ(stream_bitmask, 0);
        stream_bitmask = 1 << obj->read.stream;
    }

    // Try to utilize the v2 features to calculate the size quickly
    uint64_t v2_size;
    if (object_seek_get_v2_object_size(afs, obj->object_id, stream_bitmask, &v2_size)) {
        return v2_size;
    }

    // Save the current read position
    afs_read_position_t prev_pos;
    afs_object_save_read_position(afs_handle, object_handle, &prev_pos);

    // Advance to the last block
    object_seek_to_last_block(afs, obj);

    // Read until the end of the object (should return false once we hit the end)
    uint32_t read_bytes;
    while (object_read_process(afs, obj, NULL, UINT32_MAX, &read_bytes)) {
        // Keep reading
    }

    // Get the size based on the current position
    uint64_t size = 0;
    for (uint8_t i = 0; i < AFS_NUM_STREAMS; i++) {
        if (stream_bitmask & (1 << i)) {
            size += obj->object_offset[i];
        }
    }

    // Restore the previous read position
    afs_object_restore_read_position(afs_handle, object_handle, &prev_pos);

    return size;
}

void afs_object_save_read_position(afs_handle_t afs_handle, afs_object_handle_t object_handle, afs_read_position_t* read_position) {
    afs_obj_impl_t* obj = GET_IMPL(afs_obj_impl_t, object_handle);
    AFS_ASSERT_EQ(obj->state, OBJ_STATE_READING);
    afs_read_pos_impl_t* pos = GET_IMPL(afs_read_pos_impl_t, read_position);

    *pos = (afs_read_pos_impl_t) {
        .storage_offset = obj->read.storage_offset,
        .data_chunk_length = obj->read.data_chunk_length,
        .current_stream = obj->read.current_stream,
    };
    memcpy(pos->object_offset, obj->object_offset, sizeof(pos->object_offset));
    memcpy(pos->block_offset, obj->block_offset, sizeof(pos->block_offset));
}

void afs_object_restore_read_position(afs_handle_t afs_handle, afs_object_handle_t object_handle, afs_read_position_t* read_position) {
    afs_impl_t* afs = GET_AFS_IMPL_IN_USE(afs_handle);
    (void)afs;
    afs_obj_impl_t* obj = GET_IMPL(afs_obj_impl_t, object_handle);
    AFS_ASSERT_EQ(obj->state, OBJ_STATE_READING);
    afs_read_pos_impl_t* pos = GET_IMPL(afs_read_pos_impl_t, read_position);

    memcpy(obj->object_offset, pos->object_offset, sizeof(pos->object_offset));
    memcpy(obj->block_offset, pos->block_offset, sizeof(pos->block_offset));
    obj->read.data_chunk_length = pos->data_chunk_length;
    obj->read.storage_offset = pos->storage_offset;
    obj->read.current_stream = pos->current_stream;
}

bool afs_object_close(afs_handle_t afs_handle, afs_object_handle_t object_handle) {
    afs_impl_t* afs = GET_AFS_IMPL_IN_USE(afs_handle);
    afs_obj_impl_t* obj = GET_IMPL(afs_obj_impl_t, object_handle);
    AFS_ASSERT_NOT_EQ(obj->state, OBJ_STATE_INVALID);

    if (obj->state == OBJ_STATE_WRITING && !object_write_finish(afs, obj)) {
        return false;
    }

    open_object_list_remove(afs, obj);
    obj->state = OBJ_STATE_INVALID;
    return true;
}

bool afs_object_list(afs_handle_t afs_handle, afs_object_list_entry_t* entry) {
    afs_impl_t* afs = GET_AFS_IMPL_IN_USE(afs_handle);
    afs_object_list_entry_impl_t* context = GET_IMPL(afs_object_list_entry_impl_t, entry);

    // Find the next block which contains the first block of an object
    const uint16_t object_id = lookup_table_iter_get_next_object(&afs->lookup_table, &context->block);
    if (object_id != INVALID_OBJECT_ID) {
        entry->object_id = object_id;
        return true;
    }

    // Check the objects which are open for writing and haven't written to the storage yet
    const uint16_t next_object_id = open_object_list_get_writing_no_storage(afs, context->open_index);
    if (next_object_id == INVALID_OBJECT_ID) {
        return false;
    }
    context->open_index++;
    entry->object_id = next_object_id;
    return true;
}

uint16_t afs_object_get_num_blocks(afs_handle_t afs_handle, uint16_t object_id) {
    afs_impl_t* afs = GET_AFS_IMPL_IN_USE(afs_handle);
    AFS_ASSERT_NOT_EQ(object_id, INVALID_OBJECT_ID);
    return lookup_table_get_num_blocks(&afs->lookup_table, object_id);
}

void afs_object_delete(afs_handle_t afs_handle, uint16_t object_id) {
    afs_impl_t* afs = GET_AFS_IMPL_IN_USE(afs_handle);
    AFS_ASSERT_NOT_EQ(object_id, INVALID_OBJECT_ID);

    // Make sure the object isn't open
    AFS_ASSERT(!open_object_list_contains(afs, object_id));

    // Remove the object from our lookup table
    AFS_LOG_DEBUG("Deleting object (%u)", object_id);
    const uint16_t first_block = lookup_table_delete_object(&afs->lookup_table, object_id);
    storage_erase(&afs->storage, first_block);
}

void afs_wipe(afs_handle_t afs_handle, bool secure) {
    afs_impl_t* afs = GET_AFS_IMPL_IN_USE(afs_handle);
    AFS_ASSERT(open_object_list_is_empty(afs));
    uint16_t block = 0;
    while (true) {
        bool should_erase = secure;
        block = lookup_table_wipe_next_in_use(&afs->lookup_table, block, &should_erase);
        if (block == INVALID_BLOCK) {
            break;
        }
        if (should_erase) {
            storage_erase(&afs->storage, block);
        }
    }
}

uint16_t afs_size(afs_handle_t afs_handle) {
    afs_impl_t* afs = GET_AFS_IMPL_IN_USE(afs_handle);
    return lookup_table_get_total_num_blocks(&afs->lookup_table);
}

bool afs_is_storage_full(afs_handle_t afs_handle) {
    afs_impl_t* afs = GET_AFS_IMPL_IN_USE(afs_handle);
    return lookup_table_is_full(&afs->lookup_table);
}

void afs_prepare_storage(afs_handle_t afs_handle, uint16_t num_blocks) {
    afs_impl_t* afs = GET_AFS_IMPL_IN_USE(afs_handle);
    AFS_ASSERT(num_blocks > 0);
    // Check how many are already erased
    const uint16_t num_erased = lookup_table_get_num_erased(&afs->lookup_table);
    if (num_erased >= num_blocks) {
        return;
    }
    num_blocks -= num_erased;
    // Find some blocks which can be erased
    uint16_t erase_block = 0;
    while (num_blocks > 0) {
        erase_block = lookup_table_get_next_pending_erase(&afs->lookup_table, erase_block);
        if (erase_block == INVALID_BLOCK) {
            break;
        }
        storage_erase(&afs->storage, erase_block);
        num_blocks--;
    }
}
