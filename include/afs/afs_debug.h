#pragma once

#include "afs/afs.h"

//! Dumps the contents of the file system
void afs_dump(afs_handle_t afs_handle);

//! Dumps the contents of a single block of the file system
void afs_dump_block(afs_handle_t afs_handle, uint16_t block, uint32_t max_chunks);

//! Dumps the blocks used by an object
void afs_dump_object(afs_handle_t afs_handle, uint16_t object_id);
