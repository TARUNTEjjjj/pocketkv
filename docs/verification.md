# Verification

## Code-review fixes · 2026-11-15

A code review of the library, CLI, packaging and tests produced eighteen
findings. Main had already fixed three of them when these fixes were moved onto
`d14a540`: databases on closed standard descriptors (PR #2's `20c42f7`, which
`aefb245` had carried forward and `d14a540` merged; see "Closed standard
descriptors" below), the argument-size limit of `put` (`aefb245`, through
`put-stdin`), and this record's entry count (the dated sections below). This
change fixes the other fifteen, one of them in part (see the review notes under
"Independent integration checks"), and carries them into the newer features: a
snapshot's private file is close-on-exec from creation, each of its records takes
one `writev`, and its file and directory use the durable sync; `keys-prefix`,
`put-stdin` and `snapshot` close the database before they print, like every other
command. Private files now come from `mkostemp` instead of `mkstemp`, and a
descriptor it returns at 0–2 is still moved above 2 as described below.

On macOS ARM64 with Apple Clang 21, CMake 4.4.5 and Python 3.9.6, with warnings
treated as errors, three builds configured like the CI matrix legs passed all
nine CTest entries: Debug with AddressSanitizer plus UndefinedBehaviorSanitizer,
Release with custom `tools/bin`, `lib64` and `headers` install directories, and a
build with no build type. CI configures every build with
`-DPOCKETKV_BUILD_BENCHMARKS=ON`, which registers `benchmark-accounting`. The
README's Release and sanitizer builds leave that option off and passed the other
eight entries.

| Check | Coverage | Local result |
| --- | --- | --- |
| `storage-core` | Fifteen groups, 7,440 runtime checks | Pass in all five builds |
| `key-pages` | 357 checks | Pass in all five builds |
| `snapshots` | 28 checks | Pass in all five builds |
| `storage-faults` | Sixteen groups, 194 injected-fault checks; one group needs macOS `F_FULLFSYNC`, and sanitizer builds skip the two allocation groups (14 groups, 181 checks) | Pass in all five builds |
| `cli-help` | CLI can launch and print help | Pass in all five builds |
| `cli-workflows` | 356 command/format checks with Python; sanitizer builds skip the 20 that need a CLI build with an allocation limit (336 checks) | Pass in all five builds |
| `real-world-scenarios` | About 500 checks (493 to 508 in these runs); the count varies with completed writes during racing process termination | Pass in all five builds |
| `benchmark-accounting` | Only with `-DPOCKETKV_BUILD_BENCHMARKS=ON`: 106 checks of the benchmark's CSV rows, v1 record and file-byte arithmetic, `--directory` with an invalid `TMPDIR`, input validation and cleanup. They raise errors explicitly, so `python -O` cannot strip them | Pass in the three benchmark builds |
| `installed-consumer` | Move the installed package, run CLI put/get, then configure a separate consumer as the README describes (`CMAKE_PREFIX_PATH` alone for `lib`, plus `PocketKV_DIR` for other library directories), require that it used the moved package, and link and run it | Pass in all five builds, including the custom layout |

The same nine entries also passed with `F_FULLFSYNC` undefined at compile time,
which selects the plain-`fsync` path that every non-Apple platform takes (fault
suite: 15 groups and 175 checks, or 13 and 162 under sanitizers). The Linux GCC
and Clang legs themselves, and LeakSanitizer, run only in GitHub Actions and were
not run for this record.

The check lists and review notes under "Storage checks" and "Independent
integration checks" below were extended in place for these fixes. The 2026-11-01
paragraph and its table keep their historical counts.

## Closed standard descriptors · 2026-11-15

PR #2, the fix that keeps database descriptors off 0–2, was brought up to date
with main, which had already carried the same change forward on 2026-11-10.
After the merge no source or test file differs from main. The statements PR #2
added to the earlier record at the end of this page are kept there, and that
record's table keeps its 2026-11-01 counts.

On macOS ARM64 with Apple Clang 21, CMake 4.4.5 and Python 3.9.6, all nine CTest
entries passed in each of the four CI matrix entries: no build type, Release
with `tools/bin`, `lib64` and `headers` for both the Linux GCC and macOS
entries, and Debug ASan/UBSan. Apple Clang stood in for the Linux compilers.
Warnings were treated as errors and benchmarks were enabled. Apple Clang has no
LeakSanitizer, so this is not a leak check. These counts were measured on the
merged suite and were identical in every configuration except where noted:

| Check | Coverage |
| --- | --- |
| `storage-core` | Twelve groups, 7,342 runtime checks |
| `key-pages` | 357 checks |
| `snapshots` | 28 checks |
| `storage-faults` | Eight groups, 59 injected-I/O checks |
| `cli-help` | CLI can launch and print help |
| `cli-workflows` | 268 command/format checks with Python |
| `real-world-scenarios` | 493 or more checks: each further write completed before a racing kill adds five and each repaired incomplete tail adds two; Release runs usually reported 508 |
| `benchmark-accounting` | Benchmark CSV accounting, input validation and cleanup |
| `installed-consumer` | Move the installed package, run CLI put/get, then configure/link/run a separate consumer |

The closed-descriptor regression cases cover creating and reopening a Store with
each of stdin, stdout and stderr closed, failed relocation under
`RLIMIT_NOFILE=3`, and the `init`, `put`, `get`, `get-raw`, `keys`, `stats`,
`verify`, `delete` and `repair` commands. The three commands added since PR #2
was written reach descriptors only through the same relocating paths: `snapshot`
creates its private file through the `mkstemp` wrapper and synchronizes the
directory through the `open` wrapper, `keys-prefix` only reads an opened Store,
and `put-stdin` reads standard input before any database is opened.

This was also exercised outside the automated suite. Fifty-eight command cases,
covering all twelve commands, their error paths, help and usage errors, were
each run with every combination of stdin, stdout and stderr closed, both by
closing the descriptors before exec and through a shell's `<&-`, `>&-` and
`2>&-`, against the Release and sanitizer builds. Every resulting file was
byte-identical to the same case run with all three open, and `verify` accepted
every database and snapshot except the deliberately damaged inputs. The one
intended difference is `put-stdin` with stdin closed, which stored nothing: it
exited with status 2 before opening the database. A descriptor-call trace of the
Release runs showed each `open` or `mkstemp` result at 0–2 moved above 2, and
the slot closed again, before any read, write, lock or sync. This is additional
evidence from one platform, not a substitute for suite coverage of the newer
commands.

## Submission review · 2026-11-13

Reviewed the storage and format implementation, CLI, snapshot publication,
installation configuration and existing failure tests at baseline `37826e2`.
No new storage correctness defect was reproduced in this review. The README now
distinguishes compact copies from future in-place compaction and describes
bounded payload buffering without claiming a single value-sized allocation.

On Linux x86-64 with GCC 14.2.0, CMake 4.4.4 and Python 3.12.14, all **nine CTest
entries passed** in both configurations below. Warnings were treated as errors
and benchmarks were enabled. Release also tested the relocated package with
custom binary, library and header directories.

```sh
cmake -S . -B build-review -DCMAKE_BUILD_TYPE=Release \
  -DPOCKETKV_WARNINGS_AS_ERRORS=ON -DPOCKETKV_BUILD_BENCHMARKS=ON \
  -DCMAKE_INSTALL_BINDIR=tools/bin -DCMAKE_INSTALL_LIBDIR=lib64 \
  -DCMAKE_INSTALL_INCLUDEDIR=headers
cmake --build build-review --parallel
ctest --test-dir build-review --output-on-failure

cmake -S . -B build-sanitize-review -DCMAKE_BUILD_TYPE=Debug \
  -DPOCKETKV_SANITIZERS=ON -DPOCKETKV_WARNINGS_AS_ERRORS=ON \
  -DPOCKETKV_BUILD_BENCHMARKS=ON
cmake --build build-sanitize-review --parallel
ASAN_OPTIONS=detect_leaks=0 ctest --test-dir build-sanitize-review --output-on-failure
```

The suites cover storage recovery/corruption, key pagination, snapshots, injected
I/O failures, CLI help/workflows, real-process scenarios, benchmark accounting and
an installed external consumer. The initial sanitizer run could not complete
LeakSanitizer's process inspection in this execution environment. Only leak
detection was disabled for the successful rerun; AddressSanitizer and
UndefinedBehaviorSanitizer remained enabled. This is **not a passing local leak
check**. The normal CI sanitizer configuration remains unchanged.

These are deterministic and process-failure checks, not physical power-loss,
network-filesystem or concurrent-reader guarantees. Earlier dated results below
describe their own source revisions and platforms.

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
- Zero bytes after the last complete record, from 1 byte to a 4,096-byte block,
  are an incomplete tail: a normal open preserves them, and repair removes exactly
  them, keeping a final record that ends in zero bytes of its own. Stale bytes,
  zeros followed by data or by a complete record, and a header followed by zeros
  stay corruption in both modes, and the error names the offset where the valid
  records end.
- Same-process, child-process, and hard-link alias lock contention. A child
  acknowledges a put then calls `_exit` without running the Store destructor;
  the parent can reopen and read it.
- Twenty rounds of eight synchronized first creators. Each child must either
  succeed or report lock contention. Every acknowledged record must survive and
  no temporary file may remain after normal creation/error handling. This is a
  scheduling stress test, not proof of every possible interleaving.
- A child-only `RLIMIT_FSIZE` forces partial initialization and partial append.
  Failed initialization leaves no destination or temporary file. A failed append
  poisons that Store and is truncated back to the acknowledged records, so the
  next open needs no repair and keeps earlier acknowledged data. Resource limits
  in the parent process are unchanged.
- Controlled sync failures (`fsync`, or `F_FULLFSYNC` on macOS) during
  initialization, put, erase, repair and directory synchronization. Tests verify
  writer rejection after failed sync, valid remaining data and cleanup, and that a
  put or erase whose sync failed is truncated away, with the truncation synced, so
  a reopen sees only acknowledged records.
- Each put and erase reaches the file in one `writev` call. An interrupted write,
  and short writes cut at every byte of a record, resume where they stopped and
  replay correctly. A snapshot makes one call for its file header and one for
  each record.
- In a sticky directory that other users can write, an existing database owned by
  another user, including a competing creator's, is refused without being changed
  or locked. One owned by the directory's owner, or kept where other users cannot
  add files, still opens. Wrappers report the foreign owner.
- Outside sanitizer builds, a replaced `operator new` fails each allocation of a
  put in turn: the put must fail before writing anything and leave the Store
  usable. Opening 18 records of one key must allocate exactly as much as opening 2.
- Every commit point (new header, new directory entry, put, erase, repair, and a
  snapshot's file and directory) must reach the durable sync: `F_FULLFSYNC` on
  macOS, never plain `fsync` while `F_FULLFSYNC` works. Where the filesystem
  rejects `F_FULLFSYNC` as unsupported, creation, puts and snapshots fall back to
  `fsync`; an `F_FULLFSYNC` EIO is reported, never retried as `fsync`.
- A new private file, a database's or a snapshot's, must already be close-on-exec
  when `mkostemp` returns it.
- Reopening a database whose directory sync failed, and losing a creation race to
  a rival that published a header-only file, both sync the directory; opening a
  database that has records does not.
- Injected EINTR (including on the directory sync), short reads, lookup read
  errors and startup read errors. Tests verify retries, byte preservation and
  release of locks after failed opens.
  Substitutions apply only to a separate test build of the Store; the production
  library calls ordinary system functions.
- Children with stdin, stdout or stderr closed create and reopen a Store. That
  descriptor must stay closed, so stray writes to it fail, and the file must hold
  exactly the acknowledged records.
- A child forked while a Store is open keeps its inherited copy while the parent
  destroys the Store: the parent can reopen, and the child can open a fresh Store,
  while the child is still alive. A child that destroys its inherited copy leaves
  the parent's lock in place.
- `/dev/fd/N` aliases of an open database, with and without `O_APPEND`, are
  refused without writing anything or leaving a lock on the caller's descriptor.

## Independent integration checks

The Python suite decodes the produced file with `struct` and `zlib.crc32`, checking
headers, payloads, updates, empty values and deletion records independently of the
C++ codec. It also exercises missing keys/files, invalid arguments, byte-escaped
keys, exact binary output, recovery, and complete corruption that repair must
refuse. A zero-filled tail after acknowledged records asks for repair, and repair
restores exactly the acknowledged bytes; other tails are refused with the offset
where the valid records end. CLI tests reject invalid keys before creating files and
check stdout failures for all output-producing commands, with stdout both
unwritable and closed (`>&-`). Saved mutations are verified after an
acknowledgement-output failure, including creation, updates, deletion and repair.
With stdout or stderr closed, the file must hold only acknowledged records.
Commands given the database as `/dev/fd/N`, page queries and snapshots included,
must fail without changing it or creating a copy. A `keys` listing of
4,096 long keys, a full `keys-prefix` page of 1,000 long keys and a 1 MiB `get-raw`
whose output nobody reads yet must release the lock by their first output byte, so a
concurrent `put` succeeds. Outside sanitizer builds, a test build of the CLI fails
every allocation above a limit, and runs the same `keys` and `get-raw` with limits
from a quarter of their output's size to three times it: each run must either print
the complete output and exit 0, or exit 2 with an error, and both outcomes must
occur. The README's command sequence was run on a fresh temporary database and its
shown output matched.

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
A later review found that a CLI started with stdout or stderr closed opened the
database on that descriptor and appended its own output to the log. Descriptors
now stay above 2, with library and CLI regression coverage.
The code review recorded under "Code-review fixes" above found that a child forked
while a Store was open kept the lock after the parent closed it; that a new database's
descriptor was briefly inheritable across exec; that macOS commits used
`fsync`, which does not flush the drive's cache; that a macOS `/dev/fd/N` path
bypassed `O_APPEND` and the lock; and that opens after a failed directory sync
never synced the directory. Each is fixed with regression coverage.
The same review found that a record whose sync failed stayed in the file for the
next open to adopt; that a record took three `write` calls; that a synced put
could still throw `std::bad_alloc`; that replay allocated twice per record; that an
existing database in a shared sticky directory was adopted whoever owned it; and
that CLI reads kept the lock while their output waited for a reader. These are
also fixed with regression coverage.
It also found that repair could not remove the zeros a crash can leave after the
last record, and that format errors gave no byte offset; that the benchmark
checks were bare `assert` statements, which `python -O` or `PYTHONOPTIMIZE`
strips; and that the consumer test always passed `PocketKV_DIR`, so the README's
prefix-only discovery was never tested (it fails for `tools/lib`, and for `lib64`
on Debian and Ubuntu). These are fixed with regression coverage, and the README is
corrected. A final record torn after its header, and stale bytes after the last
record, still stop every command, `repair` included; the README describes the
manual cut that recovers the earlier records.

## What these checks do not establish

No physical power-cut tests, device fault injection, network-filesystem tests,
or throughput claims are included. Syscall substitutions and the partial-write
test model selected operating-system failures, not every storage failure. CRC32
does not provide cryptographic integrity. Compaction and concurrent-reader support
are not implemented.
