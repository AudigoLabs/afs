#pragma once

#include "afs/afs.h"

#include <inttypes.h>
#include <stdbool.h>

#define INVALID_BLOCK                           UINT16_MAX

#define LOOKUP_TABLE_BLOCK_STATE_ERASED         0x0000
#define LOOKUP_TABLE_BLOCK_STATE_MAYBE_ERASED   0x0001
#define LOOKUP_TABLE_BLOCK_STATE_UNKNOWN        0x0002
#define LOOKUP_TABLE_BLOCK_STATE_GARBAGE        0x0003

#define LOOKUP_TABLE_GET_OBJECT_ID(X) ((uint16_t)((X) >> 16))
#define LOOKUP_TABLE_GET_OBJECT_BLOCK_INDEX(X) ((uint16_t)(X))
#define LOOKUP_TABLE_GET_BLOCK_STATE(X) ((uint16_t)(X))
#define LOOKUP_TABLE_VALUE(OBJECT_ID, OBJECT_BLOCK_INDEX) \
    ((((uint32_t)(OBJECT_ID) & 0xffff) << 16) | ((OBJECT_BLOCK_INDEX) & 0xffff))
#define LOOKUP_TABLE_FREE_BLOCK_VALUE(STATE) \
    ((((uint32_t)(INVALID_OBJECT_ID) & 0xffff) << 16) | ((STATE) & 0xffff))

#define FOREACH_OPEN_OBJECT(AFS, VAR) \
    for (afs_obj_impl_t* VAR = (AFS)->open_object_list_head; VAR; VAR = (VAR)->next_open_object)

typedef struct {
    // Pointer to the underlying buffer
    uint8_t* buffer;
    // Size of the buffer
    uint32_t size;
    // Length of data currently in the buffer
    uint32_t length;
    // The block the cache is associated with
    uint16_t block;
    // The offset within the block the cache is associated with
    uint32_t offset;
} afs_cache_t;

// In-memory object context
typedef struct afs_obj_impl {
    // Next open object in the linked list
    struct afs_obj_impl* next_open_object;
    // Context state
    enum {
        STATE_INVALID = 0,
        STATE_READING,
        STATE_WRITING,
    } state;
    // Object ID
    uint16_t object_id;
    // Per-object cache
    afs_cache_t cache;
    // The current offset within the object of each stream
    uint64_t offset[AFS_NUM_STREAMS];
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
} afs_obj_impl_t;

// In-memory context for an AFS instance
typedef struct {
    // Whether or not this instance is in use
    bool in_use;
    // Low-level storage functions
    struct {
        afs_read_func_t read;
        afs_write_func_t write;
        afs_erase_func_t erase;
    } storage_func;
    // Configured block size
    uint32_t block_size;
    // Configured number of blocks
    uint16_t num_blocks;
    // Seed used to generate object IDs
    uint32_t object_id_seed;
    // Cache for general file system operations
    afs_cache_t cache;
    // Lookup table
    afs_lookup_table_entry_t* lookup_table;
    // Open object linked list
    afs_obj_impl_t* open_object_list_head;
} afs_impl_t;

// In-memory context for the read position
typedef struct {
    // The current offset within the object of all streams
    uint64_t offset[AFS_NUM_STREAMS];
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

//! Translates an afs_handle_t to an afs_impl_t*
static inline afs_impl_t* get_afs_impl(afs_handle_t handle) {
    return (afs_impl_t*)handle->priv;
}

//! Translates an afs_object_handle_t to an afs_obj_impl_t*
static inline afs_obj_impl_t* get_obj_impl(afs_object_handle_t handle) {
    return (afs_obj_impl_t*)handle->priv;
}

//! Translates an afs_read_position_t* to an afs_read_pos_impl_t*
static inline afs_read_pos_impl_t* get_read_pos_impl(afs_read_position_t* read_position) {
    return (afs_read_pos_impl_t*)read_position->priv;
}

//! Translates an afs_object_list_entry_t* to an afs_object_list_entry_impl_t*
static inline afs_object_list_entry_impl_t* get_object_list_entry_impl(afs_object_list_entry_t* entry) {
    return (afs_object_list_entry_impl_t*)entry->priv;
}
