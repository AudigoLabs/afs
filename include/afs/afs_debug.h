#pragma once

#include "afs/afs.h"

//! Dumps the contents of the file system
void afs_dump(afs_handle_t afs_handle);

//! Dumps the contents of a single block of the file system
void afs_dump_block(afs_handle_t afs_handle, uint16_t block);
