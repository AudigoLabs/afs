#pragma once

#include "impl_types.h"

//! Adds an open object to the list
void open_object_list_add(afs_impl_t* afs, afs_obj_impl_t* obj);

//! Removes an open object from the list
void open_object_list_remove(afs_impl_t* afs, afs_obj_impl_t* obj);

//! Check if the open list contains an object
bool open_object_list_contains(const afs_impl_t* afs, uint16_t object_id);

//! Check is the open list is empty
bool open_object_list_is_empty(const afs_impl_t* afs);

//! Gets the next object ID which is open for writing with no data on storage yet
uint16_t open_object_list_get_writing_no_storage(afs_impl_t* afs, uint16_t prev_index);
