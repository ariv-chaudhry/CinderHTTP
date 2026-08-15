#!/usr/bin/env python3
"""Generate deterministic static benchmark fixtures (not committed)."""

from __future__ import annotations

import argparse
import hashlib
from pathlib import Path

# Deterministic pattern: repeating SHA-256 digest of the size key.
SIZES = {
    "4k": 4 * 1024,
    "64k": 64 * 1024,
    "1m": 1024 * 1024,
    "8m": 8 * 1024 * 1024,
}


def pattern_bytes(size: int, label: str) -> bytes:
    seed = hashlib.sha256(f"cinderhttp-fixture:{label}:{size}".encode()).digest()
    out = bytearray()
    while len(out) < size:
        out.extend(seed)
        seed = hashlib.sha256(seed).digest()
    return bytes(out[:size])


def generate(out_dir: Path) -> list[Path]:
    out_dir.mkdir(parents=True, exist_ok=True)
    written: list[Path] = []
    for label, size in SIZES.items():
        path = out_dir / f"benchmark-{label}.bin"
        path.write_bytes(pattern_bytes(size, label))
        written.append(path)
    return written


def main() -> int:
    parser = argparse.ArgumentParser(description="Generate CinderHTTP static benchmark fixtures.")
    parser.add_argument(
        "--out-dir",
        type=Path,
        default=Path(__file__).resolve().parent / "fixtures",
        help="Directory for generated fixtures (default: benchmarks/fixtures)",
    )
    args = parser.parse_args()
    paths = generate(args.out_dir)
    for p in paths:
        print(f"wrote {p} ({p.stat().st_size} bytes)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
