#include "lookup_table.h"

#include "afs_config.h"
#include "storage.h"
#include "util.h"

#include <string.h>

#define LOOKUP_TABLE_BLOCK_STATE_ERASED         0x0000
#define LOOKUP_TABLE_BLOCK_STATE_MAYBE_ERASED   0x0001
#define LOOKUP_TABLE_BLOCK_STATE_UNKNOWN        0x0002
#define LOOKUP_TABLE_BLOCK_STATE_GARBAGE        0x0003

#define LOOKUP_TABLE_GET_OBJECT_ID(X) ((uint16_t)((X) >> 16))
#define LOOKUP_TABLE_GET_OBJECT_BLOCK_INDEX(X) ((uint16_t)(X))
#define LOOKUP_TABLE_GET_BLOCK_STATE(X) ((uint16_t)(X))
#define LOOKUP_TABLE_VALUE(OBJECT_ID, OBJECT_BLOCK_INDEX) \
    ((((uint32_t)(OBJECT_ID) & 0xffff) << 16) | ((OBJECT_BLOCK_INDEX) & 0xffff))
#define LOOKUP_TABLE_FREE_BLOCK_VALUE(STATE) \
    LOOKUP_TABLE_VALUE(INVALID_OBJECT_ID, STATE)

static inline void set_value(lookup_table_t* lookup_table, uint16_t block, uint16_t object_id, uint16_t object_block_index) {
    lookup_table->values[block] = LOOKUP_TABLE_VALUE(object_id, object_block_index);
}

static inline void set_free(lookup_table_t* lookup_table, uint16_t block, uint16_t state) {
    set_value(lookup_table, block, INVALID_OBJECT_ID, state);
}

static inline void set_is_v2(lookup_table_t* lookup_table, uint16_t block, bool value) {
    lookup_table->version_bitmap[block / sizeof(uint8_t)] |= (value ? 1 : 0) << (block & 0x7);
}

static inline bool get_is_v2(const lookup_table_t* lookup_table, uint16_t block) {
    return lookup_table->version_bitmap[block / sizeof(uint8_t)] & (1 << (block & 0x07));
}

static uint32_t get_object_data_from_cache(cache_t* cache, uint8_t* stream) {
    AFS_ASSERT_EQ(cache->length, cache->size);
    AFS_ASSERT_EQ(cache->position.offset, 0);

    uint32_t read_offset = sizeof(block_header_t);
    *stream = AFS_WILDCARD_STREAM;
    uint32_t data_length = 0;
    while (true) {
        // Read the chunk header
        chunk_header_t header;
        if (cache->length - read_offset < sizeof(header)) {
            break;
        }
        memcpy(&header, &cache->buffer[read_offset], sizeof(header));
        read_offset += sizeof(header);

        // Check if this is a data chunk
        const uint8_t chunk_type = CHUNK_TAG_GET_TYPE(header.tag);
        const uint8_t chunk_stream = chunk_type & 0xf;
        if (chunk_type < CHUNK_TYPE_DATA_FIRST || chunk_type > CHUNK_TYPE_DATA_LAST) {
            break;
        }
        // Check its of the same stream we previously read if it's not the first one
        *stream = *stream == AFS_WILDCARD_STREAM ? chunk_stream : *stream;
        if (chunk_stream != *stream) {
            break;
        }
        // Read and shift the data down within the cache in order to reuse its buffer
        const uint32_t chunk_length = MIN_VAL(CHUNK_TAG_GET_LENGTH(header.tag), cache->length - read_offset);
        memmove(&cache->buffer[data_length], &cache->buffer[read_offset], chunk_length);
        read_offset += chunk_length;
        data_length += chunk_length;
    }

    // Wipe the cache since we reused its buffer
    cache->length = 0;
    return data_length;
}

static void populate_for_block(lookup_table_t* lookup_table, storage_t* storage, uint16_t block, afs_object_found_callback_t object_found_callback) {
    position_t position = {
        .block = block,
        .offset = 0,
    };
    block_header_t header;
    storage_read_block_header(storage, &position, &header);
    bool is_v2 = false;
    if (util_is_block_header_valid(&header, &is_v2)) {
        set_value(lookup_table, block, header.object_id, header.object_block_index);
        if (header.object_block_index == 0 && object_found_callback) {
            // Call the object found callback
            cache_t* cache = &storage->cache;
            AFS_ASSERT_EQ(cache->position.block, block);
            uint8_t stream;
            const uint32_t data_length = get_object_data_from_cache(cache, &stream);
            object_found_callback(header.object_id, stream, cache->buffer, data_length);
        }
    } else {
        // Check if the header is completely empty as that might be an indication that the block is erased, so we'll
        // use this block before we use other ones that might have more-expensive erase operations
        const block_header_t EMPTY_BLOCK_HEADER = {};
        const bool maybe_erased = !memcmp(&header, &EMPTY_BLOCK_HEADER, sizeof(header));
        set_free(lookup_table, block, maybe_erased ? LOOKUP_TABLE_BLOCK_STATE_MAYBE_ERASED : LOOKUP_TABLE_BLOCK_STATE_UNKNOWN);
    }
    set_is_v2(lookup_table, block, is_v2);
    // Use the lookup value to generate some randomness in our seed
    lookup_table->object_id_seed ^= lookup_table->values[block];
}

static bool is_object_deleted(const lookup_table_t* lookup_table, uint16_t object_id) {
    const uint32_t search_lookup_value = LOOKUP_TABLE_VALUE(object_id, 0);
    for (uint32_t j = 0; j < lookup_table->num_blocks; j++) {
        if (lookup_table->values[j] == search_lookup_value) {
            return true;
        }
    }
    return false;
}

void lookup_table_populate(afs_impl_t* afs, afs_object_found_callback_t object_found_callback) {
    // Populate our lookup table from the storage
    for (uint16_t block = 0; block < afs->storage_config.num_blocks; block++) {
        populate_for_block(&afs->lookup_table, &afs->storage, block, object_found_callback);
    }

    // Remove any entries from our lookup table for deleted objects
    for (uint16_t i = 0; i < afs->storage_config.num_blocks; i++) {
        const uint32_t value = afs->lookup_table.values[i];
        const uint16_t object_id = LOOKUP_TABLE_GET_OBJECT_ID(value);
        if (object_id == INVALID_OBJECT_ID) {
            // Free block
            continue;
        }
        const uint16_t object_block_index = LOOKUP_TABLE_GET_OBJECT_BLOCK_INDEX(value);
        if (object_block_index == 0) {
            // This is the first block, so the object is valid
            continue;
        }
        if (!is_object_deleted(&afs->lookup_table, object_id)) {
            AFS_LOG_DEBUG("Removing deleted object from lookup table (object_id=%u, object_block_index=%u)", object_id, object_block_index);
            set_free(&afs->lookup_table, i, LOOKUP_TABLE_BLOCK_STATE_GARBAGE);
        }
    }
}

uint32_t lookup_table_get_block(const lookup_table_t* lookup_table, uint16_t object_id, uint16_t object_block_index) {
    const uint32_t expected_lookup_value = LOOKUP_TABLE_VALUE(object_id, object_block_index);
    for (uint16_t i = 0; i < lookup_table->num_blocks; i++) {
        if (lookup_table->values[i] == expected_lookup_value) {
            return i;
        }
    }
    return INVALID_BLOCK;
}

uint16_t lookup_table_get_num_blocks(const lookup_table_t* lookup_table, uint16_t object_id) {
    uint16_t num_blocks = 0;
    for (uint16_t i = 0; i < lookup_table->num_blocks; i++) {
        const uint32_t value = lookup_table->values[i];
        if (LOOKUP_TABLE_GET_OBJECT_ID(value) == object_id) {
            num_blocks = MAX_VAL(num_blocks, LOOKUP_TABLE_GET_OBJECT_BLOCK_INDEX(value) + 1);
        }
    }
    return num_blocks;
}

uint16_t lookup_table_get_last_block(const lookup_table_t* lookup_table, uint16_t object_id) {
    uint16_t last_block = INVALID_BLOCK;
    uint16_t max_block_index = 0;
    for (uint16_t i = 0; i < lookup_table->num_blocks; i++) {
        const uint32_t value = lookup_table->values[i];
        if (LOOKUP_TABLE_GET_OBJECT_ID(value) != object_id) {
            continue;
        }
        const uint16_t block_index = LOOKUP_TABLE_GET_OBJECT_BLOCK_INDEX(value);
        if (last_block == INVALID_BLOCK || block_index > max_block_index) {
            last_block = i;
            max_block_index = block_index;
        }
    }
    return last_block;
}

bool lookup_table_get_is_v2(const lookup_table_t* lookup_table, uint16_t block) {
    return get_is_v2(lookup_table, block);
}

uint16_t lookup_table_get_next_object_id(lookup_table_t* lookup_table) {
    // In the worst case, this function is O(num_blocks^2), but statistically, there is a num_blocks / 2^16 chance that
    // we find a valid object ID with each loop, so in practice it should be very fast.
    while (true) {
        // Very simple psuedo-random number generator which uniformly generates 16-bit values
        lookup_table->object_id_seed = lookup_table->object_id_seed * 1664525 + 1013904223;
        const uint16_t object_id = (uint16_t)lookup_table->object_id_seed;
        if (object_id == INVALID_OBJECT_ID) {
            continue;
        }
        bool in_use = false;
        for (uint16_t j = 0; j < lookup_table->num_blocks; j++) {
            if (object_id == LOOKUP_TABLE_GET_OBJECT_ID(lookup_table->values[j])) {
                in_use = true;
                break;
            }
        }
        if (!in_use) {
            return object_id;
        }
    }
}

uint16_t lookup_table_iter_get_next_object(const lookup_table_t* lookup_table, uint16_t *block) {
    for (uint16_t i = *block; i < lookup_table->num_blocks; i++) {
        const uint32_t lookup_value = lookup_table->values[i];
        const uint16_t object_id = LOOKUP_TABLE_GET_OBJECT_ID(lookup_value);
        const uint16_t object_block_index = LOOKUP_TABLE_GET_OBJECT_BLOCK_INDEX(lookup_value);
        if (object_id == INVALID_OBJECT_ID || object_block_index != 0) {
            // This block is free or not the first block in the object
            continue;
        }
        *block = i + 1;
        return object_id;
    }
    return INVALID_OBJECT_ID;
}

uint16_t lookup_table_delete_object(lookup_table_t* lookup_table, uint16_t object_id) {
    uint16_t first_block = INVALID_BLOCK;
    for (uint16_t i = 0; i < lookup_table->num_blocks; i++) {
        const uint32_t value = lookup_table->values[i];
        if (LOOKUP_TABLE_GET_OBJECT_ID(value) != object_id) {
            continue;
        }
        const uint16_t object_block_index = LOOKUP_TABLE_GET_OBJECT_BLOCK_INDEX(value);
        if (object_block_index == 0) {
            first_block = i;
        }
        AFS_LOG_DEBUG("Clearing lookup table for block (block=%u, object_block_index=%u)", object_id, object_block_index);
        set_free(lookup_table, i, object_block_index == 0 ? LOOKUP_TABLE_BLOCK_STATE_ERASED : LOOKUP_TABLE_BLOCK_STATE_GARBAGE);
    }
    AFS_ASSERT_NOT_EQ(first_block, INVALID_BLOCK);
    return first_block;
}

uint16_t lookup_table_get_total_num_blocks(const lookup_table_t* lookup_table) {
    uint16_t blocks_used = 0;
    for (uint16_t i = 0; i < lookup_table->num_blocks; i++) {
        if (LOOKUP_TABLE_GET_OBJECT_ID(lookup_table->values[i]) != INVALID_OBJECT_ID) {
            blocks_used++;
        }
    }
    return blocks_used;
}

bool lookup_table_is_full(const lookup_table_t* lookup_table) {
    for (uint16_t i = 0; i < lookup_table->num_blocks; i++) {
        if (LOOKUP_TABLE_GET_OBJECT_ID(lookup_table->values[i]) == INVALID_OBJECT_ID) {
            return false;
        }
    }
    return true;
}

uint16_t lookup_table_acquire_block(lookup_table_t* lookup_table, uint16_t object_id, uint16_t object_block_index, bool* is_erased) {
    // Look for a block which is ideally already erased
    // Find the first free / best block from our lookup table (the underlying storage handles wear leveling for us)
    uint16_t best_block = INVALID_BLOCK;
    uint16_t best_block_state = UINT16_MAX;
    for (uint16_t i = 0; i < lookup_table->num_blocks; i++) {
        const uint32_t value = lookup_table->values[i];
        if (LOOKUP_TABLE_GET_OBJECT_ID(value) == INVALID_OBJECT_ID) {
            const uint16_t state = LOOKUP_TABLE_GET_BLOCK_STATE(value);
            if (state < best_block_state) {
                best_block = i;
                best_block_state = state;
            }
            if (state == LOOKUP_TABLE_BLOCK_STATE_ERASED) {
                break;
            }
        }
    }

    if (best_block == INVALID_BLOCK) {
        return INVALID_BLOCK;
    }

    lookup_table->values[best_block] = LOOKUP_TABLE_VALUE(object_id, object_block_index);
    set_is_v2(lookup_table, best_block, true);
    *is_erased = best_block_state == LOOKUP_TABLE_BLOCK_STATE_ERASED;
    return best_block;
}

uint16_t lookup_table_wipe_next_in_use(lookup_table_t* lookup_table, uint16_t start_block, bool* should_erase) {
    for (uint16_t i = start_block; i < lookup_table->num_blocks; i++) {
        const uint32_t lookup_value = lookup_table->values[i];
        const uint16_t object_id = LOOKUP_TABLE_GET_OBJECT_ID(lookup_value);
        if (object_id == INVALID_OBJECT_ID) {
            // This block is free
            continue;
        }
        const uint16_t object_block_index = LOOKUP_TABLE_GET_OBJECT_BLOCK_INDEX(lookup_value);
        // Should always erase the first block
        *should_erase = object_block_index == 0 || *should_erase;
        if (*should_erase) {
            AFS_LOG_DEBUG("Erasing block (block=%u, object_id=%u, object_block_index%u)", i, object_id, object_block_index);
        }
        set_free(lookup_table, i, *should_erase ? LOOKUP_TABLE_BLOCK_STATE_ERASED : LOOKUP_TABLE_BLOCK_STATE_GARBAGE);
        return i;
    }
    return INVALID_BLOCK;
}

uint16_t lookup_table_get_num_erased(const lookup_table_t* lookup_table) {
    uint16_t num_erased = 0;
    for (uint16_t i = 0; i < lookup_table->num_blocks; i++) {
        if (lookup_table->values[i] == LOOKUP_TABLE_FREE_BLOCK_VALUE(LOOKUP_TABLE_BLOCK_STATE_ERASED)) {
            num_erased++;
        }
    }
    return num_erased;
}

uint16_t lookup_table_get_next_pending_erase(lookup_table_t* lookup_table, uint16_t start_block) {
    for (uint16_t i = start_block; i < lookup_table->num_blocks; i++) {
        const uint32_t value = lookup_table->values[i];
        if (LOOKUP_TABLE_GET_OBJECT_ID(value) == INVALID_OBJECT_ID && LOOKUP_TABLE_GET_BLOCK_STATE(value) != LOOKUP_TABLE_BLOCK_STATE_ERASED) {
            set_free(lookup_table, i, LOOKUP_TABLE_BLOCK_STATE_ERASED);
            return i;
        }
    }
    return INVALID_BLOCK;
}

bool lookup_table_debug_dump_block(const lookup_table_t* lookup_table, uint16_t block) {
    const uint32_t value = lookup_table->values[block];
    if (!value) {
        return false;
    }
    const uint16_t object_id = LOOKUP_TABLE_GET_OBJECT_ID(value);
    const uint16_t object_block_index = LOOKUP_TABLE_GET_OBJECT_BLOCK_INDEX(value);
    AFS_LOG_INFO("[%3u]={object_id=%u, object_block_index=%u}", block, object_id, object_block_index);
    return true;
}

void lookup_table_debug_dump_object(const lookup_table_t* lookup_table, uint16_t object_id) {
    for (uint16_t i = 0; i < lookup_table->num_blocks; i++) {
        const uint32_t value = lookup_table->values[i];
        if (LOOKUP_TABLE_GET_OBJECT_ID(value) == object_id) {
            AFS_LOG_INFO("[%3u]={object_block_index=%u}", i, LOOKUP_TABLE_GET_OBJECT_BLOCK_INDEX(value));
        }
    }
}
