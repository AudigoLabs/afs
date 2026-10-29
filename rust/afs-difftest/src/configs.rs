use afs::Config;

use crate::diff::Params;

pub const PRIMARY: Params = Params {
    name: "primary",
    config: Config {
        block_size: 4 * 1024 * 1024,
        num_blocks: 8192,
        sub_blocks_per_block: 256,
        min_read_write_size: 512,
    },
    buffer_sizes: &[32 * 1024, 32 * 1024, 32 * 1024, 16 * 1024, 4096, 512, 64 * 1024],
    small_write: 2048,
    medium_write: 48 * 1024,
    large_write: 1536 * 1024,
    ops: 200,
    streams: 16,
    v1_objects: 3,
    v1_max_size: 6 * 1024 * 1024,
    full_dumps: false,
};

pub const SMALL: Params = Params {
    name: "small",
    config: Config { block_size: 64 * 1024, num_blocks: 12, sub_blocks_per_block: 8, min_read_write_size: 512 },
    buffer_sizes: &[512, 1024, 2048, 4096, 8192, 16 * 1024],
    small_write: 2048,
    medium_write: 24 * 1024,
    large_write: 200 * 1024,
    ops: 200,
    streams: 16,
    v1_objects: 4,
    v1_max_size: 150 * 1024,
    full_dumps: true,
};

pub const TINY: Params = Params {
    name: "tiny",
    config: Config { block_size: 4096, num_blocks: 16, sub_blocks_per_block: 4, min_read_write_size: 256 },
    buffer_sizes: &[256, 512, 1024, 2048],
    small_write: 400,
    medium_write: 3000,
    large_write: 16 * 1024,
    ops: 200,
    streams: 16,
    v1_objects: 4,
    v1_max_size: 12 * 1024,
    full_dumps: true,
};

pub const MIN_IO: Params = Params {
    name: "min_io",
    config: Config { block_size: 2048, num_blocks: 24, sub_blocks_per_block: 8, min_read_write_size: 128 },
    buffer_sizes: &[256, 512, 1024],
    small_write: 200,
    medium_write: 1500,
    large_write: 8000,
    ops: 200,
    streams: 14,
    v1_objects: 4,
    v1_max_size: 5000,
    full_dumps: true,
};

pub const SINGLE_SUB_BLOCK: Params = Params {
    name: "single_sub_block",
    config: Config { block_size: 16 * 1024, num_blocks: 20, sub_blocks_per_block: 1, min_read_write_size: 512 },
    buffer_sizes: &[512, 1024, 2048, 4096],
    small_write: 1500,
    medium_write: 8000,
    large_write: 64 * 1024,
    ops: 200,
    streams: 4,
    v1_objects: 3,
    v1_max_size: 40 * 1024,
    full_dumps: true,
};
