use core::fmt;

/// Errors returned by AFS operations
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Error<E> {
    /// The underlying storage returned an error
    Storage(E),
    /// The storage configuration is invalid
    InvalidConfig,
    /// A buffer has an invalid size
    InvalidBuffer,
    /// The object doesn't exist
    NotFound,
    /// The object (or, for wiping, any object) is open
    ObjectOpen,
    /// The maximum number of objects is already open
    TooManyOpen,
    /// There are no free blocks left (see [`Afs::is_full`](crate::Afs::is_full))
    StorageFull,
    /// The storage contents are not valid AFS data
    Corrupt,
    /// The debug output writer returned an error
    Fmt,
}

impl<E> From<fmt::Error> for Error<E> {
    fn from(_: fmt::Error) -> Self {
        Self::Fmt
    }
}

impl<E: fmt::Display> fmt::Display for Error<E> {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        match self {
            Self::Storage(e) => write!(f, "storage error: {e}"),
            Self::InvalidConfig => f.write_str("invalid storage configuration"),
            Self::InvalidBuffer => f.write_str("invalid buffer size"),
            Self::NotFound => f.write_str("object not found"),
            Self::ObjectOpen => f.write_str("object is open"),
            Self::TooManyOpen => f.write_str("too many open objects"),
            Self::StorageFull => f.write_str("storage is full"),
            Self::Corrupt => f.write_str("unexpected storage contents"),
            Self::Fmt => f.write_str("formatter error"),
        }
    }
}

impl<E: fmt::Debug + fmt::Display> core::error::Error for Error<E> {}
