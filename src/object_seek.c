#include "object_seek.h"

#include "afs_config.h"
#include "binary_search.h"
#include "lookup_table.h"
#include "storage.h"
#include "util.h"

#include <string.h>

#define SEARCH_RESULT_NO_CHANGE         UINT16_MAX

#define MIN_DATA_OFFSET_FOR_DENSITY     1024
#define DENSITY_MULTIPLIER              1000000
#define DEFAULT_DENSITY                 980000
#define MIN_DENSITY                     1000

static inline uint64_t estimate_update_density(uint64_t data_offset, uint64_t storage_offset) {
    if (data_offset < MIN_DATA_OFFSET_FOR_DENSITY) {
        // Not enough data to accurately calculate the density, so just assume the default
        return DEFAULT_DENSITY;
    }
    const uint64_t density = data_offset * DENSITY_MULTIPLIER / storage_offset;
    return CLAMP_VAL(density, MIN_DENSITY, DENSITY_MULTIPLIER);
}

static inline uint16_t estimate_calculate_index(uint64_t density, uint64_t target_offset, uint32_t region_size) {
    return target_offset * DENSITY_MULTIPLIER / density / region_size;
}

static bool get_offset_chunk_data(afs_impl_t* afs, uint16_t object_id, uint16_t block_index, offset_chunk_data_t* data) {
    const uint16_t block = lookup_table_get_block(&afs->lookup_table, object_id, block_index);
    return storage_read_block_header_offset_data(&afs->storage, block, data);
}

static uint64_t get_block_stream_offset(afs_impl_t* afs, afs_obj_impl_t* obj, uint16_t block_index, offset_chunk_data_t* data) {
    memset(data, 0, sizeof(*data));
    if (!get_offset_chunk_data(afs, obj->object_id, block_index, data)) {
        // There must not be any data in this block since the offset chunk wasn't written - return the max offset
        return UINT64_MAX;
    }
    return util_get_stream_offset(data->offsets, obj->read.stream);
}

static uint16_t search_block_index(afs_impl_t* afs, afs_obj_impl_t* obj, uint64_t target_offset, uint64_t* new_stream_offsets) {
    const uint32_t block_size = obj->storage.config->block_size;
    const uint16_t current_index = obj->read.storage_offset / block_size;
    const uint16_t max_index = lookup_table_get_num_blocks(&afs->lookup_table, obj->object_id) - 1;
    if (current_index == max_index) {
        // Can't go any higher, so assume we're already at the right index
        return SEARCH_RESULT_NO_CHANGE;
    }

    uint64_t density = estimate_update_density(util_get_stream_offset(obj->object_offset, obj->read.stream), obj->read.storage_offset);

    // Find an index which is above the target offset (ideally as close as possible)
    uint16_t index = MIN_VAL(estimate_calculate_index(density, target_offset, block_size) + 1, max_index);
    bool prev_check_was_prev_index = false;
    offset_chunk_data_t offset_data;
    while (true) {
        if (get_block_stream_offset(afs, obj, index, &offset_data) > target_offset) {
            // Found the index we're after for this loop
            if (prev_check_was_prev_index) {
                // We previously checked the previous index and it was lower, so that was the target index
                index--;
                return index == current_index ? SEARCH_RESULT_NO_CHANGE : index;
            }
            break;
        }
        // Need to go higher
        memcpy(new_stream_offsets, offset_data.offsets, sizeof(offset_data.offsets));
        if (index == max_index) {
            // Can't go higher, so just bail
            return SEARCH_RESULT_NO_CHANGE;
        }
        density = estimate_update_density(util_get_stream_offset(offset_data.offsets, obj->read.stream), (uint64_t)index * block_size);
        const uint16_t new_estimate = estimate_calculate_index(density, target_offset, block_size) + 1;
        index++;
        prev_check_was_prev_index = true;
        if (new_estimate <= max_index && new_estimate > index) {
            // Jump ahead to the new estimate instead
            index = new_estimate;
            prev_check_was_prev_index = false;
        }
    }

    if (index == current_index) {
        // Can't go lower - should never happen
        AFS_LOG_ERROR("Failed to find sub-block index (%u)", current_index);
        return SEARCH_RESULT_NO_CHANGE;
    }

    // Linearly loop down towards the current index
    while (--index > current_index) {
        if (get_block_stream_offset(afs, obj, index, &offset_data) > target_offset) {
            // Still need to go lower
            continue;
        }
        // Found the index which contains the target offset
        break;
    }

    if (index == current_index) {
        // Already on this index
        return SEARCH_RESULT_NO_CHANGE;
    }

    memcpy(new_stream_offsets, offset_data.offsets, sizeof(offset_data.offsets));
    return index;
}

static uint64_t get_sub_block_offset(afs_impl_t* afs, afs_obj_impl_t* obj, uint16_t index, seek_chunk_data_t* data) {
    memset(data, 0, sizeof(*data));
    const uint16_t block_index = obj->read.storage_offset / obj->storage.config->block_size;
    const uint16_t block = lookup_table_get_block(&afs->lookup_table, obj->object_id, block_index);
    if (!storage_read_seek_data(&afs->storage, block, index, data)) {
        // There must not be any data in this sub-block since the seek chunk wasn't written - return the max offset
        return UINT64_MAX;
    }
    return util_get_block_offset(data->offsets, obj->read.stream);
}

static uint16_t search_sub_block_index(afs_impl_t* afs, afs_obj_impl_t* obj, uint64_t target_offset, uint32_t* new_block_offsets) {
    const uint32_t sub_block_size = obj->storage.config->block_size / obj->storage.config->sub_blocks_per_block;
    const uint16_t current_index = (obj->read.storage_offset % obj->storage.config->block_size) / sub_block_size;
    const uint16_t max_index = obj->storage.config->sub_blocks_per_block - 1;
    if (current_index == max_index) {
        // Can't go any higher, so assume we're already at the right index
        return SEARCH_RESULT_NO_CHANGE;
    }

    const uint64_t density = estimate_update_density(util_get_stream_offset(obj->object_offset, obj->read.stream), obj->read.storage_offset);

    // Find an index which is above the target offset (ideally as close as possible)
    uint16_t index = MIN_VAL(estimate_calculate_index(density, target_offset, sub_block_size) + 1, max_index);
    bool prev_check_was_prev_index = false;
    seek_chunk_data_t seek_data;
    while (true) {
        if (get_sub_block_offset(afs, obj, index, &seek_data) > target_offset) {
            // Found the index we're after for this loop
            if (prev_check_was_prev_index) {
                // We previously checked the previous index and it was lower, so that was the target index
                index--;
                return index == current_index ? SEARCH_RESULT_NO_CHANGE : index;
            }
            break;
        }
        // Need to go higher
        memcpy(new_block_offsets, seek_data.offsets, sizeof(seek_data.offsets));
        if (index == max_index) {
            // Can't go higher, so just bail
            return SEARCH_RESULT_NO_CHANGE;
        }
        index++;
        prev_check_was_prev_index = true;
    }

    if (index == current_index) {
        // Can't go lower - should never happen
        AFS_LOG_ERROR("Failed to find sub-block index (%u)", current_index);
        return SEARCH_RESULT_NO_CHANGE;
    }

    // Linearly loop down towards the current index
    while (--index > current_index) {
        if (get_sub_block_offset(afs, obj, index, &seek_data) > target_offset) {
            // Still need to go lower
            continue;
        }
        // Found the index which contains the target offset
        break;
    }

    if (index == current_index) {
        // Already on this index
        return SEARCH_RESULT_NO_CHANGE;
    }

    memcpy(new_block_offsets, seek_data.offsets, sizeof(seek_data.offsets));
    return index;
}

uint64_t object_seek_to_block(afs_impl_t* afs, afs_obj_impl_t* obj, uint64_t offset) {
    const uint64_t prev_stream_offset = util_get_stream_offset(obj->object_offset, obj->read.stream);
    const uint64_t target_stream_offset = prev_stream_offset + offset;
    uint64_t new_stream_offsets[AFS_NUM_STREAMS];
    const uint16_t new_index = search_block_index(afs, obj, target_stream_offset, new_stream_offsets);
    if (new_index == SEARCH_RESULT_NO_CHANGE) {
        return offset;
    }

    // Advance to the new block
    obj->read.storage_offset = new_index * afs->storage_config.block_size;
    obj->read.data_chunk_length = 0;
    memcpy(obj->object_offset, new_stream_offsets, sizeof(new_stream_offsets));
    memset(obj->block_offset, 0, sizeof(obj->block_offset));
    const uint64_t amount_moved = util_get_stream_offset(new_stream_offsets, obj->read.stream) - prev_stream_offset;
    AFS_ASSERT(amount_moved <= offset);
    return offset - amount_moved;
}

uint64_t object_seek_to_sub_block(afs_impl_t* afs, afs_obj_impl_t* obj, uint64_t offset) {
    const uint16_t block_index = obj->read.storage_offset / obj->storage.config->block_size;
    const uint16_t block = lookup_table_get_block(&afs->lookup_table, obj->object_id, block_index);
    AFS_ASSERT_NOT_EQ(block, INVALID_BLOCK);
    if (!lookup_table_get_is_v2(&afs->lookup_table, block)) {
        // No sub-blocks
        return offset;
    }

    const uint64_t prev_block_offset = util_get_block_offset(obj->block_offset, obj->read.stream);
    const uint64_t target_block_offset = prev_block_offset + offset;
    uint32_t new_block_offsets[AFS_NUM_STREAMS];
    const uint16_t new_index = search_sub_block_index(afs, obj, target_block_offset, new_block_offsets);
    if (new_index == SEARCH_RESULT_NO_CHANGE) {
        return offset;
    }

    // Advance to the new sub-block
    const uint32_t sub_block_size = afs->storage_config.block_size / afs->storage_config.sub_blocks_per_block;
    obj->read.storage_offset = block_index * afs->storage_config.block_size + new_index * sub_block_size;
    obj->read.data_chunk_length = 0;
    for (uint8_t i = 0; i < AFS_NUM_STREAMS; i++) {
        obj->object_offset[i] += new_block_offsets[i] - obj->block_offset[i];
    }
    memcpy(obj->block_offset, new_block_offsets, sizeof(new_block_offsets));
    const uint64_t amount_moved = util_get_block_offset(new_block_offsets, obj->read.stream) - prev_block_offset;
    AFS_ASSERT(amount_moved <= offset);
    return offset - amount_moved;
}

void object_seek_to_last_block(afs_impl_t* afs, afs_obj_impl_t* obj) {
    // Advance to the last block
    const uint16_t current_block_index = obj->read.storage_offset / afs->storage_config.block_size;
    uint16_t last_block_index = lookup_table_get_num_blocks(&afs->lookup_table, obj->object_id) - 1;
    while (last_block_index > current_block_index) {
        offset_chunk_data_t offset_data = {};
        if (!get_offset_chunk_data(afs, obj->object_id, last_block_index, &offset_data)) {
            // Must not have written the offsets to this block, so ignore it and try the previous one
            last_block_index--;
            continue;
        }
        obj->read.storage_offset = last_block_index * afs->storage_config.block_size;
        obj->read.data_chunk_length = 0;
        memcpy(obj->object_offset, offset_data.offsets, sizeof(offset_data.offsets));
        break;
    }
}

bool object_seek_get_v2_object_size(afs_impl_t* afs, uint16_t object_id, afs_stream_bitmask_t stream_bitmask, uint64_t* size) {
    *size = 0;
    const uint16_t last_block = lookup_table_get_last_block(&afs->lookup_table, object_id);
    if (last_block == INVALID_BLOCK || !lookup_table_get_is_v2(&afs->lookup_table, last_block)) {
        return false;
    }

    // Read the seek chunk from the end of the last block
    seek_chunk_data_t seek_data = {};
    if (!storage_read_block_footer_seek_data(&afs->storage, last_block, &seek_data)) {
        return false;
    }

    // Read the offset data from the start of the last block if there are more than 1
    offset_chunk_data_t offset_data = {};
    if (lookup_table_get_num_blocks(&afs->lookup_table, object_id) > 1) {
        if (!storage_read_block_header_offset_data(&afs->storage, last_block, &offset_data)) {
            return false;
        }
    }

    for (uint8_t i = 0; i < AFS_NUM_STREAMS; i++) {
        if (stream_bitmask & (1 << i)) {
            *size += offset_data.offsets[i] + seek_data.offsets[i];
        }
    }
    return true;
}
