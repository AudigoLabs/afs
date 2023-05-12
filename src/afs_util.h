#pragma once

#include "afs/afs.h"

#include "afs_impl_types.h"
#include "afs_storage_types.h"

#include <stddef.h>

#define MIN_VAL(A,B) ({ \
        __typeof__(A) _a = A; \
        __typeof__(B) _b = B; \
        _a < _b ? _a : _b; \
    })

#define MAX_VAL(A,B) ({ \
        __typeof__(A) _a = A; \
        __typeof__(B) _b = B; \
        _a > _b ? _a : _b; \
    })

#define ALIGN_DOWN(A,B) ({ \
        __typeof__(A) _a = A; \
        __typeof__(B) _b = B; \
        _a - (_a % _b); \
    })

#define ALIGN_UP(A,B) ({ \
        __typeof__(A) _a = A; \
        __typeof__(B) _b = B; \
        __typeof__(A) _tmp = _a + _b - 1; \
        _tmp - (_tmp % _b); \
    })

//! Helper macro to iterate over every block
#define AFS_UTIL_FOREACH_BLOCK(AFS, VAR, START_BLOCK) \
    for (VAR = (afs_block_iter_context_t) {.block = START_BLOCK - 1}; afs_util_block_iter_next(AFS, &(VAR));)

//! Helper macro to iterate over every chunk
#define AFS_UTIL_FOREACH_CHUNK(AFS, VAR, BLOCK) \
    for (VAR = (afs_chunk_iter_context_t) {.block = block}; afs_util_chunk_iter_next(AFS, &(VAR));)

//! Context used by afs_util_block_iter_*()
typedef struct {
    // The current block number
    uint16_t block;
    // The header of the current block
    block_header_t header;
} afs_block_iter_context_t;

//! Context used by afs_util_chunk_iter_*()
typedef struct {
    // The current block number
    uint16_t block;
    // The header of the current chunk
    chunk_header_t header;
    // The offset within the block of the chunk header
    uint32_t offset;
} afs_chunk_iter_context_t;

//! Chunk iterator callback (return true to continue iteration or false to stop) - passed the chunk header
typedef bool (*afs_util_chunk_iter_cb_t)(afs_impl_t* impl, void* context, uint32_t offset, const chunk_header_t* header);

//! Returns whether or not a block header is valid
bool afs_util_is_block_header_valid(block_header_t* header);

//! Advances the iterator and populates the context with the next value (returns true if the value exists)
bool afs_util_block_iter_next(afs_impl_t* afs, afs_block_iter_context_t* context);

//! Advances the iterator and populates the context with the next value (returns true if the value exists)
bool afs_util_chunk_iter_next(afs_impl_t* afs, afs_chunk_iter_context_t* context);

//! Extracts all the data chunks from a cache which is at the start of an object and are of the same stream
uint32_t afs_util_get_object_data_from_cache(afs_cache_t* cache, uint8_t* stream);
