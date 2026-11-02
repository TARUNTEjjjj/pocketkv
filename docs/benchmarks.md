# What grows when a database is overwritten?

PocketKV retains every old record but indexes only the current value of each key.
This experiment asks whether reopening follows the historical log size while a
lookup in an already-open Store stays comparatively stable. It also measures puts
with the production `fsync`-per-mutation policy intact.

## Reproduce

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release \
  -DPOCKETKV_BUILD_BENCHMARKS=ON -DPOCKETKV_WARNINGS_AS_ERRORS=ON
cmake --build build --parallel
ctest --test-dir build --output-on-failure

./build/pocketkv-benchmark --keys 32 --rounds 8 --trials 5 \
  --reads 4096 --value-bytes 64 --directory build > keys-32.csv
./build/pocketkv-benchmark --keys 128 --rounds 8 --trials 5 \
  --reads 4096 --value-bytes 64 --directory build > keys-128.csv
python3 benchmarks/summarize.py keys-32.csv keys-128.csv
```

`--directory` chooses an existing parent on the filesystem to measure. Each trial
creates its own private temporary directory there and removes only that directory
on normal exit. No existing database is opened or overwritten. Without this
option, the system temporary directory is used. A forced process termination can
leave the private directory behind. Check the exit status: failed validation or
I/O can leave partial CSV output.

The benchmark is optional and not installed with the library. CI builds it and
runs a tiny correctness/accounting test; it does not impose a speed threshold.

## Workload and timing boundaries

- Each trial starts with an empty database. Round 1 inserts all keys; each later
  round replaces every value. Keys are `key0`, `key1`, etc.; values are deterministic
  binary strings of the configured size. There are no deletions or missing-key reads.
- With `K` live keys after round `r`, there are `K*r` records. The fraction of
  records superseded by a later put is `(r-1)/r`. Live value bytes stay `K*V`.
- `put_batch_ms` times exactly `K` calls to `Store::put`. Values are prepared before
  timing. Database creation, the writer's initial reopen, destruction and CSV
  output are excluded. Every put still calls `fsync`; no batching or bypass is used.
- `reopen_ms` times construction of a fresh Store after closing the writer. This
  includes open/lock, record validation and rebuilding the index, but excludes
  destruction and subsequent validation. It is **warm-cache reopening**: the file
  has just been written, and the benchmark never drops OS caches.
- Before timing reads, every live value is checked in full against the expected
  latest value. `get_batch_ms` then times cyclic successful gets in an already-open
  Store. The loop includes result-presence checks and checksum accumulation; an
  independently computed checksum is checked after timing. It excludes reopening,
  so it is not the latency of invoking the CLI once per read.
- Counts and file size are checked after every reopen. Put/get microseconds per
  operation in the summary are batch duration divided by operation count. They are
  **not individual-operation latency percentiles**.

Rounds grow in ascending order within each trial. Five trials provide descriptive
medians and observed ranges, not confidence intervals. For broader conclusions,
repeat on other filesystems and machines, vary payload size and live-key count,
and design a separate controlled cold-cache experiment.

## Recorded example

Measured on 2 November 2026 in the local coding environment: macOS 27.2,
Darwin arm64, Apple Clang 21.0.0. Release flags were `-std=c++20 -O3 -DNDEBUG`,
with the repository's warnings enabled and treated as errors. Storage was the
workspace filesystem under `build`; storage hardware and filesystem type were
not independently characterised. No assertion about a particular SSD is made.

The production sources came from revision
`463305312fc737ded7055d62f174a4d9256d26c8`; the benchmark was added without changing
storage implementation or sync policy. CMake was unavailable in that environment,
so the executable was built directly using:

```sh
clang++ -std=c++20 -O3 -DNDEBUG -Wall -Wextra -Wpedantic \
  -Wconversion -Wsign-conversion -Wshadow -Werror -Iinclude \
  src/store.cpp src/format.cpp benchmarks/storage_benchmark.cpp \
  -o build/pocketkv-benchmark
```

The workload commands were exactly those above. Raw results contain all eight
rounds and five trials: [32 keys](benchmarks/keys-32.csv),
[128 keys](benchmarks/keys-128.csv). Selected rounds are shown here:

| Keys | Round | Records | File bytes | Reopen ms, median [min, max] | Put us/op, median | Get us/op, median |
| ---: | ---: | ---: | ---: | --- | ---: | ---: |
| 32 | 1 | 32 | 2982 | 0.044 [0.041, 0.058] | 23.59 | 1.00 |
| 32 | 2 | 64 | 5948 | 0.079 [0.068, 0.084] | 25.06 | 0.98 |
| 32 | 4 | 128 | 11880 | 0.128 [0.110, 0.144] | 22.75 | 1.01 |
| 32 | 8 | 256 | 23744 | 0.202 [0.200, 0.219] | 23.30 | 0.98 |
| 128 | 1 | 128 | 11938 | 0.128 [0.119, 0.129] | 22.53 | 0.98 |
| 128 | 2 | 256 | 23860 | 0.212 [0.203, 0.230] | 22.37 | 0.98 |
| 128 | 4 | 512 | 47704 | 0.408 [0.398, 0.421] | 23.15 | 0.98 |
| 128 | 8 | 1024 | 95392 | 0.781 [0.748, 0.844] | 21.71 | 0.98 |

For 128 live keys, eight times as many records produced about **6.1 times the
median reopen time** while live values remained 8192 bytes. Median warm get time
stayed close to one microsecond in these small cases. This is consistent with
replaying history on open and using an index for subsequent reads; it is not proof
of asymptotic complexity or a general performance guarantee.

## Relating the measurements to the implementation

Let `R` be historical records, `L` the maximum live-key count during replay, and
`B` total log bytes.
Reopening reads and checksums all `B` bytes and updates a `std::map` per record.
For bounded key lengths its work is approximately `O(B + R log(L+1))`; the index
stores `O(L)` entries plus key bytes. Key comparisons can add work when keys are
long. Fixed `L` and fixed-size records isolate growth with history in this experiment.

An already-open get performs a map lookup and reads/checksums only the selected
record, plus a file metadata check. A put adds a record and synchronises the file;
OS/filesystem sync behaviour can dominate the measured time. The modest file sizes
here do not test memory pressure, sustained large-log performance, or power-loss
durability. Process RSS and cold-cache I/O are not measured.

The results motivate compaction as future work: reducing history could reduce file
size and reopen cost. Any compaction design must preserve the existing locking and
failure-handling guarantees before speed comparisons are meaningful.
