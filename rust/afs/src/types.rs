use core::fmt;
use core::num::NonZeroU16;

/// The number of streams within an object
pub const NUM_STREAMS: usize = 16;

/// A unique (non-zero) object ID
#[derive(Clone, Copy, Debug, PartialEq, Eq, PartialOrd, Ord, Hash)]
pub struct ObjectId(NonZeroU16);

impl ObjectId {
    /// Creates an object ID from its raw value (returns `None` for 0)
    pub const fn new(raw: u16) -> Option<Self> {
        match NonZeroU16::new(raw) {
            Some(id) => Some(Self(id)),
            None => None,
        }
    }

    /// Gets the raw value of the object ID
    pub const fn get(self) -> u16 {
        self.0.get()
    }
}

impl fmt::Display for ObjectId {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        self.0.fmt(f)
    }
}

/// A stream within an object
#[repr(u8)]
#[derive(Clone, Copy, Debug, PartialEq, Eq, PartialOrd, Ord, Hash)]
pub enum Stream {
    /// Stream 0
    S0 = 0,
    /// Stream 1
    S1 = 1,
    /// Stream 2
    S2 = 2,
    /// Stream 3
    S3 = 3,
    /// Stream 4
    S4 = 4,
    /// Stream 5
    S5 = 5,
    /// Stream 6
    S6 = 6,
    /// Stream 7
    S7 = 7,
    /// Stream 8
    S8 = 8,
    /// Stream 9
    S9 = 9,
    /// Stream 10
    S10 = 10,
    /// Stream 11
    S11 = 11,
    /// Stream 12
    S12 = 12,
    /// Stream 13
    S13 = 13,
    /// Stream 14
    S14 = 14,
    /// Stream 15
    S15 = 15,
}

impl Stream {
    /// All streams in order
    pub const ALL: [Stream; NUM_STREAMS] = [
        Stream::S0,
        Stream::S1,
        Stream::S2,
        Stream::S3,
        Stream::S4,
        Stream::S5,
        Stream::S6,
        Stream::S7,
        Stream::S8,
        Stream::S9,
        Stream::S10,
        Stream::S11,
        Stream::S12,
        Stream::S13,
        Stream::S14,
        Stream::S15,
    ];

    /// Gets the stream with the given index (returns `None` if it's not below [`NUM_STREAMS`])
    pub const fn from_index(index: u8) -> Option<Self> {
        if (index as usize) < NUM_STREAMS { Some(Self::ALL[index as usize]) } else { None }
    }

    /// Gets the index of the stream
    pub const fn index(self) -> u8 {
        self as u8
    }
}

impl TryFrom<u8> for Stream {
    type Error = u8;

    fn try_from(index: u8) -> Result<Self, u8> {
        Self::from_index(index).ok_or(index)
    }
}

impl From<Stream> for u8 {
    fn from(stream: Stream) -> u8 {
        stream.index()
    }
}

/// Type used to represent a stream bitmask
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq, Hash)]
pub struct StreamSet(u16);

impl StreamSet {
    /// No streams
    pub const EMPTY: Self = Self(0);
    /// All streams
    pub const ALL: Self = Self(u16::MAX);

    /// Creates a set from a bitmask (bit N is stream N)
    pub const fn from_bits(bits: u16) -> Self {
        Self(bits)
    }

    /// Gets the bitmask (bit N is stream N)
    pub const fn bits(self) -> u16 {
        self.0
    }

    /// Returns the set with the stream added
    pub const fn with(self, stream: Stream) -> Self {
        Self(self.0 | 1 << stream.index())
    }

    /// Returns whether or not the set contains the stream
    pub const fn contains(self, stream: Stream) -> bool {
        self.0 & 1 << stream.index() != 0
    }

    /// Returns whether or not the set is empty
    pub const fn is_empty(self) -> bool {
        self.0 == 0
    }
}

impl From<Stream> for StreamSet {
    fn from(stream: Stream) -> Self {
        Self::EMPTY.with(stream)
    }
}

impl FromIterator<Stream> for StreamSet {
    fn from_iter<I: IntoIterator<Item = Stream>>(iter: I) -> Self {
        iter.into_iter().fold(Self::EMPTY, Self::with)
    }
}
