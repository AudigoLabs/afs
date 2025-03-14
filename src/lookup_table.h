#pragma once

#include "impl_types.h"

//! Populates the lookup table by reading through the underlying storage
void lookup_table_populate(afs_impl_t* afs, afs_object_found_callback_t object_found_callback);

//! Gets the block for a given object_id and object_block_index
uint32_t lookup_table_get_block(const lookup_table_t* lookup_table, uint16_t object_id, uint16_t object_block_index);

//! Gets the number of blocks for a given object_id
uint16_t lookup_table_get_num_blocks(const lookup_table_t* lookup_table, uint16_t object_id);

//! Gets the last block for a given object_id
uint16_t lookup_table_get_last_block(const lookup_table_t* lookup_table, uint16_t object_id);

//! Gets whether a block is v2 or not
bool lookup_table_get_is_v2(const lookup_table_t* lookup_table, uint16_t block);

//! Gets an unused, psuedo-random object ID
uint16_t lookup_table_get_next_object_id(lookup_table_t* lookup_table);

//! Gets the next object in the lookup table (useful for iterating through all objects)
uint16_t lookup_table_iter_get_next_object(const lookup_table_t* lookup_table, uint16_t *block);

//! Deletes an object from the lookup table and returns the first block
uint16_t lookup_table_delete_object(lookup_table_t* lookup_table, uint16_t object_id);

//! Gets the total number of blocks being used
uint16_t lookup_table_get_total_num_blocks(const lookup_table_t* lookup_table);

//! Checks if all blocks are in use
bool lookup_table_is_full(const lookup_table_t* lookup_table);

//! Gets the next free block and assigns it to the specified object.
uint16_t lookup_table_acquire_block(lookup_table_t* lookup_table, uint16_t object_id, uint16_t object_block_index, bool* is_erased);

//! Gets the next block which is in use and marks it to be wiped
uint16_t lookup_table_wipe_next_in_use(lookup_table_t* lookup_table, uint16_t start_block, bool* should_erase);

//! Gets the number of erased blocks
uint16_t lookup_table_get_num_erased(const lookup_table_t* lookup_table);

//! Gets the next block which is pending being erased and marks it as erased
uint16_t lookup_table_get_next_pending_erase(lookup_table_t* lookup_table, uint16_t start_block);

//! Dumps the lookup table entry for a given block for debugging
bool lookup_table_debug_dump_block(const lookup_table_t* lookup_table, uint16_t block);

//! Dumps the lookup table entries for an object for debugging
void lookup_table_debug_dump_object(const lookup_table_t* lookup_table, uint16_t object_id);
