#include "test_helpers.h"
#include "test_storage.h"

#include "afs/afs.h"

#include "gtest/gtest.h"

#include <stdlib.h>

static void randomize_write_data(void* buffer, size_t length) {
  while (length > 0) {
    const size_t cpy_len = length < sizeof(int) ? length : sizeof(int);
    int val = rand();
    memcpy(&buffer, &val, cpy_len);
    length -= cpy_len;
  }
}

class AFSEnvironment : public ::testing::Environment {
  public:
    void SetUp() override {
      srand(time(NULL));
    }
};

class AFSFixture : public testing::Test {
  protected:
    void SetUp() override {
      test_storage_init();

      // Initialize AFS
      afs_init_t init_afs;
      test_storage_get_afs_init(&init_afs);
      afs_init(afs_, &init_afs);
    }

    void TearDown() override {
      afs_deinit(afs_);
      test_storage_deinit();
    }

    afs_handle_def_t afs_def_;
    const afs_handle_t afs_ = &afs_def_;
};

// Verify that the storage is empty if we don't write anything
TEST_F(AFSFixture, Empty) {
  STORAGE_EXPECTATIONS_START();
  STORAGE_EXPECTATIONS_END();
}

// Verify that we can read data written with AFS v1
TEST_F(AFSFixture, ReadV1) {
  // Manually create the object within the storage
  const uint16_t object_id = 0x1234;
  uint8_t write_data[8];
  randomize_write_data(write_data, sizeof(write_data));
  test_storage_generate_v1_block(0, object_id, write_data, sizeof(write_data));

  // Reinit AFS to pick up the new block
  afs_deinit(afs_);
  afs_init_t init_afs;
  test_storage_get_afs_init(&init_afs);
  afs_init(afs_, &init_afs);

  // Verify the contents of the storage
  STORAGE_EXPECTATIONS_START();
  STORAGE_EXPECTATIONS_EXPECT_BLOCK_HEADER_V1(object_id, 0);
  STORAGE_EXPECTATIONS_EXPECT_DATA_CHUNK(1, write_data, sizeof(write_data));
  STORAGE_EXPECTATIONS_EXPECT_DATA_CHUNK(2, write_data, sizeof(write_data));
  STORAGE_EXPECTATIONS_EXPECT_END_CHUNK();
  STORAGE_EXPECTATIONS_END();

  AFS_OBJECT_HANDLE_DEF(obj);
  static uint8_t buffer[1024];
  const afs_object_config_t config = {
    .buffer = buffer,
    .buffer_size = sizeof(buffer),
  };

  // Open the object and verify for streams 1 and 2
  for (uint8_t i = 1; i <= 2; i++) {
    ASSERT_TRUE(afs_object_open(afs_, obj, 1, object_id, &config));

    // Verify the size
    ASSERT_EQ(afs_object_size(afs_, obj, 0), sizeof(write_data));

    // Verify the data
    uint8_t read_data[sizeof(write_data)];
    ASSERT_EQ(afs_object_read(afs_, obj, read_data, sizeof(read_data), NULL), sizeof(read_data));
    ASSERT_DATA_MATCHES(read_data, write_data, sizeof(write_data));

    // Make sure there's no more data to read
    ASSERT_EQ(afs_object_read(afs_, obj, read_data, sizeof(read_data), NULL), 0);

    // Close the object
    ASSERT_TRUE(afs_object_close(afs_, obj));
  }

  // Open the object and verify for wildcard stream
  ASSERT_TRUE(afs_object_open(afs_, obj, AFS_WILDCARD_STREAM, object_id, &config));

  // Verify the size
  ASSERT_EQ(afs_object_size(afs_, obj, 0xffff), sizeof(write_data) * 2);

  // Verify the data
  uint8_t read_data[sizeof(write_data)];
  uint8_t stream;
  ASSERT_EQ(afs_object_read(afs_, obj, read_data, sizeof(read_data), &stream), sizeof(read_data));
  ASSERT_EQ(stream, 1);
  ASSERT_DATA_MATCHES(read_data, write_data, sizeof(write_data));
  ASSERT_EQ(afs_object_read(afs_, obj, read_data, sizeof(read_data), &stream), sizeof(read_data));
  ASSERT_EQ(stream, 2);
  ASSERT_DATA_MATCHES(read_data, write_data, sizeof(write_data));

  // Make sure there's no more data to read
  ASSERT_EQ(afs_object_read(afs_, obj, read_data, sizeof(read_data), &stream), 0);

  // Close the object
  ASSERT_TRUE(afs_object_close(afs_, obj));
}

// Verify a single write which fits both within a single block and within the caches
TEST_F(AFSFixture, WriteSingleSmallChunk) {
  AFS_OBJECT_HANDLE_DEF(obj);
  static uint8_t buffer[1024];
  const afs_object_config_t config = {
    .buffer = buffer,
    .buffer_size = sizeof(buffer),
  };
  uint8_t write_data[8];
  randomize_write_data(write_data, sizeof(write_data));

  // Create the object
  const uint16_t object_id = afs_object_create(afs_, obj, &config);

  // Write a single small chunk to the object
  ASSERT_TRUE(afs_object_write(afs_, obj, 0, write_data, sizeof(write_data)));

  // Close the object
  ASSERT_TRUE(afs_object_close(afs_, obj));

  // Verify the contents of the storage
  STORAGE_EXPECTATIONS_START();
  STORAGE_EXPECTATIONS_EXPECT_BLOCK_HEADER(object_id, 0);
  STORAGE_EXPECTATIONS_EXPECT_DATA_CHUNK(0, write_data, sizeof(write_data));
  STORAGE_EXPECTATIONS_EXPECT_END_CHUNK();
  STORAGE_EXPECTATIONS_EXPECT_UNUSED_UNTIL_FOOTER();
  STORAGE_EXPECTATIONS_EXPECT_BLOCK_FOOTER();
  STORAGE_EXPECTATIONS_EXPECT_SEEK_CHUNK(1, sizeof(write_data));
  STORAGE_EXPECTATIONS_EXPECT_UNUSED_UNTIL_BLOCK_END();
  STORAGE_EXPECTATIONS_END();

  // Open the object
  ASSERT_TRUE(afs_object_open(afs_, obj, 0, object_id, &config));

  // Verify the size
  ASSERT_EQ(afs_object_size(afs_, obj, 0), sizeof(write_data));

  // Verify the data
  uint8_t read_data[sizeof(write_data)];
  ASSERT_EQ(afs_object_read(afs_, obj, read_data, sizeof(read_data), NULL), sizeof(read_data));
  ASSERT_DATA_MATCHES(read_data, write_data, sizeof(write_data));

  // Make sure there's no more data to read
  ASSERT_EQ(afs_object_read(afs_, obj, read_data, sizeof(read_data), NULL), 0);

  // Close the object
  ASSERT_TRUE(afs_object_close(afs_, obj));
}

// Verify multiple small writes across multiple streams
TEST_F(AFSFixture, WriteMultipleStreamsSmallChunk) {
  AFS_OBJECT_HANDLE_DEF(obj);
  static uint8_t buffer[1024];
  const afs_object_config_t config = {
    .buffer = buffer,
    .buffer_size = sizeof(buffer),
  };
  uint8_t write_data[8];
  randomize_write_data(write_data, sizeof(write_data));

  // Create the object
  const uint16_t object_id = afs_object_create(afs_, obj, &config);

  // Write a single small chunk between the two streams in an arbitrary order / pattern
  const uint8_t STREAM_PATTERN[] = {1, 1, 2, 1, 2, 2, 1};
  for (size_t i = 0; i < sizeof(STREAM_PATTERN)/sizeof(*STREAM_PATTERN); i++) {
    ASSERT_TRUE(afs_object_write(afs_, obj, STREAM_PATTERN[i], write_data, sizeof(write_data)));
  }

  // Close the object
  ASSERT_TRUE(afs_object_close(afs_, obj));

  // Verify the contents of the storage
  STORAGE_EXPECTATIONS_START();
  STORAGE_EXPECTATIONS_EXPECT_BLOCK_HEADER(object_id, 0);
  STORAGE_EXPECTATIONS_EXPECT_DATA_CHUNK(1, write_data, sizeof(write_data));
  STORAGE_EXPECTATIONS_EXPECT_DATA_CHUNK(1, write_data, sizeof(write_data));
  STORAGE_EXPECTATIONS_EXPECT_DATA_CHUNK(2, write_data, sizeof(write_data));
  STORAGE_EXPECTATIONS_EXPECT_DATA_CHUNK(1, write_data, sizeof(write_data));
  STORAGE_EXPECTATIONS_EXPECT_DATA_CHUNK(2, write_data, sizeof(write_data));
  STORAGE_EXPECTATIONS_EXPECT_DATA_CHUNK(2, write_data, sizeof(write_data));
  STORAGE_EXPECTATIONS_EXPECT_DATA_CHUNK(1, write_data, sizeof(write_data));
  STORAGE_EXPECTATIONS_EXPECT_END_CHUNK();
  STORAGE_EXPECTATIONS_EXPECT_UNUSED_UNTIL_FOOTER();
  STORAGE_EXPECTATIONS_EXPECT_BLOCK_FOOTER();
  STORAGE_EXPECTATIONS_EXPECT_SEEK_CHUNK(2, (1 << 28) | sizeof(write_data) * 4, (2 << 28) | sizeof(write_data) * 3);
  STORAGE_EXPECTATIONS_EXPECT_UNUSED_UNTIL_BLOCK_END();
  STORAGE_EXPECTATIONS_END();

  // Open the object
  ASSERT_TRUE(afs_object_open(afs_, obj, AFS_WILDCARD_STREAM, object_id, &config));

  // Verify the size
  ASSERT_EQ(afs_object_size(afs_, obj, 1 << 1), sizeof(write_data) * 4);
  ASSERT_EQ(afs_object_size(afs_, obj, 1 << 2), sizeof(write_data) * 3);
  ASSERT_EQ(afs_object_size(afs_, obj, (1 << 1) | (1 << 2)), sizeof(write_data) * 7);

  // Verify the data
  uint8_t read_data[sizeof(write_data)];
  uint8_t stream;
  for (size_t i = 0; i < sizeof(STREAM_PATTERN)/sizeof(*STREAM_PATTERN); i++) {
    const uint8_t expect_stream = STREAM_PATTERN[i];
    ASSERT_EQ(afs_object_read(afs_, obj, read_data, sizeof(read_data), &stream), sizeof(read_data));
    ASSERT_EQ(stream, expect_stream);
    ASSERT_DATA_MATCHES(read_data, write_data, sizeof(write_data));
  }

  // Make sure there's no more data to read
  ASSERT_EQ(afs_object_read(afs_, obj, read_data, sizeof(read_data), &stream), 0);

  // Close the object
  ASSERT_TRUE(afs_object_close(afs_, obj));
}

// Verify a single large write which fits within a single block, but not within the caches or sub-blocks
TEST_F(AFSFixture, WriteSingleLargeChunk) {
  AFS_OBJECT_HANDLE_DEF(obj);
  static uint8_t buffer[1024];
  const afs_object_config_t config = {
    .buffer = buffer,
    .buffer_size = sizeof(buffer),
  };
  uint8_t write_data[1024*1024];
  randomize_write_data(write_data, sizeof(write_data));

  // Create the object
  const uint16_t object_id = afs_object_create(afs_, obj, &config);

  // Write a large chunk of data
  ASSERT_TRUE(afs_object_write(afs_, obj, 0, write_data, sizeof(write_data)));

  // Close the object
  ASSERT_TRUE(afs_object_close(afs_, obj));

  // Verify the contents of the storage
  const uint8_t* exp_write_data;
  STORAGE_EXPECTATIONS_START();
  STORAGE_EXPECTATIONS_EXPECT_BLOCK_HEADER(object_id, 0);
  STORAGE_EXPECTATIONS_EXPECT_DATA_CHUNK(0, write_data, 0x7fff4);
  STORAGE_EXPECTATIONS_EXPECT_SEEK_CHUNK(1, 0x7fff4);
  STORAGE_EXPECTATIONS_EXPECT_DATA_CHUNK(0, write_data + 0x7fff4, 0x7fff4);
  STORAGE_EXPECTATIONS_EXPECT_SEEK_CHUNK(1, 0xfffe8);
  STORAGE_EXPECTATIONS_EXPECT_DATA_CHUNK(0, write_data + 0xfffe8, 0x18);
  STORAGE_EXPECTATIONS_EXPECT_END_CHUNK();
  STORAGE_EXPECTATIONS_EXPECT_UNUSED_UNTIL_FOOTER();
  STORAGE_EXPECTATIONS_EXPECT_BLOCK_FOOTER();
  STORAGE_EXPECTATIONS_EXPECT_SEEK_CHUNK(1, sizeof(write_data));
  STORAGE_EXPECTATIONS_EXPECT_UNUSED_UNTIL_BLOCK_END();
  STORAGE_EXPECTATIONS_END();

  // Open the object
  ASSERT_TRUE(afs_object_open(afs_, obj, 0, object_id, &config));

  // Verify the size
  ASSERT_EQ(afs_object_size(afs_, obj, 0), sizeof(write_data));

  // Verify the data
  uint8_t read_data[sizeof(write_data)];
  ASSERT_EQ(afs_object_read(afs_, obj, read_data, sizeof(read_data), NULL), sizeof(read_data));

  // Make sure there's no more data to read
  ASSERT_EQ(afs_object_read(afs_, obj, read_data, sizeof(read_data), NULL), 0);

  // Close the object
  ASSERT_TRUE(afs_object_close(afs_, obj));
}

// Verify multiple large writes which end up spanning multiple blocks
TEST_F(AFSFixture, WriteMultipleLargeChunks) {
  AFS_OBJECT_HANDLE_DEF(obj);
  static uint8_t buffer[1024];
  const afs_object_config_t config = {
    .buffer = buffer,
    .buffer_size = sizeof(buffer),
  };
  uint8_t write_data[1024*1024];
  randomize_write_data(write_data, sizeof(write_data));

  // Create the object
  const uint16_t object_id = afs_object_create(afs_, obj, &config);

  // Write a large chunk of data 10 times
  for (int i = 0; i < 10; i++) {
    ASSERT_TRUE(afs_object_write(afs_, obj, (i % 2) + 1, write_data, sizeof(write_data)));
  }

  // Close the object
  ASSERT_TRUE(afs_object_close(afs_, obj));

  // Verify the contents of the storage
  const uint32_t overflow_data_length1 = 0x18 + 0x80;
  const uint32_t overflow_data_length2 = overflow_data_length1 + 0x30 + 0x80;
  STORAGE_EXPECTATIONS_START();
  STORAGE_EXPECTATIONS_EXPECT_BLOCK_HEADER(object_id, 0);
  STORAGE_EXPECTATIONS_EXPECT_DATA_CHUNK(1, write_data, 0x7fff4);
  STORAGE_EXPECTATIONS_EXPECT_SEEK_CHUNK(1, (1 << 28) | 0x7fff4);
  STORAGE_EXPECTATIONS_EXPECT_DATA_CHUNK(1, write_data + 0x7fff4, 0x7fff4);
  STORAGE_EXPECTATIONS_EXPECT_SEEK_CHUNK(1, (1 << 28) | 0xfffe8);
  STORAGE_EXPECTATIONS_EXPECT_DATA_CHUNK(1, write_data + 0xfffe8, 0x18);
  STORAGE_EXPECTATIONS_EXPECT_DATA_CHUNK(2, write_data, 0x7ffd8);
  STORAGE_EXPECTATIONS_EXPECT_SEEK_CHUNK(2, (1 << 28) | 0x100000, (2 << 28) | 0x7ffd8);
  STORAGE_EXPECTATIONS_EXPECT_DATA_CHUNK(2, write_data + 0x7ffd8, 0x7fff0);
  STORAGE_EXPECTATIONS_EXPECT_SEEK_CHUNK(2, (1 << 28) | 0x100000, (2 << 28) | 0xfffc8);
  STORAGE_EXPECTATIONS_EXPECT_DATA_CHUNK(2, write_data + 0xfffc8, 0x38);
  STORAGE_EXPECTATIONS_EXPECT_DATA_CHUNK(1, write_data, 0x7ffb4);
  STORAGE_EXPECTATIONS_EXPECT_SEEK_CHUNK(2, (1 << 28) | 0x17ffb4, (2 << 28) | 0x100000);
  STORAGE_EXPECTATIONS_EXPECT_DATA_CHUNK(1, write_data + 0x7ffb4, 0x7fff0);
  STORAGE_EXPECTATIONS_EXPECT_SEEK_CHUNK(2, (1 << 28) | 0x1fffa4, (2 << 28) | 0x100000);
  STORAGE_EXPECTATIONS_EXPECT_DATA_CHUNK(1, write_data + 0xfffa4, 0x5c);
  STORAGE_EXPECTATIONS_EXPECT_DATA_CHUNK(2, write_data, 0x7ff90);
  STORAGE_EXPECTATIONS_EXPECT_SEEK_CHUNK(2, (1 << 28) | 0x200000, (2 << 28) | 0x17ff90);
  STORAGE_EXPECTATIONS_EXPECT_DATA_CHUNK(2, write_data + 0x7ff90, 0x7ff70);
  STORAGE_EXPECTATIONS_EXPECT_BLOCK_FOOTER();
  STORAGE_EXPECTATIONS_EXPECT_SEEK_CHUNK(2, (1 << 28) | 0x200000, (2 << 28) | 0x1fff00);
  STORAGE_EXPECTATIONS_EXPECT_UNUSED_UNTIL_BLOCK_END();
  STORAGE_EXPECTATIONS_EXPECT_BLOCK_HEADER(object_id, 1);
  STORAGE_EXPECTATIONS_EXPECT_OFFSET_CHUNK(2, (1ULL << 60) | 0x200000, (2ULL << 60) | 0x1fff00);
  STORAGE_EXPECTATIONS_EXPECT_DATA_CHUNK(2, write_data + 0xfff00, 0x100);
  STORAGE_EXPECTATIONS_EXPECT_DATA_CHUNK(1, write_data, 0x7fedc);
  STORAGE_EXPECTATIONS_EXPECT_SEEK_CHUNK(2, (1 << 28) | 0x7fedc, (2 << 28) | 0x100);
  STORAGE_EXPECTATIONS_EXPECT_DATA_CHUNK(1, write_data + 0x7fedc, 0x7fff0);
  STORAGE_EXPECTATIONS_EXPECT_SEEK_CHUNK(2, (1 << 28) | 0xffecc, (2 << 28) | 0x100);
  STORAGE_EXPECTATIONS_EXPECT_DATA_CHUNK(1, write_data + 0xffecc, 0x134);
  STORAGE_EXPECTATIONS_EXPECT_DATA_CHUNK(2, write_data, 0x7feb8);
  STORAGE_EXPECTATIONS_EXPECT_SEEK_CHUNK(2, (1 << 28) | 0x100000, (2 << 28) | 0x7ffb8);
  STORAGE_EXPECTATIONS_EXPECT_DATA_CHUNK(2, write_data + 0x7feb8, 0x7fff0);
  STORAGE_EXPECTATIONS_EXPECT_SEEK_CHUNK(2, (1 << 28) | 0x100000, (2 << 28) | 0xfffa8);
  STORAGE_EXPECTATIONS_EXPECT_DATA_CHUNK(2, write_data + 0xffea8, 0x158);
  STORAGE_EXPECTATIONS_EXPECT_DATA_CHUNK(1, write_data, 0x7fe94);
  STORAGE_EXPECTATIONS_EXPECT_SEEK_CHUNK(2, (1 << 28) | 0x17fe94, (2 << 28) | 0x100100);
  STORAGE_EXPECTATIONS_EXPECT_DATA_CHUNK(1, write_data + 0x7fe94, 0x7fff0);
  STORAGE_EXPECTATIONS_EXPECT_SEEK_CHUNK(2, (1 << 28) | 0x1ffe84, (2 << 28) | 0x100100);
  STORAGE_EXPECTATIONS_EXPECT_DATA_CHUNK(1, write_data + 0xffe84, 0x17c);
  STORAGE_EXPECTATIONS_EXPECT_DATA_CHUNK(2, write_data, 0x7fe70);
  STORAGE_EXPECTATIONS_EXPECT_SEEK_CHUNK(2, (1 << 28) | 0x200000, (2 << 28) | 0x17ff70);
  STORAGE_EXPECTATIONS_EXPECT_DATA_CHUNK(2, write_data + 0x7fe70, 0x7ff70);
  STORAGE_EXPECTATIONS_EXPECT_BLOCK_FOOTER();
  STORAGE_EXPECTATIONS_EXPECT_SEEK_CHUNK(2, (1 << 28) | 0x200000, (2 << 28) | 0x1ffee0);
  STORAGE_EXPECTATIONS_EXPECT_UNUSED_UNTIL_BLOCK_END();
  STORAGE_EXPECTATIONS_EXPECT_BLOCK_HEADER(object_id, 2);
  STORAGE_EXPECTATIONS_EXPECT_OFFSET_CHUNK(2, (1ULL << 60) | 0x400000, (2ULL << 60) | 0x3ffde0);
  STORAGE_EXPECTATIONS_EXPECT_DATA_CHUNK(2, write_data + 0xffde0, 0x220);
  STORAGE_EXPECTATIONS_EXPECT_DATA_CHUNK(1, write_data, 0x7fdbc);
  STORAGE_EXPECTATIONS_EXPECT_SEEK_CHUNK(2, (1 << 28) | 0x7fdbc, (2 << 28) | 0x220);
  STORAGE_EXPECTATIONS_EXPECT_DATA_CHUNK(1, write_data + 0x7fdbc, 0x7fff0);
  STORAGE_EXPECTATIONS_EXPECT_SEEK_CHUNK(2, (1 << 28) | 0xffdac, (2 << 28) | 0x220);
  STORAGE_EXPECTATIONS_EXPECT_DATA_CHUNK(1, write_data + 0xffdac, 0x254);
  STORAGE_EXPECTATIONS_EXPECT_DATA_CHUNK(2, write_data, 0x7fd98);
  STORAGE_EXPECTATIONS_EXPECT_SEEK_CHUNK(2, (1 << 28) | 0x100000, (2 << 28) | 0x7ffb8);
  STORAGE_EXPECTATIONS_EXPECT_DATA_CHUNK(2, write_data + 0x7fd98, 0x7fff0);
  STORAGE_EXPECTATIONS_EXPECT_SEEK_CHUNK(2, (1 << 28) | 0x100000, (2 << 28) | 0xfffa8);
  STORAGE_EXPECTATIONS_EXPECT_DATA_CHUNK(2, write_data + 0xffd88, 0x278);
  STORAGE_EXPECTATIONS_EXPECT_END_CHUNK();
  STORAGE_EXPECTATIONS_EXPECT_UNUSED_UNTIL_FOOTER();
  STORAGE_EXPECTATIONS_EXPECT_BLOCK_FOOTER();
  STORAGE_EXPECTATIONS_EXPECT_SEEK_CHUNK(2, (1 << 28) | 0x100000, (2 << 28) | 0x100220);
  STORAGE_EXPECTATIONS_EXPECT_UNUSED_UNTIL_BLOCK_END();
  STORAGE_EXPECTATIONS_END();

  // Open the object
  ASSERT_TRUE(afs_object_open(afs_, obj, AFS_WILDCARD_STREAM, object_id, &config));

  // Verify the size
  ASSERT_EQ(afs_object_size(afs_, obj, 0x6), sizeof(write_data) * 10);

  // Verify the data
  uint8_t read_data[sizeof(write_data)];
  for (int i = 0; i < 10; i++) {
    uint32_t read_length = 0;
    while (read_length < sizeof(read_data)) {
      uint8_t stream;
      read_length += afs_object_read(afs_, obj, read_data + read_length, sizeof(read_data) - read_length, &stream);
      ASSERT_EQ(stream, (i % 2) + 1);
    }
    ASSERT_EQ(read_length, sizeof(read_data));
    ASSERT_DATA_MATCHES(read_data, write_data, sizeof(write_data));
  }

  // Make sure there's no more data to read
  uint8_t stream;
  ASSERT_EQ(afs_object_read(afs_, obj, read_data, sizeof(read_data), &stream), 0);

  // Close the object
  ASSERT_TRUE(afs_object_close(afs_, obj));
}

// Verify that things work ok if we leave empty space at the end of sub-blocks / blocks
TEST_F(AFSFixture, EmptySpaceAtEndOfRegions) {
  AFS_OBJECT_HANDLE_DEF(obj);
  static uint8_t buffer[16*1024];
  const afs_object_config_t config = {
    .buffer = buffer,
    .buffer_size = sizeof(buffer),
  };
  uint8_t write_data[512*1024];
  randomize_write_data(write_data, sizeof(write_data));

  // Create the object
  const uint16_t object_id = afs_object_create(afs_, obj, &config);

  // Write in the specific pattern to leave the desired empty space
  const uint32_t NUM_WRITES = 9;
  const uint32_t write_sizes[NUM_WRITES] = {
    0x7fff0, // 1st sub-block - 4 bytes free
    0x7fff2, // 2nd sub-block - 2 bytes free
    0x7fff3, // 3rd sub-block - 1 byte free
    0x7fff4, // 4th sub-block - 0 bytes free
    0x7fff4, // 5th sub-block - 0 bytes free
    0x7fff4, // 6th sub-block - 0 bytes free
    0x7fff4, // 7th sub-block - 0 bytes free
    0x7ff73, // 8th sub-block - 1 byte free
    0x100, // 9th sub-block (in 2nd block)
  };
  uint32_t cumulative_size[NUM_WRITES] = {};
  for (int i = 0; i < NUM_WRITES; i++) {
    const uint32_t write_size = write_sizes[i];
    ASSERT_TRUE(afs_object_write(afs_, obj, 0, write_data, write_size));
    for (int j = i; j < NUM_WRITES; j++) {
      cumulative_size[j] += write_size;
    }
  }

  // Close the object
  ASSERT_TRUE(afs_object_close(afs_, obj));

  // Verify the contents of the storage
  STORAGE_EXPECTATIONS_START();
  STORAGE_EXPECTATIONS_EXPECT_BLOCK_HEADER(object_id, 0);
  STORAGE_EXPECTATIONS_EXPECT_DATA_CHUNK(0, write_data, write_sizes[0]);
  STORAGE_EXPECTATIONS_EXPECT_UNUSED_BYTES(0x7fff4 - write_sizes[0]);
  STORAGE_EXPECTATIONS_EXPECT_SEEK_CHUNK(1, cumulative_size[0]);
  STORAGE_EXPECTATIONS_EXPECT_DATA_CHUNK(0, write_data, write_sizes[1]);
  STORAGE_EXPECTATIONS_EXPECT_UNUSED_BYTES(0x7fff4 - write_sizes[1]);
  STORAGE_EXPECTATIONS_EXPECT_SEEK_CHUNK(1, cumulative_size[1]);
  STORAGE_EXPECTATIONS_EXPECT_DATA_CHUNK(0, write_data, write_sizes[2]);
  STORAGE_EXPECTATIONS_EXPECT_UNUSED_BYTES(0x7fff4 - write_sizes[2]);
  STORAGE_EXPECTATIONS_EXPECT_SEEK_CHUNK(1, cumulative_size[2]);
  STORAGE_EXPECTATIONS_EXPECT_DATA_CHUNK(0, write_data, write_sizes[3]);
  STORAGE_EXPECTATIONS_EXPECT_UNUSED_BYTES(0x7fff4 - write_sizes[3]);
  STORAGE_EXPECTATIONS_EXPECT_SEEK_CHUNK(1, cumulative_size[3]);
  STORAGE_EXPECTATIONS_EXPECT_DATA_CHUNK(0, write_data, write_sizes[4]);
  STORAGE_EXPECTATIONS_EXPECT_UNUSED_BYTES(0x7fff4 - write_sizes[4]);
  STORAGE_EXPECTATIONS_EXPECT_SEEK_CHUNK(1, cumulative_size[4]);
  STORAGE_EXPECTATIONS_EXPECT_DATA_CHUNK(0, write_data, write_sizes[5]);
  STORAGE_EXPECTATIONS_EXPECT_UNUSED_BYTES(0x7fff4 - write_sizes[5]);
  STORAGE_EXPECTATIONS_EXPECT_SEEK_CHUNK(1, cumulative_size[5]);
  STORAGE_EXPECTATIONS_EXPECT_DATA_CHUNK(0, write_data, write_sizes[6]);
  STORAGE_EXPECTATIONS_EXPECT_UNUSED_BYTES(0x7fff4 - write_sizes[6]);
  STORAGE_EXPECTATIONS_EXPECT_SEEK_CHUNK(1, cumulative_size[6]);
  STORAGE_EXPECTATIONS_EXPECT_DATA_CHUNK(0, write_data, write_sizes[7]);
  STORAGE_EXPECTATIONS_EXPECT_UNUSED_BYTES(0x7ff74 - write_sizes[7]);
  STORAGE_EXPECTATIONS_EXPECT_BLOCK_FOOTER();
  STORAGE_EXPECTATIONS_EXPECT_SEEK_CHUNK(1, cumulative_size[7]);
  STORAGE_EXPECTATIONS_EXPECT_UNUSED_UNTIL_BLOCK_END();
  STORAGE_EXPECTATIONS_EXPECT_BLOCK_HEADER(object_id, 1);
  STORAGE_EXPECTATIONS_EXPECT_OFFSET_CHUNK(1, cumulative_size[7]);
  STORAGE_EXPECTATIONS_EXPECT_DATA_CHUNK(0, write_data, write_sizes[8]);
  STORAGE_EXPECTATIONS_EXPECT_END_CHUNK();
  STORAGE_EXPECTATIONS_EXPECT_UNUSED_UNTIL_FOOTER();
  STORAGE_EXPECTATIONS_EXPECT_BLOCK_FOOTER();
  STORAGE_EXPECTATIONS_EXPECT_SEEK_CHUNK(1, write_sizes[8]);
  STORAGE_EXPECTATIONS_EXPECT_UNUSED_UNTIL_BLOCK_END();
  STORAGE_EXPECTATIONS_END();

  // Open the object
  ASSERT_TRUE(afs_object_open(afs_, obj, 0, object_id, &config));

  // Verify the size
  ASSERT_EQ(afs_object_size(afs_, obj, 0), cumulative_size[8]);

  // Verify the data
  uint8_t read_data[sizeof(write_data)];
  for (int i = 0; i < NUM_WRITES; i++) {
    ASSERT_EQ(afs_object_read(afs_, obj, read_data, write_sizes[i], NULL), write_sizes[i]);
    ASSERT_DATA_MATCHES(read_data, write_data, write_sizes[i]);
  }

  // Make sure there's no more data to read
  ASSERT_EQ(afs_object_read(afs_, obj, read_data, sizeof(read_data), NULL), 0);

  // Close the object
  ASSERT_TRUE(afs_object_close(afs_, obj));
}

// Verify a small data chunk at the end of the block
TEST_F(AFSFixture, SmallDataAtEndOfBlock) {
  AFS_OBJECT_HANDLE_DEF(obj);
  static uint8_t buffer[1024];
  const afs_object_config_t config = {
    .buffer = buffer,
    .buffer_size = sizeof(buffer),
  };
  uint8_t write_data[512*1024];
  randomize_write_data(write_data, sizeof(write_data));

  // Create the object
  const uint16_t object_id = afs_object_create(afs_, obj, &config);

  // Write to the object in a way that we leave just enough empty space to fit a final 2 byte data chunk at the end of
  // the first block
  const uint32_t NUM_WRITES = 10;
  const uint32_t write_sizes[NUM_WRITES] = {
    0x7fff4,
    0x7fff4,
    0x7fff4,
    0x7fff4,
    0x7fff4,
    0x7fff4,
    0x7fff4,
    0x7ff6e,
    0x2,
    0x100,
  };
  uint32_t cumulative_size[NUM_WRITES] = {};
  for (int i = 0; i < NUM_WRITES; i++) {
    const uint32_t write_size = write_sizes[i];
    ASSERT_TRUE(afs_object_write(afs_, obj, 0, write_data, write_size));
    for (int j = i; j < NUM_WRITES; j++) {
      cumulative_size[j] += write_size;
    }
  }

  // Close the object
  ASSERT_TRUE(afs_object_close(afs_, obj));

  // Verify the contents of the storage
  STORAGE_EXPECTATIONS_START();
  STORAGE_EXPECTATIONS_EXPECT_BLOCK_HEADER(object_id, 0);
  STORAGE_EXPECTATIONS_EXPECT_DATA_CHUNK(0, write_data, write_sizes[0]);
  STORAGE_EXPECTATIONS_EXPECT_SEEK_CHUNK(1, cumulative_size[0]);
  STORAGE_EXPECTATIONS_EXPECT_DATA_CHUNK(0, write_data, write_sizes[1]);
  STORAGE_EXPECTATIONS_EXPECT_SEEK_CHUNK(1, cumulative_size[1]);
  STORAGE_EXPECTATIONS_EXPECT_DATA_CHUNK(0, write_data, write_sizes[2]);
  STORAGE_EXPECTATIONS_EXPECT_SEEK_CHUNK(1, cumulative_size[2]);
  STORAGE_EXPECTATIONS_EXPECT_DATA_CHUNK(0, write_data, write_sizes[3]);
  STORAGE_EXPECTATIONS_EXPECT_SEEK_CHUNK(1, cumulative_size[3]);
  STORAGE_EXPECTATIONS_EXPECT_DATA_CHUNK(0, write_data, write_sizes[4]);
  STORAGE_EXPECTATIONS_EXPECT_SEEK_CHUNK(1, cumulative_size[4]);
  STORAGE_EXPECTATIONS_EXPECT_DATA_CHUNK(0, write_data, write_sizes[5]);
  STORAGE_EXPECTATIONS_EXPECT_SEEK_CHUNK(1, cumulative_size[5]);
  STORAGE_EXPECTATIONS_EXPECT_DATA_CHUNK(0, write_data, write_sizes[6]);
  STORAGE_EXPECTATIONS_EXPECT_SEEK_CHUNK(1, cumulative_size[6]);
  STORAGE_EXPECTATIONS_EXPECT_DATA_CHUNK(0, write_data, write_sizes[7]);
  STORAGE_EXPECTATIONS_EXPECT_DATA_CHUNK(0, write_data, write_sizes[8]);
  STORAGE_EXPECTATIONS_EXPECT_BLOCK_FOOTER();
  STORAGE_EXPECTATIONS_EXPECT_SEEK_CHUNK(1, cumulative_size[8]);
  STORAGE_EXPECTATIONS_EXPECT_UNUSED_UNTIL_BLOCK_END();
  STORAGE_EXPECTATIONS_EXPECT_BLOCK_HEADER(object_id, 1);
  STORAGE_EXPECTATIONS_EXPECT_OFFSET_CHUNK(1, cumulative_size[8]);
  STORAGE_EXPECTATIONS_EXPECT_DATA_CHUNK(0, write_data, write_sizes[9]);
  STORAGE_EXPECTATIONS_EXPECT_END_CHUNK();
  STORAGE_EXPECTATIONS_EXPECT_UNUSED_UNTIL_FOOTER();
  STORAGE_EXPECTATIONS_EXPECT_BLOCK_FOOTER();
  STORAGE_EXPECTATIONS_EXPECT_SEEK_CHUNK(1, write_sizes[9]);
  STORAGE_EXPECTATIONS_EXPECT_UNUSED_UNTIL_BLOCK_END();
  STORAGE_EXPECTATIONS_END();

  // Open the object
  ASSERT_TRUE(afs_object_open(afs_, obj, 0, object_id, &config));

  // Verify the size
  ASSERT_EQ(afs_object_size(afs_, obj, 0), cumulative_size[9]);

  // Verify the data
  uint8_t read_data[sizeof(write_data)];
  for (int i = 0; i < NUM_WRITES; i++) {
    ASSERT_EQ(afs_object_read(afs_, obj, read_data, write_sizes[i], NULL), write_sizes[i]);
    ASSERT_DATA_MATCHES(read_data, write_data, write_sizes[i]);
  }

  // Make sure there's no more data to read
  ASSERT_EQ(afs_object_read(afs_, obj, read_data, sizeof(read_data), NULL), 0);

  // Close the object
  ASSERT_TRUE(afs_object_close(afs_, obj));
}

// Verify insecure wipe deletes all the objects
TEST_F(AFSFixture, InsecureWipe) {
  AFS_OBJECT_HANDLE_DEF(obj);
  static uint8_t buffer[1024];
  const afs_object_config_t config = {
    .buffer = buffer,
    .buffer_size = sizeof(buffer),
  };
  uint8_t write_data[1024*1024];
  randomize_write_data(write_data, sizeof(write_data));

  // Create some objects which span multiple blocks
  uint16_t object_ids[10];
  for (int i = 0; i < 5; i++) {
    object_ids[i] = afs_object_create(afs_, obj, &config);
    ASSERT_TRUE(afs_object_write(afs_, obj, 0, write_data, sizeof(write_data)));
    ASSERT_TRUE(afs_object_write(afs_, obj, 0, write_data, sizeof(write_data)));
    ASSERT_TRUE(afs_object_write(afs_, obj, 0, write_data, sizeof(write_data)));
    ASSERT_TRUE(afs_object_write(afs_, obj, 0, write_data, sizeof(write_data)-24));
    ASSERT_TRUE(afs_object_write(afs_, obj, 0, write_data, sizeof(write_data)));
    ASSERT_TRUE(afs_object_close(afs_, obj));
  }
  // Create some objects which span a single block
  for (int i = 5; i < 10; i++) {
    object_ids[i] = afs_object_create(afs_, obj, &config);
    ASSERT_TRUE(afs_object_write(afs_, obj, 0, write_data, sizeof(write_data)));
    ASSERT_TRUE(afs_object_close(afs_, obj));
  }

  // Wipe the storage insecurely
  afs_wipe(afs_, false);

  // Make sure that there are no remaining objects
  afs_object_list_entry_t entry = {0};
  ASSERT_FALSE(afs_object_list(afs_, &entry));
  ASSERT_EQ(afs_size(afs_), 0);

  // Storage should not be empty
  ASSERT_STORAGE_NOT_EMPTY();
}

// Verify secure wipe completely wipes the storage
TEST_F(AFSFixture, SecureWipe) {
  AFS_OBJECT_HANDLE_DEF(obj);
  static uint8_t buffer[1024];
  const afs_object_config_t config = {
    .buffer = buffer,
    .buffer_size = sizeof(buffer),
  };
  uint8_t write_data[1024*1024];
  randomize_write_data(write_data, sizeof(write_data));

  // Create some objects which span multiple blocks
  uint16_t object_ids[10];
  for (int i = 0; i < 5; i++) {
    object_ids[i] = afs_object_create(afs_, obj, &config);
    ASSERT_TRUE(afs_object_write(afs_, obj, 0, write_data, sizeof(write_data)));
    ASSERT_TRUE(afs_object_write(afs_, obj, 0, write_data, sizeof(write_data)));
    ASSERT_TRUE(afs_object_write(afs_, obj, 0, write_data, sizeof(write_data)));
    ASSERT_TRUE(afs_object_write(afs_, obj, 0, write_data, sizeof(write_data)-24));
    ASSERT_TRUE(afs_object_write(afs_, obj, 0, write_data, sizeof(write_data)));
    ASSERT_TRUE(afs_object_close(afs_, obj));
  }
  // Create some objects which span a single block
  for (int i = 5; i < 10; i++) {
    object_ids[i] = afs_object_create(afs_, obj, &config);
    ASSERT_TRUE(afs_object_write(afs_, obj, 0, write_data, sizeof(write_data)));
    ASSERT_TRUE(afs_object_close(afs_, obj));
  }

  // Wipe the storage
  afs_wipe(afs_, true);

  // Make sure that the storage is completely empty
  STORAGE_EXPECTATIONS_START();
  STORAGE_EXPECTATIONS_END();
}

// A less structured test of most of the APIs
TEST_F(AFSFixture, Complete) {
  AFS_OBJECT_HANDLE_DEF(obj1);
  AFS_OBJECT_HANDLE_DEF(obj2);
  static uint8_t buffer1[1024];
  const afs_object_config_t config1 = {
    .buffer = buffer1,
    .buffer_size = sizeof(buffer1),
  };
  static uint8_t buffer2[1024];
  const afs_object_config_t config2 = {
    .buffer = buffer2,
    .buffer_size = sizeof(buffer2),
  };
  uint8_t write_data[2][256*1024];
  randomize_write_data(write_data[0], sizeof(write_data[0]));
  randomize_write_data(write_data[1], sizeof(write_data[1]));

  // Create the first object
  const uint16_t object_id1 = afs_object_create(afs_, obj1, &config1);

  // Write to the first object
  for (int i = 0; i < 30; i++) {
    ASSERT_TRUE(afs_object_write(afs_, obj1, 0, write_data[0], sizeof(write_data[0])));
  }

  // Create the second object
  const uint16_t object_id2 = afs_object_create(afs_, obj2, &config2);

  // Write to both objects
  for (int i = 0; i < 100; i++) {
    ASSERT_TRUE(afs_object_write(afs_, obj1, 0, write_data[0], sizeof(write_data[0])));
    ASSERT_TRUE(afs_object_write(afs_, obj2, 0, write_data[0], sizeof(write_data[0])));
    if ((i % 7) == 0) {
      // Write to stream 1
      ASSERT_TRUE(afs_object_write(afs_, obj1, 1, write_data[1], sizeof(write_data[1])));
      ASSERT_TRUE(afs_object_write(afs_, obj2, 1, write_data[1], sizeof(write_data[1])));
    }
  }

  // Close the objects
  ASSERT_TRUE(afs_object_close(afs_, obj1));
  ASSERT_TRUE(afs_object_close(afs_, obj2));
  ASSERT_FALSE(afs_is_storage_full(afs_));

  // Test the object list
  afs_object_list_entry_t entry1 = {0};
  uint16_t list_index = 0;
  while (afs_object_list(afs_, &entry1)) {
    switch (list_index) {
      case 0:
        ASSERT_EQ(entry1.object_id, object_id1);
        break;
      case 1:
        ASSERT_EQ(entry1.object_id, object_id2);
        break;
      default:
        ASSERT_TRUE(0);
        break;
    }
    list_index++;
  }

  // Test seeking in each file
  afs_object_list_entry_t entry2 = {0};
  uint16_t first_object_id = 0;
  while (afs_object_list(afs_, &entry2)) {
    first_object_id = first_object_id ? first_object_id : entry2.object_id;
    ASSERT_TRUE(afs_object_open(afs_, obj1, 1, entry2.object_id, &config1));
    ASSERT_TRUE(afs_object_seek(afs_, obj1, 0x1231f0));
    ASSERT_EQ(afs_object_size(afs_, obj1, 0), 3932160);
    afs_read_position_t read_pos;
    afs_object_save_read_position(afs_, obj1, &read_pos);
    ASSERT_TRUE(afs_object_close(afs_, obj1));
    ASSERT_TRUE(afs_object_open(afs_, obj1, 1, entry2.object_id, &config1));
    afs_object_restore_read_position(afs_, obj1, &read_pos);
    uint32_t value;
    ASSERT_EQ(afs_object_read(afs_, obj1, (uint8_t*)&value, sizeof(value), NULL), sizeof(value));
    uint32_t expect_value;
    memcpy(&expect_value, &write_data[0][0x1231f0 % sizeof(write_data[0])], sizeof(expect_value));
    ASSERT_EQ(value, expect_value);
    ASSERT_TRUE(afs_object_close(afs_, obj1));
  }

  // Test wildcard stream
  uint64_t stream0_length = 0;
  uint64_t stream1_length = 0;
  ASSERT_TRUE(afs_object_open(afs_, obj1, AFS_WILDCARD_STREAM, first_object_id, &config1));
  while (true) {
    uint8_t stream = UINT8_MAX;
    uint8_t buffer[1024*1024];
    const uint32_t bytes_read = afs_object_read(afs_, obj1, buffer, sizeof(buffer), &stream);
    if (!bytes_read) {
      break;
    }
    switch (stream) {
      case 0:
        stream0_length += bytes_read;
          break;
      case 1:
        stream1_length += bytes_read;
        break;
      default:
        ASSERT_TRUE(0);
        break;
    }
  }
  ASSERT_EQ(afs_object_size(afs_, obj1, 0x1), 34078720);
  ASSERT_EQ(afs_object_size(afs_, obj1, 0x2), 3932160);
  ASSERT_EQ(afs_object_size(afs_, obj1, 0x3), 38010880);
  ASSERT_TRUE(afs_object_close(afs_, obj1));
  ASSERT_EQ(stream0_length, 34078720);
  ASSERT_EQ(stream1_length, 3932160);

  // Test seeking within wildcard stream
  ASSERT_TRUE(afs_object_open(afs_, obj1, AFS_WILDCARD_STREAM, first_object_id, &config1));
  ASSERT_TRUE(afs_object_seek(afs_, obj1, 0x901111));
  uint32_t value;
  uint8_t stream;
  ASSERT_EQ(afs_object_read(afs_, obj1, (uint8_t*)&value, sizeof(value), &stream), sizeof(value));
  uint32_t expect_value;
  memcpy(&expect_value, &write_data[0][0x1111], sizeof(expect_value));
  ASSERT_EQ(value, expect_value);
  ASSERT_TRUE(afs_object_close(afs_, obj1));

  // List all the objects
  afs_object_list_entry_t entry3 = {0};
  while (afs_object_list(afs_, &entry3)) {
    for (uint8_t stream = 0; stream <= 1; stream++) {
      ASSERT_TRUE(afs_object_open(afs_, obj1, stream, entry3.object_id, &config1));
      uint64_t total_bytes_read = 0;
      uint8_t buffered_byte;
      bool has_buffered_byte = false;
      while (true) {
        uint8_t buffer[200 * 1024];
        uint8_t* buffer_ptr = buffer;
        uint32_t max_read_length = sizeof(buffer);
        if (has_buffered_byte) {
          buffer[0] = buffered_byte;
          buffer_ptr++;
          max_read_length -= 1;
        }
        uint32_t read_length = afs_object_read(afs_, obj1, buffer_ptr, max_read_length, NULL);
        if (read_length == 0) {
          break;
        }
        if (has_buffered_byte) {
          read_length += 1;
        }
        if (read_length % 2) {
          buffered_byte = buffer[read_length-1];
          has_buffered_byte = true;
        } else {
          has_buffered_byte = false;
        }
        for (uint32_t i = 0; i < read_length; i += 2) {
          uint16_t expect_value;
          memcpy(&expect_value, &write_data[stream][total_bytes_read % sizeof(write_data[stream])], sizeof(expect_value));
          uint16_t read_value;
          memcpy(&read_value, &buffer[i], sizeof(read_value));
          ASSERT_EQ(read_value, expect_value);
          total_bytes_read += sizeof(read_value);
        }
      }
      ASSERT_TRUE(afs_object_close(afs_, obj1));
      if (entry3.object_id == object_id1) {
        ASSERT_EQ(total_bytes_read, stream == 0 ? 34078720 : 3932160);
        ASSERT_EQ(afs_object_get_num_blocks(afs_, entry3.object_id), 10);
      } else if (entry3.object_id == object_id2) {
        ASSERT_EQ(total_bytes_read, stream == 0 ? 26214400 : 3932160);
        ASSERT_EQ(afs_object_get_num_blocks(afs_, entry3.object_id), 8);
      }
    }
  }

  ASSERT_EQ(afs_size(afs_), 18);
}

int main(int argc, char **argv) {
  ::testing::AddGlobalTestEnvironment(new AFSEnvironment());
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
