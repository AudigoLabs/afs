#pragma once

extern "C" {

#include "afs/afs.h"
#include "../src/storage_types.h"

};

#include "gtest/gtest.h"

#define STORAGE_EXPECTATIONS_START() \
  assert_storage_expectations_start()

struct BlockHeaderExp {
  uint16_t object_id;
  uint16_t object_block_index;
  ::testing::AssertionResult Assert(const char* exp1, const BlockHeaderExp& exp) const;
};
#define STORAGE_EXPECTATIONS_EXPECT_BLOCK_HEADER(OBJECT_ID, OBJECT_BLOCK_INDEX) do { \
    const BlockHeaderExp exp = { .object_id = OBJECT_ID, .object_block_index = OBJECT_BLOCK_INDEX }; \
    ASSERT_PRED_FORMAT1(exp.Assert, exp); \
  } while (0)

struct BlockHeaderV1Exp {
  uint16_t object_id;
  uint16_t object_block_index;
  ::testing::AssertionResult Assert(const char* exp1, const BlockHeaderV1Exp& exp) const;
};
#define STORAGE_EXPECTATIONS_EXPECT_BLOCK_HEADER_V1(OBJECT_ID, OBJECT_BLOCK_INDEX) do { \
    const BlockHeaderV1Exp exp = { .object_id = OBJECT_ID, .object_block_index = OBJECT_BLOCK_INDEX }; \
    ASSERT_PRED_FORMAT1(exp.Assert, exp); \
  } while (0)

struct DataChunkExp {
  uint8_t stream;
  void* data;
  uint32_t length;
  ::testing::AssertionResult Assert(const char* exp1, const DataChunkExp& exp) const;
};
#define STORAGE_EXPECTATIONS_EXPECT_DATA_CHUNK(STREAM, DATA, LENGTH) do { \
    const DataChunkExp exp = { .stream = STREAM, .data = DATA, .length = LENGTH }; \
    ASSERT_PRED_FORMAT1(exp.Assert, exp); \
  } while (0)

struct OffsetChunkExp {
  uint8_t num_offsets;
  uint64_t values[16];
  ::testing::AssertionResult Assert(const char* exp1, const OffsetChunkExp& exp) const;
};
#define STORAGE_EXPECTATIONS_EXPECT_OFFSET_CHUNK(NUM_OFFSETS, ...) do { \
    const OffsetChunkExp exp = { .num_offsets = NUM_OFFSETS, .values = { __VA_ARGS__ } }; \
    ASSERT_PRED_FORMAT1(exp.Assert, exp); \
  } while (0)

struct SeekChunkExp {
  uint8_t num_offsets;
  uint32_t values[16];
  ::testing::AssertionResult Assert(const char* exp1, const SeekChunkExp& exp) const;
};
#define STORAGE_EXPECTATIONS_EXPECT_SEEK_CHUNK(NUM_OFFSETS, ...) do { \
    const SeekChunkExp exp = { .num_offsets = NUM_OFFSETS, .values = { __VA_ARGS__ } }; \
    ASSERT_PRED_FORMAT1(exp.Assert, exp); \
  } while (0)

struct EndChunkExp {
  ::testing::AssertionResult Assert(const char* exp1, const EndChunkExp& exp) const;
};
#define STORAGE_EXPECTATIONS_EXPECT_END_CHUNK() do { \
    const EndChunkExp exp; \
    ASSERT_PRED_FORMAT1(exp.Assert, exp); \
  } while (0)

struct BlockFooterExp {
  ::testing::AssertionResult Assert(const char* exp1, const BlockFooterExp& exp) const;
};
#define STORAGE_EXPECTATIONS_EXPECT_BLOCK_FOOTER() do { \
    const BlockFooterExp exp; \
    ASSERT_PRED_FORMAT1(exp.Assert, exp); \
  } while (0)

struct UnusedExp {
  enum class Until {
    bytes,
    footer,
    end,
    storage_end,
  };
  Until until;
  uint32_t bytes;
  ::testing::AssertionResult Assert(const char* exp1, const UnusedExp& exp) const;
};
#define STORAGE_EXPECTATIONS_EXPECT_UNUSED_BYTES(LENGTH) do { \
    const UnusedExp exp = { .until = UnusedExp::Until::bytes, .bytes = LENGTH }; \
    ASSERT_PRED_FORMAT1(exp.Assert, exp); \
  } while (0)
#define STORAGE_EXPECTATIONS_EXPECT_UNUSED_UNTIL_FOOTER() do { \
    const UnusedExp exp = { .until = UnusedExp::Until::footer }; \
    ASSERT_PRED_FORMAT1(exp.Assert, exp); \
  } while (0)
#define STORAGE_EXPECTATIONS_EXPECT_UNUSED_UNTIL_BLOCK_END() do { \
    const UnusedExp exp = { .until = UnusedExp::Until::end }; \
    ASSERT_PRED_FORMAT1(exp.Assert, exp); \
  } while (0)

::testing::AssertionResult AssertStoragePosition(const char* exp1, uint32_t offset);
#define STORAGE_EXPECTATIONS_EXPECT_STORAGE_POSITION(VALUE) do { \
    ASSERT_PRED_FORMAT1(AssertStoragePosition, VALUE); \
  } while (0)

#define STORAGE_EXPECTATIONS_END() do { \
    const UnusedExp exp = { .until = UnusedExp::Until::storage_end }; \
    ASSERT_PRED_FORMAT1(exp.Assert, exp); \
    assert_storage_expectations_end(); \
  } while (0)

::testing::AssertionResult AssertStorageNotEmpty(const char* exp1, bool unused);
#define ASSERT_STORAGE_NOT_EMPTY() do { \
    ASSERT_PRED_FORMAT1(AssertStorageNotEmpty, true); \
  } while (0)

extern "C" {

/* Storage expectation functions / macros */

void test_storage_init(void);

void test_storage_deinit(void);

void test_storage_get_afs_init(afs_init_t* init);

void test_storage_generate_v1_block(uint16_t block, uint16_t object_id, const void* data, uint32_t data_length);

void assert_storage_expectations_start(void);

void assert_storage_expectations_end(void);

};
