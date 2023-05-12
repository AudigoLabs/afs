#include "afs_cache.h"

#include "afs_util.h"

#include <string.h>

bool afs_cache_contains(const afs_cache_t* cache, uint16_t block, uint32_t offset) {
    return cache->length && cache->block == block && cache->offset == ALIGN_DOWN(offset, cache->size);
}

uint32_t afs_cache_read(const afs_cache_t* cache, uint16_t block, uint32_t offset, uint8_t* buf, uint32_t length) {
    const uint32_t aligned_offset = ALIGN_DOWN(offset, cache->size);
    if (!cache->length || cache->block != block || cache->offset != aligned_offset) {
        return 0;
    }
    const uint32_t buffer_read_index = offset - aligned_offset;
    length = MIN_VAL(length, cache->length - buffer_read_index);
    memcpy(buf, &cache->buffer[buffer_read_index], length);
    return length;
}

void afs_cache_invalidate(afs_cache_t* cache, uint16_t block, uint32_t offset, uint32_t length) {
    if (block != cache->block) {
        // Different block
        return;
    } else if (offset > cache->offset + cache->size) {
        // Beyond the end of what's cached
        return;
    } else if (offset + length <= cache->offset) {
        // Before the start of what's cached
        return;
    }
    cache->length = 0;
}
