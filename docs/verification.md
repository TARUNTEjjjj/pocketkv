# Verification

## Binary imports and process recovery · 2026-11-10

On macOS ARM64 with Apple Clang 21, CMake 4.4.5 and Python 3.9.6, all eight
CTest entries passed in Release and Debug ASan/UBSan builds, with warnings
treated as errors and benchmarks enabled. Release also exercised relocated
installation using `tools/bin`, `lib64` and `headers`. The core suite passed
7,342 checks across 12 groups, key pages 357, fault injection 49, and CLI
integration 242. The new `real-world-scenarios` suite passed 508 checks in Release
and 495 under sanitizers; its count varies with completed writes during racing
process termination.

The new suite runs real CLI and library processes against disposable data:

- A 24-session workload inserts JSON values, updates half, deletes every third
  session, reopens for each command, and pages through surviving keys. A Python
  dictionary and independent CRC decoder verify the live state and full log.
- File and pipe imports round-trip empty values, embedded NUL/high bytes,
  trailing newlines and the full 1 MiB limit. Bad keys, wrong argument counts,
  closed/unreadable stdin and oversized input preserve existing bytes and leave
  absent databases absent. Oversized input is rejected with the pipe still open.
- An import waiting for EOF leaves the database accessible. Killing that import
  preserves every existing byte.
- A worker acknowledges eight fsync-completed writes before SIGKILL. All survive
  reopening; a competing process is rejected while the owner is alive and can
  write after its death. Three additional racing kills permit at most one
  complete unacknowledged append. Strict reopening never changes bytes, and only
  an incomplete tail can require explicit repair.
- Closed standard streams never become database descriptors. A child with
  `RLIMIT_NOFILE=3` forces descriptor relocation to fail, checking temporary-file
  cleanup, preserved data and released locks.

Before the descriptor fix, a `put` started with stdout closed exited successfully
but appended its acknowledgement to the database; subsequent verification failed
with invalid record magic. This regression now passes. The fix carries forward
the pending change from PR #2 onto the current main branch.

Run the application scenarios alone with
`ctest --test-dir build -R real-world-scenarios --output-on-failure`.
These are process-death and failure-handling tests on synthetic data. Racing
kills do not guarantee a particular interrupted syscall; deterministic partial
write and truncation tests remain in the core suite. No physical power loss is
simulated.

## Reliability and key discovery · 2026-11-09

Release and Debug ASan/UBSan builds, with warnings treated as errors and
benchmarks enabled, passed all seven CTest entries. The new `key-pages` suite
passed 357 checks against a separately filtered key ledger: binary/NUL prefixes,
exclusive cursors before/inside/after a prefix, absent and deleted cursor keys,
page chaining, boundary limits, invalid inputs, reopening and mutations between
pages. Reads leave record and byte counts unchanged. CLI integration passed
182 checks, including bounded output, raw cursor semantics, malformed limits,
missing databases, escaping and failed stdout writes.

The new fork regression failed before the ownership guard because an inherited
child handle accepted a write. With the guard, the core suite passes 11 groups
and 7,326 parent-process checks; child assertions additionally verify rejection
of reads, writes, missing-key operations, enumeration and statistics, retention
of the parent's lock, and successful use of a fresh child-owned database. Parent
data and subsequent writes survive reopening. The fault suite passes 49 checks,
including rejection of page reads after an uncertain write. This is targeted
regression evidence, not a physical power-loss or concurrency guarantee.

The sections below retain the earlier verification record and its historical
counts. GitHub Actions exercises the current suite on Linux GCC/Clang and macOS.

Local verification on 2026-11-01 used Apple Clang 21 on macOS ARM64, CMake 4.4.5,
and Python 3.9.6. Both Release and Debug with AddressSanitizer plus
UndefinedBehaviorSanitizer passed all five CTest entries with compiler warnings
treated as errors. A Release build using custom install directories also passed
all five entries. The same suite runs in [GitHub Actions](https://github.com/TARUNTEjjjj/pocketkv/actions).

| Check | Coverage | Local result |
| --- | --- | --- |
| `storage-core` | Ten groups, 7,320 runtime checks | Pass in both builds |
| `storage-faults` | Seven groups, 47 injected-I/O checks | Pass in both builds |
| `cli-help` | CLI can launch and print help | Pass in both builds |
| `cli-workflows` | 155 command/format checks with Python | Pass in both builds |
| `installed-consumer` | Move the installed package, run CLI put/get, then configure/link/run a separate consumer | Pass in both builds and custom layout |

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
- Thirteen impossible partial length prefixes remain byte-for-byte unchanged in
  normal and repair modes. All 23 header cuts for seven boundary-size records
  still recover correctly, including zero low bytes in valid lengths.
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
- Controlled `fsync` failures during initialization, put, erase, repair and
  directory synchronization. Tests verify writer rejection after failed sync,
  valid remaining data and cleanup, and complete uncertain writes on reopen.
- Injected EINTR, short reads, lookup read errors and startup read errors. Tests
  verify retries, byte preservation and release of locks after failed opens.
  Substitutions apply only to a separate test build of the Store; the production
  library calls ordinary system functions.

## Independent integration checks

The Python suite decodes the produced file with `struct` and `zlib.crc32`, checking
headers, payloads, updates, empty values and deletion records independently of the
C++ codec. It also exercises missing keys/files, invalid arguments, byte-escaped
keys, exact binary output, recovery, and complete corruption that repair must
refuse. CLI tests reject invalid keys before creating files and check stdout
failures for all output-producing commands. Saved mutations are verified after
an acknowledgement-output failure, including creation, updates, deletion and
repair. The README's command sequence was run on a fresh temporary database and
its shown output matched.

The installed-consumer check discovered an exported target-name collision between
the CLI and static library; the CLI now exports as `PocketKV::cli`, while clients
link `PocketKV::pocketkv`. Independent review also found a first-creation race;
initialization now locks and syncs a private file before atomic publication.

The follow-up review found that repair accepted impossible incomplete length
fields, help/version ignored output errors, invalid puts could create empty
databases, and the exported header path ignored custom install directories.
These are fixed with regression coverage. The relocated installation is also
tested with `tools/bin`, `lib64` and `headers` as custom destinations.
The Linux GCC run also caught libc inline wrappers bypassing the original read
fault hook. A test-only forced include header now loads system headers before applying
the substitutions; injection counters verify the fault path is actually exercised.

## What these checks do not establish

No physical power-cut tests, device fault injection, network-filesystem tests,
or throughput claims are included. Syscall substitutions and the partial-write
test model selected operating-system failures, not every storage failure. CRC32
does not provide cryptographic integrity. Compaction and concurrent-reader support
are not implemented.
