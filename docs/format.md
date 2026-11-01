# PocketKV file format, version 1

All integers are unsigned and little-endian. Checksums use IEEE CRC32 (reflected
polynomial `0xedb88320`, initial state `0xffffffff`, final XOR `0xffffffff`). The
check value for ASCII `123456789` is `0xcbf43926`. Structures are encoded byte by
byte; the format does not depend on C++ struct padding or host byte order.

## File header (16 bytes)

| Offset | Bytes | Meaning |
| --- | --- | --- |
| 0 | 8 | ASCII `POCKETKV` |
| 8 | 4 | Version, currently `1` |
| 12 | 4 | CRC32 of bytes 0–11 |

An empty database contains this header alone. An empty file or incomplete file
header is invalid, including in repair mode.

## Record header (24 bytes)

| Offset | Bytes | Meaning |
| --- | --- | --- |
| 0 | 4 | ASCII `PKVR` |
| 4 | 1 | Operation: `1` put, `2` erase |
| 5 | 1 | Flags, must be zero |
| 6 | 2 | Reserved, must be zero |
| 8 | 4 | Key length, 1–4,096 |
| 12 | 4 | Value length, 0–1,048,576; zero for erase |
| 16 | 4 | CRC32 of concatenated key and value bytes |
| 20 | 4 | CRC32 of header bytes 0–19 |

The key immediately follows the header; the value follows the key. No terminator,
padding, alignment, or text encoding is implied. A record consumes
`24 + key_length + value_length` bytes. An erase contains its key and no value.

## Replay and validation

Scan sequentially from byte 16. Validate a complete header checksum before using
its lengths; then validate operation, reserved fields and bounds. Read its payload
and check the payload checksum. Put replaces the key's latest offset in the index;
erase removes it. Deleting a missing key through the API writes no record, though
a valid externally encoded erase for an absent key is a replay no-op and still
counts as a log record.

A short final header is eligible for explicit repair only if its available fields
are consistent: magic prefix, operation, flags/reserved bytes, complete length
fields, and any present header-checksum bytes. A complete valid header whose
payload extends beyond end-of-file is also structurally incomplete. Missing bytes
cannot be checksum-validated. Repair discards the whole incomplete record; normal
opens reject it. Fully present malformed records are corruption and are never
discarded by repair.

There is no resynchronization after an invalid record. CRC32 is a corruption check,
not a cryptographic guarantee against deliberate modification or collisions.
