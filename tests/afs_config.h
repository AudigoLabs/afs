#include <assert.h>
#include <stdio.h>

// Set this to 1 to enable debug logs
#define ENABLE_AFS_DEBUG_LOGS 0

#define AFS_ASSERT(COND) assert(COND)
#define AFS_ASSERT_EQ(A, B) assert((A) == (B))
#define AFS_ASSERT_NOT_EQ(A, B) assert((A) != (B))
#define AFS_FAIL(MSG) assert(!MSG)

#if ENABLE_AFS_DEBUG_LOGS
#define AFS_LOG_DEBUG(FMT, ...) printf("[DEBUG] " FMT "\n", ##__VA_ARGS__)
#else
#define AFS_LOG_DEBUG(FMT, ...)
#endif
#define AFS_LOG_INFO(FMT, ...) printf("[INFO] " FMT "\n", ##__VA_ARGS__)
#define AFS_LOG_WARN(FMT, ...) printf("[WARN] " FMT "\n", ##__VA_ARGS__)
#define AFS_LOG_ERROR(FMT, ...) printf("[ERROR] " FMT "\n", ##__VA_ARGS__)
