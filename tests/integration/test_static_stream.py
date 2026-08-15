#!/usr/bin/env python3
"""Stage 10 static streaming, persistent static, and request-timeout checks."""

from __future__ import annotations

import argparse
import hashlib
import os
import signal
import socket
import subprocess
import sys
import tempfile
import time
import urllib.error
import urllib.request
from pathlib import Path
from typing import Dict, Optional, Tuple


class HttpConn:
    def __init__(self, sock: socket.socket) -> None:
        self.sock = sock
        self.buf = bytearray()

    def close(self) -> None:
        self.sock.close()

    def send_raw(self, data: bytes | str) -> None:
        if isinstance(data, str):
            data = data.encode("latin1")
        self.sock.sendall(data)

    def _recv_more(self) -> bytes:
        chunk = self.sock.recv(65536)
        if not chunk:
            return b""
        self.buf.extend(chunk)
        return chunk

    def read_response(self, *, expect_body: bool = True) -> Tuple[int, Dict[str, str], bytes]:
        while b"\r\n\r\n" not in self.buf:
            if not self._recv_more():
                raise ConnectionError("connection closed before response headers")
            if len(self.buf) > 2 * 1024 * 1024:
                raise RuntimeError("response headers too large")

        header_blob, _rest = bytes(self.buf).split(b"\r\n\r\n", 1)
        lines = header_blob.split(b"\r\n")
        parts = lines[0].decode("latin1", errors="replace").split(" ", 2)
        if len(parts) < 2:
            raise RuntimeError(f"bad status line: {lines[0]!r}")
        status = int(parts[1])

        headers: Dict[str, str] = {}
        for line in lines[1:]:
            if b":" not in line:
                continue
            name, value = line.split(b":", 1)
            headers[name.decode("latin1").strip().lower()] = value.decode("latin1").strip()

        if "content-length" not in headers:
            raise RuntimeError("missing Content-Length")
        length = int(headers["content-length"])
        header_len = len(header_blob) + 4
        self.buf = bytearray(self.buf[header_len:])

        if not expect_body:
            return status, headers, b""

        while len(self.buf) < length:
            if not self._recv_more():
                raise ConnectionError("connection closed while reading body")
        body = bytes(self.buf[:length])
        self.buf = bytearray(self.buf[length:])
        return status, headers, body

    def recv_closed(self, timeout: float) -> bool:
        self.sock.settimeout(timeout)
        try:
            return self.sock.recv(8) == b""
        except socket.timeout:
            return False


def connect(port: int) -> HttpConn:
    sock = socket.create_connection(("127.0.0.1", port), timeout=3.0)
    sock.settimeout(5.0)
    return HttpConn(sock)


class Server:
    def __init__(
        self,
        binary: Path,
        root: Path,
        port: int,
        keep_alive: int,
        request_timeout: int,
    ) -> None:
        self.binary = binary
        self.root = root
        self.port = port
        self.keep_alive = keep_alive
        self.request_timeout = request_timeout
        self.proc: Optional[subprocess.Popen[str]] = None
        self.log_path = Path("/tmp/cinderhttp-stage10.log")

    def start(self) -> None:
        self.log_path.write_text("", encoding="utf-8")
        log_fh = self.log_path.open("w", encoding="utf-8")
        self.proc = subprocess.Popen(
            [
                str(self.binary),
                "--port",
                str(self.port),
                "--workers",
                "2",
                "--queue-size",
                "16",
                "--root",
                str(self.root),
                "--keep-alive-timeout",
                str(self.keep_alive),
                "--request-timeout",
                str(self.request_timeout),
            ],
            stdout=log_fh,
            stderr=subprocess.STDOUT,
            text=True,
        )
        log_fh.close()
        deadline = time.monotonic() + 5.0
        url = f"http://127.0.0.1:{self.port}/api/health"
        while time.monotonic() < deadline:
            if self.proc.poll() is not None:
                raise RuntimeError(f"server exited early\n{self.log_path.read_text()}")
            try:
                with urllib.request.urlopen(url, timeout=0.5) as resp:
                    if resp.status == 200:
                        return
            except (urllib.error.URLError, TimeoutError, ConnectionError):
                time.sleep(0.05)
        raise RuntimeError(f"server not ready\n{self.log_path.read_text()}")

    def stop(self) -> None:
        if self.proc is None:
            return
        if self.proc.poll() is None:
            self.proc.send_signal(signal.SIGTERM)
            try:
                self.proc.wait(timeout=5)
            except subprocess.TimeoutExpired:
                self.proc.kill()
                self.proc.wait(timeout=2)


def write_fixture(root: Path, name: str, data: bytes) -> None:
    path = root / name
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(data)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--port", type=int, default=18084)
    args = parser.parse_args()

    repo = Path(__file__).resolve().parents[2]
    binary = repo / "bin" / "cinderhttp"
    if not binary.is_file():
        print("FAIL: missing bin/cinderhttp", file=sys.stderr)
        return 1

    failures = 0

    def check(name: str, ok: bool, detail: str = "") -> None:
        nonlocal failures
        if ok:
            print(f"PASS: {name}")
        else:
            failures += 1
            extra = f" ({detail})" if detail else ""
            print(f"FAIL: {name}{extra}")

    with tempfile.TemporaryDirectory(prefix="cinderhttp-stage10-") as tmp:
        root = Path(tmp)
        small = b"hello-static\n"
        binary_blob = bytes(range(256)) * 16  # 4 KiB patterned
        large = (b"ABCDEFGH" * 128) * 1024  # 1 MiB
        zero = b""
        write_fixture(root, "small.txt", small)
        write_fixture(root, "blob.bin", binary_blob)
        write_fixture(root, "large.bin", large)
        write_fixture(root, "empty.dat", zero)
        write_fixture(root, "index.html", b"<html>ok</html>")

        server = Server(binary, root, args.port, keep_alive=1, request_timeout=1)
        try:
            server.start()

            s = connect(args.port)
            s.send_raw("GET /blob.bin HTTP/1.1\r\nHost: localhost\r\n\r\n")
            st, hdrs, body = s.read_response()
            check("binary integrity status", st == 200)
            check("binary integrity bytes", body == binary_blob)
            check("binary content-length", int(hdrs["content-length"]) == len(binary_blob))
            s.close()

            s = connect(args.port)
            s.send_raw("GET /empty.dat HTTP/1.1\r\nHost: localhost\r\n\r\n")
            st, hdrs, body = s.read_response()
            check("zero-byte file", st == 200 and body == b"" and hdrs["content-length"] == "0")
            s.close()

            s = connect(args.port)
            s.send_raw("GET /large.bin HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n")
            st, _, body = s.read_response()
            check("large file", st == 200 and body == large)
            check("large sha256", hashlib.sha256(body).hexdigest() == hashlib.sha256(large).hexdigest())
            s.close()

            s = connect(args.port)
            s.send_raw("HEAD /large.bin HTTP/1.1\r\nHost: localhost\r\n\r\n")
            st, hdrs, body = s.read_response(expect_body=False)
            check(
                "HEAD large no body",
                st == 200 and body == b"" and int(hdrs["content-length"]) == len(large),
            )
            s.send_raw("GET /api/health HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n")
            st2, _, body2 = s.read_response()
            check("HEAD then API", st2 == 200 and b"ok" in body2)
            s.close()

            s = connect(args.port)
            s.send_raw(
                "GET /blob.bin HTTP/1.1\r\nHost: localhost\r\n\r\n"
                "GET /api/health HTTP/1.1\r\nHost: localhost\r\n\r\n"
                "GET /small.txt HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n"
            )
            r1 = s.read_response()
            r2 = s.read_response()
            r3 = s.read_response()
            check(
                "persistent static+api+static",
                r1[0] == 200
                and r1[2] == binary_blob
                and r2[0] == 200
                and b"ok" in r2[2]
                and r3[0] == 200
                and r3[2] == small,
            )
            s.close()

            # Client disconnect during large transfer should not kill the server.
            s = connect(args.port)
            s.send_raw("GET /large.bin HTTP/1.1\r\nHost: localhost\r\n\r\n")
            # Read only headers, then abort.
            while b"\r\n\r\n" not in s.buf:
                s._recv_more()
            s.close()
            time.sleep(0.2)
            with urllib.request.urlopen(f"http://127.0.0.1:{args.port}/api/health", timeout=2) as resp:
                check("alive after client abort mid-file", resp.status == 200)

            # Idle keep-alive closes silently.
            s = connect(args.port)
            s.send_raw("GET /api/health HTTP/1.1\r\nHost: localhost\r\n\r\n")
            s.read_response()
            check("idle close", s.recv_closed(3.0))
            s.close()

            # Partial request → 408
            s = connect(args.port)
            s.send_raw("GET /api/health HTTP/1.1\r\nHo")
            st, hdrs, _ = s.read_response()
            check("partial 408", st == 408)
            check("partial Connection close", hdrs.get("connection", "").lower() == "close")
            check("partial peer closed", s.recv_closed(2.0))
            s.close()

            # Slow trickle stays under per-recv idle but exceeds request deadline.
            s = connect(args.port)
            fragments = [
                b"G",
                b"ET /api/h",
                b"ealth HTTP/1.1\r\n",
                b"Host: local",
                b"host\r\n\r\n",
            ]
            # Spread over >1s total with gaps < keep-alive would not matter once started.
            for frag in fragments[:-1]:
                s.send_raw(frag)
                time.sleep(0.35)
            # Do not send the final fragment; wait for deadline.
            st, hdrs, _ = s.read_response()
            check("trickle 408", st == 408)
            check("trickle close", hdrs.get("connection", "").lower() == "close")
            s.close()

            with urllib.request.urlopen(f"http://127.0.0.1:{args.port}/api/health", timeout=2) as resp:
                check("alive after trickle timeout", resp.status == 200)

        except Exception as exc:
            print(f"FAIL: harness exception: {exc}", file=sys.stderr)
            failures += 1
        finally:
            server.stop()

    if failures:
        print(f"{failures} Stage 10 check(s) failed", file=sys.stderr)
        try:
            print(server.log_path.read_text())
        except OSError:
            pass
        return 1
    print("All Stage 10 static/timeout integration checks passed.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
