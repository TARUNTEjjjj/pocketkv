#!/usr/bin/env python3
"""Exercise the CLI and recovery policy against disposable synthetic databases."""

from __future__ import annotations

import argparse
import os
from pathlib import Path
import select
import struct
import subprocess
import tempfile
import zlib


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("binary", type=Path)
    parser.add_argument("--allocation-limited", type=Path,
                        help="test build of the CLI whose allocations above POCKETKV_TEST_ALLOCATION_LIMIT bytes fail")
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

    def aliased(database: str, *arguments: str) -> subprocess.CompletedProcess:
        # Pass the database as /dev/fd/N of a read/write descriptor, as
        # `pocketkv /dev/fd/3 put ... 3<>db` does. macOS opens that path as a
        # duplicate of descriptor N, without O_APPEND or a lock of its own.
        descriptor = os.open(database, os.O_RDWR)
        try:
            return subprocess.run([str(binary), f"/dev/fd/{descriptor}", *arguments], capture_output=True,
                                  timeout=10, check=False, pass_fds=(descriptor,))
        finally:
            os.close(descriptor)

    def unread_output(database: str, *arguments: str) -> bytes:
        # Nobody reads this command's output yet, and it is larger than a pipe
        # holds, so the command blocks while printing, as under a pager. By its
        # first output byte it must have closed its Store and released the lock.
        reader = subprocess.Popen([str(binary), database, *arguments],
                                  stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        try:
            ready, _, _ = select.select([reader.stdout], [], [], 10)
            first = os.read(reader.stdout.fileno(), 1) if ready else b""
            check(first != b"", f"blocked reader started its output: {arguments!r}")
            writer = subprocess.run([str(binary), database, "put", "concurrent", "writer"],
                                    capture_output=True, timeout=10, check=False)
            check(writer.returncode == 0 and writer.stdout == b"Saved.\n",
                  f"a reader blocked on its output must not hold the lock: {arguments!r}; {writer}")
        finally:
            try:
                rest, errors = reader.communicate(timeout=10)
            except subprocess.TimeoutExpired:
                reader.kill()
                rest, errors = reader.communicate()
        check(reader.returncode == 0 and errors == b"", f"blocked reader finished: {arguments!r}; {errors!r}")
        return first + rest

    def encode(records: list[tuple[int, bytes, bytes]]) -> bytes:
        """Independent encoder for fixtures too large for a command line."""
        header = struct.pack("<8sI", b"POCKETKV", 1)
        parts = [header, struct.pack("<I", zlib.crc32(header))]
        for kind, key, value in records:
            record = struct.pack("<4sBBHIII", b"PKVR", kind, 0, 0, len(key), len(value), zlib.crc32(key + value))
            parts += [record, struct.pack("<I", zlib.crc32(record)), key, value]
        return b"".join(parts)

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

        # A crash can leave the file longer than its last acknowledged record,
        # with space that was never written reading back as zeros. Only zeros
        # hold no record, so repair discards them; any other tail is corruption,
        # reported with the offset where the valid records end.
        zero_database = str(root / "zero-tail.db")
        run(zero_database, "put", "alpha", "acknowledged-1", output=b"Saved.\n")
        run(zero_database, "put", "beta", "acknowledged-2", output=b"Saved.\n")
        acknowledged = Path(zero_database).read_bytes()
        end = len(acknowledged)
        for length in (5, 24, 4096):
            Path(zero_database).write_bytes(acknowledged + bytes(length))
            result = run(zero_database, "verify", code=2)
            check(f"at byte {end} (only zero bytes)".encode() in result.stderr and
                  b"Run the explicit repair command" in result.stderr,
                  f"a zero-filled tail asks for repair: {result.stderr!r}")
            check(Path(zero_database).read_bytes() == acknowledged + bytes(length), "verify preserves a zero-filled tail")
            run(zero_database, "repair", output=f"Repaired {length} bytes.\n".encode())
            check(Path(zero_database).read_bytes() == acknowledged, "repair cuts exactly at the last acknowledged record")
            run(zero_database, "get", "beta", output=b"acknowledged-2\n")
        for tail in (bytes(range(0x30, 0x58)), bytes(24) + b"\x01", b"PKVR" + bytes(40)):
            Path(zero_database).write_bytes(acknowledged + tail)
            for command in ("verify", "repair"):
                result = run(zero_database, command, code=2)
                check(result.stderr.startswith(b"error: ") and result.stderr.endswith(f" at byte {end}\n".encode()),
                      f"corruption names where the valid records end: {command}; {result.stderr!r}")
                check(Path(zero_database).read_bytes() == acknowledged + tail, "repair refuses a tail that is not all zeros")

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

        # A /dev/fd/N alias is refused before anything is written: on macOS it
        # would append at the caller's offset (over the file header) and share
        # the caller's lock; Linux refuses the /proc symlink through O_NOFOLLOW.
        alias_database = str(root / "alias.db")
        run(alias_database, "put", "alpha", "first", output=b"Saved.\n")
        before = Path(alias_database).read_bytes()
        alias_copy = root / "alias-copy.db"
        for arguments in [("put", "gamma", "third"), ("delete", "alpha"), ("get", "alpha"), ("verify",),
                          ("keys-prefix", ""), ("snapshot", str(alias_copy))]:
            result = aliased(alias_database, *arguments)
            check(result.returncode == 2 and result.stdout == b"" and bool(result.stderr),
                  f"descriptor alias must be refused: {arguments!r}; {result}")
            check(Path(alias_database).read_bytes() == before, "a refused descriptor alias never changes the database")
        check(not alias_copy.exists(), "a refused descriptor alias is never copied to a snapshot")
        run(alias_database, "get", "alpha", output=b"first\n")

        # Commands close the database before printing, so output that nobody
        # reads yet (a pager, a slow pipe) never locks other commands out.
        listing_database = str(root / "long-listing.db")
        listed = [f"listed-{index:04}-".encode().ljust(64, b"k") for index in range(4096)]
        Path(listing_database).write_bytes(encode([(1, key, b"") for key in listed]))
        check(unread_output(listing_database, "keys") == b"".join(key + b"\n" for key in listed),
              "a keys listing that blocked is complete")
        value_database = str(root / "large-value.db")
        large_value = bytes(range(256)) * 4096
        Path(value_database).write_bytes(encode([(1, b"large", large_value)]))
        check(unread_output(value_database, "get-raw", "large") == large_value, "a get-raw that blocked is complete")
        page_database = str(root / "long-page.db")
        paged_keys = [f"paged-{index:04}-".encode().ljust(1024, b"k") for index in range(1000)]
        Path(page_database).write_bytes(encode([(1, key, b"") for key in paged_keys]))
        check(unread_output(page_database, "keys-prefix", "paged-", "1000") == b"".join(key + b"\n" for key in paged_keys),
              "a keys-prefix page that blocked is complete")
        for blocked in [listing_database, value_database, page_database]:
            run(blocked, "get", "concurrent", output=b"writer\n")

        # Running out of memory is reported like any other output failure: a
        # command prints its whole output and exits 0, or exits 2 with an error.
        # A test build of the CLI fails every allocation above a limit; the
        # limits span each output's size, from too little to run at all to plenty.
        if args.allocation_limited is not None:
            limited = args.allocation_limited.resolve(strict=True)
            for arguments in [(listing_database, "keys"), (value_database, "get-raw", "large")]:
                expected = run(*arguments).stdout
                outcomes = set()
                for percent in (25, 50, 75, 100, 125, 150, 200, 300):
                    limit = len(expected) * percent // 100
                    result = subprocess.run([str(limited), *arguments], capture_output=True, timeout=10, check=False,
                                            env=dict(os.environ, POCKETKV_TEST_ALLOCATION_LIMIT=str(limit)))
                    outcomes.add(result.returncode)
                    if result.returncode == 0:
                        check(result.stdout == expected and result.stderr == b"",
                              f"exit 0 under an allocation limit means complete output: {arguments[1:]!r}, "
                              f"limit {limit}; printed {len(result.stdout)} of {len(expected)} bytes")
                    else:
                        check(result.returncode == 2 and result.stderr.startswith(b"error: "),
                              f"running out of memory is reported: {arguments[1:]!r}, limit {limit}; "
                              f"exit={result.returncode}; stderr={result.stderr!r}")
                check(outcomes == {0, 2}, f"the allocation limits both stop and permit {arguments[1:]!r}: {outcomes}")
    print(f"PASS {checks} CLI and independent format checks, including argument validation, output failures and recovery")


if __name__ == "__main__":
    main()
