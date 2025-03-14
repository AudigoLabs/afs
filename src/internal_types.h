#pragma once

#include "afs/afs.h"

#pragma pack(push, 1)

//! Type used to represent a position within the file system
typedef struct {
    // The block index
    uint16_t block;
    // The offset within the block
    uint32_t offset;
} position_t;

//! Type used to represent the data of an offset chunk
typedef struct {
    // The offsets of each stream as of the start of a block
    uint64_t offsets[AFS_NUM_STREAMS];
} offset_chunk_data_t;

//! Type used to represent the data of a seek chunk
typedef struct {
    // The offsets of each stream as of the current position within the block
    uint32_t offsets[AFS_NUM_STREAMS];
} seek_chunk_data_t;

#pragma pack(pop)
