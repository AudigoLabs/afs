#include "cache.h"

#include "afs_config.h"
#include "util.h"

#include <string.h>

bool cache_contains(const cache_t* cache, const position_t* position) {
    return cache->length && cache->position.block == position->block && cache->position.offset == ALIGN_DOWN(position->offset, cache->size);
}

uint32_t cache_read(const cache_t* cache, const position_t* position, void* buf, uint32_t length) {
    const uint32_t aligned_offset = ALIGN_DOWN(position->offset, cache->size);
    if (!cache->length || cache->position.block != position->block || cache->position.offset != aligned_offset) {
        return 0;
    }
    const uint32_t buffer_read_index = position->offset - aligned_offset;
    length = MIN_VAL(length, cache->length - buffer_read_index);
    memcpy(buf, &cache->buffer[buffer_read_index], length);
    return length;
}

void cache_write(cache_t* cache, const void* data, uint32_t length) {
    AFS_ASSERT(cache->size - cache->length >= length);
    if (data) {
        memcpy(&cache->buffer[cache->length], data, length);
    } else {
        memset(&cache->buffer[cache->length], 0, length);
    }
    cache->length += length;
}

void cache_invalidate(cache_t* cache, const position_t* position, uint32_t length) {
    if (position->block != cache->position.block) {
        // Different block
        return;
    } else if (position->offset > cache->position.offset + cache->size) {
        // Beyond the end of what's cached
        return;
    } else if (position->offset + length <= cache->position.offset) {
        // Before the start of what's cached
        return;
    }
    cache->length = 0;
}
