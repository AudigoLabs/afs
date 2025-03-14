#pragma once

typedef struct {
    uint16_t lower;
    uint16_t upper;
    uint16_t mid;
} binary_search_context_t;

#define _BINARY_SEARCH_UPDATE() ({ \
        _binary_search.mid = ((_binary_search.upper + _binary_search.lower + 1) / 2); \
    })

#define _BINARY_SEARCH_IS_DONE() ({ \
        AFS_ASSERT(_binary_search.lower <= _binary_search.upper); \
        _binary_search.upper == _binary_search.lower; \
    })
