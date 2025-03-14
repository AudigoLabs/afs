#pragma once

#include "storage_types.h"

#include <stdbool.h>
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

#define CLAMP_VAL(X,LOWER,UPPER) ({ \
        __typeof__(X) _x = X; \
        __typeof__(LOWER) _l = LOWER; \
        __typeof__(UPPER) _u = UPPER; \
        _x > _u ? _u : (_x < _l ? _l : _x); \
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

//! Returns whether or not a block header is valid
bool util_is_block_header_valid(block_header_t* header, bool* is_v2);

//! Gets the offset for a given stream from a list of stream offsets.
uint64_t util_get_stream_offset(const uint64_t* stream_offsets, uint8_t stream);

//! Gets the offset for a given stream from a list of block offsets.
uint32_t util_get_block_offset(const uint32_t* block_offsets, uint8_t stream);
