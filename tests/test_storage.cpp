#include "test_storage.h"
#include "test_helpers.h"

#include "../src/afs_storage_types.h"

#include "gtest/gtest.h"

#include <stdlib.h>
#include <string.h>
#include <sys/types.h>

#define READ_WRITE_SIZE               512
#define BLOCK_SIZE                    (4 * 1024 * 1024)
#define STORAGE_SIZE                  (1 * 1024 * 1024 * 1024)
#define NUM_BLOCKS                    (STORAGE_SIZE / BLOCK_SIZE)

#define ASSERT_MEM_FILLED(CHECK, EXPECT, LEN) do { \
    uint8_t* expect_bytes = (uint8_t*)malloc(LEN); \
    memset(expect_bytes, EXPECT, LEN); \
    const uint8_t* check_bytes = CHECK; \
    ASSERT_TRUE(DataMatches(check_bytes, expect_bytes, LEN)); \
    free(expect_bytes); \
  } while (0)

static uint8_t* m_storage;

static void read_func(uint8_t* buf, uint16_t block, uint32_t offset, uint32_t length) {
  ASSERT_TRUE(block < NUM_BLOCKS);
  ASSERT_TRUE((uint64_t)length + (uint64_t)offset <= BLOCK_SIZE);
  ASSERT_EQ(offset % READ_WRITE_SIZE, 0);
  ASSERT_EQ(length % READ_WRITE_SIZE, 0);
  memcpy(buf, &m_storage[(uint64_t)block * BLOCK_SIZE + offset], length);
}

static void write_func(const uint8_t* buf, uint16_t block, uint32_t offset, uint32_t length) {
  ASSERT_TRUE(block < NUM_BLOCKS);
  ASSERT_TRUE((uint64_t)length + (uint64_t)offset <= BLOCK_SIZE);
  ASSERT_EQ(offset % READ_WRITE_SIZE, 0);
  ASSERT_EQ(length % READ_WRITE_SIZE, 0);
  memcpy(&m_storage[(uint64_t)block * BLOCK_SIZE + offset], buf, length);
}

static void erase_func(uint16_t block) {
  memset(&m_storage[(uint64_t)block * BLOCK_SIZE], 0xff, BLOCK_SIZE);
}

void test_storage_init(void) {
  m_storage = (uint8_t*)malloc(STORAGE_SIZE);
  ASSERT_TRUE(m_storage != NULL);
  memset(m_storage, 0xff, STORAGE_SIZE);
}

void test_storage_deinit(void) {
  free(m_storage);
  m_storage = NULL;
}

void test_storage_get_afs_init(afs_init_t* init) {
  static afs_lookup_table_entry_t lookup_table[STORAGE_SIZE / BLOCK_SIZE];
  static uint8_t read_write_buffer[READ_WRITE_SIZE];
  *init = (afs_init_t) {
    .block_size = BLOCK_SIZE,
    .num_blocks = STORAGE_SIZE / BLOCK_SIZE,
    .read_write_size = READ_WRITE_SIZE,
    .read_write_buffer = read_write_buffer,
    .lookup_table = lookup_table,
    .read_func = read_func,
    .write_func = write_func,
    .erase_func = erase_func,
  };
}

void assert_storage_expectations_impl(const storage_expectation_t* expectations, size_t length) {
  size_t offset = 0;
  for (size_t i = 0; i < length; i++) {
    const storage_expectation_t* exp = &expectations[i];
    if (exp->type == BLOCK_HEADER) {
      const block_header_t block_header = {
        .magic = {'A', 'F', 'S', '1'},
        .object_id = exp->block_header.object_id,
        .object_block_index = exp->block_header.object_block_index,
      };
      ASSERT_TRUE(DataMatches(&m_storage[offset], (const uint8_t*)&block_header, sizeof(block_header)));
      offset += sizeof(block_header);
    } else if (exp->type == DATA_CHUNK) {
      const uint32_t expected_tag = ((0xd0 | exp->data_chunk.stream) << 24) | exp->data_chunk.length;
      uint32_t stored_tag;
      memcpy(&stored_tag, &m_storage[offset], sizeof(stored_tag));
      ASSERT_EQ(stored_tag, expected_tag);
      offset += sizeof(expected_tag);
      ASSERT_TRUE(DataMatches(&m_storage[offset], (const uint8_t*)exp->data_chunk.data, exp->data_chunk.length));
      offset += exp->data_chunk.length;
    } else if (exp->type == OFFSET_CHUNK) {
      const uint32_t offsets_length = exp->offset_chunk.num_offsets * sizeof(uint64_t);
      const uint32_t expected_tag = (0x3e << 24) | offsets_length;
      uint32_t stored_tag;
      memcpy(&stored_tag, &m_storage[offset], sizeof(stored_tag));
      ASSERT_EQ(stored_tag, expected_tag);
      offset += sizeof(expected_tag);
      for (uint32_t i = 0; i < exp->offset_chunk.num_offsets; i++) {
        const uint64_t expected_offset_value = exp->offset_chunk.offsets[i];
        uint64_t stored_offset_value;
        memcpy(&stored_offset_value, &m_storage[offset], sizeof(stored_offset_value));
        offset += sizeof(stored_offset_value);
        ASSERT_EQ(stored_offset_value, expected_offset_value);
      }
    } else if (exp->type == END_CHUNK) {
      const uint32_t tag = 0xed << 24;
      ASSERT_TRUE(DataMatches(&m_storage[offset], (const uint8_t*)&tag, sizeof(tag)));
      offset += sizeof(tag);
    } else if (exp->type == UNUSED_STORAGE) {
      ASSERT_MEM_FILLED(&m_storage[offset], 0xff, exp->unused_storage.length);
      offset += exp->unused_storage.length;
    } else {
      FAIL() << "Invalid expectation type: " << exp->type;
    }
  }
  ASSERT_MEM_FILLED(&m_storage[offset], 0xff, STORAGE_SIZE - offset);
}

void assert_storage_not_empty_impl(void) {
  uint8_t* expect_bytes = (uint8_t*)malloc(STORAGE_SIZE);
  memset(expect_bytes, 0xff, STORAGE_SIZE);
  ASSERT_FALSE(DataMatches(m_storage, expect_bytes, STORAGE_SIZE));
  free(expect_bytes);
}
