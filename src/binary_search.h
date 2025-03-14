#pragma once

#include "binary_search_impl.h"

// Defines a local binary search context
#define BINARY_SEARCH_DEF(LOWER, UPPER) \
    binary_search_context_t _binary_search = { .lower = LOWER, .upper = UPPER }; \

// Binary search iterator
#define BINARY_SEARCH_ITER() \
    for (_BINARY_SEARCH_UPDATE(); !_BINARY_SEARCH_IS_DONE(); _BINARY_SEARCH_UPDATE())

// Gets the current binary search value
#define BINARY_SEARCH_VALUE() \
    _binary_search.mid

// Marks the result of the current binary search loop as before the current value
#define BINARY_SEARCH_RESULT_BEFORE() do { \
        _binary_search.upper = _binary_search.mid - 1; \
    } while (0)

// Marks the result of the current binary search loop as after the current value
#define BINARY_SEARCH_RESULT_AFTER() do { \
        _binary_search.lower = _binary_search.mid; \
    } while (0)
