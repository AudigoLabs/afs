#include "afs/afs_debug.h"

#include "afs_config.h"
#include "lookup_table.h"
#include "storage.h"
#include "util.h"

#include <stdio.h>
#include <string.h>

#define DATA_BUFFER_SIZE 128

#define FOREACH_CHUNK(AFS, VAR, BLOCK) \
    for (VAR = (chunk_iter_context_t) {.block = block}; chunk_iter_next(AFS, &(VAR));)

typedef struct {
    uint16_t block;
    chunk_header_t header;
    uint32_t offset;
    uint8_t data[32];
} chunk_iter_context_t;

static inline afs_impl_t* get_afs_impl(afs_handle_t handle) {
    AFS_ASSERT(handle);
    afs_impl_t* afs = (afs_impl_t*)handle->priv;;
    AFS_ASSERT(afs && afs->in_use);
    return afs;
}

static bool chunk_iter_next(afs_impl_t* afs, chunk_iter_context_t* context) {
    if (context->offset == 0) {
        context->offset += sizeof(block_header_t);
    } else {
        context->offset += sizeof(chunk_header_t) + CHUNK_TAG_GET_LENGTH(context->header.tag);
    }
    if (context->offset + sizeof(chunk_header_t) + 1 >= afs->storage_config.block_size) {
        return false;
    }

    position_t position = {
        .block = context->block,
        .offset = context->offset,
    };
    storage_read_chunk_header(&afs->storage, &position, &context->header);
    const uint8_t type = CHUNK_TAG_GET_TYPE(context->header.tag);
    const uint32_t data_length = CHUNK_TAG_GET_LENGTH(context->header.tag);
    switch (type) {
        case CHUNK_TYPE_DATA_FIRST ... CHUNK_TYPE_DATA_LAST:
        case CHUNK_TYPE_OFFSET:
        case CHUNK_TYPE_SEEK:
            storage_read_data(&afs->storage, &position, context->data, MIN_VAL(data_length, sizeof(context->data)));
            return true;
        case CHUNK_TYPE_END:
            return true;
        case CHUNK_TYPE_INVALID_ZERO:
        case CHUNK_TYPE_INVALID_ONE:
            return false;
        default:
            AFS_LOG_ERROR("Unexpected chunk type (0x%x)", type);
            return false;
    }
}

static void populate_data_chunk_data_string(char* str, const chunk_iter_context_t* chunk_iter) {
    memset(str, 0, DATA_BUFFER_SIZE);
    const uint32_t data_length = MIN_VAL(CHUNK_TAG_GET_LENGTH(chunk_iter->header.tag), sizeof(chunk_iter->data));
    int offset = 0;
    for (uint32_t i = 0; i < data_length; i++) {
        const int res = snprintf(&str[offset], DATA_BUFFER_SIZE - offset, "%02x", chunk_iter->data[i]);
        AFS_ASSERT(res > 0 && res < DATA_BUFFER_SIZE - offset);
        offset += res;
    }
}

static void populate_offset_chunk_data_string(char* str, const chunk_iter_context_t* chunk_iter) {
    memset(str, 0, DATA_BUFFER_SIZE);
    const uint32_t data_length = CHUNK_TAG_GET_LENGTH(chunk_iter->header.tag);
    const uint32_t num_offsets = MIN_VAL(data_length, sizeof(chunk_iter->data)) / sizeof(uint64_t);
    int offset = 0;
    if (data_length % sizeof(uint64_t)) {
        const int res = snprintf(&str[offset], DATA_BUFFER_SIZE - offset, "<invalid length (%"PRIu32")>", data_length);
        AFS_ASSERT(res > 0 && res < DATA_BUFFER_SIZE - offset);
        offset += res;
        return;
    }
    // Write out the offset pairs
    const uint64_t* offsets = (const uint64_t*)chunk_iter->data;
    for (uint32_t i = 0; i < num_offsets; i++) {
        const uint64_t offset_value = OFFSET_DATA_GET_OFFSET(offsets[i]);
        const int res = snprintf(&str[offset], DATA_BUFFER_SIZE - offset, "{0x%x,0x%02"PRIx32"%08"PRIx32"}", OFFSET_DATA_GET_STREAM(offsets[i]), (uint32_t)(offset_value >> 32), (uint32_t)(offset_value & 0xffffffff));
        AFS_ASSERT(res > 0 && res < DATA_BUFFER_SIZE - offset);
        offset += res;
        if (i < num_offsets - 1) {
            AFS_ASSERT(offset < DATA_BUFFER_SIZE);
            strcpy(&str[offset], ",");
            offset++;
        }
    }
}

static void populate_seek_chunk_data_string(char* str, const chunk_iter_context_t* chunk_iter) {
    memset(str, 0, DATA_BUFFER_SIZE);
    const uint32_t data_length = CHUNK_TAG_GET_LENGTH(chunk_iter->header.tag);
    const uint32_t num_offsets = MIN_VAL(data_length, sizeof(chunk_iter->data)) / sizeof(uint32_t);
    int offset = 0;
    if (data_length % sizeof(uint32_t)) {
        const int res = snprintf(&str[offset], DATA_BUFFER_SIZE - offset, "<invalid length (%"PRIu32")>", data_length);
        AFS_ASSERT(res > 0 && res < DATA_BUFFER_SIZE - offset);
        offset += res;
        return;
    }
    // Write out the offset pairs
    const uint32_t* offsets = (const uint32_t*)chunk_iter->data;
    for (uint32_t i = 0; i < num_offsets; i++) {
        const uint32_t offset_value = SEEK_OFFSET_DATA_GET_OFFSET(offsets[i]);
        const int res = snprintf(&str[offset], DATA_BUFFER_SIZE - offset, "{0x%x,0x%08"PRIx32"}", SEEK_OFFSET_DATA_GET_STREAM(offsets[i]), offset_value);
        AFS_ASSERT(res > 0 && res < DATA_BUFFER_SIZE - offset);
        offset += res;
        if (i < num_offsets - 1) {
            AFS_ASSERT(offset < DATA_BUFFER_SIZE);
            strcpy(&str[offset], ",");
            offset++;
        }
    }
}

static bool chunk_iter_callback(afs_impl_t* afs, const chunk_iter_context_t* chunk_iter) {
    char data_str[DATA_BUFFER_SIZE];
    const uint8_t type = CHUNK_TAG_GET_TYPE(chunk_iter->header.tag);
    switch (type) {
        case CHUNK_TYPE_DATA_FIRST ... CHUNK_TYPE_DATA_LAST:
            populate_data_chunk_data_string(data_str, chunk_iter);
            AFS_LOG_INFO("  [0x%06"PRIx32"]=Data(stream=0x%x, length=%"PRIu32", data=%s)", chunk_iter->offset, type & 0xf, CHUNK_TAG_GET_LENGTH(chunk_iter->header.tag), data_str);
            break;
        case CHUNK_TYPE_END:
            AFS_LOG_INFO("  [0x%06"PRIx32"]=End()", chunk_iter->offset);
            break;
        case CHUNK_TYPE_OFFSET:
            populate_offset_chunk_data_string(data_str, chunk_iter);
            AFS_LOG_INFO("  [0x%06"PRIx32"]=Offset(num=%u, data=%s)", chunk_iter->offset, (uint8_t)(CHUNK_TAG_GET_LENGTH(chunk_iter->header.tag) / sizeof(uint64_t)), data_str);
            break;
        case CHUNK_TYPE_SEEK:
            populate_seek_chunk_data_string(data_str, chunk_iter);
            AFS_LOG_INFO("  [0x%06"PRIx32"]=Seek(num=%u, data=%s)", chunk_iter->offset, (uint8_t)(CHUNK_TAG_GET_LENGTH(chunk_iter->header.tag) / sizeof(uint32_t)), data_str);
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
    // Iterate over the blocks from the lookup table
    for (uint16_t block = 0; block < afs->storage_config.num_blocks; block++) {
        afs_dump_block(afs_handle, block, UINT32_MAX);
    }
}

void afs_dump_block(afs_handle_t afs_handle, uint16_t block, uint32_t max_chunks) {
    afs_impl_t* afs = get_afs_impl(afs_handle);

    // Dump the block info from the lookup table
    if (!lookup_table_debug_dump_block(&afs->lookup_table, block)) {
        return;
    }

    // Iterate over the chunks within the block
    chunk_iter_context_t chunk_iter;
    FOREACH_CHUNK(afs, chunk_iter, block) {
        chunk_iter_callback(afs, &chunk_iter);
        if (--max_chunks == 0) {
            break;
        }
    }
}

void afs_dump_object(afs_handle_t afs_handle, uint16_t object_id) {
    afs_impl_t* afs = get_afs_impl(afs_handle);
    // Dump the object from the lookup table
    lookup_table_debug_dump_object(&afs->lookup_table, object_id);
}
