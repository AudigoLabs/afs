#pragma once

#include <inttypes.h>

#define CHUNK_TYPE_DATA_FIRST           0xd0
#define CHUNK_TYPE_DATA_LAST            0xdf
#define CHUNK_TYPE_END                  0xed
#define CHUNK_TYPE_OFFSET               0x3e
#define CHUNK_TYPE_INVALID_ZERO         0x00
#define CHUNK_TYPE_INVALID_ONE          0xff

#define CHUNK_MAX_LENGTH                0xffffff

#define CHUNK_TAG_GET_TYPE(TAG) ((uint8_t)((TAG) >> 24))
#define CHUNK_TAG_GET_LENGTH(TAG) ((TAG) & 0xffffff)
#define CHUNK_TAG_VALUE(TYPE, LENGTH) ((uint32_t)((TYPE) & 0xff) << 24 | ((LENGTH) & 0xffffff))

#define INVALID_OBJECT_ID               0

// Block header magic value
static const char MAGIC_VALUE[] = {'A', 'F', 'S', '1'};

#pragma pack(push, 1)

// On-disk block header type
typedef struct {
    // Magic value (AFS1)
    char magic[4];
    // The object ID which is stored in this block
    uint16_t object_id;
    // The block index of the object stored in this block
    uint16_t object_block_index;
} block_header_t;

// On-disk chunk header type
typedef struct {
    // The upper 8 bits are the type and the lower 24 are the length of data which follows the header
    uint32_t tag;
} chunk_header_t;

#pragma pack(pop)
