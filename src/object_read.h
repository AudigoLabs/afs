#pragma once

#include "impl_types.h"
#include "internal_types.h"

//! Reads the next available part of the object and returns whether or not there is more data remaining to read.
bool object_read_process(const afs_impl_t* afs, afs_obj_impl_t* obj, uint8_t* data, uint32_t max_length, uint32_t* read_bytes);
