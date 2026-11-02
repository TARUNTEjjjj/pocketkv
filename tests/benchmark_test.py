"""Check benchmark accounting/cleanup, never machine-dependent timing thresholds."""
import csv
import io
import math
from pathlib import Path
import subprocess
import sys
import tempfile


binary = str(Path(sys.argv[1]).resolve())
with tempfile.TemporaryDirectory(prefix="pocketkv-benchmark-test-") as directory:
    sentinel = Path(directory) / "existing.db"
    sentinel.write_bytes(b"do not touch")
    result = subprocess.run(
        [binary, "--keys", "3", "--rounds", "3", "--trials", "2",
         "--reads", "7", "--value-bytes", "17", "--directory", directory],
        capture_output=True, text=True, check=True,
    )
    rows = list(csv.DictReader(io.StringIO(result.stdout)))
    assert len(rows) == 6
    for index, row in enumerate(rows):
        trial, round_number = index // 3 + 1, index % 3 + 1
        assert int(row["trial"]) == trial
        assert int(row["round"]) == round_number
        assert int(row["live_keys"]) == 3
        assert int(row["records"]) == 3 * round_number
        # v1 format: 16-byte file header; 24-byte records + 4-byte keys + values.
        assert int(row["file_bytes"]) == 16 + 3 * round_number * (24 + 4 + 17)
        assert int(row["live_value_bytes"]) == 51
        assert abs(float(row["overwrite_fraction"]) - (round_number - 1) / round_number) < 1e-6
        assert int(row["put_operations"]) == 3
        assert int(row["get_operations"]) == 7
        assert row["sync_policy"] == "fsync_per_put"
        assert row["cache_mode"] == "warm"
        for field in ("put_batch_ms", "reopen_ms", "get_batch_ms"):
            assert math.isfinite(float(row[field])) and float(row[field]) >= 0
    assert list(Path(directory).iterdir()) == [sentinel]
    assert sentinel.read_bytes() == b"do not touch"

    for arguments in (["--keys", "0"], ["--keys", "-1"], ["--rounds", "1x"],
                      ["--trials"], ["--value-bytes", "1048577"], ["--unknown", "1"]):
        failed = subprocess.run([binary, *arguments], capture_output=True, text=True)
        assert failed.returncode == 2, arguments
        assert not failed.stdout, arguments

print("Benchmark CSV accounting, input validation and cleanup passed.")
