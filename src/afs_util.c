#include "afs_util.h"

#include "afs_storage.h"

#include "afs_config.h"

#include <string.h>

bool afs_util_is_block_header_valid(block_header_t* header) {
    return !memcmp(header->magic, MAGIC_VALUE, sizeof(header->magic));
}

bool afs_util_block_iter_next(afs_impl_t* afs, afs_block_iter_context_t* context) {
    if (++context->block >= afs->num_blocks) {
        // Done iterating
        return false;
    }
    AFS_ASSERT_EQ(afs_storage_read(afs, &afs->cache, context->block, 0, (uint8_t*)&context->header, sizeof(context->header)), sizeof(context->header));
    return true;
}

bool afs_util_chunk_iter_next(afs_impl_t* afs, afs_chunk_iter_context_t* context) {
    if (context->offset == 0) {
        context->offset += sizeof(block_header_t);
    } else {
        context->offset += sizeof(chunk_header_t) + CHUNK_TAG_GET_LENGTH(context->header.tag);
    }
    if (context->offset + sizeof(chunk_header_t) + 1 >= afs->block_size) {
        return false;
    }

    uint8_t* header_bytes = (uint8_t*)&context->header;
    uint32_t bytes_read = afs_storage_read(afs, &afs->cache, context->block, context->offset, header_bytes, sizeof(context->header));
    if (bytes_read < sizeof(context->header)) {
        // We only read a partial chunk header, so read the rest from the next offset
        header_bytes += bytes_read;
        bytes_read += afs_storage_read(afs, &afs->cache, context->block, context->offset + bytes_read, header_bytes, sizeof(context->header) - bytes_read);
        AFS_ASSERT_EQ(bytes_read, sizeof(context->header));
    }
    const uint8_t type = CHUNK_TAG_GET_TYPE(context->header.tag);
    switch (type) {
        case CHUNK_TYPE_DATA_FIRST ... CHUNK_TYPE_DATA_LAST:
        case CHUNK_TYPE_END:
        case CHUNK_TYPE_OFFSET:
            return true;
        case CHUNK_TYPE_INVALID_ZERO:
        case CHUNK_TYPE_INVALID_ONE:
            return false;
        default:
            AFS_LOG_ERROR("Unexpected chunk type (0x%x)", type);
            return false;
    }
}

uint32_t afs_util_get_object_data_from_cache(afs_cache_t* cache, uint8_t* stream) {
    AFS_ASSERT_EQ(cache->length, cache->size);
    AFS_ASSERT_EQ(cache->offset, 0);

    uint32_t read_offset = sizeof(block_header_t);
    *stream = AFS_WILDCARD_STREAM;
    uint32_t data_length = 0;
    while (true) {
        // Read the chunk header
        chunk_header_t header;
        if (cache->length - read_offset < sizeof(header)) {
            break;
        }
        memcpy(&header, &cache->buffer[read_offset], sizeof(header));
        read_offset += sizeof(header);

        // Check if this is a data chunk
        const uint8_t chunk_type = CHUNK_TAG_GET_TYPE(header.tag);
        const uint8_t chunk_stream = chunk_type & 0xf;
        if (chunk_type < CHUNK_TYPE_DATA_FIRST || chunk_type > CHUNK_TYPE_DATA_LAST) {
            break;
        }
        // Check its of the same stream we previously read if it's not the first one
        *stream = *stream == AFS_WILDCARD_STREAM ? chunk_stream : *stream;
        if (chunk_stream != *stream) {
            break;
        }
        // Read and shift the data down within the cache in order to reuse its buffer
        const uint32_t chunk_length = MIN_VAL(CHUNK_TAG_GET_LENGTH(header.tag), cache->length - read_offset);
        memmove(&cache->buffer[data_length], &cache->buffer[read_offset], chunk_length);
        read_offset += chunk_length;
        data_length += chunk_length;
    }

    // Wipe the cache since we reused its buffer
    cache->length = 0;
    return data_length;
}
