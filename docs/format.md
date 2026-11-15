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
are consistent: magic prefix, operation, flags/reserved bytes, all present length
bytes, and any present header-checksum bytes. A partial length must have at least
one possible completion in its allowed range. For example, key-length bytes
`00 00` cannot become a valid key length: the completed number is either zero or
at least 65,536. A single `00` byte can still become 256–4,096 and is accepted.
Any nonzero value-length byte is invalid for an erase. A complete valid header whose
payload extends beyond end-of-file is also structurally incomplete. So is a run of
zero bytes from a record boundary to end-of-file, which is how space that a crash
extended but never wrote reads back: every record starts with `PKVR`, so those
bytes hold no record. Missing bytes cannot be checksum-validated. Repair discards
the whole incomplete record; normal opens reject it. Fully present malformed
records are corruption and are never discarded by repair, and neither is a tail
that mixes zeros with other bytes. That includes a complete final record whose
payload reads back as zeros after a crash tore it, and stale non-zero bytes after
the last record.

There is no resynchronization after an invalid record; the error names the byte
offset of the record that failed, where the valid records end. Keeping only the
bytes before that offset, by hand, recovers the earlier records and discards the
failing record and everything after it (the README shows the steps). CRC32 is a
corruption check, not a cryptographic guarantee against deliberate modification
or collisions.
