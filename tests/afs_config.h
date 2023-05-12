#include <assert.h>
#define AFS_ASSERT(COND) assert(COND)
#define AFS_ASSERT_EQ(A, B) assert((A) == (B))
#define AFS_ASSERT_NOT_EQ(A, B) assert((A) != (B))
#define AFS_FAIL(MSG) assert(!MSG)

#include <stdio.h>
#define AFS_LOG_DEBUG(FMT, ...) printf("[DEBUG] " FMT "\n", ##__VA_ARGS__)
#define AFS_LOG_INFO(FMT, ...) printf("[DEBUG] " FMT "\n", ##__VA_ARGS__)
#define AFS_LOG_WARN(FMT, ...) printf("[DEBUG] " FMT "\n", ##__VA_ARGS__)
#define AFS_LOG_ERROR(FMT, ...) printf("[DEBUG] " FMT "\n", ##__VA_ARGS__)
