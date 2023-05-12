# AFS Design

The Audigo File System (AFS) is a very simple logging file system which is highly optimized for storing audio data.

## Requirements

The requirements which AFS aims to satisfy are as follows:
* Bounded RAM usage based on the size of the underlying storage
* Highly-predictable performance
* Limited erase cycles of the underlying storage
* Fast listing of objects
* No performance hit from having either many objects or very large objects or both

## Assumptions and Constraints

AFS is built around the concept of objects (roughly equivalent to files) and heavily relies on the following key
assumptions for individual objects:
* They are always written sequentially
* They are only written once and are never modified in place
* They are generally at least multiple MB in size

### SD Cards

AFS is designed to be deployed on large SD cards, which have a built in flash translation layer (FTL). The FTL has many
benefits such as handling wear leveling and error detection. However, if not properly designed for, it can also
introduce significant performance constraints based on how it's swapping out blocks of flash under-the-hood. In general,
the FTL splits up the underlying flash into allocation units which are typically 4MB in size. If new data is written
to an AU over existing data, the FTL will need to allocate a new block of flash to write the new data to, and then copy
over the remaining data from the previous flash block, which can be very slow. It may also need to erase an unused flash
block before it can start writing to it, which can also be very slow.

## Design

AFS splits the available storage into blocks which are intended to align with the underlying allocation unit of the
storage device (typically 4MB). All blocks must be the same size, and this size should never change for a given device.

Each block is assigned to a single object. One of the downsides of this approach is that this means that storage space
is consumed in 4MB chunks, so objects always consume at least 4MB of storage space. AFS keeps an in-memory lookup table
containing information on what is stored in each block. Because each block is so large, this lookup table can be kept
relatively small, even for very large SD cards. For example, a 32GB SD card requires a lookup table of just 32KB.

### Object ID

Each object is assigned a unique ID. The ID should be considered a random (non-zero) 16-bit value, but is guaranteed to
be locally unique across every object currently stored within the file system. Note that these IDs may be reused over
time as objects are deleted and others are created, and this does put a 2^16-1 limit on the number objects which can be
stored within the file system at any given time.

### Streams

Within an object, there can be multiple streams of data. For example, Wavpack can be configured to generate two streams
of compression data (a lossy stream and a correction stream which can be combined to get lossless data). There could
also be metadata being generated alongside audio data. For efficiency, we want to write these as a single object, since
they are generated at the same time. However, we need to be able to treat them separately within the object. Therefore,
we allow for up to 16 streams of data to be stored as part of a single object.

### Block Header

Every block starts with a block header which has the following fields:
* Magic (4 bytes) - Always the 4 characters "AFS1"
* Object ID (2 bytes) - The ID of the object stored in this block
* Object Block Index (2 bytes) The index of this block within the object

### Chunk Header

Data is written to a block in chunks. Each chunk starts with a 4-byte header called a tag, with the upper 8 bits of the
tag representing the type, and the the lower 24 bits indicating the length of data which follows the chunk header.
Chunks never span multiple blocks, but otherwise have no alignment requirements. The first chunk always starts
immediately after the block header, and the next chunk always starts immediately after that one. Iterating through the
chunks in a block is done by reading each header and then advancing by the length plus 4 bytes (for the chunk header)
forward in the block.

#### Data Chunk (Type 0xd*)

A data chunk contains the actual data which belongs to an object, and the header is followed by 1 or more bytes of data.
The lower 4 bits of the type are used to indicate the stream to which the data chunk belongs.

#### Offset Chunk (Type 0x3e)

An offset chunk is used to allow for faster seeking within an object. It is the first chunk in all blocks of an object
except for the first one, and is followed by an 8-byte value for each stream within the object which indicates the total
amount of data written to that stream so far in the object.

#### End Chunk (Type 0xed)

An end chunk marks the end of an object. It has no data following it, and is always the last valid chunk in a block.

#### Invalid Chunk (Type 0xff and 0x00)

It is assumed that the erased state of the storage has either all bytes set to 0xff or 0x00. Therefore, both of these
types are invalid, and indicate that there are no remaining chunks within the block. The length portion of the chunk
header is ignored for invalid chunks.

### Lookup Table

AFS maintains a lookup table containing the object ID and block index for every block of the underlying storage. The
benefits of this approach are that when writing, no I/O is required in order to find a free block, and when reading, no
I/O is required in order to either get a list of objects or find the next block for a given object. The disadvantage is
that building this lookup table is relatively expensive, as the block header must be read from every block. However, in
practice this is a fixed and relatively-small startup latency.

### Buffers

There are many memory buffers used in a few different places within AFS. AFS uses a read/write buffer to read block
headers and perform other file system maintenance operations. Also, each open object is configured with a buffer which
is used to optimize the size of the read/write operations to the underlying storage.

## Examples

The follow examples demonstrate how AFS manages data on the underlying storage. Note that for the purpose of these
examples, we are assuming there is no buffering and we write directly to storage, but in practice this isn't the case.

### Object Creating / Writing

When an object is created, AFS first finds a free block to allocate to the object by looking for the first entry in the
lookup table with a value of 0 (indicating it's free). Let's assume block 0 is free and that the object ID of this new
block is 1234. The first step is to write the block header to the start of the block:

```
           BLOCK 0 (4MB)
.---------------------------------.
|             "AFS1"              |
|----------------+----------------|
|      1234      |       0        |
|----------------+----------------|
|                                 |
|                                 |
'---------------------------------.
```

Next, let's write 8 bytes of data ("ABCDEFGH") to the object. In order to do this, we need to first write a data chunk
header followed by the data itself.

```
           BLOCK 0 (4MB)
.---------------------------------.    -
|             "AFS1"              |    |
|----------------+----------------|    | Block Header (object_id=1234, object_block_index=0)
|      1234      |       0        |    |
|---------------------------------|    -
|           0xd0000008            |    |
|----------------+----------------|    |
|             "ABCD"              |    | Data Chunk (stream=0, length=8, data="ABCDEFGH")
|---------------------------------|    |
|             "EFGH"              |    |
|---------------------------------|    -
|                                 |
|                                 |
'---------------------------------.
```

Let's skip ahead and assume we fill the rest of the block with more data. What happens when we try to write more data
than will fit in this block? Let's assume when there is only 5 bytes of space left in the block, we attempt to write the
string "IJKL" to the end of the block. There isn't enough room to fit another chunk header (4 bytes) plus our data (4
bytes), so we look for the next free block (let's assume it's block 3), and write out another block header followed by
an offset chunk and then our data chunk:

```
           BLOCK 3 (4MB)
.---------------------------------.    -
|             "AFS1"              |    |
|----------------+----------------|    | Block Header (object_id=1234, object_block_index=1)
|      1234      |       1        |    |
|---------------------------------|    -
|           0x3e000008            |    |
|----------------+----------------|    | Offset Chunk (length=8, offset=[(0x0, 0x0000000003FFA73)])
|       0x00000000003FFA73        |    |
|                                 |    |
|----------------+----------------|    -
|           0xd0000004            |    |
|----------------+----------------|    | Data Chunk (stream=0, length=4, data="IJKL")
|             "IJKL"              |    |
|----------------+----------------|    -
|                                 |
|                                 |
'---------------------------------.
```

Notice that our object_block_index field is now set to 1, indicating that this is the second block within this object.
Next, let's say we are done writing to this object, so we close it. At this point, we write an end chunk to indicate
that we've reached the end of the object:


```
           BLOCK 3 (4MB)
.---------------------------------.    -
|             "AFS1"              |    |
|----------------+----------------|    | Block Header (object_id=1234, object_block_index=1)
|      1234      |       1        |    |
|---------------------------------|    -
|           0x3e000008            |    |
|----------------+----------------|    | Offset Chunk (length=8, offset=[(0x0, 0x0000000003FFA73)])
|       0x00000000003FFA73        |    |
|                                 |    |
|----------------+----------------|    -
|           0xd0000004            |    |
|----------------+----------------|    | Data Chunk (stream=0, length=4, data="IJKL")
|             "IJKL"              |    |
|----------------+----------------|    -
|           0xed000000            |    | End Chunk
|---------------------------------|    -
|                                 |
|                                 |
'---------------------------------.
```

### Object Reading

When we later want to read out that object, we start by looking in our lookup table for which block contains an
object_id of 1234, and an object_block_index of 0. Using the data from the writing example, this would be block 0. We
then iterate over all the chunks within the object, copying the data from the data chunks into the application's read
buffer and returning it. When we reach the end of block 0, we again check our lookup table to now find the block
containing an object_id of 1234 and an object_block_index of 1, which is block 3, and we continue iterating over the
chunks in block 3.

If we want to seek to a specific point in the object which has many blocks, we first need to determine which block
contains the portion of the object which we're looking for. This is done by performing a binary search and reading the
offset chunk from the start of each block (other than the first one, which has an offset of 0). Then, we iterate through
the chunks of the block we want to find the position of the data we're looking for.
