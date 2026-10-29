#include "afs_config.h"
#include "afs/afs.h"
#include "impl_types.h"

#include <stdarg.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>

void afs_difftest_log_sink(int level, const char* message);

void afs_difftest_log(int level, const char* fmt, ...) {
    char buffer[512];
    va_list args;
    va_start(args, fmt);
    vsnprintf(buffer, sizeof(buffer), fmt, args);
    va_end(args);
    afs_difftest_log_sink(level, buffer);
}

void afs_difftest_assert_failed(const char* file, int line) {
    fprintf(stderr, "C AFS assertion failed at %s:%d\n", file, line);
    abort();
}

size_t afs_difftest_sizeof(int which) {
    switch (which) {
        case 0: return sizeof(afs_handle_def_t);
        case 1: return sizeof(afs_object_handle_def_t);
        case 2: return sizeof(afs_read_position_t);
        case 3: return sizeof(afs_object_list_entry_t);
        case 4: return sizeof(afs_init_t);
        case 5: return sizeof(afs_object_config_t);
        case 6: return offsetof(afs_init_t, lookup_table_buffer);
        default: return 0;
    }
}

uint32_t afs_difftest_get_seed(afs_handle_t handle) {
    return ((afs_impl_t*)handle->priv)->lookup_table.object_id_seed;
}

void afs_difftest_set_seed(afs_handle_t handle, uint32_t seed) {
    ((afs_impl_t*)handle->priv)->lookup_table.object_id_seed = seed;
}
