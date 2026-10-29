//! An async, `no_std`, allocation-free implementation of the Audigo File System (AFS).
//!
//! The on-disk format is identical to the C implementation's (see `DESIGN.md`).

#![cfg_attr(not(test), no_std)]

mod cache;
mod debug;
mod error;
mod format;
mod fs;
mod lookup;
mod read;
mod seek;
mod storage;
mod types;
mod write;

#[cfg(test)]
mod tests;

pub use error::Error;
pub use fs::{Afs, ListCursor, lookup_table_len};
pub use read::{ObjectReader, ReadPosition, StreamReader};
pub use storage::{Config, Storage};
pub use types::{NUM_STREAMS, ObjectId, Stream, StreamSet};
pub use write::{CloseError, ObjectWriter};
