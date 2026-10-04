# NessoDB page format

Database pages have a fixed size of 4096 bytes. Multibyte integers are encoded in
little-endian order and are never written by copying C++ object representations.
This document describes page format version 1; compatibility is not guaranteed
until the project reaches a stable release.

## Common header

Every initialized page begins with a 32-byte common header.

| Offset | Size | Field |
|------:|-----:|-------|
| 0 | 4 | Magic bytes `MDBP` |
| 4 | 2 | Page format version (`1`) |
| 6 | 2 | Page type |
| 8 | 8 | Page identifier |
| 16 | 8 | Log sequence number |
| 24 | 4 | CRC32C checksum |
| 28 | 4 | Reserved; must be zero |

Page type `1` identifies the database header and is valid only for page `0`. Page
type `2` identifies a heap page, type `3` an index leaf, and type `4` an index
internal page. All three require a nonzero page identifier. The maximum unsigned
64-bit value is reserved as the invalid page identifier and must not be serialized.

The checksum covers the complete 4096-byte page while treating bytes 24 through 27
as zero. Pages use the CRC32C Castagnoli polynomial. The checksum is updated before
a page is written and verified before a page is decoded.

## Heap page

A heap page extends the common header with a 16-byte header.

| Offset | Size | Field |
|------:|-----:|-------|
| 32 | 2 | Number of slots |
| 34 | 2 | Offset where record data begins |
| 36 | 2 | Heap page format version (`1`) |
| 38 | 2 | Reserved; must be zero |
| 40 | 8 | Next heap page, or zero when absent |

The slot directory starts at offset 48 and grows toward the end of the page. Each
slot occupies four bytes: a 16-bit record offset followed by a 16-bit record size.
Slot identifiers are zero-based indexes into this directory.
Deleted slots contain a zero offset and size and may be reused by a later insert.

Record data grows backward from byte 4095 toward the slot directory:

```text
0                 32      48                         4096
+-----------------+-------+----------+    +---------------+
| common header   | heap  | slots -> |    | <- records    |
+-----------------+-------+----------+    +---------------+
```

The bytes between the end of the slot directory and the beginning of record data
are free space. Records are nonempty and packed without gaps. Slot ranges must not
overlap the directory, exceed the page, overlap one another, or leave unaccounted
space in the record area.

Readers reject unknown page versions, unknown page types, nonzero reserved fields,
invalid identifiers and inconsistent slot directories.

## B+ tree index pages

Leaf and internal index pages extend the common header with the same 32-byte index
header.

| Offset | Size | Field |
|------:|-----:|-------|
| 32 | 2 | Index page format version (`1`) |
| 34 | 2 | Number of entries |
| 36 | 2 | End of the slot directory |
| 38 | 2 | Beginning of entry data |
| 40 | 8 | Parent page, or zero for a root |
| 48 | 8 | Next leaf or leftmost child |
| 56 | 2 | Tree level (`0` for leaves, positive for internal pages) |
| 58 | 2 | Reserved; must be zero |
| 60 | 4 | Reserved; must be zero |

The slot directory begins at offset 64. Like heap slots, every four-byte entry
contains a 16-bit payload offset and size. Payloads are nonempty, packed without
gaps, and grow backward from the end of the page.

A leaf payload contains an encoded key followed by an eight-byte heap page ID and
a two-byte heap slot ID. Leaf entries are ordered by key and then record ID, so
duplicate index keys remain deterministic. Offset 48 links leaves in key order.

An internal payload contains an encoded separator key followed by the eight-byte
right-child page ID. Offset 48 contains the leftmost child. Separator keys are
nondecreasing and child page IDs within a page are distinct.

Encoded keys carry their own format version and value tags. Signed integers use
big-endian bytes after flipping the sign bit. Text uses a zero-byte escape and
terminator, preserving binary lexicographic order and embedded zero bytes. `NULL`
sorts after non-null values. Index page readers reject malformed keys, invalid or
self-referential links, inconsistent levels, unordered entries, overlapping
payloads, and unsupported versions.

## Database header page

Page `0` stores database-wide metadata after the common header.

| Offset | Size | Field |
|------:|-----:|-------|
| 32 | 4 | Page size (`4096`) |
| 36 | 4 | Reserved; must be zero |
| 40 | 8 | Total page count, including page `0` |
| 48 | 8 | Catalog root page, or zero when absent |
| 56 | 8 | Free-list head page, or zero when absent |

The remaining bytes are reserved and must be zero. Page references must be nonzero
and smaller than the stored page count. The free-list field is reserved for
future page reclamation and is currently zero in databases created by NessoDB.
