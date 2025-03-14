#pragma once

#include "impl_types.h"
#include "internal_types.h"

#include <inttypes.h>
#include <stdbool.h>

//! Checks if the cache contains the specified offset
bool cache_contains(const cache_t* cache, const position_t* position);

//! Reads available data from the cache
uint32_t cache_read(const cache_t* cache, const position_t* position, void* buf, uint32_t length);

//! Writes data into the cache
void cache_write(cache_t* cache, const void* data, uint32_t length);

//! Invalidates any portion of the cache with overlaps with the specified region
void cache_invalidate(cache_t* cache, const position_t* position, uint32_t length);
