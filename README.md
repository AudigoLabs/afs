# Audigo File System

The Audigo File System (AFS) is a very simple logging file system which is highly optimized for storing audio data.

## Version 2

Version 2 added support for seek chunks to speed up seeking within a block. This is to address slow seek performance in
version 1 in use-cases where there are thousands of chunks within a block. Version 2 is completely backwards compatible
with data written by version 1.

## Design

For information about the design of AFS, check out [DESIGN.md](DESIGN.md).

## Config

The configuration should be specified within an `afs_config.h` file which is added to the include path while building.
See [tests/afs_config.h](tests/afs_config.h) for an example.

## Example

```c
#include "afs/afs.h"

#include <stdio.h>

#define BLOCK_SIZE              (4 * 1024 * 1024)
#define NUM_BLOCKS              8192
#define READ_WRITE_SIZE         512
#define SUB_BLOCKS_PER_BLOCK    256

AFS_HANDLE_DEF(m_afs);
AFS_OBJECT_HANDLE_DEF(m_object_handle);

static uint8_t m_read_write_buffer[READ_WRITE_SIZE];
static uint8_t m_lookup_table_buffer[AFS_LOOKUP_TABLE_SIZE(NUM_BLOCKS)];
static uint8_t m_object_buffer[1024];
static const afs_object_config_t OBJECT_CONFIG = {
    .buffer = m_object_buffer,
    .buffer_size = sizeof(m_object_buffer),
};

static void read_func(uint8_t* buf, uint16_t block, uint32_t offset, uint32_t length) {
    // Perform read
}

static void write_func(const uint8_t* buf, uint16_t block, uint32_t offset, uint32_t length) {
    // Perform write
}

static void erase_func(uint16_t block) {
    // Perform erase
}

int main(void) {
    const afs_init_t init_afs = {
        .storage_config = {
            .block_size = BLOCK_SIZE,
            .num_blocks = NUM_BLOCKS,
            .sub_blocks_per_block = SUB_BLOCKS_PER_BLOCK,
            .min_read_write_size = READ_WRITE_SIZE,
            .read = read_func,
            .write = write_func,
            .erase = erase_func,
        },
        .read_write_buffer = m_read_write_buffer,
        .lookup_table_buffer = m_lookup_table_buffer,
    };
    afs_init(m_afs, &init_afs);

    const uint16_t object_id = afs_object_create(m_afs, m_object_handle, &OBJECT_CONFIG);
    const char* str = "Hello World";
    afs_object_write(m_afs, m_object_handle, 0x1, (const uint8_t*)str, sizeof(str));
    afs_object_close(m_afs, m_object_handle);

    afs_object_open(m_afs, m_object_handle, AFS_WILDCARD_STREAM, object_id, &OBJECT_CONFIG);
    char buffer[120];
    uint8_t stream;
    const uint32_t len = afs_object_read(m_afs, m_object_handle, (uint8_t*)buffer, sizeof(buffer), &stream);
    printf("Read data (len=%u, stream=%u): '%s'\n", len, stream, buffer);
    afs_object_close(m_afs, m_object_handle);

    return 0;
}
```

## Testing

You can run the test suite by running `make` within the `tests/` directory.

## License

AFS is provided under the MIT license. See [LICENSE.md](LICENSE.md) for more information.
