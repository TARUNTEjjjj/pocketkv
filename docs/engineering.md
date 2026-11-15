# Engineering decisions

## Append-only data and an offset index

The file is the source of truth. A `std::map` holds each live key and the location,
lengths, and checksum of its most recent put. Values stay on disk. Reopening costs
O(total log bytes + record count × log(live keys)); index memory is O(live key bytes
+ live key count). Replay reads every record into one reused buffer and copies a
key only when it enters the index, so its allocations follow live keys rather than
history. Lookup finds an offset in O(log(live keys)), then reads and
validates that record. A put/erase appends one record and requests synchronization.
Actual latency includes filesystem work and the sync: `fsync`, or
`fcntl(F_FULLFSYNC)` on macOS.

Keeping the ordered map makes `keys` deterministic. The format is deliberately
small enough to inspect with independent tools. Separate header and payload
checksums prevent unchecked record lengths from controlling allocations.

## Operation ordering

1. Validate arguments and ensure the file's observed size matches the index. A put
   of a new key also allocates its index node now.
2. Encode the record and append its header, key, and value with one `writev` call,
   resuming after interrupted and short writes until complete or failed. With a
   single call, a signal that kills the process cannot land between the header
   and payload writes; a kill during a large write, a full disk or a power cut can
   still leave an incomplete final record.
3. Call `fsync` (`fcntl(F_FULLFSYNC)` on macOS) and require success.
4. Update the index and counters, then report success. Nothing here allocates, so
   a synced mutation always returns normally; running out of memory surfaces as
   `std::bad_alloc` before anything is written.

If step 2 or 3 fails, the store truncates the file back to the last acknowledged
record, syncs that, and becomes unusable until reopened. Otherwise the next open
would adopt the failed record from the page cache: after a failed writeback, Linux
can keep those pages readable without ever writing them, and once the cache drops
them, every record acknowledged after the failed one becomes unreachable. If the
truncation or its sync fails too, the suffix may be absent, partial, or complete;
the store stays unusable rather than serve an index known to disagree with the
file, but a failed mutation has no all-or-nothing guarantee. A missing-key erase
is a no-op.

RAII closes descriptors on every exception path. Creation initializes and locks
a private temporary file in the destination directory, syncs its header, then
publishes it using a no-clobber hard link. This prevents another opener from
observing a half-initialized database. Competing creators open the winner's file
after discarding their own temporary files. New files use mode `0600` (subject to
umask); existing files are never truncated at open. In a sticky directory that
other users can write, whether world-writable such as `/tmp` or group-writable, an
existing file (including a competing creator's) is adopted only if it belongs to
the caller or to the directory's owner. Otherwise another user could plant a valid
database that collects the caller's records; it is rejected with
`ErrorCode::invalid_argument` before it is locked. Linux applies the same rule to
`O_CREAT` opens with `fs.protected_regular=2`; level 1, the default on many
distributions, exempts group-writable directories. Ownership is not checked
where other users cannot add files, or where they could replace any file anyway
(a writable directory without the sticky bit).
Creation requires permission to create and remove entries in the parent
directory, read permission to open and sync it, and support for hard links.
Opening a database that still holds only its header also requires read
permission on the parent directory, because the Store opens and syncs it (see
Durability limits); without it, every command on such a file fails with "cannot
open database directory".
Normal error unwinding removes the private temporary file. A process killed
during creation can leave a `.pocketkv-*` temporary entry; it never publishes
a partial header at the requested database path. A failure after publication
can leave a valid database even though its constructor reported an error; the
next Store to open that header-only file syncs the directory itself.
`O_NOFOLLOW` rejects a final-component symlink. Intermediate directory symlinks
are not prohibited. Non-regular files are rejected. `O_CLOEXEC` prevents a
descriptor leak across exec; `O_NONBLOCK` prevents FIFO opens from hanging before
the file-type check.
Newly created temporary files come from `mkostemp(O_CLOEXEC)`, so they are
close-on-exec from the moment they exist and another thread's fork/exec cannot
inherit them.
Descriptors never stay at 0–2. If a process starts with stdin, stdout or stderr
closed (a shell's `>&-`), `open` and `mkostemp` return that free slot, and the
program's own console output would be appended to the log. The Store moves such a
descriptor above 2 with `F_DUPFD_CLOEXEC` and closes the original, so those writes
keep failing and the CLI reports a closed stdout as an output error. Do not write
to a closed standard stream from another thread while a Store opens.
A Store needs its own open file description. macOS opens `/dev/fd/N` as a
duplicate of the caller's descriptor `N`, which lacks `O_APPEND` (records would
overwrite the log at the caller's offset) and shares the caller's lock. Before
locking an existing file, the Store therefore requires `O_APPEND` to be in effect
and, on macOS, the path's own metadata to match the opened file; a `/dev/fd` path
reports devfs metadata, so it is rejected with `ErrorCode::invalid_argument`. On
Linux, `open` always creates a new description and `/dev/fd/N` is a symlink,
which `O_NOFOLLOW` already refuses.

## Locking and concurrency

The database descriptor holds an exclusive nonblocking `flock` for the Store's
lifetime. A separate opener fails promptly with `ErrorCode::locked`. Locking the
data inode avoids a stale lock-file protocol, but pathname replacement would
introduce a new inode: do not rename, unlink, replace, or externally modify an
open database. Hard links refer to the same inode and therefore the same lock on
supported local filesystems. A Store has no internal thread synchronization and
must not be reused across fork. A fresh child-process open still respects an
existing parent lock.
A `flock` belongs to the open file description, which `fork` shares with the
child, so closing the descriptor alone would leave the database locked until every
forked copy closed too. The Store therefore unlocks explicitly (`LOCK_UN`) when it
is destroyed in the process that locked it. A child destroying its inherited copy
only closes that copy's descriptor; unlocking there would release the parent's
lock.

All opens use `O_RDWR`, even if the caller only reads. This is an intentional first
version constraint. Read-only access, shared reader locks, and compaction require
a broader concurrency protocol and remain future work.

## Recovery boundary

Replay never edits the file unless the caller explicitly selects repair. Repair
only trims a final structurally incomplete record after validating its available
header fields, then syncs the truncation. It cannot prove why that suffix is
incomplete, and cannot verify bytes that are missing. It refuses invalid full
headers, checksum failures in complete payloads, and an incomplete file header.
It never skips a bad record to search for a later one.

Zero bytes from a record boundary to the end of the file count as an incomplete
record too. A crash can leave a file whose size covers an append that never
reached the disk (ext4 with `data=writeback`, or a drive cache that reorders
writes), and those blocks read back as zeros. Every record starts with nonzero
magic, so such zeros can neither hold a record nor hide a later one, and
discarding them loses no record bytes. The check runs only when a record boundary
starts with a zero byte, so valid files pay nothing for it. Zeros followed by
other bytes, a present header followed by zeros, and non-zero stale bytes stay
corruption: treating them as crash debris would mean guessing about bytes that
are present, and would make partial headers with impossible all-zero lengths (next
paragraph) repairable again. Format errors name the byte offset of the failing
record, which is where the valid records end. Such a file, including one whose
final record a crash tore after its header (later blocks reading back as zeros),
is recovered only by a manual cut at that offset, which discards the failing
record and everything after it; the README shows the steps.

Present bytes of an incomplete little-endian length constrain every possible
completion. The validator computes the smallest completion in the allowed range
and rejects the prefix if none exists. This also rejects all-zero partial key
lengths once any nonzero missing byte would exceed the key limit. It preserves
genuine partial headers at valid length boundaries.

Reads validate the indexed header, payload checksum, and key. Size checks detect
external growth/truncation. These are useful diagnostics, not protection against
uncooperative concurrent edits or malicious file substitution.

## Durability limits

Successful mutations request synchronization using `fsync`. On macOS, `fsync`
only hands the data to the drive, which may keep it in a volatile cache and write
it later or out of order, so every sync uses `fcntl(F_FULLFSYNC)`, which also
flushes that cache. A filesystem that does not support `F_FULLFSYNC` rejects the
request itself (`ENOTSUP`, `EOPNOTSUPP`, `ENOTTY`, `EINVAL` or `ENODEV`); the
Store then falls back to `fsync`. Any other `F_FULLFSYNC` failure, such as EIO, is
an error and is never retried as `fsync`. A full flush costs milliseconds per
mutation on macOS, where a plain `fsync` costs tens of microseconds.
Database creation syncs both its initial header and its parent directory; any
sync failure is an error. A creator publishes its header-only file before it
syncs the directory, so a Store that opens a database still holding only its
header (an existing file, or one a competing creator published first) also syncs
the parent directory before using it. This covers a creator that failed or died
between publication and its directory sync. A snapshot syncs its copy and the
copy's directory the same way, but a copy that already holds records is outside
that header-only rule: if its directory sync fails, which the snapshot reports, or
the process dies before it, later opens of the copy do not repeat the sync.
Device caches, filesystem behavior, and OS guarantees matter. This project does
not claim physical power-loss durability or network-filesystem support.
The tests distinguish a child process exiting without a Store destructor from
actual system power loss; only the former is exercised.

A separate test executable compiles the Store with `fsync`, `pread`, `mkostemp`,
`writev`, `fstat` and `geteuid`, plus `fcntl` on macOS, redirected to controlled
wrappers. The wrappers inject EIO, EINTR, short reads and writes and an
unsupported `F_FULLFSYNC`, simulate a competing creator and a database owned by
another user, and count the syncs and writes that reach the system; then they
call the real system functions. Tests verify that every commit point reaches the
durable sync, that each record takes one `writev`, that failed syncs are never
acknowledged and are truncated away, that later operations reject an uncertain
writer, and that a foreign database in a shared sticky directory is refused. Read
failures preserve the file and failed constructors release their locks. Outside
sanitizer builds, which bring their own allocator, the executable also replaces
`operator new` to fail allocations during a put and to count replay allocations.
Production library objects are compiled without the substitutions. These tests
simulate syscall outcomes; they do not simulate a physical power failure.
The test-only forced include header loads system headers before defining substitutions, so fortified
inline libc wrappers cannot replace the injected calls in optimized Linux builds.

## CLI and installation

The CLI checks key/value bounds before opening a Store. Each command closes its
Store, releasing the lock, before it prints, so a consumer that stops reading the
output cannot lock other commands out. The command keeps the results themselves
(a value, a key list or page, counts) until it prints them, not text formatted
into a buffer, so a large value or listing is not copied again, and running out
of memory while printing is reported like any other output failure instead of
truncating the output. The CLI also flushes and checks stdout for all output
commands. A failure writing a mutation's acknowledgement returns status 2 after
the data operation may already have completed; there is no rollback implied by
that output error.
Writing to a closed pipe instead ends the command with SIGPIPE (shells report
status 141), and a mutation may already be saved in that case too.

The CMake package exports the configured header directory. Installation tests move
the prefix, run CLI put/get commands, and build a separate library consumer,
configured as the README describes: `CMAKE_PREFIX_PATH` alone for the default `lib`
directory, plus `PocketKV_DIR` for any other library directory. Under a prefix,
`find_package` searches `lib` everywhere, `lib64` only on some platforms (CMake
skips it on Debian and Ubuntu), and a directory such as `tools/lib` never. The test
also requires that the consumer used the moved package, not another installation the
search reached first. CI checks both the default layout and custom `tools/bin`,
`lib64`, and `headers` directories. Isolated installation tests are skipped for
absolute destinations because `cmake --install --prefix` cannot redirect them into
scratch space.

Useful system references:

- [Linux fsync](https://man7.org/linux/man-pages/man2/fsync.2.html)
- [Linux flock](https://man7.org/linux/man-pages/man2/flock.2.html)
- [Apple fsync documentation](https://developer.apple.com/library/archive/documentation/System/Conceptual/ManPages_iPhoneOS/man2/fsync.2.html)
