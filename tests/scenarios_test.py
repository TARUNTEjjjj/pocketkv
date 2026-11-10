#!/usr/bin/env python3
"""Run application, binary import, lock and SIGKILL scenarios on disposable data."""

from __future__ import annotations

import argparse
import os
from pathlib import Path
import select
import signal
import struct
import subprocess
import tempfile
import time
import zlib


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("binary", type=Path)
    parser.add_argument("driver", type=Path)
    args = parser.parse_args()
    binary = args.binary.resolve(strict=True)
    driver = args.driver.resolve(strict=True)
    checks = 0

    def check(condition: bool, message: str) -> None:
        nonlocal checks
        checks += 1
        if not condition:
            raise AssertionError(message)

    def run(database: Path, *arguments: str, data: bytes | None = None,
            code: int = 0) -> subprocess.CompletedProcess:
        result = subprocess.run([str(binary), str(database), *arguments], input=data,
                                capture_output=True, timeout=10, check=False)
        check(result.returncode == code,
              f"{arguments[:2]!r}: exit {result.returncode}, stderr={result.stderr!r}")
        return result

    def replay(database: Path) -> tuple[dict[bytes, bytes], int]:
        """An independent standard-library decoder checks every record and CRC."""
        data = database.read_bytes()
        check(len(data) >= 16, "complete database header")
        magic, version, checksum = struct.unpack_from("<8sII", data)
        check(magic == b"POCKETKV" and version == 1 and checksum == zlib.crc32(data[:12]),
              "valid independent database header")
        state: dict[bytes, bytes] = {}
        records = 0
        offset = 16
        while offset < len(data):
            check(len(data) - offset >= 24, "complete record header")
            magic, kind, flags, reserved, key_size, value_size, crc, header_crc = struct.unpack_from(
                "<4sBBHIIII", data, offset)
            check(magic == b"PKVR" and kind in (1, 2) and flags == reserved == 0,
                  "valid independent record metadata")
            check(header_crc == zlib.crc32(data[offset:offset + 20]), "record header CRC")
            check(1 <= key_size <= 4096 and value_size <= 1048576 and
                  (kind != 2 or value_size == 0), "bounded record lengths")
            offset += 24
            payload = data[offset:offset + key_size + value_size]
            check(len(payload) == key_size + value_size and zlib.crc32(payload) == crc,
                  "complete independently checksummed record payload")
            key, value = payload[:key_size], payload[key_size:]
            if kind == 1:
                state[key] = value
            else:
                state.pop(key, None)
            offset += len(payload)
            records += 1
        return state, records

    def read_line(process: subprocess.Popen) -> bytes:
        # Unbuffered stdout avoids Python prefetch hiding data from select().
        deadline = time.monotonic() + 10
        result = bytearray()
        while not result.endswith(b"\n"):
            remaining = deadline - time.monotonic()
            if remaining <= 0 or not select.select([process.stdout], [], [], remaining)[0]:
                raise AssertionError("writer did not reach the expected protocol barrier")
            byte = os.read(process.stdout.fileno(), 1)
            if not byte:
                raise AssertionError(f"writer exited before ACK: {bytes(result)!r}")
            result.extend(byte)
        return bytes(result)

    def kill(process: subprocess.Popen) -> tuple[bytes, bytes]:
        process.kill()
        out, err = process.communicate(timeout=10)
        check(process.returncode == -signal.SIGKILL, "process really ended with SIGKILL")
        return out, err

    def send_prefix(process: subprocess.Popen, data: bytes) -> None:
        """Deliver input with a deadline, leaving the pipe open without EOF."""
        os.set_blocking(process.stdin.fileno(), False)
        remaining = memoryview(data)
        deadline = time.monotonic() + 10
        while remaining:
            wait = deadline - time.monotonic()
            if wait <= 0 or not select.select([], [process.stdin], [], wait)[1]:
                raise AssertionError("import did not consume pipe input")
            try:
                sent = os.write(process.stdin.fileno(), remaining)
                remaining = remaining[sent:]
            except BlockingIOError:
                continue

    with tempfile.TemporaryDirectory(prefix="pocketkv-scenarios-") as directory:
        root = Path(directory)

        # Session-cache lifecycle: every command is a fresh process/reopen, and
        # a Python dictionary independently tracks updates and expired entries.
        database = root / "sessions.db"
        expected: dict[bytes, bytes] = {}
        records = 0
        for number in range(24):
            key = f"session/{number:03d}"
            value = f'{{"user":{number},"visits":1}}'.encode()
            run(database, "put-stdin", key, data=value)
            expected[key.encode()] = value
            records += 1
        run(database, "put", "config/theme", "dark")
        expected[b"config/theme"] = b"dark"
        records += 1
        for number in range(0, 24, 2):
            key = f"session/{number:03d}"
            value = f'{{"user":{number},"visits":2}}'.encode()
            run(database, "put-stdin", key, data=value)
            expected[key.encode()] = value
            records += 1
        for number in range(0, 24, 3):
            key = f"session/{number:03d}"
            run(database, "delete", key)
            del expected[key.encode()]
            records += 1
        found: list[bytes] = []
        cursor: str | None = None
        while True:
            arguments = ["keys-prefix", "session/", "5"]
            if cursor is not None:
                arguments.append(cursor)
            page = run(database, *arguments).stdout.splitlines()
            if not page:
                break
            check(len(page) <= 5 and (not found or page[0] > found[-1]), "bounded forward pages")
            found.extend(page)
            cursor = page[-1].decode()
        check(found == sorted(key for key in expected if key.startswith(b"session/")),
              "session pagination matches independent live-key ledger")
        for key, value in expected.items():
            check(run(database, "get-raw", key.decode()).stdout == value, "persisted session value")
        check(replay(database) == (expected, records), "session log exactly matches independent ledger")

        # File import and pipeline input preserve all bytes, including embedded
        # NUL, high bytes and newline, across independent CLI invocations.
        binary_db = root / "assets.db"
        payload = bytes(range(256)) * 4096  # Exactly the supported 1 MiB maximum.
        source = root / "asset.bin"
        source.write_bytes(payload)
        with source.open("rb") as stream:
            result = subprocess.run([str(binary), str(binary_db), "put-stdin", "asset"],
                                    stdin=stream, capture_output=True, timeout=10, check=False)
        check(result.returncode == 0 and result.stdout == b"Saved.\n", "maximum-size file import")
        check(run(binary_db, "get-raw", "asset").stdout == payload, "full binary file roundtrip")
        for value in [b"", b"first\x00second\xff\n", payload]:
            run(binary_db, "put-stdin", "pipe", data=value)
            check(run(binary_db, "get-raw", "pipe").stdout == value, "binary pipe roundtrip")
        before = binary_db.read_bytes()
        for target in [binary_db, root / "rejected.db"]:
            run(target, "put-stdin", "asset", data=payload + b"x", code=2)
            check(target.read_bytes() == before if target == binary_db else not target.exists(),
                  "oversize input neither changes nor creates a database")
            for key in ["", "x" * 4097]:
                run(target, "put-stdin", key, data=b"replacement", code=2)
                check(target.read_bytes() == before if target == binary_db else not target.exists(),
                      "invalid import key preserves database")

        # A closed descriptor and a directory descriptor are input errors, not
        # a successful empty value. Test both existing and absent databases.
        for target in [binary_db, root / "bad-input.db"]:
            for invalid_input in ["closed", "directory"]:
                descriptor = os.open(root, os.O_RDONLY) if invalid_input == "directory" else None
                try:
                    result = subprocess.run(
                        [str(binary), str(target), "put-stdin", "asset"],
                        stdin=descriptor, capture_output=True, timeout=10, check=False,
                        preexec_fn=(lambda: os.close(0)) if invalid_input == "closed" else None)
                finally:
                    if descriptor is not None:
                        os.close(descriptor)
                check(result.returncode == 2 and bool(result.stderr), "unreadable stdin is rejected")
                check(target.read_bytes() == before if target == binary_db else not target.exists(),
                      "stdin errors preserve existing and absent database state")

        # Rejected arguments must not wait for input. Oversize streams must
        # stop at max+1 bytes, even when their producer never closes the pipe.
        rejected = root / "open-pipe-rejected.db"
        for target in [binary_db, rejected]:
            for arguments, data in [
                (["put-stdin"], None),
                (["put-stdin", "asset", "extra"], None),
                (["put-stdin", ""], None),
                (["put-stdin", "x" * 4097], None),
                (["put-stdin", "asset"], payload + b"x"),
            ]:
                process = subprocess.Popen([str(binary), str(target), *arguments],
                                           stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                           stderr=subprocess.PIPE, bufsize=0)
                try:
                    if data is not None:
                        send_prefix(process, data)
                    # stdin is deliberately still open here; timeout indicates
                    # an incorrect wait for EOF or unbounded stream reader.
                    check(process.wait(timeout=10) == 2, "invalid input exits before pipe EOF")
                    out, err = process.communicate(timeout=10)
                    check(out == b"" and bool(err), "rejected input reports only an error")
                    check(target.read_bytes() == before if target == binary_db else not target.exists(),
                          "open-pipe rejection preserves existing and absent database state")
                finally:
                    if process.poll() is None:
                        process.kill()
                    process.communicate(timeout=10)

        # Keep stdin open after delivering a substantial prefix. A reader must
        # not take the database lock or commit a partial value before EOF.
        process = subprocess.Popen([str(binary), str(binary_db), "put-stdin", "asset"],
                                   stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                   stderr=subprocess.PIPE, bufsize=0)
        try:
            send_prefix(process, payload[:524288])
            check(process.poll() is None, "partial input waits for EOF")
            check(run(binary_db, "get-raw", "asset").stdout == payload,
                  "slow input does not lock or replace the existing value")
            kill(process)
            check(binary_db.read_bytes() == before, "killed partial import leaves every database byte intact")
        finally:
            if process.poll() is None:
                process.kill()
            process.communicate(timeout=10)

        # Kill the lock owner only after receiving ACKs. Pipe barriers prove
        # all these puts returned successfully before SIGKILL; no timing sleeps.
        crash_db = root / "worker.db"
        process = subprocess.Popen([str(driver), str(crash_db)], stdin=subprocess.PIPE,
                                   stdout=subprocess.PIPE, stderr=subprocess.PIPE, bufsize=0)
        try:
            check(read_line(process) == b"READY\n", "worker owns the database")
            for sequence in range(8):
                process.stdin.write(b"W")
                check(read_line(process) == f"ACK {sequence}\n".encode(), "durable write acknowledged")
            before = crash_db.read_bytes()
            locked = run(crash_db, "get-raw", "job/0", code=2)
            check(b"lock" in locked.stderr.lower(), "second process reports lock contention")
            check(crash_db.read_bytes() == before, "lock contender preserves database")
            _, errors = kill(process)
            check(errors == b"", "worker had no write errors")
        finally:
            if process.poll() is None:
                process.kill()
            process.communicate(timeout=10)
        expected_jobs = {f"job/{number}".encode(): bytes([ord("a") + number]) * 4096 for number in range(8)}
        check(replay(crash_db) == (expected_jobs, 8), "SIGKILL preserves every acknowledged write")
        run(crash_db, "verify")
        run(crash_db, "put", "after-restart", "works")
        check(run(crash_db, "get-raw", "after-restart").stdout == b"works", "lock is released after process death")

        # A racing kill may land before, during or after any append. Collect all
        # delivered ACKs and allow the final unacknowledged append to be durable.
        # Recovery must retain every ACK; this does not simulate power loss.
        for attempt in range(3):
            target = root / f"inflight-{attempt}.db"
            process = subprocess.Popen([str(driver), str(target)], stdin=subprocess.PIPE,
                                       stdout=subprocess.PIPE, stderr=subprocess.PIPE, bufsize=0)
            try:
                check(read_line(process) == b"READY\n", "in-flight worker ready")
                process.stdin.write(b"R")
                first_ack = read_line(process)
                check(first_ack == b"ACK 0\n", "first batch write acknowledged")
                queued, errors = kill(process)
                check(errors == b"", "in-flight worker had no errors")
                acknowledgements = [int(line.split()[1]) for line in (first_ack + queued).splitlines()]
                check(acknowledgements == list(range(len(acknowledgements))), "ACK sequence is contiguous")
            finally:
                if process.poll() is None:
                    process.kill()
                process.communicate(timeout=10)
            damaged = target.read_bytes()
            verification = subprocess.run([str(binary), str(target), "verify"], capture_output=True,
                                          timeout=10, check=False)
            check(target.read_bytes() == damaged, "strict reopen after kill never changes bytes")
            if verification.returncode != 0:
                check(verification.returncode == 2 and b"explicit repair" in verification.stderr,
                      "only an incomplete final record may require recovery")
                run(target, "repair")
            state, count = replay(target)
            check(len(acknowledgements) <= count <= len(acknowledgements) + 1,
                  "at most the in-progress unacknowledged write can be durable")
            expected_crash = {f"job/{number}".encode(): bytes([ord("a") + number % 26]) * 4096
                              for number in range(count)}
            check(state == expected_crash, "restart retains acknowledged jobs without corrupted values")
            run(target, "verify")

    print(f"PASS {checks} real-scenario checks: sessions, binary imports, lock contention, SIGKILL and restart")


if __name__ == "__main__":
    main()
