#include "test_helpers.h"
#include "test_storage.h"

#include "afs/afs.h"

#include "gtest/gtest.h"

#include <stdlib.h>

static void logging_write_func(const char* str) {
  printf("%s", str);
  fflush(stdout);
}

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
  ASSERT_STORAGE_EXPECTATIONS();
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
  ASSERT_STORAGE_EXPECTATIONS(
    STORAGE_EXPECT_BLOCK_HEADER(object_id, 0),
    STORAGE_EXPECT_DATA_CHUNK(0, write_data, sizeof(write_data)),
    STORAGE_EXPECT_END_CHUNK(),
  );

  // Open the object
  ASSERT_TRUE(afs_object_open(afs_, obj, 0, object_id, &config));

  // Verify the size
  ASSERT_EQ(afs_object_size(afs_, obj, 0), sizeof(write_data));

  // Verify the data
  uint8_t read_data[sizeof(write_data)];
  ASSERT_EQ(afs_object_read(afs_, obj, read_data, sizeof(read_data), NULL), sizeof(read_data));
  ASSERT_TRUE(DataMatches(read_data, write_data, sizeof(write_data)));

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
  ASSERT_STORAGE_EXPECTATIONS(
    STORAGE_EXPECT_BLOCK_HEADER(object_id, 0),
    STORAGE_EXPECT_DATA_CHUNK(1, write_data, sizeof(write_data)),
    STORAGE_EXPECT_DATA_CHUNK(1, write_data, sizeof(write_data)),
    STORAGE_EXPECT_DATA_CHUNK(2, write_data, sizeof(write_data)),
    STORAGE_EXPECT_DATA_CHUNK(1, write_data, sizeof(write_data)),
    STORAGE_EXPECT_DATA_CHUNK(2, write_data, sizeof(write_data)),
    STORAGE_EXPECT_DATA_CHUNK(2, write_data, sizeof(write_data)),
    STORAGE_EXPECT_DATA_CHUNK(1, write_data, sizeof(write_data)),
    STORAGE_EXPECT_END_CHUNK(),
  );

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
    ASSERT_TRUE(DataMatches(read_data, write_data, sizeof(write_data)));
  }

  // Make sure there's no more data to read
  ASSERT_EQ(afs_object_read(afs_, obj, read_data, sizeof(read_data), &stream), 0);

  // Close the object
  ASSERT_TRUE(afs_object_close(afs_, obj));
}

// Verify a single large write which fits within a single block, but not within the caches
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
  ASSERT_STORAGE_EXPECTATIONS(
    STORAGE_EXPECT_BLOCK_HEADER(object_id, 0),
    STORAGE_EXPECT_DATA_CHUNK(0, write_data, sizeof(write_data)),
    STORAGE_EXPECT_END_CHUNK(),
  );

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
    ASSERT_TRUE(afs_object_write(afs_, obj, 0, write_data, sizeof(write_data)));
  }

  // Close the object
  ASSERT_TRUE(afs_object_close(afs_, obj));

  // Verify the contents of the storage
  ASSERT_STORAGE_EXPECTATIONS(
    STORAGE_EXPECT_BLOCK_HEADER(object_id, 0),
    STORAGE_EXPECT_DATA_CHUNK(0, write_data, sizeof(write_data)),
    STORAGE_EXPECT_DATA_CHUNK(0, write_data, sizeof(write_data)),
    STORAGE_EXPECT_DATA_CHUNK(0, write_data, sizeof(write_data)),
    STORAGE_EXPECT_DATA_CHUNK(0, write_data, 0xfffe8),
    STORAGE_EXPECT_BLOCK_HEADER(object_id, 1),
    STORAGE_EXPECT_OFFSET_CHUNK(1, 0x3fffe8),
    STORAGE_EXPECT_DATA_CHUNK(0, &write_data[0xfffe8], 0x18),
    STORAGE_EXPECT_DATA_CHUNK(0, write_data, sizeof(write_data)),
    STORAGE_EXPECT_DATA_CHUNK(0, write_data, sizeof(write_data)),
    STORAGE_EXPECT_DATA_CHUNK(0, write_data, sizeof(write_data)),
    STORAGE_EXPECT_DATA_CHUNK(0, write_data, 0xfffc0),
    STORAGE_EXPECT_BLOCK_HEADER(object_id, 2),
    STORAGE_EXPECT_OFFSET_CHUNK(1, 0x7fffc0),
    STORAGE_EXPECT_DATA_CHUNK(0, &write_data[0xfffc0], 0x40),
    STORAGE_EXPECT_DATA_CHUNK(0, write_data, sizeof(write_data)),
    STORAGE_EXPECT_DATA_CHUNK(0, write_data, sizeof(write_data)),
    STORAGE_EXPECT_END_CHUNK(),
  );

  // Open the object
  ASSERT_TRUE(afs_object_open(afs_, obj, 0, object_id, &config));

  // Verify the size
  ASSERT_EQ(afs_object_size(afs_, obj, 0), sizeof(write_data) * 10);

  // Verify the data
  uint8_t read_data[sizeof(write_data)];
  for (int i = 0; i < 10; i++) {
    ASSERT_EQ(afs_object_read(afs_, obj, read_data, sizeof(read_data), NULL), sizeof(read_data));
  }

  // Make sure there's no more data to read
  ASSERT_EQ(afs_object_read(afs_, obj, read_data, sizeof(read_data), NULL), 0);

  // Close the object
  ASSERT_TRUE(afs_object_close(afs_, obj));
}

// Verify that things work ok if we leave empty space at the end of the first block
TEST_F(AFSFixture, EmptySpaceAtEndOfBlock) {
  AFS_OBJECT_HANDLE_DEF(obj);
  static uint8_t buffer[1024];
  const afs_object_config_t config = {
    .buffer = buffer,
    .buffer_size = sizeof(buffer),
  };
  uint8_t write_data[256*1024];
  randomize_write_data(write_data, sizeof(write_data));

  // Create the object
  const uint16_t object_id = afs_object_create(afs_, obj, &config);

  // Write to the object in a way that we leave empty space at the end of the first block
  for (int i = 0; i < 15; i++) {
    ASSERT_TRUE(afs_object_write(afs_, obj, 0, write_data, sizeof(write_data)));
  }
  ASSERT_TRUE(afs_object_write(afs_, obj, 0, write_data, sizeof(write_data)-74));
  ASSERT_TRUE(afs_object_write(afs_, obj, 0, write_data, sizeof(write_data)));

  // Close the object
  ASSERT_TRUE(afs_object_close(afs_, obj));

  // Verify the contents of the storage
  ASSERT_STORAGE_EXPECTATIONS(
    STORAGE_EXPECT_BLOCK_HEADER(object_id, 0),
    STORAGE_EXPECT_DATA_CHUNK(0, write_data, sizeof(write_data)),
    STORAGE_EXPECT_DATA_CHUNK(0, write_data, sizeof(write_data)),
    STORAGE_EXPECT_DATA_CHUNK(0, write_data, sizeof(write_data)),
    STORAGE_EXPECT_DATA_CHUNK(0, write_data, sizeof(write_data)),
    STORAGE_EXPECT_DATA_CHUNK(0, write_data, sizeof(write_data)),
    STORAGE_EXPECT_DATA_CHUNK(0, write_data, sizeof(write_data)),
    STORAGE_EXPECT_DATA_CHUNK(0, write_data, sizeof(write_data)),
    STORAGE_EXPECT_DATA_CHUNK(0, write_data, sizeof(write_data)),
    STORAGE_EXPECT_DATA_CHUNK(0, write_data, sizeof(write_data)),
    STORAGE_EXPECT_DATA_CHUNK(0, write_data, sizeof(write_data)),
    STORAGE_EXPECT_DATA_CHUNK(0, write_data, sizeof(write_data)),
    STORAGE_EXPECT_DATA_CHUNK(0, write_data, sizeof(write_data)),
    STORAGE_EXPECT_DATA_CHUNK(0, write_data, sizeof(write_data)),
    STORAGE_EXPECT_DATA_CHUNK(0, write_data, sizeof(write_data)),
    STORAGE_EXPECT_DATA_CHUNK(0, write_data, sizeof(write_data)),
    STORAGE_EXPECT_DATA_CHUNK(0, write_data, sizeof(write_data)-74),
    STORAGE_EXPECT_UNUSED_STORAGE(2),
    STORAGE_EXPECT_BLOCK_HEADER(object_id, 1),
    STORAGE_EXPECT_OFFSET_CHUNK(1, 0x3fffb6),
    STORAGE_EXPECT_DATA_CHUNK(0, write_data, sizeof(write_data)),
    STORAGE_EXPECT_END_CHUNK(),
  );

  // Open the object
  ASSERT_TRUE(afs_object_open(afs_, obj, 0, object_id, &config));

  // Verify the size
  ASSERT_EQ(afs_object_size(afs_, obj, 0), sizeof(write_data) * 17 - 74);

  // Verify the data
  uint8_t read_data[sizeof(write_data)];
  for (int i = 0; i < 16; i++) {
    ASSERT_EQ(afs_object_read(afs_, obj, read_data, sizeof(read_data), NULL), sizeof(read_data));
  }
  ASSERT_EQ(afs_object_read(afs_, obj, read_data, sizeof(read_data)-74, NULL), sizeof(read_data)-74);

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
  uint8_t write_data[256*1024];
  randomize_write_data(write_data, sizeof(write_data));

  // Create the object
  const uint16_t object_id = afs_object_create(afs_, obj, &config);

  // Write to the object in a way that we leave empty space at the end of the first block
  for (int i = 0; i < 15; i++) {
    ASSERT_TRUE(afs_object_write(afs_, obj, 0, write_data, sizeof(write_data)));
  }
  ASSERT_TRUE(afs_object_write(afs_, obj, 0, write_data, sizeof(write_data)-78));
  ASSERT_TRUE(afs_object_write(afs_, obj, 0, write_data, 2));
  ASSERT_TRUE(afs_object_write(afs_, obj, 0, write_data, sizeof(write_data)));

  // Close the object
  ASSERT_TRUE(afs_object_close(afs_, obj));

  // Verify the contents of the storage
  ASSERT_STORAGE_EXPECTATIONS(
    STORAGE_EXPECT_BLOCK_HEADER(object_id, 0),
    STORAGE_EXPECT_DATA_CHUNK(0, write_data, sizeof(write_data)),
    STORAGE_EXPECT_DATA_CHUNK(0, write_data, sizeof(write_data)),
    STORAGE_EXPECT_DATA_CHUNK(0, write_data, sizeof(write_data)),
    STORAGE_EXPECT_DATA_CHUNK(0, write_data, sizeof(write_data)),
    STORAGE_EXPECT_DATA_CHUNK(0, write_data, sizeof(write_data)),
    STORAGE_EXPECT_DATA_CHUNK(0, write_data, sizeof(write_data)),
    STORAGE_EXPECT_DATA_CHUNK(0, write_data, sizeof(write_data)),
    STORAGE_EXPECT_DATA_CHUNK(0, write_data, sizeof(write_data)),
    STORAGE_EXPECT_DATA_CHUNK(0, write_data, sizeof(write_data)),
    STORAGE_EXPECT_DATA_CHUNK(0, write_data, sizeof(write_data)),
    STORAGE_EXPECT_DATA_CHUNK(0, write_data, sizeof(write_data)),
    STORAGE_EXPECT_DATA_CHUNK(0, write_data, sizeof(write_data)),
    STORAGE_EXPECT_DATA_CHUNK(0, write_data, sizeof(write_data)),
    STORAGE_EXPECT_DATA_CHUNK(0, write_data, sizeof(write_data)),
    STORAGE_EXPECT_DATA_CHUNK(0, write_data, sizeof(write_data)),
    STORAGE_EXPECT_DATA_CHUNK(0, write_data, sizeof(write_data)-78),
    STORAGE_EXPECT_DATA_CHUNK(0, write_data, 2),
    STORAGE_EXPECT_BLOCK_HEADER(object_id, 1),
    STORAGE_EXPECT_OFFSET_CHUNK(1, 0x3fffb4),
    STORAGE_EXPECT_DATA_CHUNK(0, write_data, sizeof(write_data)),
    STORAGE_EXPECT_END_CHUNK(),
  );

  // Open the object
  ASSERT_TRUE(afs_object_open(afs_, obj, 0, object_id, &config));

  // Verify the size
  ASSERT_EQ(afs_object_size(afs_, obj, 0), sizeof(write_data) * 17 - 76);

  // Verify the data
  uint8_t read_data[sizeof(write_data)];
  for (int i = 0; i < 15; i++) {
    ASSERT_EQ(afs_object_read(afs_, obj, read_data, sizeof(read_data), NULL), sizeof(read_data));
  }
  ASSERT_EQ(afs_object_read(afs_, obj, read_data, sizeof(read_data)-76, NULL), sizeof(read_data)-76);
  ASSERT_EQ(afs_object_read(afs_, obj, read_data, sizeof(read_data), NULL), sizeof(read_data));

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
  ASSERT_STORAGE_EXPECTATIONS();
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
    ASSERT_TRUE(afs_object_read(afs_, obj1, (uint8_t*)&value, sizeof(value), NULL) == sizeof(value));
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
