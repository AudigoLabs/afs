#pragma once

extern "C" {

#include "afs/afs.h"

#include <inttypes.h>
#include <stddef.h>

/* Storage expectation functions / macros */

void test_storage_init(void);

void test_storage_deinit(void);

void test_storage_get_afs_init(afs_init_t* init);

#define STORAGE_EXPECT_BLOCK_HEADER(OBJECT_ID, OBJECT_BLOCK_INDEX) \
  {.type = BLOCK_HEADER, .block_header = {.object_id=OBJECT_ID, .object_block_index=OBJECT_BLOCK_INDEX}}

#define STORAGE_EXPECT_DATA_CHUNK(STREAM, DATA, LENGTH) \
  {.type = DATA_CHUNK, .data_chunk = {.stream=STREAM, .length=LENGTH, .data=DATA}}

#define STORAGE_EXPECT_OFFSET_CHUNK(NUM_OFFSETS, ...) \
  {.type = OFFSET_CHUNK, .offset_chunk = {.num_offsets=NUM_OFFSETS, .offsets={__VA_ARGS__}}}

#define STORAGE_EXPECT_END_CHUNK() \
  {.type = END_CHUNK}

#define STORAGE_EXPECT_UNUSED_STORAGE(LENGTH) \
  {.type = UNUSED_STORAGE, .unused_storage = {.length=LENGTH}}

#define ASSERT_STORAGE_EXPECTATIONS(...) do { \
    const storage_expectation_t _EXP[] = { __VA_ARGS__ }; \
    assert_storage_expectations_impl(_EXP, sizeof(_EXP)/sizeof(storage_expectation_t)); \
  } while (0)

#define ASSERT_STORAGE_NOT_EMPTY() \
  assert_storage_not_empty_impl();


/* Implementation types / functions which should not be used directly */

typedef enum {
  BLOCK_HEADER,
  DATA_CHUNK,
  OFFSET_CHUNK,
  END_CHUNK,
  UNUSED_STORAGE,
} storage_expectation_type_t;

typedef struct {
  storage_expectation_type_t type;
  union {
    struct {
      uint16_t object_id;
      uint16_t object_block_index;
    } block_header;
    struct {
      uint8_t stream:4;
      uint32_t length:24;
      const void* data;
    } data_chunk;
    struct {
      uint8_t num_offsets;
      uint64_t offsets[16];
    } offset_chunk;
    struct {
      uint64_t length;
    } unused_storage;
  };
} storage_expectation_t;

void assert_storage_expectations_impl(const storage_expectation_t* expectations, size_t length);

void assert_storage_not_empty_impl(void);

};
