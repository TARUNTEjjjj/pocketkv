#!/usr/bin/env python3
"""Exercise the CLI and recovery policy against disposable synthetic databases."""

from __future__ import annotations

import argparse
import os
from pathlib import Path
import struct
import subprocess
import tempfile
import zlib


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("binary", type=Path)
    args = parser.parse_args()
    binary = args.binary.resolve(strict=True)
    checks = 0

    def check(condition: bool, message: str) -> None:
        nonlocal checks
        checks += 1
        if not condition:
            raise AssertionError(message)

    def run(*arguments: str, code: int = 0, output: bytes | None = None) -> subprocess.CompletedProcess:
        nonlocal checks
        result = subprocess.run([str(binary), *arguments], capture_output=True, timeout=10, check=False)
        checks += 1
        if result.returncode != code or (output is not None and result.stdout != output):
            raise AssertionError(f"CLI contract failed: {arguments!r}; exit={result.returncode}; stdout={result.stdout!r}; stderr={result.stderr!r}")
        return result

    def failed_output(*arguments: str) -> None:
        # A read-only descriptor forces a write/flush failure on Linux/macOS
        # without depending on a full filesystem or platform-specific device.
        with open(os.devnull, "rb") as output:
            result = subprocess.run([str(binary), *arguments], stdout=output,
                                    stderr=subprocess.PIPE, timeout=10, check=False)
        check(result.returncode == 2 and result.stderr == b"error: could not write command output\n",
              f"output failure must report status 2: {arguments!r}; {result}")

    def closed_stream(descriptor: int, *arguments: str, code: int = 2) -> None:
        # `>&-` or `2>&-` starts the CLI with that standard descriptor closed.
        # The database must never fill the free slot, or the CLI's own output
        # would be appended to the log; a closed stdout is an output failure.
        result = subprocess.run([str(binary), *arguments], capture_output=True, timeout=10,
                                check=False, preexec_fn=lambda: os.close(descriptor))
        error = b"error: could not write command output\n" if descriptor == 1 else b""
        check(result.returncode == code and result.stdout == b"" and result.stderr == error,
              f"closed descriptor {descriptor} contract failed: {arguments!r}; {result}")

    def decode(data: bytes) -> list[tuple[int, bytes, bytes]]:
        """Independent little-endian decoder using Python's standard CRC32."""
        check(len(data) >= 16, "complete file header")
        magic, version, checksum = struct.unpack_from("<8sII", data)
        check(magic == b"POCKETKV" and version == 1, "file magic and version")
        check(checksum == zlib.crc32(data[:12]), "independent file-header CRC")
        offset = 16
        records = []
        while offset < len(data):
            check(len(data) - offset >= 24, "complete record header")
            magic, kind, flags, reserved, key_size, value_size, payload_crc, header_crc = struct.unpack_from("<4sBBHIIII", data, offset)
            check(magic == b"PKVR" and kind in (1, 2) and flags == reserved == 0, "record metadata")
            check(header_crc == zlib.crc32(data[offset:offset + 20]), "independent record-header CRC")
            check(1 <= key_size <= 4096 and value_size <= 1024 * 1024, "record bounds")
            check(kind != 2 or value_size == 0, "tombstone contains no value")
            offset += 24
            payload = data[offset:offset + key_size + value_size]
            check(len(payload) == key_size + value_size, "complete record payload")
            check(payload_crc == zlib.crc32(payload), "independent payload CRC")
            records.append((kind, payload[:key_size], payload[key_size:]))
            offset += key_size + value_size
        check(offset == len(data), "all database bytes decoded")
        return records

    run("--help")
    run("--version")
    for flag in ["--help", "-h", "--version"]:
        failed_output(flag)
    run(code=2)
    with tempfile.TemporaryDirectory(prefix="pocketkv-cli-") as directory:
        root = Path(directory)
        invalid_arguments = [
            ("put", "", "bad"), ("put", "k" * 4097, "bad"),
            ("get", ""), ("get-raw", "k" * 4097), ("delete", ""),
            ("put", "key"), ("nonsense",), ("get", "key", "extra"),
        ]
        for index, arguments in enumerate(invalid_arguments):
            invalid_path = root / f"invalid-{index}.db"
            result = run(str(invalid_path), *arguments, code=2)
            check(bool(result.stderr) and not invalid_path.exists(), "invalid command arguments must never create a database")
        maximum_key = "k" * 4096
        paged = str(root / "pages.db")
        run(paged, "keys-prefix", "", code=2)
        check(not Path(paged).exists(), "page reads must not create a missing database")
        for key in ["user/1", "user/2", "user/3", "other", "user/line\nbreak"]:
            run(paged, "put", key, "value")
        unchanged = Path(paged).read_bytes()
        run(paged, "keys-prefix", "user/", "2", output=b"user/1\nuser/2\n")
        run(paged, "keys-prefix", "user/", "2", "user/2", output=b"user/3\nuser/line\\x0abreak\n")
        run(paged, "keys-prefix", "", "1", output=b"other\n")
        run(paged, "keys-prefix", "missing", output=b"")
        run(paged, "keys-prefix", "user/", output=b"user/1\nuser/2\nuser/3\nuser/line\\x0abreak\n")
        for limit in ["0", "1001", "-1", "+1", "1x", "", " 2", "184467440737095516160"]:
            run(paged, "keys-prefix", "", limit, code=2)
        for arguments in [("keys-prefix",), ("keys-prefix", "x" * 4097), ("keys-prefix", "", "1", ""), ("keys-prefix", "", "1", "x" * 4097), ("keys-prefix", "", "1", "a", "extra")]:
            run(paged, *arguments, code=2)
        failed_output(paged, "keys-prefix", "user/")
        check(Path(paged).read_bytes() == unchanged, "page queries and rejected inputs preserve every database byte")
        boundary_database = str(root / "boundary.db")
        run(boundary_database, "put", maximum_key, "allowed", output=b"Saved.\n")
        run(boundary_database, "get", maximum_key, output=b"allowed\n")
        database = str(root / "example.db")
        for arguments in [(database, "get", "theme"), (database, "get-raw", "theme"), (database, "repair"), (database, "verify"), (database, "stats"), (database, "keys"), (database, "delete", "theme")]:
            missing = run(*arguments, code=2)
            check(bool(missing.stderr) and not Path(database).exists(), "read/delete/repair never create a missing database")

        initialized = str(root / "initialized.db")
        run(initialized, "init", output=b"Ready.\n")
        initial_bytes = Path(initialized).read_bytes()
        check(decode(initial_bytes) == [], "init writes an empty valid database")
        run(initialized, "init", output=b"Ready.\n")
        check(Path(initialized).read_bytes() == initial_bytes, "repeated init preserves existing bytes")
        run(initialized, "verify", output=b"OK: 0 records, 0 keys.\n")
        Path(initialized).write_bytes(b"")
        run(initialized, "init", code=2)
        check(Path(initialized).read_bytes() == b"", "init refuses a truncated existing file")

        run(database, "put", "theme", "dark", output=b"Saved.\n")
        run(database, "get", "theme", output=b"dark\n")
        run(database, "get-raw", "theme", output=b"dark")
        run(database, "put", "theme", "light", output=b"Saved.\n")
        run(database, "put", "empty", "", output=b"Saved.\n")
        run(database, "get", "empty", output=b"\n")
        run(database, "keys", output=b"empty\ntheme\n")
        run(database, "verify", output=b"OK: 3 records, 2 keys.\n")
        run(database, "stats", output=b"keys: 2\nrecords: 3\nfile_bytes: 112\nlive_value_bytes: 5\n")
        run(database, "delete", "theme", output=b"Deleted.\n")
        check(run(database, "get", "theme", code=1).stderr == b"Key not found.\n", "missing get error")
        check(run(database, "delete", "theme", code=1).stderr == b"Key not found.\n", "missing delete error")
        run(database, "verify", output=b"OK: 4 records, 1 keys.\n")
        before = Path(database).read_bytes()
        check(decode(before) == [(1, b"theme", b"dark"), (1, b"theme", b"light"), (1, b"empty", b""), (2, b"theme", b"")], "independent decoding matches every acknowledged put and tombstone")
        for arguments in invalid_arguments:
            result = run(database, *arguments, code=2)
            check(bool(result.stderr) and Path(database).read_bytes() == before, "invalid CLI calls preserve database")

        # The final record is deliberately incomplete, with all previous records
        # still intact. Opening must fail without changing a single byte.
        run(database, "put", "tail", "pending", output=b"Saved.\n")
        complete = Path(database).read_bytes()
        damaged = complete[:-2]
        Path(database).write_bytes(damaged)
        for command in ["verify", "stats"]:
            result = run(database, command, code=2)
            check(bool(result.stderr) and Path(database).read_bytes() == damaged, "strict CLI reads preserve incomplete tail")
        removed = len(damaged) - len(before)
        run(database, "repair", output=f"Repaired {removed} bytes.\n".encode())
        check(Path(database).read_bytes() == before, "repair preserves previous record bytes")
        run(database, "repair", output=b"Repaired 0 bytes.\n")
        run(database, "get", "empty", output=b"\n")
        run(database, "put", "recovered", "yes", output=b"Saved.\n")
        run(database, "get", "recovered", output=b"yes\n")

        # A fully present checksummed record is corruption, never a repairable
        # partial suffix. Repair must refuse and preserve the evidence.
        corrupt = bytearray(Path(database).read_bytes())
        corrupt[-1] ^= 1
        Path(database).write_bytes(corrupt)
        result = run(database, "repair", code=2)
        check(bool(result.stderr) and Path(database).read_bytes() == corrupt, "repair refuses complete corruption without mutation")

        escaped = str(root / "escaped.db")
        for key in ["line\nbreak", "path\\key", "tab\tkey", "\u00e9"]:
            run(escaped, "put", key, "value", output=b"Saved.\n")
        run(escaped, "keys", output=b"line\\x0abreak\npath\\\\key\ntab\\x09key\n\\xc3\\xa9\n")
        run(escaped, "get-raw", "line\nbreak", output=b"value")

        # A synthetic external fixture exercises NUL-bearing CLI output, which
        # cannot be supplied through an operating-system argument vector.
        raw_database = str(root / "binary.db")
        key, value = b"binary", b"first\x00second\n\xff"
        header = struct.pack("<8sI", b"POCKETKV", 1)
        header += struct.pack("<I", zlib.crc32(header))
        payload = key + value
        record = struct.pack("<4sBBHIII", b"PKVR", 1, 0, 0, len(key), len(value), zlib.crc32(payload))
        record += struct.pack("<I", zlib.crc32(record))
        Path(raw_database).write_bytes(header + record + payload)
        run(raw_database, "get-raw", "binary", output=value)
        run(raw_database, "get", "binary", output=value + b"\n")

        # A failed acknowledgement does not undo a completed mutation. Verify
        # the actual persisted result through a fresh command after each error.
        output_database = str(root / "output-failure.db")
        failed_output(output_database, "init")
        run(output_database, "verify", output=b"OK: 0 records, 0 keys.\n")
        failed_output(output_database, "put", "persisted", "value")
        run(output_database, "get", "persisted", output=b"value\n")
        before = Path(output_database).read_bytes()
        for arguments in [("get", "persisted"), ("get-raw", "persisted"), ("keys",), ("stats",), ("verify",), ("init",)]:
            failed_output(output_database, *arguments)
            check(Path(output_database).read_bytes() == before, "failed read/init output preserves database bytes")
        failed_output(output_database, "delete", "persisted")
        run(output_database, "get", "persisted", code=1)
        before = Path(output_database).read_bytes()
        Path(output_database).write_bytes(before + b"PKV")
        failed_output(output_database, "repair")
        check(Path(output_database).read_bytes() == before, "repair completes even if its acknowledgement cannot be printed")
        run(output_database, "verify", output=b"OK: 2 records, 0 keys.\n")

        # The same contract with stdout or stderr closed rather than unwritable:
        # only acknowledged records may reach the file, through every command.
        closed_database = str(root / "closed-descriptor.db")
        closed_stream(1, closed_database, "init")
        check(decode(Path(closed_database).read_bytes()) == [], "init with closed stdout writes only a valid header")
        closed_stream(1, closed_database, "put", "kept", "value")
        before = Path(closed_database).read_bytes()
        check(decode(before) == [(1, b"kept", b"value")], "put with closed stdout appends only its record")
        for arguments in [("get", "kept"), ("get-raw", "kept"), ("keys",), ("stats",), ("verify",), ("init",)]:
            closed_stream(1, closed_database, *arguments)
            check(Path(closed_database).read_bytes() == before, "reads with closed stdout preserve database bytes")
        for arguments in [("get", "absent"), ("get-raw", "absent"), ("delete", "absent")]:
            closed_stream(2, closed_database, *arguments, code=1)
            check(Path(closed_database).read_bytes() == before, "missing keys with closed stderr preserve database bytes")
        closed_stream(1, closed_database, "delete", "kept")
        check(decode(Path(closed_database).read_bytes()) == [(1, b"kept", b"value"), (2, b"kept", b"")],
              "delete with closed stdout appends only its tombstone")
        before = Path(closed_database).read_bytes()
        Path(closed_database).write_bytes(before + b"PKV")
        closed_stream(1, closed_database, "repair")
        check(Path(closed_database).read_bytes() == before, "repair with closed stdout leaves only the valid prefix")
        run(closed_database, "verify", output=b"OK: 2 records, 0 keys.\n")
        snapshot_source = str(root / "snapshot-source.db")
        snapshot_target = str(root / "snapshot-copy.db")
        run(snapshot_source, "put", "kept", "first")
        run(snapshot_source, "put", "kept", "latest")
        run(snapshot_source, "put", "deleted", "gone")
        run(snapshot_source, "delete", "deleted")
        original = Path(snapshot_source).read_bytes()
        run(snapshot_source, "snapshot", snapshot_target, output=b"Snapshot saved.\n")
        check(decode(Path(snapshot_target).read_bytes()) == [(1, b"kept", b"latest")],
              "snapshot contains one independently decoded record per live key")
        check(Path(snapshot_source).read_bytes() == original, "snapshot never changes source log")
        run(snapshot_target, "get", "kept", output=b"latest\n")
        run(snapshot_source, "snapshot", snapshot_target, code=2)
        run(snapshot_source, "snapshot", snapshot_source, code=2)
        check(Path(snapshot_source).read_bytes() == original, "same-path snapshot never overwrites source")
        absent_source = str(root / "absent-source.db")
        run(absent_source, "snapshot", str(root / "absent-copy.db"), code=2)
        check(not Path(absent_source).exists(), "snapshot never creates a missing source")
        failed_output(snapshot_source, "snapshot", str(root / "unacknowledged.db"))
        run(str(root / "unacknowledged.db"), "get", "kept", output=b"latest\n")
    print(f"PASS {checks} CLI and independent format checks, including argument validation, output failures and recovery")


if __name__ == "__main__":
    main()
