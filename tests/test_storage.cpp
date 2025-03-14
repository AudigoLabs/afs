#include "test_storage.h"
#include "test_helpers.h"

#include "gtest/gtest.h"

#include <stdlib.h>
#include <string.h>
#include <sys/types.h>

#define ENABLE_IO_PRINTS              0

#define READ_WRITE_SIZE               512
#define BLOCK_SIZE                    (4 * 1024 * 1024)
#define STORAGE_SIZE                  (1 * 1024 * 1024 * 1024)
#define NUM_BLOCKS                    (STORAGE_SIZE / BLOCK_SIZE)
#define SUB_BLOCKS_PER_BLOCK          8

struct HexValue32 {
  uint32_t value;
};

struct HexValue64 {
  uint64_t value;
};

void PrintTo(const HexValue32& value, ::std::ostream* os) {
  *os << std::hex << "0x" << value.value;
}

void PrintTo(const HexValue64& value, ::std::ostream* os) {
  *os << std::hex << "0x" << value.value;
}

bool operator==(const HexValue32& lhs, const HexValue32& rhs) {
  return lhs.value == rhs.value;
}

bool operator==(const HexValue64& lhs, const HexValue64& rhs) {
  return lhs.value == rhs.value;
}

// Helper macros for `assert_storage_expectations_impl()`
#define ASSERT_U32_VALUE(VAL) ({ \
    HexValue32 _actual; \
    HexValue32 _exp = { .value = uint32_t(VAL) }; \
    memcpy(&_actual.value, &m_storage[m_exp_offset], sizeof(uint32_t)); \
    ASSERT_EQ(_actual, _exp); \
    m_exp_offset += sizeof(uint32_t); \
  })
#define ASSERT_U64_VALUE(VAL) ({ \
    HexValue64 _actual; \
    HexValue64 _exp = { .value = uint64_t(VAL) }; \
    memcpy(&_actual.value, &m_storage[m_exp_offset], sizeof(uint64_t)); \
    ASSERT_EQ(_actual, _exp); \
    m_exp_offset += sizeof(uint64_t); \
  })
#define ASSERT_DATA(DATA, LENGTH) ({ \
    ASSERT_DATA_MATCHES(&m_storage[m_exp_offset], (const uint8_t*)(DATA), LENGTH); \
    m_exp_offset += LENGTH; \
  })

static uint8_t* m_storage;
static uint32_t m_exp_offset;

static void read_func(uint8_t* buf, uint16_t block, uint32_t offset, uint32_t length) {
  ASSERT_TRUE(block < NUM_BLOCKS);
  ASSERT_TRUE((uint64_t)length + (uint64_t)offset <= BLOCK_SIZE);
  ASSERT_EQ(offset % READ_WRITE_SIZE, 0);
  ASSERT_EQ(length % READ_WRITE_SIZE, 0);
  memcpy(buf, &m_storage[(uint64_t)block * BLOCK_SIZE + offset], length);
#if ENABLE_IO_PRINTS
  if (block == 0) {
    for (uint32_t i = 0; i < length; i++) {
      if (buf[i] || offset + i >= 0x003fff80) {
        printf("READ [0x%08x] = 0x%x\n", offset + i, buf[i]);
      }
    }
  }
#endif
}

static void write_func(const uint8_t* buf, uint16_t block, uint32_t offset, uint32_t length) {
  ASSERT_TRUE(block < NUM_BLOCKS);
  ASSERT_TRUE((uint64_t)length + (uint64_t)offset <= BLOCK_SIZE);
  ASSERT_EQ(offset % READ_WRITE_SIZE, 0);
  ASSERT_EQ(length % READ_WRITE_SIZE, 0);
#if ENABLE_IO_PRINTS
  if (block == 0) {
    for (uint32_t i = 0; i < length; i++) {
      if (buf[i] || offset + i >= 0x003fff80) {
        printf("WRITE [0x%08x] = 0x%x\n", offset + i, buf[i]);
      }
    }
  }
#endif
  memcpy(&m_storage[(uint64_t)block * BLOCK_SIZE + offset], buf, length);
}

static void erase_func(uint16_t block) {
  memset(&m_storage[(uint64_t)block * BLOCK_SIZE], 0, BLOCK_SIZE);
}

void test_storage_init(void) {
  m_storage = (uint8_t*)malloc(STORAGE_SIZE);
  ASSERT_TRUE(m_storage != NULL);
  memset(m_storage, 0, STORAGE_SIZE);
}

void test_storage_deinit(void) {
  free(m_storage);
  m_storage = NULL;
}

void test_storage_get_afs_init(afs_init_t* init) {
  static uint8_t lookup_table_buffer[AFS_LOOKUP_TABLE_SIZE(NUM_BLOCKS)];
  static uint8_t read_write_buffer[READ_WRITE_SIZE];
  *init = (afs_init_t) {
    .storage_config = {
      .block_size = BLOCK_SIZE,
      .num_blocks = NUM_BLOCKS,
      .sub_blocks_per_block = SUB_BLOCKS_PER_BLOCK,
      .min_read_write_size = READ_WRITE_SIZE,
      .read = read_func,
      .write = write_func,
      .erase = erase_func,
    },
    .read_write_buffer = read_write_buffer,
    .lookup_table_buffer = lookup_table_buffer,
  };
}

void test_storage_generate_v1_block(uint16_t block, uint16_t object_id, const void* data, uint32_t data_length) {
  uint8_t* storage_ptr = &m_storage[(uint64_t)block * BLOCK_SIZE];

  // Write the block header
  block_header_t header = {
    .magic.val = HEADER_MAGIC_VALUE_V1.val,
    .object_id = object_id,
    .object_block_index = 0,
  };
  memcpy(storage_ptr, &header, sizeof(header));
  storage_ptr += sizeof(header);

  // Write the data chunk for stream 1
  const uint32_t data_chunk_header1 = (0xd1 << 24) | data_length;
  memcpy(storage_ptr, &data_chunk_header1, sizeof(data_chunk_header1));
  storage_ptr += sizeof(data_chunk_header1);
  memcpy(storage_ptr, data, data_length);
  storage_ptr += data_length;

  // Write the data chunk for stream 2
  const uint32_t data_chunk_header2 = (0xd2 << 24) | data_length;
  memcpy(storage_ptr, &data_chunk_header2, sizeof(data_chunk_header2));
  storage_ptr += sizeof(data_chunk_header2);
  memcpy(storage_ptr, data, data_length);
  storage_ptr += data_length;

  // Write the end chunk
  const uint32_t end_chunk = 0xed << 24;
  memcpy(storage_ptr, &end_chunk, sizeof(end_chunk));
  storage_ptr += sizeof(end_chunk);
}

void test_storage_raw_write(uint64_t offset, const void* data, uint32_t length) {
  memcpy(&m_storage[offset], data, length);
}

void assert_storage_expectations_start(void) {
  m_exp_offset = 0;
}

#define CUSTOM_ASSERTION_ASSERT_EQ(LABEL, A, B) do { \
    if ((A) != (B)) { \
      std::stringstream stream; \
      stream << std::hex << "Expected equality (" << LABEL << ")" << ":\n  Actual: 0x" << uint64_t(A) << \
        "\n  Expected: 0x" << uint64_t(B); \
      return ::testing::AssertionFailure() << stream.str(); \
    } \
  } while (0)

#define CUSTOM_ASSERTION_ASSERT_DATA_EQ(A, B, LENGTH) do { \
    for (size_t _i = 0; _i < LENGTH; _i++) { \
      const uint8_t _actual = ((const uint8_t*)(A))[_i]; \
      const uint8_t _exp = ((const uint8_t*)(B))[_i]; \
      if (_actual != _exp) { \
        std::stringstream stream; \
        stream << std::hex << "Expected zero ([0x" << int(_i) << "]):\n  Actual: 0x" << uint32_t(_actual) << \
          "\n  Expected: 0x" << uint32_t(_exp); \
        return ::testing::AssertionFailure() << stream.str(); \
      } \
    } \
  } while (0)

#define CUSTOM_ASSERTION_ASSERT_DATA_ZERO(A, LENGTH) do { \
    for (size_t _i = 0; _i < LENGTH; _i++) { \
      const uint8_t _actual = ((const uint8_t*)(A))[_i]; \
      if (_actual != 0) { \
        std::stringstream stream; \
        stream << std::hex << "Expected zero ([0x" << int(_i) << "]):\n  Actual: 0x" << uint32_t(_actual); \
        return ::testing::AssertionFailure() << stream.str(); \
      } \
    } \
  } while (0)

::testing::AssertionResult BlockHeaderExp::Assert(const char* exp1, const BlockHeaderExp& exp) const {
  block_header_t actual;
  memcpy(&actual, &m_storage[m_exp_offset], sizeof(actual));
  m_exp_offset += sizeof(actual);

  const uint32_t EXPECTED_MAGIC = ((uint32_t)'A') | (((uint32_t)'F') << 8) | (((uint32_t)'S') << 16) | (((uint32_t)'2') << 24);
  CUSTOM_ASSERTION_ASSERT_EQ("magic", actual.magic.val, EXPECTED_MAGIC);
  CUSTOM_ASSERTION_ASSERT_EQ("object_id", actual.object_id, exp.object_id);
  CUSTOM_ASSERTION_ASSERT_EQ("object_block_index", actual.object_block_index, exp.object_block_index);
  return ::testing::AssertionSuccess();
}

::testing::AssertionResult BlockHeaderV1Exp::Assert(const char* exp1, const BlockHeaderV1Exp& exp) const {
  block_header_t header;
  memcpy(&header, &m_storage[m_exp_offset], sizeof(header));
  m_exp_offset += sizeof(header);

  const uint32_t EXPECTED_MAGIC = ((uint32_t)'A') | (((uint32_t)'F') << 8) | (((uint32_t)'S') << 16) | (((uint32_t)'1') << 24);
  CUSTOM_ASSERTION_ASSERT_EQ("magic", header.magic.val, EXPECTED_MAGIC);
  CUSTOM_ASSERTION_ASSERT_EQ("object_id", header.object_id, exp.object_id);
  CUSTOM_ASSERTION_ASSERT_EQ("object_block_index", header.object_block_index, exp.object_block_index);

  return ::testing::AssertionSuccess();
}

::testing::AssertionResult DataChunkExp::Assert(const char* exp1, const DataChunkExp& exp) const {
  chunk_header_t header;
  memcpy(&header, &m_storage[m_exp_offset], sizeof(header));
  m_exp_offset += sizeof(header);

  const uint32_t exp_tag = ((0xd0 | exp.stream) << 24) | exp.length;
  CUSTOM_ASSERTION_ASSERT_EQ("tag", header.tag, exp_tag);
  CUSTOM_ASSERTION_ASSERT_DATA_EQ(&m_storage[m_exp_offset], exp.data, exp.length);
  m_exp_offset += exp.length;

  return ::testing::AssertionSuccess();
}

::testing::AssertionResult OffsetChunkExp::Assert(const char* exp1, const OffsetChunkExp& exp) const {
  chunk_header_t header;
  memcpy(&header, &m_storage[m_exp_offset], sizeof(header));
  m_exp_offset += sizeof(header);

  const uint32_t exp_tag = (0x3e << 24) | (exp.num_offsets * sizeof(uint64_t));
  CUSTOM_ASSERTION_ASSERT_EQ("tag", header.tag, exp_tag);
  for (uint8_t i = 0; i < exp.num_offsets; i++) {
    uint64_t value;
    memcpy(&value, &m_storage[m_exp_offset], sizeof(value));
    m_exp_offset += sizeof(value);
    CUSTOM_ASSERTION_ASSERT_EQ("offset[0x" << int(i) << "]", value, exp.values[i]);
  }

  return ::testing::AssertionSuccess();
}

::testing::AssertionResult SeekChunkExp::Assert(const char* exp1, const SeekChunkExp& exp) const {
  chunk_header_t header;
  memcpy(&header, &m_storage[m_exp_offset], sizeof(header));
  m_exp_offset += sizeof(header);

  const uint32_t exp_tag = (0x5e << 24) | exp.num_offsets * sizeof(uint32_t);
  CUSTOM_ASSERTION_ASSERT_EQ("tag", header.tag, exp_tag);

  for (uint8_t i = 0; i < exp.num_offsets; i++) {
    uint32_t value;
    memcpy(&value, &m_storage[m_exp_offset], sizeof(value));
    m_exp_offset += sizeof(value);
    CUSTOM_ASSERTION_ASSERT_EQ("offset[0x" << int(i) << "]", value, exp.values[i]);
  }

  return ::testing::AssertionSuccess();
}

::testing::AssertionResult EndChunkExp::Assert(const char* exp1, const EndChunkExp& exp) const {
  chunk_header_t header;
  memcpy(&header, &m_storage[m_exp_offset], sizeof(header));
  m_exp_offset += sizeof(header);

  const uint32_t exp_tag = 0xed << 24;
  CUSTOM_ASSERTION_ASSERT_EQ("tag", header.tag, exp_tag);

  return ::testing::AssertionSuccess();
}

::testing::AssertionResult BlockFooterExp::Assert(const char* exp1, const BlockFooterExp& exp) const {
  block_footer_t footer;
  memcpy(&footer, &m_storage[m_exp_offset], sizeof(footer));
  m_exp_offset += sizeof(footer);

  const uint32_t EXPECTED_MAGIC = ((uint32_t)'a') | (((uint32_t)'f') << 8) | (((uint32_t)'s') << 16) | (((uint32_t)'2') << 24);
  CUSTOM_ASSERTION_ASSERT_EQ("magic", footer.magic.val, EXPECTED_MAGIC);

  return ::testing::AssertionSuccess();
}

::testing::AssertionResult UnusedExp::Assert(const char* exp1, const UnusedExp& exp) const {
  uint32_t length;
  switch (exp.until) {
    case Until::bytes:
      length = exp.bytes;
      break;
    case Until::footer:
      length = BLOCK_SIZE - 128 - (m_exp_offset % BLOCK_SIZE);
      break;
    case Until::end:
      length = BLOCK_SIZE - (m_exp_offset % BLOCK_SIZE);
      break;
    case Until::storage_end:
      length = STORAGE_SIZE - m_exp_offset;
      break;
  }
  CUSTOM_ASSERTION_ASSERT_DATA_ZERO(&m_storage[m_exp_offset], length);
  m_exp_offset += length;
  return ::testing::AssertionSuccess();
}

::testing::AssertionResult AssertStoragePosition(const char* exp1, uint32_t exp) {
  CUSTOM_ASSERTION_ASSERT_EQ("<storage offset>", m_exp_offset, exp);
  return ::testing::AssertionSuccess();
}

void assert_storage_expectations_end(void) {
  ASSERT_EQ(m_exp_offset, STORAGE_SIZE);
}

::testing::AssertionResult AssertStorageNotEmpty(const char* exp1, bool unused) {
  bool empty = true;
  for (size_t i = 0; i < STORAGE_SIZE; i++) {
    if (m_storage[i] != 0) {
      empty = false;
    }
  }
  if (empty) {
    return ::testing::AssertionFailure() << "Storage is empty";
  }
  return ::testing::AssertionSuccess();
}
