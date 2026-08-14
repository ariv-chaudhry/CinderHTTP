#!/usr/bin/env python3
"""Send exact raw HTTP bytes to a host:port and print the response.

Usage:
  raw_http.py HOST PORT [--timeout SEC] [--half-close] [--chunks MS] [--]
      BYTESTRING

BYTESTRING may contain escapes: \\r \\n \\t \\0 \\\\ \\xHH

Or pass --file PATH to read raw request bytes from a file.
"""
from __future__ import annotations

import argparse
import socket
import sys
import time


def decode_bytes(text: str) -> bytes:
    out = bytearray()
    i = 0
    while i < len(text):
        if text[i] == "\\" and i + 1 < len(text):
            nxt = text[i + 1]
            if nxt == "r":
                out.append(0x0D)
                i += 2
            elif nxt == "n":
                out.append(0x0A)
                i += 2
            elif nxt == "t":
                out.append(0x09)
                i += 2
            elif nxt == "0":
                out.append(0x00)
                i += 2
            elif nxt == "\\":
                out.append(0x5C)
                i += 2
            elif nxt == "x" and i + 3 < len(text):
                out.append(int(text[i + 2 : i + 4], 16))
                i += 4
            else:
                out.append(ord(text[i]))
                i += 1
        else:
            out.append(ord(text[i]))
            i += 1
    return bytes(out)


def main() -> int:
    parser = argparse.ArgumentParser(description="Raw HTTP client helper")
    parser.add_argument("host")
    parser.add_argument("port", type=int)
    parser.add_argument("--timeout", type=float, default=2.0)
    parser.add_argument("--half-close", action="store_true")
    parser.add_argument(
        "--chunk-pause",
        type=float,
        default=0.0,
        help="Seconds to pause between --chunk pieces",
    )
    parser.add_argument(
        "--chunk",
        action="append",
        default=[],
        help="Send this piece (escaped); may be repeated",
    )
    parser.add_argument("--file", help="Read request bytes from file")
    parser.add_argument("payload", nargs="?", default="")
    args = parser.parse_args()

    if args.file:
        with open(args.file, "rb") as fh:
            pieces = [fh.read()]
    elif args.chunk:
        pieces = [decode_bytes(c) for c in args.chunk]
    else:
        pieces = [decode_bytes(args.payload)]

    sock = socket.create_connection((args.host, args.port), timeout=args.timeout)
    sock.settimeout(args.timeout)
    try:
        for idx, piece in enumerate(pieces):
            if piece:
                sock.sendall(piece)
            if args.chunk_pause > 0 and idx + 1 < len(pieces):
                time.sleep(args.chunk_pause)
        if args.half_close:
            try:
                sock.shutdown(socket.SHUT_WR)
            except OSError:
                pass

        chunks = []
        while True:
            try:
                data = sock.recv(65536)
            except socket.timeout:
                break
            if not data:
                break
            chunks.append(data)
        sys.stdout.buffer.write(b"".join(chunks))
        return 0
    finally:
        sock.close()


if __name__ == "__main__":
    sys.exit(main())
