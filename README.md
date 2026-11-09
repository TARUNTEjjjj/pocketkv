# PocketKV

[![C++ checks](https://github.com/TARUNTEjjjj/pocketkv/actions/workflows/ci.yml/badge.svg)](https://github.com/TARUNTEjjjj/pocketkv/actions/workflows/ci.yml)

A small persistent key-value database in C++20. PocketKV stores changes in an
append-only binary file, rebuilds an in-memory index when opened, and verifies
checksums before trusting records. Use it from the terminal or link its library
into a C++ application.

The project explores storage-engine fundamentals: binary formats, disk offsets,
resource ownership, file locking, failure handling, and recovery. Version 0.1.0
targets Linux and macOS, with no third-party runtime dependencies.

## Try it

Requires a C++20 compiler (GCC or Clang), CMake 3.20+, and a build tool such as Make
or Ninja. Python 3.9+ enables the additional CLI integration tests.
Creating a database requires a writable parent directory on a local filesystem
that supports hard links, advisory locking, and directory synchronization.

```sh
git clone https://github.com/TARUNTEjjjj/pocketkv.git
cd pocketkv
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DPOCKETKV_WARNINGS_AS_ERRORS=ON
cmake --build build --parallel
ctest --test-dir build --output-on-failure

./build/pocketkv demo.db put theme dark
# Saved.
./build/pocketkv demo.db get theme
# dark
./build/pocketkv demo.db put theme light
# Saved.
./build/pocketkv demo.db stats
# keys: 1
# records: 2
# file_bytes: 83
# live_value_bytes: 5
./build/pocketkv demo.db delete theme
# Deleted.
./build/pocketkv demo.db verify
# OK: 3 records, 0 keys.
```

Each command is a new process, so `get` demonstrates reopening persisted data.
The displayed counts assume a new `demo.db`.

## Commands

Syntax: `pocketkv DATABASE COMMAND [ARGUMENTS]`.

| Command | Result |
| --- | --- |
| `init` | Create an empty database or validate an existing database |
| `put KEY VALUE` | Insert or replace a value; create the database if absent |
| `get KEY` | Print a value and a trailing newline |
| `get-raw KEY` | Write the exact value bytes without an added newline |
| `delete KEY` | Append a deletion record if the key exists |
| `keys` | List current keys in bytewise sorted order, escaping nonprintable bytes and backslashes |
| `keys-prefix PREFIX [LIMIT [AFTER]]` | List a bounded page of matching keys, strictly after an optional raw-key cursor; default 100, maximum 1,000 |
| `stats` | Show live keys, total log records, file size, and live value bytes |
| `verify` | Scan and validate the complete database without changing it |
| `repair` | Explicitly discard a structurally incomplete final record |

Exit codes: **0** success, **1** missing key, **2** invalid input, database error,
or output-write failure. Key/value arguments are validated before the database is
opened, so an invalid `put` does not create an empty file. Every output command,
including help and version, checks its final write. A mutation may already be
saved when printing its acknowledgement fails; an output error does not roll it back.
Writing to a closed pipe instead ends the command with SIGPIPE (shells report
status 141), and a mutation may already be saved in that case too.
Only `init` and `put` create missing files. Keys are 1–4,096 bytes; values are
0–1,048,576 bytes. The library supports embedded NUL bytes. Command-line arguments
cannot carry NUL bytes; `get` and `get-raw` emit stored bytes unchanged.

## C++ API

```cpp
#include <pocketkv/store.hpp>
#include <iostream>

int main() {
    pocketkv::Store db("settings.db");
    db.put("theme", "dark");
    if (auto value = db.get("theme")) {
        std::cout << *value << '\n';
    }
    db.erase("theme");
}
```

The `Store` owns its file descriptor and exclusive advisory lock through RAII.
It is neither copyable nor movable. Methods throw `pocketkv::Error`, with a
structured `ErrorCode`; `get` returns `std::nullopt` for absent keys.

### Bounded key discovery

Use `keys_page` when copying every live key would use too much memory:

```cpp
std::optional<std::string> cursor;
for (;;) {
    auto page = db.keys_page("user/", 100,
        cursor ? std::optional<std::string_view>(*cursor) : std::nullopt);
    if (page.empty()) break;
    for (const auto& key : page) std::cout << key << '\n';
    cursor = page.back();
}
```

An empty prefix matches all keys. Prefixes may contain NUL bytes and are limited
to 4,096 bytes; a supplied cursor must be a valid nonempty key. Limits are
1–1,000 inclusive. The ordered index seeks to the prefix or exclusive cursor,
then copies at most the requested number of matching keys: O(log N + L) index
steps for N live keys and page limit L, plus string comparison/copying costs.
This bounds result memory, not the existing in-memory index or startup scan.
The file format is unchanged, and page reads append no records.

Pages reflect the current index, not a snapshot. Between calls, deleted cursor
keys remain valid positions; inserted keys before the cursor will not appear.
Continue using the last returned raw key until a page is empty. The CLI equivalent
is `pocketkv data.db keys-prefix 'user/' 100 'user/099'`. CLI output escapes keys
like `keys`; the `AFTER` argument is the original key, not its escaped display.

Install with `cmake --install build --prefix "$PWD/install"`. A consuming CMake
project uses:

```cmake
find_package(PocketKV CONFIG REQUIRED)
target_link_libraries(my_app PRIVATE PocketKV::pocketkv)
```

Configure that project with `-DCMAKE_PREFIX_PATH=/absolute/path/to/pocketkv/install`.
See [the example](examples/basic.cpp) and [external consumer test](tests/consumer).
Custom `CMAKE_INSTALL_BINDIR`, `CMAKE_INSTALL_LIBDIR` and
`CMAKE_INSTALL_INCLUDEDIR` values are supported. Keep them relative to move the
installation as a unit. Tests run the installed CLI and a separate library
consumer after moving the installation, including with custom directories.

## Integrity and recovery

- File and record headers have independent CRC32 checksums; each record also has
  a payload checksum. Length limits are checked before allocating payload memory.
- Opening scans every record, including overwritten values and deletion records.
  A bad checksum or invalid format stops the scan and leaves the file unchanged.
- An incomplete final record is an error by default. `repair` validates all
  available header fields, then truncates only that incomplete suffix. It reports
  the number of removed bytes. Earlier records remain intact.
- Partial length fields must still admit a valid completion. If the bytes already
  imply an impossible key/value length or a value on a deletion record, repair
  reports corruption and preserves the file.
- Repair does not reconstruct missing bytes or fix a complete corrupted record.
  A short header has missing checksum bytes that cannot be verified. An otherwise
  valid truncated suffix cannot reveal whether its cause was a crash or corruption.
- Each successful mutation calls `fsync` before updating the in-memory index and
  returning. New-file initialization also syncs the parent directory. After an
  append or sync failure, the object refuses further operations until reopened;
  the failed operation's on-disk outcome may be uncertain.

Keep a copy before deliberately running `repair`. CRC32 detects accidental damage;
it is not authentication or encryption. Tests exercise process exits and truncated
writes, not physical power loss. OS, filesystem, and hardware behavior still govern
what survives a power failure; PocketKV does not promise production-grade durability.

## Scope and tradeoffs

PocketKV permits **one open Store per database**, including read commands. Every
open requires read/write access, and callers must serialize use of a Store across
threads. `flock` is advisory: other programs can ignore it. Renaming, unlinking, or
replacing an open database, changing it externally, and using an inherited Store
after `fork` are unsupported. Open fresh Store instances in child processes.

The log grows with every update; deleted and overwritten payloads remain on disk.
There is no compaction, transaction API, SQL, concurrent reader mode, encryption,
Windows backend, or network-filesystem guarantee. A bounded value size limits each
read allocation, but total index memory grows with the number and length of live
keys. Every CLI invocation scans the log, so this implementation favors clarity
and inspectability over large-database throughput.

The [reproducible storage experiment](docs/benchmarks.md) measures warm reopen,
indexed reads and fsync-per-put writes as overwrite history grows. It includes
the benchmark executable, raw trial data, timing boundaries and limitations.

Next steps: atomic compaction with a documented locking protocol and configurable
sync policy. These are future work.

## Testing and design

```sh
cmake -S . -B build-sanitize -DCMAKE_BUILD_TYPE=Debug \
  -DPOCKETKV_SANITIZERS=ON -DPOCKETKV_WARNINGS_AS_ERRORS=ON
cmake --build build-sanitize --parallel
ctest --test-dir build-sanitize --output-on-failure
```

Tests cover persistence, updates/deletions, binary values, bounds, randomized
operations against an independent map, locking, partial writes, corruption,
explicit repair, CLI contracts, and installation into a separate C++ project.
An isolated test build also injects sync failures, read failures, interruptions
and short reads to check error handling and recovery without changing the
production library's system calls.
GitHub Actions runs GCC and Clang on Linux and Clang on macOS, including an
AddressSanitizer/UndefinedBehaviorSanitizer build.

- [File format](docs/format.md)
- [Engineering decisions](docs/engineering.md)
- [Verification details](docs/verification.md)
- [Storage benchmark: overwrite history versus live keys](docs/benchmarks.md)

Licensed under [MIT](LICENSE).

