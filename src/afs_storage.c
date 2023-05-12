#include "afs_storage.h"

#include "afs_cache.h"
#include "afs_util.h"

#include "afs_config.h"

#include <string.h>

static void cache_populate(const afs_impl_t* afs, afs_cache_t* cache, uint16_t block, uint32_t offset) {
    cache->block = block;
    cache->offset = ALIGN_DOWN(offset, cache->size);
    afs->storage_func.read(cache->buffer, cache->block, cache->offset, cache->size);
    cache->length = cache->size;
}

uint32_t afs_storage_read(const afs_impl_t* afs, afs_cache_t* cache, uint16_t block, uint32_t offset, uint8_t* buf, uint32_t length) {
    if (!afs_cache_contains(cache, block, offset)) {
        cache_populate(afs, cache, block, offset);
    }
    return afs_cache_read(cache, block, offset, buf, length);
}

void afs_storage_write_cache(afs_impl_t* afs, afs_cache_t* cache, bool pad) {
    // Pad what we're writing up to the size of the file system cache (minimum read/write size)
    const uint32_t aligned_length = ALIGN_UP(cache->length, afs->cache.size);
    if (aligned_length > cache->length) {
        AFS_ASSERT(pad);
        AFS_ASSERT(aligned_length <= cache->size);
        memset(&cache->buffer[cache->length], 0xff, aligned_length - cache->length);
    }
    AFS_ASSERT(cache->offset + aligned_length <= afs->block_size);

    // Write the data
    afs->storage_func.write(cache->buffer, cache->block, cache->offset, aligned_length);

    // Invalidate the file system cache
    afs_cache_invalidate(&afs->cache, cache->block, cache->offset, aligned_length);

    // Advance the cache forward
    cache->offset += aligned_length;
    cache->length = 0;
    AFS_ASSERT(cache->offset <= afs->block_size);
    if (cache->offset == afs->block_size) {
        // No more space in the current block or we added padding, so advance to the next once
        cache->block = INVALID_BLOCK;
        cache->offset = 0;
    }
}

void afs_storage_erase(afs_impl_t* afs, uint16_t block) {
    afs->storage_func.erase(block);
    afs_cache_invalidate(&afs->cache, block, 0, afs->block_size);
}
