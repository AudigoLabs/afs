#pragma once

#include <inttypes.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

#define AFS_NUM_STREAMS             16
#define AFS_WILDCARD_STREAM         UINT8_MAX

//! Type used to represent a stream bitmask
typedef uint16_t afs_stream_bitmask_t;
_Static_assert(sizeof(afs_stream_bitmask_t) * 8 == AFS_NUM_STREAMS, "Invalid bitmask size");

//! Creates an AFS handle
#define AFS_HANDLE_DEF(NAME) \
    static afs_handle_def_t _##NAME##_def; \
    static const afs_handle_t NAME = &_##NAME##_def

//! Creates an AFS object handle
#define AFS_OBJECT_HANDLE_DEF(NAME) \
    static afs_object_handle_def_t _##NAME##_def; \
    static const afs_object_handle_t NAME = &_##NAME##_def

//! Type used to define an AFS object handle - should be created with AFS_OBJECT_HANDLE_DEF()
typedef struct __attribute__((aligned(sizeof(uintptr_t)))) {
    // Private memory used by AFS internally
    uint8_t priv[sizeof(uintptr_t) == 8 ? 88 : 56];
} afs_handle_def_t;

//! Type used to define an AFS object handle - should be created with AFS_OBJECT_HANDLE_DEF()
typedef struct __attribute__((aligned(sizeof(uintptr_t)))) {
    // Private memory used by AFS internally
    uint8_t priv[sizeof(uintptr_t) == 8 ? 192 : 184];
} afs_object_handle_def_t;

//! Type which represents an entry in the lookup table used for properly statically allocating space
typedef uint32_t afs_lookup_table_entry_t;

//! Function type for reading the underlying storage
typedef void (*afs_read_func_t)(uint8_t* buf, uint16_t block, uint32_t offset, uint32_t length);

//! Function type for writing to the underlying storage
typedef void (*afs_write_func_t)(const uint8_t* buf, uint16_t block, uint32_t offset, uint32_t length);

//! Function type for erasing the underlying storage
typedef void (*afs_erase_func_t)(uint16_t block);

//! AFS initialization type
typedef struct {
    // The size of a block (should match the AU size of the SD card - typically 4MB)
    uint32_t block_size;
    // The total number of blocks
    uint16_t num_blocks;
    // The minimum read/write size (should match the block size of the SD card - typically 512 bytes)
    uint32_t read_write_size;
    // A buffer of size read_write_size used internally by AFS
    uint8_t* read_write_buffer;
    // Buffer for the lookup table used internally (must have space for `num_blocks` entries)
    afs_lookup_table_entry_t* lookup_table;
    // Function used to read data from the underlying storage device
    afs_read_func_t read_func;
    // Function used to write data to the underlying storage device
    afs_write_func_t write_func;
    // Function used to erase a block on the underlying storage device
    afs_erase_func_t erase_func;
    // Optional callbacks used during mounting of the file system
    struct {
        // A handler to call as objects are found
        void (*object_found)(uint16_t object_id, uint8_t stream, const uint8_t* data, uint32_t data_length);
    } mount_callbacks;
} afs_init_t;

//! Configuration type used when creating or opening objects
typedef struct {
    // Memory buffer allocated for the object
    uint8_t* buffer;
    // Size of the memory buffer
    uint32_t buffer_size;
} afs_object_config_t;

//! Read position used by afs_object_save_read_position() and afs_object_restore_read_position()
typedef struct __attribute__((aligned(sizeof(uintptr_t)))) {
    // Private memory used by AFS internally
    uint8_t priv[sizeof(uintptr_t) == 8 ? 144 : 144];
} afs_read_position_t;

//! Iterator context used by afs_object_list()
typedef struct {
    // Private memory used by AFS internally
    uint8_t priv[4];
    // The current object ID
    uint16_t object_id;
} afs_object_list_entry_t;

//! Type used to represent an AFS instance
typedef afs_handle_def_t* afs_handle_t;

//! Type used to represent an AFS object
typedef afs_object_handle_def_t* afs_object_handle_t;

//! Initializes and mounts the file system
void afs_init(afs_handle_t afs_handle, const afs_init_t* init);

//! De-initializes the file system
void afs_deinit(afs_handle_t afs_handle);

//! Creates a new object for writing (returning the object ID)
uint16_t afs_object_create(afs_handle_t afs_handle, afs_object_handle_t object_handle, const afs_object_config_t* config);

//! Writes data to an object which was created with afs_object_create()
//! Returns false on error (i.e. if the storage is full - see afs_is_storage_full())
bool afs_object_write(afs_handle_t afs_handle, afs_object_handle_t object_handle, uint8_t stream, const uint8_t* data, uint32_t length);

//! Opens an existing object for reading (returns false if the object doesn't exist)
bool afs_object_open(afs_handle_t afs_handle, afs_object_handle_t object_handle, uint8_t stream, uint16_t object_id, const afs_object_config_t* config);

//! Reads data from the selected stream within an object which was opened with afs_object_open() and returns the number of bytes read
uint32_t afs_object_read(afs_handle_t afs_handle, afs_object_handle_t object_handle, uint8_t* data, uint32_t max_length, uint8_t* stream);

//! Seeks the requested amount further into the object stream
bool afs_object_seek(afs_handle_t afs_handle, afs_object_handle_t object_handle, uint64_t offset);

//! Gets the total size of the object stream
uint64_t afs_object_size(afs_handle_t afs_handle, afs_object_handle_t object_handle, afs_stream_bitmask_t stream_bitmask);

//! Saves the current read position
void afs_object_save_read_position(afs_handle_t afs_handle, afs_object_handle_t object_handle, afs_read_position_t* read_position);

//! Restores a previously-saved read position
void afs_object_restore_read_position(afs_handle_t afs_handle, afs_object_handle_t object_handle, afs_read_position_t* read_position);

//! Closes an object handle which was created with afs_object_create() or afs_object_open()
//! Returns false if the object was open for writing and writing the end chunk failed due to insufficient space
bool afs_object_close(afs_handle_t afs_handle, afs_object_handle_t object_handle);

//! Lists all objects in the file system (returns false if there are no more)
//! NOTE: The context should be initially cleared and then continually passed to retrieve the next object
bool afs_object_list(afs_handle_t afs_handle, afs_object_list_entry_t* entry);

//! Gets the number of blocks used by an object (will be larger than the actual object data size)
uint16_t afs_object_get_num_blocks(afs_handle_t afs_handle, uint16_t object_id);

//! Deletes an object from the file system
void afs_object_delete(afs_handle_t afs_handle, uint16_t object_id);

//! Deletes all objects from the file system
void afs_wipe(afs_handle_t afs_handle, bool secure);

//! Gets the total size of the file system as a total number of blocks being used
uint16_t afs_size(afs_handle_t afs_handle);

//! Returns whether or not the store is full (which causes writes to fail)
bool afs_is_storage_full(afs_handle_t afs_handle);

//! Prepares the backing storage for writing to the specified number of blocks.
void afs_prepare_storage(afs_handle_t afs_handle, uint16_t num_blocks);

#ifdef __cplusplus
};
#endif
