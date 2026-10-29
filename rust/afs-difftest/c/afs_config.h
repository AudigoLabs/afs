#pragma once

#include <stddef.h>
#include <stdio.h>

void afs_difftest_assert_failed(const char* file, int line);
void afs_difftest_log(int level, const char* fmt, ...) __attribute__((format(printf, 2, 3)));

#define AFS_ASSERT(COND) do { if (!(COND)) { afs_difftest_assert_failed(__FILE__, __LINE__); } } while (0)
#define AFS_ASSERT_EQ(A, B) AFS_ASSERT((A) == (B))
#define AFS_ASSERT_NOT_EQ(A, B) AFS_ASSERT((A) != (B))
#define AFS_FAIL(MSG) afs_difftest_assert_failed(__FILE__, __LINE__)

#define AFS_LOG_DEBUG(FMT, ...)
#define AFS_LOG_INFO(FMT, ...) afs_difftest_log(1, FMT, ##__VA_ARGS__)
#define AFS_LOG_WARN(FMT, ...) afs_difftest_log(2, FMT, ##__VA_ARGS__)
#define AFS_LOG_ERROR(FMT, ...) afs_difftest_log(3, FMT, ##__VA_ARGS__)
