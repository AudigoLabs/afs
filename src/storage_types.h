#pragma once

#include <inttypes.h>

#define CHUNK_TYPE_DATA_FIRST           0xd0
#define CHUNK_TYPE_DATA_LAST            0xdf
#define CHUNK_TYPE_END                  0xed
#define CHUNK_TYPE_OFFSET               0x3e
#define CHUNK_TYPE_SEEK                 0x5e
#define CHUNK_TYPE_INVALID_ZERO         0x00
#define CHUNK_TYPE_INVALID_ONE          0xff

#define CHUNK_MAX_LENGTH                0xffffff

#define CHUNK_TAG_GET_TYPE(TAG) ((uint8_t)((TAG) >> 24))
#define CHUNK_TAG_GET_LENGTH(TAG) ((TAG) & 0xffffff)
#define CHUNK_TAG_VALUE(TYPE, LENGTH) ((uint32_t)((TYPE) & 0xff) << 24 | ((LENGTH) & 0xffffff))

#define OFFSET_DATA_GET_STREAM(DATA) ((uint8_t)((DATA) >> 60))
#define OFFSET_DATA_GET_OFFSET(DATA) ((DATA) & 0x0fffffffffffffff)
#define OFFSET_DATA_VALUE(STREAM, OFFSET) (((uint64_t)(STREAM) << 60) | ((OFFSET) & 0x0fffffffffffffff))

#define SEEK_OFFSET_DATA_GET_STREAM(DATA) ((uint8_t)((DATA) >> 28))
#define SEEK_OFFSET_DATA_GET_OFFSET(DATA) ((DATA) & 0x0fffffff)
#define SEEK_OFFSET_DATA_VALUE(STREAM, OFFSET) (((uint32_t)(STREAM) << 28) | ((OFFSET) & 0x0fffffff))

#define BLOCK_FOOTER_LENGTH             128

#define INVALID_OBJECT_ID               0

typedef union {
    char str[4];
    uint32_t val;
} magic_value_t;
_Static_assert(sizeof(magic_value_t) == 4, "Invalid magic value size");

// Block magic values
static const magic_value_t HEADER_MAGIC_VALUE_V1 = {.str = {'A', 'F', 'S', '1'}};
static const magic_value_t HEADER_MAGIC_VALUE_V2 = {.str = {'A', 'F', 'S', '2'}};
static const magic_value_t FOOTER_MAGIC_VALUE = {.str = {'a', 'f', 's', '2'}};

#pragma pack(push, 1)

// On-disk block header type
typedef struct {
    // Magic value
    magic_value_t magic;
    // The object ID which is stored in this block
    uint16_t object_id;
    // The block index of the object stored in this block
    uint16_t object_block_index;
} block_header_t;

// On-disk block footer type
typedef struct {
    // Magic value
    magic_value_t magic;
} block_footer_t;

// On-disk chunk header type
typedef struct {
    // The upper 8 bits are the type and the lower 24 are the length of data which follows the header
    uint32_t tag;
} chunk_header_t;

#pragma pack(pop)
