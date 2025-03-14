#pragma once

#include "impl_types.h"
#include "internal_types.h"

//! Writes object data
uint32_t object_write_process(afs_impl_t* afs, afs_obj_impl_t* obj, uint8_t stream, const void* data, uint32_t length);

//! Finishes writing an object
bool object_write_finish(afs_impl_t* afs, afs_obj_impl_t* obj);
