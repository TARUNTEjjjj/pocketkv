"""Check benchmark accounting/cleanup, never machine-dependent timing thresholds."""
import csv
import io
import math
import os
from pathlib import Path
import subprocess
import sys
import tempfile


checks = 0


def check(condition: bool, message: str) -> None:
    # Raise explicitly rather than use `assert`: python -O, and a PYTHONOPTIMIZE
    # inherited by CTest's plain interpreter command, strip assert statements.
    global checks
    checks += 1
    if not condition:
        raise AssertionError(message)


binary = str(Path(sys.argv[1]).resolve())
with tempfile.TemporaryDirectory(prefix="pocketkv-benchmark-test-") as directory:
    sentinel = Path(directory) / "existing.db"
    sentinel.write_bytes(b"do not touch")
    result = subprocess.run(
        [binary, "--keys", "3", "--rounds", "3", "--trials", "2",
         "--reads", "7", "--value-bytes", "17", "--directory", directory],
        capture_output=True, text=True, check=True,
    )
    # A caller-selected destination must work even when the process has an
    # invalid temporary-directory environment. Querying temp_directory_path()
    # eagerly in Config used to reject this valid invocation before parsing it.
    invalid_temp = str(Path(directory) / "missing-temp-directory")
    environment = dict(os.environ)
    for variable in ("TMPDIR", "TMP", "TEMP", "TEMPDIR"):
        environment[variable] = invalid_temp
    overridden = subprocess.run(
        [binary, "--keys", "3", "--rounds", "3", "--trials", "2",
         "--reads", "7", "--value-bytes", "17", "--directory", directory],
        capture_output=True, text=True, check=True, env=environment,
    )
    overridden_rows = list(csv.DictReader(io.StringIO(overridden.stdout)))
    rows = list(csv.DictReader(io.StringIO(result.stdout)))
    check(len(overridden_rows) == len(rows) == 6,
          f"two trials of three rounds give six CSV rows: {len(rows)} and {len(overridden_rows)}")
    # Compare all deterministic fields; measured timings naturally differ.
    timings = {"put_batch_ms", "reopen_ms", "get_batch_ms"}
    for expected, actual in zip(rows, overridden_rows):
        check({key: value for key, value in expected.items() if key not in timings} == {
            key: value for key, value in actual.items() if key not in timings
        }, f"an invalid TMPDIR changes no deterministic field: {expected} != {actual}")
    check(len(rows) == 6, f"six CSV rows: {len(rows)}")
    for index, row in enumerate(rows):
        trial, round_number = index // 3 + 1, index % 3 + 1
        check(int(row["trial"]) == trial, f"row {index} trial: {row}")
        check(int(row["round"]) == round_number, f"row {index} round: {row}")
        check(int(row["live_keys"]) == 3, f"row {index} live keys: {row}")
        check(int(row["records"]) == 3 * round_number, f"row {index} records: {row}")
        # v1 format: 16-byte file header; 24-byte records + 4-byte keys + values.
        check(int(row["file_bytes"]) == 16 + 3 * round_number * (24 + 4 + 17), f"row {index} file bytes: {row}")
        check(int(row["live_value_bytes"]) == 51, f"row {index} live value bytes: {row}")
        check(abs(float(row["overwrite_fraction"]) - (round_number - 1) / round_number) < 1e-6,
              f"row {index} overwrite fraction: {row}")
        check(int(row["put_operations"]) == 3, f"row {index} put operations: {row}")
        check(int(row["get_operations"]) == 7, f"row {index} get operations: {row}")
        check(row["sync_policy"] == "fsync_per_put", f"row {index} sync policy: {row}")
        check(row["cache_mode"] == "warm", f"row {index} cache mode: {row}")
        for field in ("put_batch_ms", "reopen_ms", "get_batch_ms"):
            check(math.isfinite(float(row[field])) and float(row[field]) >= 0, f"row {index} {field}: {row}")
    remaining = sorted(Path(directory).iterdir())
    check(remaining == [sentinel], f"the benchmark removes its private directories: {remaining}")
    check(sentinel.read_bytes() == b"do not touch", "the benchmark never touches an existing file")

    for arguments in (["--keys", "0"], ["--keys", "-1"], ["--rounds", "1x"],
                      ["--trials"], ["--value-bytes", "1048577"], ["--unknown", "1"]):
        failed = subprocess.run([binary, *arguments], capture_output=True, text=True)
        check(failed.returncode == 2, f"invalid arguments exit with status 2: {arguments}; {failed}")
        check(not failed.stdout, f"invalid arguments print no CSV: {arguments}; {failed.stdout!r}")

print(f"Benchmark CSV accounting, input validation and cleanup passed ({checks} checks).")
