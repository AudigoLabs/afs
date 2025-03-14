#pragma once

#include "afs/afs.h"
#include "internal_types.h"

#include <inttypes.h>
#include <stdbool.h>

#define INVALID_BLOCK                           UINT16_MAX

typedef struct {
    // Pointer to the underlying buffer
    uint8_t* buffer;
    // Size of the buffer
    uint32_t size;
    // Length of data currently in the buffer
    uint32_t length;
    // The position that the cache is associated with
    position_t position;
} cache_t;

typedef struct {
    // The storage config
    const afs_storage_config_t* config;
    // The cache object
    cache_t cache;
} storage_t;

typedef struct {
    // The number of blocks in the storage
    uint16_t num_blocks;
    // Lookup table values
    uint32_t* values;
    // Version bitmap
    uint8_t* version_bitmap;
    // Seed used to generate object IDs
    uint32_t object_id_seed;
} lookup_table_t;

typedef enum {
    OBJ_STATE_INVALID = 0,
    OBJ_STATE_READING,
    OBJ_STATE_WRITING,
} obj_state_t;

// In-memory object context
typedef struct afs_obj_impl {
    // Next open object in the linked list
    struct afs_obj_impl* next_open_object;
    // State
    obj_state_t state;
    // Object ID
    uint16_t object_id;
    // The current offset within the object of each stream
    uint64_t object_offset[AFS_NUM_STREAMS];
    // The current offset within the block of each stream
    uint32_t block_offset[AFS_NUM_STREAMS];
    struct {
        // The current read offset for the underlying storage
        uint64_t storage_offset;
        // The remaining bytes in the current chunk we're reading
        uint32_t data_chunk_length;
        // The stream the object was opened to read
        uint8_t stream;
        // The current stream being read (for wildcard streams)
        uint8_t current_stream;
    } read;
    struct {
        // The index of the next block within the object
        uint16_t next_block_index;
    } write;
    // The storage context for the object
    storage_t storage;
} afs_obj_impl_t;

// In-memory context for an AFS instance
typedef struct {
    // Whether or not this instance is in use
    bool in_use;
    // The storage config
    afs_storage_config_t storage_config;
    // The lookup table
    lookup_table_t lookup_table;
    // Open object linked list
    afs_obj_impl_t* open_object_list_head;
    // The storage context for file system operations
    storage_t storage;
} afs_impl_t;

// In-memory context for the read position
typedef struct {
    // The current offset within the object of all streams
    uint64_t object_offset[AFS_NUM_STREAMS];
    // The current offset within the block of all streams
    uint32_t block_offset[AFS_NUM_STREAMS];
    // The current read offset for the underlying storage
    uint64_t storage_offset;
    // The remaining bytes in the current chunk we're reading
    uint32_t data_chunk_length;
    // The current stream being read
    uint8_t current_stream;
} afs_read_pos_impl_t;

// In-memory context for listing objects
typedef struct {
    // The current block
    uint16_t block;
    // The index into the open objects list
    uint16_t open_index;
} afs_object_list_entry_impl_t;
