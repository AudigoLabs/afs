# Audigo File System

The Audigo File System (AFS) is a very simple logging file system which is highly optimized for storing audio data.

This repository contains an async, `no_std`, allocation-free implementation of AFS in Rust. Earlier versions of this
repository contained a C implementation of the same on-disk format.

## Version 2

Version 2 added support for seek chunks to speed up seeking within a block. This is to address slow seek performance in
version 1 in use-cases where there are thousands of chunks within a block. Version 2 is completely backwards compatible
with data written by version 1.

## Design

For information about the design of AFS, check out [DESIGN.md](DESIGN.md).

## Usage

The storage is accessed through the async `afs::Storage` trait, the `Afs` instance owns the storage, and all buffers
are provided by the caller. All errors, including unexpected storage contents, are returned as `afs::Error`, and at
most `MAX_OPEN` objects (a const generic parameter of `Afs` which defaults to 8) can be open at once.

```rust
use afs::{Afs, Config, Error, Storage, Stream, lookup_table_len};

const CONFIG: Config = Config {
    block_size: 4 * 1024 * 1024,
    num_blocks: 8192,
    sub_blocks_per_block: 256,
    min_read_write_size: 512,
};

async fn example<S: Storage>(storage: S, lookup_table: &mut [u32; lookup_table_len(8192)]) -> Result<(), Error<S::Error>> {
    let mut read_write_buffer = [0u8; 512];
    let mut object_buffer = [0u8; 1024];
    let mut afs: Afs<'_, S> = Afs::mount(storage, CONFIG, &mut read_write_buffer, lookup_table).await?;

    let mut writer = afs.create(&mut object_buffer)?;
    let object_id = writer.id();
    writer.write(&mut afs, Stream::S0, b"Hello World").await?;
    let object_buffer = writer.close(&mut afs).await.map_err(|e| e.error)?;

    let mut reader = afs.open(object_id, Stream::S0, object_buffer).await?;
    let mut buffer = [0u8; 120];
    let len = reader.read(&mut afs, &mut buffer).await?;
    reader.close(&mut afs);
    Ok(())
}
```

## Testing

You can run the test suite by running `cargo test`.

## License

AFS is provided under the MIT license. See [LICENSE.md](LICENSE.md) for more information.
