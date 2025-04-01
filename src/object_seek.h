#pragma once

#include "impl_types.h"

//! Seeks to the block containing an offset (relative to the current position)
uint64_t object_seek_to_block(afs_impl_t* afs, afs_obj_impl_t* obj, uint64_t offset);

//! Seeks to the sub-block containing an offset (relative to the current position)
uint64_t object_seek_to_sub_block(afs_impl_t* afs, afs_obj_impl_t* obj, uint64_t offset);

//! Seeks to the last block
void object_seek_to_last_block(afs_impl_t* afs, afs_obj_impl_t* obj);

//! Gets the object size for an AFS v2 object
bool object_seek_get_v2_object_size(afs_impl_t* afs, uint16_t object_id, afs_stream_bitmask_t stream_bitmask, uint64_t* size);
