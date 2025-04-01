#include "util.h"

#include "afs_config.h"
#include "lookup_table.h"
#include "storage.h"

#include <string.h>

bool util_is_block_header_valid(block_header_t* header, bool* is_v2) {
    if (header->magic.val == HEADER_MAGIC_VALUE_V1.val) {
        if (is_v2) {
            *is_v2 = false;
        }
        return true;
    } else if (header->magic.val == HEADER_MAGIC_VALUE_V2.val) {
        if (is_v2) {
            *is_v2 = true;
        }
        return true;
    } else {
        return false;
    }
}

uint64_t util_get_stream_offset(const uint64_t* stream_offsets, uint8_t stream) {
    if (stream == AFS_WILDCARD_STREAM) {
        uint64_t offset = 0;
        for (uint8_t i = 0; i < AFS_NUM_STREAMS; i++) {
            offset += stream_offsets[i];
        }
        return offset;
    } else {
        return stream_offsets[stream];
    }
}

uint32_t util_get_block_offset(const uint32_t* block_offsets, uint8_t stream) {
    if (stream == AFS_WILDCARD_STREAM) {
        uint64_t offset = 0;
        for (uint8_t i = 0; i < AFS_NUM_STREAMS; i++) {
            offset += block_offsets[i];
        }
        return offset;
    } else {
        return block_offsets[stream];
    }
}
