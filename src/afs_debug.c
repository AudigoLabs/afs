#include "afs/afs_debug.h"

#include "afs_util.h"

#include "afs_config.h"

#include <string.h>

static bool chunk_iter_callback(afs_impl_t* afs, uint32_t offset, const chunk_header_t* header) {
    const uint8_t type = CHUNK_TAG_GET_TYPE(header->tag);
    switch (type) {
        case CHUNK_TYPE_DATA_FIRST ... CHUNK_TYPE_DATA_LAST:
            AFS_LOG_INFO("  [0x%06"PRIx32"]={Data: stream=0x%x, length=%"PRIu32"}", offset, type & 0xf, CHUNK_TAG_GET_LENGTH(header->tag));
            break;
        case CHUNK_TYPE_END:
            AFS_LOG_INFO("  [0x%06"PRIx32"]={End}", offset);
            break;
        case CHUNK_TYPE_OFFSET:
            AFS_LOG_INFO("  [0x%06"PRIx32"]={Offset}", offset);
            break;
        case CHUNK_TYPE_INVALID_ZERO:
        case CHUNK_TYPE_INVALID_ONE:
        default:
            AFS_LOG_ERROR("Unexpected chunk type (0x%x)", type);
            return false;
    }
    return true;
}

void afs_dump(afs_handle_t afs_handle) {
    afs_impl_t* afs = get_afs_impl(afs_handle);
    AFS_ASSERT(afs);

    // Iterate over the blocks from the lookup table
    for (uint16_t block = 0; block < afs->num_blocks; block++) {
        afs_dump_block(afs_handle, block);
    }
}

void afs_dump_block(afs_handle_t afs_handle, uint16_t block) {
    afs_impl_t* afs = get_afs_impl(afs_handle);
    AFS_ASSERT(afs);
    const afs_lookup_table_entry_t lookup_value = afs->lookup_table[block];
    if (!lookup_value) {
        return;
    }
    uint16_t object_id = LOOKUP_TABLE_GET_OBJECT_ID(lookup_value);
    uint16_t object_block_index = LOOKUP_TABLE_GET_OBJECT_BLOCK_INDEX(lookup_value);
    AFS_LOG_INFO("[%3u]={object_id=%u, object_block_index=%u}", block, object_id, object_block_index);

    // Iterate over the chunks
    afs_chunk_iter_context_t chunk_iter;
    AFS_UTIL_FOREACH_CHUNK(afs, chunk_iter, block) {
        chunk_iter_callback(afs, chunk_iter.offset, &chunk_iter.header);
    }
}
