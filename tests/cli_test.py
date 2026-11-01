#!/usr/bin/env python3
"""Exercise the CLI and recovery policy against disposable synthetic databases."""

from __future__ import annotations

import argparse
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
    run(code=2)
    with tempfile.TemporaryDirectory(prefix="pocketkv-cli-") as directory:
        root = Path(directory)
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
        for arguments in [(database, "put", "key"), (database, "nonsense"), (database, "get", "empty", "extra"), (database, "put", "", "bad")]:
            result = run(*arguments, code=2)
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
    print(f"PASS {checks} CLI and independent format checks, including strict reads, explicit recovery and escaped keys")


if __name__ == "__main__":
    main()
