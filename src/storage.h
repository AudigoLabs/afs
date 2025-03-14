#pragma once

#include "impl_types.h"
#include "internal_types.h"
#include "storage_types.h"

#include <inttypes.h>
#include <stdbool.h>

//! Reads data from storage
void storage_read_data(storage_t* storage, position_t* position, void* buf, uint32_t length);

//! Reads a block header from storage
static inline void storage_read_block_header(storage_t* storage, position_t* position, block_header_t* header) {
    storage_read_data(storage, position, header, sizeof(*header));
}

//! Reads a chunk header from storage
static inline void storage_read_chunk_header(storage_t* storage, position_t* position, chunk_header_t* header) {
    storage_read_data(storage, position, header, sizeof(*header));
}

//! Reads the block footer from storage and returns the offset chunk data
bool storage_read_block_header_offset_data(storage_t* storage, uint16_t block, offset_chunk_data_t* data);

//! Reads the block footer from storage and returns the seek chunk data
bool storage_read_block_footer_seek_data(storage_t* storage, uint16_t block, seek_chunk_data_t* data);

//! Reads the seek chunk data from storage from the start of a sub-block
bool storage_read_seek_data(storage_t* storage, uint16_t block, uint32_t sub_block_index, seek_chunk_data_t* data);

//! Writes cached data out to storage
void storage_write_cache(storage_t* storage, bool pad);

//! Erases a block of storage
void storage_erase(storage_t* storage, uint16_t block);
