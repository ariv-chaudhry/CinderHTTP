#!/usr/bin/env python3
"""Stage 9 persistent-connection integration checks (stdlib only)."""

from __future__ import annotations

import argparse
import concurrent.futures
import json
import signal
import socket
import subprocess
import sys
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

    def send_raw(self, data: str) -> None:
        self.sock.sendall(data.encode("latin1"))

    def _recv_more(self) -> bytes:
        chunk = self.sock.recv(4096)
        if not chunk:
            return b""
        self.buf.extend(chunk)
        return chunk

    def read_response(self, *, expect_body: bool = True) -> Tuple[int, Dict[str, str], bytes]:
        """Read one HTTP response via Content-Length framing.

        For HEAD, pass expect_body=False so Content-Length is not treated as
        on-wire body bytes (which would steal the next response).
        """
        while b"\r\n\r\n" not in self.buf:
            if not self._recv_more():
                raise ConnectionError("connection closed before response headers")
            if len(self.buf) > 1024 * 1024:
                raise RuntimeError("response headers too large")

        header_blob, _rest = bytes(self.buf).split(b"\r\n\r\n", 1)
        lines = header_blob.split(b"\r\n")
        status_line = lines[0].decode("latin1", errors="replace")
        parts = status_line.split(" ", 2)
        if len(parts) < 2:
            raise RuntimeError(f"bad status line: {status_line!r}")
        status = int(parts[1])

        headers: Dict[str, str] = {}
        for line in lines[1:]:
            if b":" not in line:
                continue
            name, value = line.split(b":", 1)
            headers[name.decode("latin1").strip().lower()] = value.decode("latin1").strip()

        if "content-length" not in headers:
            raise RuntimeError("persistent responses require Content-Length")
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
            data = self.sock.recv(8)
            return data == b""
        except socket.timeout:
            return False


def connect(port: int) -> HttpConn:
    sock = socket.create_connection(("127.0.0.1", port), timeout=3.0)
    sock.settimeout(3.0)
    return HttpConn(sock)


class Server:
    def __init__(self, binary: Path, root: Path, port: int, timeout: int) -> None:
        self.binary = binary
        self.root = root
        self.port = port
        self.timeout = timeout
        self.proc: Optional[subprocess.Popen[str]] = None
        self.log_path = Path("/tmp/cinderhttp-keepalive.log")

    def start(self) -> None:
        self.log_path.write_text("", encoding="utf-8")
        log_fh = self.log_path.open("w", encoding="utf-8")
        self.proc = subprocess.Popen(
            [
                str(self.binary),
                "--port",
                str(self.port),
                "--workers",
                "4",
                "--queue-size",
                "32",
                "--root",
                str(self.root),
                "--keep-alive-timeout",
                str(self.timeout),
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


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--port", type=int, default=18082)
    parser.add_argument("--timeout", type=int, default=1)
    args = parser.parse_args()

    root = Path(__file__).resolve().parents[2]
    binary = root / "bin" / "cinderhttp"
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

    server = Server(binary, root / "public", args.port, args.timeout)
    try:
        server.start()

        s = connect(args.port)
        s.send_raw(
            "GET /api/health HTTP/1.1\r\nHost: localhost\r\n\r\n"
            "GET /api/stats HTTP/1.1\r\nHost: localhost\r\n\r\n"
        )
        st1, _, b1 = s.read_response()
        st2, _, b2 = s.read_response()
        check(
            "A health then stats",
            st1 == 200 and st2 == 200 and b"status" in b1 and b"requests_total" in b2,
        )
        s.close()

        s = connect(args.port)
        s.send_raw(
            "GET / HTTP/1.1\r\nHost: localhost\r\n\r\n"
            "GET /api/health HTTP/1.1\r\nHost: localhost\r\n\r\n"
            "GET /missing HTTP/1.1\r\nHost: localhost\r\n\r\n"
        )
        codes = [s.read_response()[0] for _ in range(3)]
        check("B 200/200/404", codes == [200, 200, 404])
        s.close()

        s = connect(args.port)
        s.send_raw("GET /api/health HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n")
        st, hdrs, _ = s.read_response()
        check("C status 200", st == 200)
        check("C Connection close", hdrs.get("connection", "").lower() == "close")
        check("C server closed", s.recv_closed(1.0))
        s.close()

        s = connect(args.port)
        s.send_raw("GET /api/health HTTP/1.0\r\nHost: localhost\r\n\r\n")
        st, hdrs, _ = s.read_response()
        check("D HTTP/1.0 close header", st == 200 and hdrs.get("connection", "").lower() == "close")
        check("D peer closed", s.recv_closed(1.0))
        s.close()

        s = connect(args.port)
        s.send_raw(
            "GET /api/health HTTP/1.0\r\nHost: localhost\r\nConnection: keep-alive\r\n\r\n"
        )
        st, hdrs, _ = s.read_response()
        check(
            "E HTTP/1.0 keep-alive response",
            st == 200 and hdrs.get("connection", "").lower() == "keep-alive",
        )
        s.send_raw(
            "GET /api/health HTTP/1.0\r\nHost: localhost\r\nConnection: keep-alive\r\n\r\n"
        )
        st2, _, _ = s.read_response()
        check("E second request", st2 == 200)
        s.close()

        s = connect(args.port)
        s.send_raw(
            "HEAD /api/health HTTP/1.1\r\nHost: localhost\r\n\r\n"
            "GET /api/health HTTP/1.1\r\nHost: localhost\r\n\r\n"
        )
        st_h, hdr_h, body_h = s.read_response(expect_body=False)
        st_g, _, body_g = s.read_response()
        check(
            "F HEAD then GET",
            st_h == 200
            and st_g == 200
            and body_h == b""
            and int(hdr_h["content-length"]) == len(body_g),
        )
        s.close()

        s = connect(args.port)
        s.send_raw(
            "POST /api/echo HTTP/1.1\r\nHost: localhost\r\nContent-Length: 5\r\n\r\n"
            "hello"
            "GET /api/health HTTP/1.1\r\nHost: localhost\r\n\r\n"
        )
        st_p, _, body_p = s.read_response()
        st_g, _, body_g = s.read_response()
        check(
            "G POST then GET",
            st_p == 200 and body_p == b"hello" and st_g == 200 and b"ok" in body_g,
        )
        s.close()

        s = connect(args.port)
        s.send_raw("GET / HTTP/1.1\r\nBadHeader\r\n\r\n")
        st, hdrs, _ = s.read_response()
        check("H malformed 400", st == 400)
        check("H closes", hdrs.get("connection", "").lower() == "close")
        check("H peer closed", s.recv_closed(1.0))
        s.close()

        s = connect(args.port)
        s.send_raw("GET /api/health HTTP/1.1\r\nHost: localhost\r\n\r\n")
        st, _, _ = s.read_response()
        check("I first request", st == 200)
        check("I idle close", s.recv_closed(float(args.timeout + 2)))
        s.close()

        s = connect(args.port)
        for _ in range(3):
            s.send_raw("GET /api/health HTTP/1.1\r\nHost: localhost\r\n\r\n")
            s.read_response()
        s.close()
        with urllib.request.urlopen(f"http://127.0.0.1:{args.port}/api/stats", timeout=2) as resp:
            stats = json.loads(resp.read().decode())
        check("stats connections_accepted >= 1", stats.get("connections_accepted", 0) >= 1)
        check("stats requests_total >= 3", stats.get("requests_total", 0) >= 3)

        # Final allowed response among HTTP_MAX_REQUESTS_PER_CONNECTION should
        # advertise Connection: close.
        max_req = 100
        limits = (root / "include" / "http_limits.h").read_text(encoding="utf-8")
        for line in limits.splitlines():
            if "HTTP_MAX_REQUESTS_PER_CONNECTION" in line and line.strip().startswith("#define"):
                max_req = int(line.split()[-1])
                break
        s = connect(args.port)
        last_hdrs: Dict[str, str] = {}
        ok_max = True
        for _i in range(max_req):
            s.send_raw("GET /api/health HTTP/1.1\r\nHost: localhost\r\n\r\n")
            st, hdrs, _ = s.read_response()
            if st != 200:
                ok_max = False
                break
            last_hdrs = hdrs
        check("max-requests all succeed", ok_max)
        check(
            "max-requests final Connection close",
            last_hdrs.get("connection", "").lower() == "close",
        )
        check("max-requests peer closed", s.recv_closed(1.0))
        s.close()

        def client_session(_: int) -> bool:
            sock = connect(args.port)
            try:
                for _i in range(5):
                    sock.send_raw("GET /api/health HTTP/1.1\r\nHost: localhost\r\n\r\n")
                    st_code, _, _ = sock.read_response()
                    if st_code != 200:
                        return False
                return True
            finally:
                sock.close()

        with concurrent.futures.ThreadPoolExecutor(max_workers=8) as ex:
            results = list(ex.map(client_session, range(8)))
        check("concurrent 8x5 keep-alive", all(results))

    except Exception as exc:
        print(f"FAIL: harness exception: {exc}", file=sys.stderr)
        failures += 1
    finally:
        server.stop()

    if failures:
        print(f"{failures} keep-alive check(s) failed", file=sys.stderr)
        try:
            print(server.log_path.read_text())
        except OSError:
            pass
        return 1
    print("All Stage 9 keep-alive integration checks passed.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
