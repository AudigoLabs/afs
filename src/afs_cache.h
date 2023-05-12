#pragma once

#include "afs_impl_types.h"

#include <inttypes.h>
#include <stdbool.h>

//! Checks if the cache contains the specified offset
bool afs_cache_contains(const afs_cache_t* cache, uint16_t block, uint32_t offset);

//! Reads available data from the cache
uint32_t afs_cache_read(const afs_cache_t* cache, uint16_t block, uint32_t offset, uint8_t* buf, uint32_t length);

//! Invalidates any portion of the cache with overlaps with the specified region
void afs_cache_invalidate(afs_cache_t* cache, uint16_t block, uint32_t offset, uint32_t length);
