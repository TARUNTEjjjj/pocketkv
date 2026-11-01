# Engineering decisions

## Append-only data and an offset index

The file is the source of truth. A `std::map` holds each live key and the location,
lengths, and checksum of its most recent put. Values stay on disk. Reopening costs
O(total log bytes + record count × log(live keys)); index memory is O(live key bytes
+ live key count). Lookup finds an offset in O(log(live keys)), then reads and
validates that record. A put/erase appends one record and requests synchronization.
Actual latency includes filesystem work and `fsync`.

Keeping the ordered map makes `keys` deterministic. The format is deliberately
small enough to inspect with independent tools. Separate header and payload
checksums prevent unchecked record lengths from controlling allocations.

## Operation ordering

1. Validate arguments and ensure the file's observed size matches the index.
2. Encode and append the header, key, and value, handling interrupted and short
   writes until complete or failed.
3. Call `fsync` and require success.
4. Update the index and counters, then report success.

The store becomes unusable after an append/sync failure because the suffix may be
absent, partial, or complete. If index allocation fails after a successful disk
write, the store also becomes unusable; reopening reconstructs state from disk.
This avoids serving an index known to disagree with the file. It does not give a
failed mutation an all-or-nothing guarantee. A missing-key erase is a no-op.

RAII closes descriptors on every exception path. Creation initializes and locks
a private temporary file in the destination directory, syncs its header, then
publishes it using a no-clobber hard link. This prevents another opener from
observing a half-initialized database. Competing creators open the winner's file
after discarding their own temporary files. New files use mode `0600` (subject to
umask); existing files are never truncated at open. Creation requires permission
to create and remove entries in the parent directory and support for hard links.
Normal error unwinding removes the private temporary file. A process killed
during creation can leave a `.pocketkv-*` temporary entry; it never publishes
a partial header at the requested database path. A failure after publication
can leave a valid database even though its constructor reported an error.
`O_NOFOLLOW` rejects a final-component symlink. Intermediate directory symlinks
are not prohibited. Non-regular files are rejected. `O_CLOEXEC` prevents a
descriptor leak across exec; `O_NONBLOCK` prevents FIFO opens from hanging before
the file-type check.
For newly created temporary files, portable `mkstemp` is followed immediately by
`fcntl(FD_CLOEXEC)`. Do not construct a Store concurrently with another thread's
fork/exec during that short setup window.

## Locking and concurrency

The database descriptor holds an exclusive nonblocking `flock` for the Store's
lifetime. A separate opener fails promptly with `ErrorCode::locked`. Locking the
data inode avoids a stale lock-file protocol, but pathname replacement would
introduce a new inode: do not rename, unlink, replace, or externally modify an
open database. Hard links refer to the same inode and therefore the same lock on
supported local filesystems. A Store has no internal thread synchronization and
must not be reused across fork. A fresh child-process open still respects an
existing parent lock.

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

Reads validate the indexed header, payload checksum, and key. Size checks detect
external growth/truncation. These are useful diagnostics, not protection against
uncooperative concurrent edits or malicious file substitution.

## Durability limits

Successful mutations request synchronization using `fsync`. Database creation
syncs both its initial header and its parent directory; any sync failure is an
error. Device caches, filesystem behavior, and OS guarantees matter. This project
does not claim physical power-loss durability or network-filesystem support.
The tests distinguish a child process exiting without a Store destructor from
actual system power loss; only the former is exercised.

Useful system references:

- [Linux fsync](https://man7.org/linux/man-pages/man2/fsync.2.html)
- [Linux flock](https://man7.org/linux/man-pages/man2/flock.2.html)
- [Apple fsync documentation](https://developer.apple.com/library/archive/documentation/System/Conceptual/ManPages_iPhoneOS/man2/fsync.2.html)
