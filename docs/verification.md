# Verification

Local verification on 2026-11-01 used Apple Clang 21 on macOS ARM64, CMake 4.4.5,
and Python 3.9.6. Both Release and Debug with AddressSanitizer plus
UndefinedBehaviorSanitizer passed all four CTest entries with compiler warnings
treated as errors. The same suite runs in [GitHub Actions](https://github.com/TARUNTEjjjj/pocketkv/actions).

| Check | Coverage | Local result |
| --- | --- | --- |
| `storage-core` | Nine groups, 6,624 runtime checks | Pass in both builds |
| `cli-help` | CLI can launch and print help | Pass in both builds |
| `cli-workflows` | 105 command/format checks with Python | Pass in both builds |
| `installed-consumer` | Install package, configure a separate project, link, write, close, reopen, read and erase | Pass in both builds |

## Storage checks

- Persistence after reopen; replacements and tombstones; sorted keys; live and
  cumulative statistics; absent-key deletion as a no-op.
- Embedded NUL and non-ASCII bytes, empty values, exact key/value size limits,
  rejected oversized inputs without file modification.
- Independent binary fixture generation and the standard IEEE CRC32 check value.
- 320 deterministic random operations compared with a separate `std::map` and
  byte/record ledger, including eight open/reopen batches.
- Every byte cut inside final put and erase records: ordinary opens preserve the
  damaged file; explicit repair discards exactly the incomplete record and
  supports subsequent writes and reopen.
- Corrupt magic, version, checksums, operation, reserved fields, invalid lengths,
  deletion payloads, garbage suffixes, and every short file-header length are
  rejected without modifying the file, including in repair mode.
- Same-process, child-process, and hard-link alias lock contention. A child
  acknowledges a put then calls `_exit` without running the Store destructor;
  the parent can reopen and read it.
- Twenty rounds of eight synchronized first creators. Each child must either
  succeed or report lock contention. Every acknowledged record must survive and
  no temporary file may remain after normal creation/error handling. This is a
  scheduling stress test, not proof of every possible interleaving.
- A child-only `RLIMIT_FSIZE` forces partial initialization and partial append.
  Failed initialization leaves no destination or temporary file. A failed append
  poisons that Store; reopening requires explicit repair, which preserves earlier
  acknowledged data. Resource limits in the parent process are unchanged.

## Independent integration checks

The Python suite decodes the produced file with `struct` and `zlib.crc32`, checking
headers, payloads, updates, empty values and deletion records independently of the
C++ codec. It also exercises missing keys/files, invalid arguments, byte-escaped
keys, exact binary output, recovery, and complete corruption that repair must
refuse. The README's command sequence was run on a fresh temporary database and
its shown output matched.

The installed-consumer check discovered an exported target-name collision between
the CLI and static library; the CLI now exports as `PocketKV::cli`, while clients
link `PocketKV::pocketkv`. Independent review also found a first-creation race;
initialization now locks and syncs a private file before atomic publication.

## What these checks do not establish

No physical power-cut tests, device fault injection, fsync-error injection,
network-filesystem tests, or throughput claims are included. The partial-write
test models an operating-system write failure, not every storage failure. CRC32
does not provide cryptographic integrity. Compaction and concurrent-reader support
are not implemented.
