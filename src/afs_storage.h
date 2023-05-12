#pragma once

#include "afs_impl_types.h"

#include <inttypes.h>
#include <stdbool.h>

//! Reads data from storage, leveraging the specified cache (offset has no alignment requirements)
uint32_t afs_storage_read(const afs_impl_t* afs, afs_cache_t* cache, uint16_t block, uint32_t offset, uint8_t* buf, uint32_t length);

//! Writes cached data out to storage
void afs_storage_write_cache(afs_impl_t* afs, afs_cache_t* cache, bool pad);

//! Erases a block of storage
void afs_storage_erase(afs_impl_t* afs, uint16_t block);
