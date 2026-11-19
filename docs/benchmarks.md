# What grows when a database is overwritten?

PocketKV retains every old record but indexes only the current value of each key.
This experiment asks whether reopening follows the historical log size while a
lookup in an already-open Store stays comparatively stable. It also measures puts
with the production sync-per-mutation policy intact: `fsync`, or
`fcntl(F_FULLFSYNC)` on macOS.

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
  output are excluded. Every put still syncs its record (`fsync`, or
  `fcntl(F_FULLFSYNC)` on macOS); no batching or bypass is used. The CSV
  `sync_policy` value `fsync_per_put` names this one-sync-per-put policy.
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

## Recorded results

Two recorded runs are kept. They differ in revision, sync call, OS point release,
build method and machine state, and the historical run did not record its
hardware, so the same machine cannot be assumed. **They are not a before/after
comparison** and no speed ratio between them should be inferred: any difference
mixes hardware, OS state, load and the sync call. Each run's raw CSVs and a summary
regenerated with `benchmarks/summarize.py` sit in their own directory.

Both CSVs label the policy `fsync_per_put`. That value names the one-sync-per-put
policy, not the system call; which call the sources made is recorded below.

### Current baseline: `F_FULLFSYNC` per put (19 November 2026)

| Item | Recorded value |
| --- | --- |
| Revision | `7bfe402fc5bf7e4d2a7b5998240330abd3209314` (`main`), unmodified |
| Sync policy | One `fcntl(F_FULLFSYNC)` per put (`src/store.cpp`, `sync_file`). Its `fsync` fallback runs only when a filesystem rejects `F_FULLFSYNC`; a separate probe confirmed `F_FULLFSYNC` returned 0 on the measured volume, so the fallback is not expected to have run. The benchmark does not itself log the call taken |
| OS | macOS 27.2.1 (build 26B131), Darwin 27.2.0, arm64 |
| Machine | MacBook Air (Mac17,3), Apple M5, 10 CPU cores, 16 GB memory; on battery power, Low Power Mode off |
| Compiler | Apple clang 21.0.0 (clang-2100.4.1.1), Command Line Tools, target `arm64-apple-darwin27.2.0` |
| Build | CMake 4.4.5, the Reproduce commands above. Compile flags from `compile_commands.json`: `-O3 -DNDEBUG -std=c++20 -arch arm64 -Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion -Wshadow -Werror`. All 9 CTest entries passed before measuring |
| Storage | `--directory build` in a fresh clone on the APFS Data volume (`/dev/disk3s5`), internal `APPLE SSD AP0512Z` (Apple Fabric), FileVault on, 54% full |
| Workload | Exactly the two Reproduce commands: 32 and 128 keys, 8 rounds, 5 trials, 4096 reads, 64-byte values; both exited with status 0 |
| Load | Not an isolated machine: other interactive and background work may have been running. The runs started once the one-minute load average fell below 3.0 (it had been about 6); it was 2.89 at the start, 2.74 between workloads and 3.14 at the end, 11:44:08 to 11:44:31 UTC |

Raw results: [32 keys](benchmarks/2026-11-19-fullfsync/keys-32.csv),
[128 keys](benchmarks/2026-11-19-fullfsync/keys-128.csv);
[generated summary](benchmarks/2026-11-19-fullfsync/summary.md), reproduced in full:

| Keys | Value bytes | Reads | Round | Records | File bytes | Trials | Reopen ms, median [min, max] | Put us/op, median | Get us/op, median |
| ---: | ---: | ---: | ---: | ---: | ---: | ---: | --- | ---: | ---: |
| 32 | 64 | 4096 | 1 | 32 | 2982 | 5 | 0.292 [0.090, 0.402] | 3121.72 | 2.35 |
| 32 | 64 | 4096 | 2 | 64 | 5948 | 5 | 0.188 [0.098, 0.527] | 3453.99 | 1.80 |
| 32 | 64 | 4096 | 3 | 96 | 8914 | 5 | 0.585 [0.106, 0.615] | 3722.29 | 2.41 |
| 32 | 64 | 4096 | 4 | 128 | 11880 | 5 | 0.732 [0.204, 0.770] | 3591.22 | 2.71 |
| 32 | 64 | 4096 | 5 | 160 | 14846 | 5 | 0.859 [0.272, 0.886] | 3642.53 | 2.53 |
| 32 | 64 | 4096 | 6 | 192 | 17812 | 5 | 1.002 [0.412, 1.050] | 3483.94 | 2.50 |
| 32 | 64 | 4096 | 7 | 224 | 20778 | 5 | 1.075 [0.493, 1.112] | 3439.15 | 2.44 |
| 32 | 64 | 4096 | 8 | 256 | 23744 | 5 | 1.194 [0.447, 1.232] | 3820.67 | 2.40 |
| 128 | 64 | 4096 | 1 | 128 | 11938 | 5 | 0.173 [0.136, 0.806] | 3614.08 | 1.28 |
| 128 | 64 | 4096 | 2 | 256 | 23860 | 5 | 0.303 [0.219, 1.266] | 3455.52 | 1.20 |
| 128 | 64 | 4096 | 3 | 384 | 35782 | 5 | 0.401 [0.326, 0.623] | 3429.64 | 1.14 |
| 128 | 64 | 4096 | 4 | 512 | 47704 | 5 | 0.485 [0.397, 0.939] | 3427.66 | 1.17 |
| 128 | 64 | 4096 | 5 | 640 | 59626 | 5 | 0.739 [0.533, 1.134] | 3671.64 | 1.44 |
| 128 | 64 | 4096 | 6 | 768 | 71548 | 5 | 1.122 [0.715, 2.599] | 3584.63 | 1.64 |
| 128 | 64 | 4096 | 7 | 896 | 83470 | 5 | 1.624 [1.126, 2.945] | 3644.58 | 2.04 |
| 128 | 64 | 4096 | 8 | 1024 | 95392 | 5 | 1.195 [0.774, 3.697] | 3722.68 | 1.36 |

Each put now includes a full drive-cache flush: median put time was about
3.1 to 3.8 ms per operation in every round of both workloads. That cost dominates
put timing here; it is a property of this machine's storage and flush path, not
a general figure for macOS.

Reopen and get timings were noisy in this run. For 128 keys the median reopen at
round 8 (1024 records) was about 6.9 times that at round 1, but round 7 has a
higher median than round 8 and several ranges span a factor of two to four. The
32-key medians do not rise monotonically either (round 1 exceeds round 2), and
the round-8 medians of both workloads are almost equal despite a fourfold
difference in records. Plausible causes include concurrent load, battery-power
frequency management and scheduling across performance and efficiency cores, but
these were not isolated, so this run alone does not establish how reopen time
scales with history. Median warm gets stayed between about 1.1 and 2.7
microseconds. Repeat on an idle machine on mains power before drawing scaling
conclusions from the reopen column.

### Historical: plain `fsync` per put (2 November 2026)

Measured in the local coding environment: macOS 27.2, Darwin arm64, Apple Clang
21.0.0. Release flags were `-std=c++20 -O3 -DNDEBUG`, with the repository's
warnings enabled and treated as errors. Storage was the workspace filesystem under
`build`; storage hardware and filesystem type were not independently
characterised. No assertion about a particular SSD is made.

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
rounds and five trials: [32 keys](benchmarks/2026-11-02-fsync/keys-32.csv),
[128 keys](benchmarks/2026-11-02-fsync/keys-128.csv)
([generated summary](benchmarks/2026-11-02-fsync/summary.md)). Selected rounds are shown here:

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

These measurements predate the switch to `fcntl(F_FULLFSYNC)` on macOS. Each
put then used a plain `fsync`, which on macOS does not flush the drive's write
cache, so the put column above omits that flush and does not describe current
put cost; see the current baseline above. The reopen and get columns involve no
sync.

In this historical run, for 128 live keys, eight times as many records produced about **6.1 times the
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
