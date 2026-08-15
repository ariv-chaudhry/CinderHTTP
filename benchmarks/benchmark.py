#!/usr/bin/env python3
"""CinderHTTP Stage 8 benchmark harness (stdlib only).

Starts fresh release-mode server instances across worker counts, warms up,
runs repeated measurements with wrk/hey/ab, and writes JSON/CSV summaries.

After Stage 9, CinderHTTP supports HTTP persistent connections with a bounded
keep-alive/read timeout; load tools may reuse connections.
"""

from __future__ import annotations

import argparse
import csv
import json
import os
import platform
import re
import shutil
import signal
import socket
import statistics
import subprocess
import sys
import tempfile
import time
import urllib.error
import urllib.request
from dataclasses import asdict, dataclass, field
from datetime import datetime, timezone
from pathlib import Path
from typing import Any, Dict, List, Optional, Sequence, Tuple

TOOL_PREFERENCE = ("wrk", "hey", "ab")
DEFAULT_PORT = 18081
DEFAULT_PATH = "/api/health"
DEFAULT_DURATION = 10.0
DEFAULT_CONNECTIONS = 64
DEFAULT_ITERATIONS = 3
DEFAULT_WORKERS = (1, 2, 4, 8)
DEFAULT_QUEUE_SIZE = 256
WARMUP_DURATION = 2.0
READY_TIMEOUT_S = 5.0
SHUTDOWN_TIMEOUT_S = 5.0


# ---------------------------------------------------------------------------
# Unit / parsing helpers (tested independently)
# ---------------------------------------------------------------------------


def parse_latency_to_ms(token: str) -> float:
    """Convert wrk/hey-style latency tokens to milliseconds."""
    text = token.strip().lower().replace(",", "")
    if not text:
        raise ValueError("empty latency token")

    match = re.fullmatch(r"([+-]?(?:\d+\.?\d*|\.\d+))\s*([a-zµu]*)", text)
    if not match:
        raise ValueError(f"unrecognized latency token: {token!r}")

    value = float(match.group(1))
    unit = match.group(2)
    if unit in ("", "ms", "msec", "millis", "millisecond", "milliseconds"):
        return value
    if unit in ("us", "usec", "µs", "μs", "microsecond", "microseconds"):
        return value / 1000.0
    if unit in ("s", "sec", "secs", "second", "seconds"):
        return value * 1000.0
    if unit in ("ns", "nsec"):
        return value / 1_000_000.0
    raise ValueError(f"unsupported latency unit in {token!r}")


def parse_rate(token: str) -> float:
    """Convert rates such as 12.3k or 1.2M to a plain float."""
    text = token.strip().lower().replace(",", "")
    match = re.fullmatch(r"([+-]?(?:\d+\.?\d*|\.\d+))\s*([km])?", text)
    if not match:
        raise ValueError(f"unrecognized rate token: {token!r}")
    value = float(match.group(1))
    suffix = match.group(2)
    if suffix == "k":
        return value * 1_000.0
    if suffix == "m":
        return value * 1_000_000.0
    return value


def parse_bytes_per_sec(token: str) -> float:
    """Convert transfer rates (B/KB/MB/GB per second) to bytes/sec."""
    text = token.strip().lower().replace(",", "")
    match = re.fullmatch(
        r"([+-]?(?:\d+\.?\d*|\.\d+))\s*([kmgt]?b)?(?:/?s|/sec)?",
        text,
    )
    if not match:
        raise ValueError(f"unrecognized transfer token: {token!r}")
    value = float(match.group(1))
    unit = match.group(2) or "b"
    multipliers = {
        "b": 1.0,
        "kb": 1024.0,
        "mb": 1024.0**2,
        "gb": 1024.0**3,
        "tb": 1024.0**4,
    }
    if unit not in multipliers:
        raise ValueError(f"unsupported transfer unit in {token!r}")
    return value * multipliers[unit]


def parse_workers_list(text: str) -> List[int]:
    """Parse comma-separated positive worker counts; reject duplicates."""
    if not text or not text.strip():
        raise ValueError("workers list is empty")
    parts = [p.strip() for p in text.split(",")]
    workers: List[int] = []
    seen = set()
    for part in parts:
        if not part:
            raise ValueError(f"invalid workers entry in {text!r}")
        try:
            value = int(part, 10)
        except ValueError as exc:
            raise ValueError(f"invalid workers value {part!r}") from exc
        if value <= 0:
            raise ValueError(f"workers must be positive (got {value})")
        if value in seen:
            raise ValueError(f"duplicate workers value {value}")
        seen.add(value)
        workers.append(value)
    return workers


# ---------------------------------------------------------------------------
# Result model
# ---------------------------------------------------------------------------


@dataclass
class RunResult:
    workers: int
    iteration: int
    requests_per_second: Optional[float] = None
    average_latency_ms: Optional[float] = None
    p95_latency_ms: Optional[float] = None
    max_latency_ms: Optional[float] = None
    total_requests: Optional[int] = None
    transfer_bytes_per_sec: Optional[float] = None
    errors: int = 0
    non_2xx: int = 0
    valid: bool = True
    notes: List[str] = field(default_factory=list)
    raw_excerpt: str = ""


def aggregate_runs(runs: Sequence[RunResult]) -> Dict[str, Any]:
    valid = [r for r in runs if r.valid]
    if not valid:
        return {
            "requests_per_second_mean": None,
            "requests_per_second_stdev": None,
            "average_latency_ms_mean": None,
            "p95_latency_ms_mean": None,
            "errors_total": sum(r.errors for r in runs),
            "valid_iterations": 0,
        }

    rps = [r.requests_per_second for r in valid if r.requests_per_second is not None]
    avg = [r.average_latency_ms for r in valid if r.average_latency_ms is not None]
    p95 = [r.p95_latency_ms for r in valid if r.p95_latency_ms is not None]

    def mean_or_none(values: List[float]) -> Optional[float]:
        return statistics.mean(values) if values else None

    def stdev_or_none(values: List[float]) -> Optional[float]:
        if len(values) >= 2:
            return statistics.stdev(values)
        return 0.0 if len(values) == 1 else None

    return {
        "requests_per_second_mean": mean_or_none(rps),
        "requests_per_second_stdev": stdev_or_none(rps),
        "average_latency_ms_mean": mean_or_none(avg),
        "p95_latency_ms_mean": mean_or_none(p95) if len(p95) == len(valid) else None,
        "errors_total": sum(r.errors + r.non_2xx for r in runs),
        "valid_iterations": len(valid),
    }


# ---------------------------------------------------------------------------
# Tool output parsers
# ---------------------------------------------------------------------------


def _extract_tagged_float(output: str, key: str) -> Optional[float]:
    match = re.search(rf"^{re.escape(key)}=([+-]?(?:\d+\.?\d*|\.\d+))\s*$", output, re.M)
    return float(match.group(1)) if match else None


def _extract_tagged_int(output: str, key: str) -> Optional[int]:
    match = re.search(rf"^{re.escape(key)}=(\d+)\s*$", output, re.M)
    return int(match.group(1)) if match else None


def parse_wrk_output(output: str) -> Dict[str, Any]:
    """Parse wrk stdout, preferring CINDERHTTP_WRK_* tags from wrk_report.lua."""
    result: Dict[str, Any] = {
        "requests_per_second": None,
        "average_latency_ms": None,
        "p95_latency_ms": None,
        "max_latency_ms": None,
        "total_requests": None,
        "transfer_bytes_per_sec": None,
        "errors": 0,
        "non_2xx": 0,
        "notes": [],
    }

    tagged_rps = _extract_tagged_float(output, "CINDERHTTP_WRK_RPS")
    tagged_avg = _extract_tagged_float(output, "CINDERHTTP_WRK_AVG_MS")
    tagged_p95 = _extract_tagged_float(output, "CINDERHTTP_WRK_P95_MS")
    tagged_max = _extract_tagged_float(output, "CINDERHTTP_WRK_MAX_MS")
    tagged_reqs = _extract_tagged_int(output, "CINDERHTTP_WRK_REQUESTS")
    tagged_bps = _extract_tagged_float(output, "CINDERHTTP_WRK_BYTES_PER_S")
    tagged_err = _extract_tagged_int(output, "CINDERHTTP_WRK_ERRORS")

    if tagged_rps is not None:
        result["requests_per_second"] = tagged_rps
    if tagged_avg is not None:
        result["average_latency_ms"] = tagged_avg
    if tagged_p95 is not None:
        result["p95_latency_ms"] = tagged_p95
    if tagged_max is not None:
        result["max_latency_ms"] = tagged_max
    if tagged_reqs is not None:
        result["total_requests"] = tagged_reqs
    if tagged_bps is not None:
        result["transfer_bytes_per_sec"] = tagged_bps
    if tagged_err is not None:
        result["errors"] = tagged_err

    if result["requests_per_second"] is None:
        m = re.search(r"Requests/sec:\s*([0-9.,]+[kKmM]?)", output)
        if m:
            result["requests_per_second"] = parse_rate(m.group(1))

    if result["average_latency_ms"] is None:
        m = re.search(r"^\s*Latency\s+(\S+)", output, re.M)
        if m:
            result["average_latency_ms"] = parse_latency_to_ms(m.group(1))

    if result["total_requests"] is None:
        m = re.search(r"(\d[\d,]*)\s+requests?\s+in\s+", output, re.I)
        if m:
            result["total_requests"] = int(m.group(1).replace(",", ""))

    if result["transfer_bytes_per_sec"] is None:
        m = re.search(r"Transfer/sec:\s*(\S+)", output)
        if m:
            try:
                result["transfer_bytes_per_sec"] = parse_bytes_per_sec(m.group(1))
            except ValueError:
                result["notes"].append("unable to parse Transfer/sec")

    # Standard wrk --latency lists 50/75/90/99, not 95. Do not mislabel.
    if result["p95_latency_ms"] is None:
        result["notes"].append(
            "p95 unavailable from plain wrk --latency (use wrk_report.lua for true p95)"
        )

    socket_errors = re.search(
        r"Socket errors:\s*connect\s+(\d+),\s*read\s+(\d+),\s*write\s+(\d+),\s*timeout\s+(\d+)",
        output,
        re.I,
    )
    if socket_errors and tagged_err is None:
        result["errors"] = sum(int(socket_errors.group(i)) for i in range(1, 5))

    non2xx = re.search(r"Non-2xx or 3xx responses:\s*(\d+)", output, re.I)
    if non2xx:
        result["non_2xx"] = int(non2xx.group(1))

    return result


def parse_hey_output(output: str) -> Dict[str, Any]:
    result: Dict[str, Any] = {
        "requests_per_second": None,
        "average_latency_ms": None,
        "p95_latency_ms": None,
        "max_latency_ms": None,
        "total_requests": None,
        "transfer_bytes_per_sec": None,
        "errors": 0,
        "non_2xx": 0,
        "notes": [],
    }

    m = re.search(r"Requests/sec:\s*([0-9.]+)", output)
    if m:
        result["requests_per_second"] = float(m.group(1))

    m = re.search(r"Average:\s*([0-9.]+)\s*secs", output)
    if m:
        result["average_latency_ms"] = float(m.group(1)) * 1000.0

    m = re.search(r"Slowest:\s*([0-9.]+)\s*secs", output)
    if m:
        result["max_latency_ms"] = float(m.group(1)) * 1000.0

    m = re.search(r"^\s*95%\s+in\s+([0-9.]+)\s*secs", output, re.M)
    if m:
        result["p95_latency_ms"] = float(m.group(1)) * 1000.0

    # Status code distribution
    status_total = 0
    non_2xx = 0
    for code, count in re.findall(r"\[(\d{3})\]\s+(\d+)\s+responses?", output):
        c = int(count)
        status_total += c
        if not (200 <= int(code) < 300):
            non_2xx += c
    if status_total:
        result["total_requests"] = status_total
        result["non_2xx"] = non_2xx

    m = re.search(r"Error distribution:", output)
    if m:
        # Count lines under Error distribution that look like "  12: ..."
        section = output[m.end() :]
        err_count = 0
        for line in section.splitlines()[1:]:
            if not line.strip():
                break
            em = re.match(r"\s*(\d+):", line)
            if em:
                err_count += int(em.group(1))
            elif line.startswith("[") or line.startswith("Status"):
                break
        result["errors"] = err_count

    return result


def parse_ab_output(output: str) -> Dict[str, Any]:
    result: Dict[str, Any] = {
        "requests_per_second": None,
        "average_latency_ms": None,
        "p95_latency_ms": None,
        "max_latency_ms": None,
        "total_requests": None,
        "transfer_bytes_per_sec": None,
        "errors": 0,
        "non_2xx": 0,
        "notes": [],
    }

    m = re.search(r"Requests per second:\s*([0-9.]+)", output)
    if m:
        result["requests_per_second"] = float(m.group(1))

    # Prefer mean latency (NOT "across all concurrent requests").
    m = re.search(
        r"Time per request:\s*([0-9.]+)\s*\[ms\]\s*\(mean\)(?!\s*,\s*across all concurrent)",
        output,
    )
    if m:
        result["average_latency_ms"] = float(m.group(1))
    else:
        # Fallback: first Time per request line.
        m = re.search(r"Time per request:\s*([0-9.]+)\s*\[ms\]\s*\(mean\)", output)
        if m:
            result["average_latency_ms"] = float(m.group(1))
            result["notes"].append("used first ab Time per request (mean) line")

    m = re.search(r"Complete requests:\s*(\d+)", output)
    if m:
        result["total_requests"] = int(m.group(1))

    m = re.search(r"Failed requests:\s*(\d+)", output)
    if m:
        result["errors"] = int(m.group(1))

    m = re.search(r"Non-2xx responses:\s*(\d+)", output)
    if m:
        result["non_2xx"] = int(m.group(1))

    m = re.search(r"^\s*95%\s+(\d+)", output, re.M)
    if m:
        result["p95_latency_ms"] = float(m.group(1))

    m = re.search(r"^\s*100%\s+(\d+)", output, re.M)
    if m:
        result["max_latency_ms"] = float(m.group(1))

    m = re.search(r"Transfer rate:\s*([0-9.]+)\s*\[Kbytes/sec\]", output)
    if m:
        result["transfer_bytes_per_sec"] = float(m.group(1)) * 1024.0

    return result


def detect_tool(preferred: str = "auto") -> str:
    if preferred != "auto":
        if preferred not in TOOL_PREFERENCE:
            raise ValueError(
                f"unsupported tool {preferred!r}; choose from: auto, {', '.join(TOOL_PREFERENCE)}"
            )
        path = shutil.which(preferred)
        if path is None:
            raise FileNotFoundError(
                f"benchmark tool {preferred!r} not found on PATH"
            )
        return preferred

    for name in TOOL_PREFERENCE:
        if shutil.which(name):
            return name
    raise FileNotFoundError(
        "no supported benchmark tool found on PATH "
        f"(looked for: {', '.join(TOOL_PREFERENCE)}). "
        "Install one of them, then re-run."
    )


def choose_wrk_threads(connections: int) -> int:
    """Client-side wrk threads; independent of CinderHTTP worker count."""
    cpu = os.cpu_count() or 4
    return max(1, min(8, cpu, connections))


# ---------------------------------------------------------------------------
# Server lifecycle
# ---------------------------------------------------------------------------


def port_is_free(host: str, port: int) -> bool:
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as sock:
        sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        try:
            sock.bind((host if host != "127.0.0.1" else "127.0.0.1", port))
        except OSError:
            return False
    return True


class ServerProcess:
    def __init__(
        self,
        binary: Path,
        host: str,
        port: int,
        workers: int,
        queue_size: int,
        root: Path,
    ) -> None:
        self.binary = binary
        self.host = host
        self.port = port
        self.workers = workers
        self.queue_size = queue_size
        self.root = root
        self.proc: Optional[subprocess.Popen[str]] = None
        self.log_file = tempfile.NamedTemporaryFile(
            prefix="cinderhttp-bench-",
            suffix=".log",
            delete=False,
            mode="w+",
            encoding="utf-8",
        )

    @property
    def base_url(self) -> str:
        return f"http://{self.host}:{self.port}"

    def start(self) -> None:
        if not port_is_free(self.host, self.port):
            raise RuntimeError(
                f"port {self.port} is already in use; refusing to benchmark "
                "another process"
            )
        cmd = [
            str(self.binary),
            "--port",
            str(self.port),
            "--workers",
            str(self.workers),
            "--queue-size",
            str(self.queue_size),
            "--root",
            str(self.root),
        ]
        self.proc = subprocess.Popen(
            cmd,
            stdout=self.log_file,
            stderr=subprocess.STDOUT,
            text=True,
            cwd=str(self.binary.parent.parent),
        )

    def wait_ready(self, path: str = "/api/health", timeout: float = READY_TIMEOUT_S) -> None:
        assert self.proc is not None
        deadline = time.monotonic() + timeout
        url = f"{self.base_url}{path if path.startswith('/') else '/' + path}"
        last_err = ""
        while time.monotonic() < deadline:
            if self.proc.poll() is not None:
                raise RuntimeError(
                    f"server exited early with code {self.proc.returncode}\n"
                    f"{self.read_log()}"
                )
            try:
                with urllib.request.urlopen(url, timeout=0.5) as resp:
                    if 200 <= resp.status < 300:
                        # Ensure we are talking to our process, not a stranger.
                        if self.proc.poll() is None:
                            return
            except (urllib.error.URLError, TimeoutError, ConnectionError) as exc:
                last_err = str(exc)
            time.sleep(0.05)
        raise RuntimeError(
            f"server not ready within {timeout}s on {url}\n"
            f"last error: {last_err}\n{self.read_log()}"
        )

    def read_log(self) -> str:
        self.log_file.flush()
        try:
            with open(self.log_file.name, "r", encoding="utf-8", errors="replace") as fh:
                return fh.read()
        except OSError:
            return ""

    def stop(self) -> None:
        if self.proc is None:
            self._close_log()
            return
        if self.proc.poll() is None:
            try:
                self.proc.send_signal(signal.SIGTERM)
            except OSError:
                pass
            deadline = time.monotonic() + SHUTDOWN_TIMEOUT_S
            while time.monotonic() < deadline and self.proc.poll() is None:
                time.sleep(0.05)
            if self.proc.poll() is None:
                try:
                    self.proc.kill()
                except OSError:
                    pass
                self.proc.wait(timeout=2)
        self._close_log()

    def _close_log(self) -> None:
        try:
            self.log_file.close()
        except OSError:
            pass
        try:
            os.unlink(self.log_file.name)
        except OSError:
            pass


# ---------------------------------------------------------------------------
# Tool runners
# ---------------------------------------------------------------------------


def run_external_tool(
    tool: str,
    url: str,
    connections: int,
    duration_s: float,
    script_dir: Path,
) -> Tuple[str, int]:
    duration_s = max(0.1, duration_s)
    if tool == "wrk":
        threads = choose_wrk_threads(connections)
        lua = script_dir / "wrk_report.lua"
        cmd = [
            "wrk",
            f"-t{threads}",
            f"-c{connections}",
            f"-d{duration_s:.3f}s",
            "--latency",
        ]
        if lua.is_file():
            cmd.extend(["--script", str(lua)])
        cmd.append(url)
    elif tool == "hey":
        # hey uses -z for duration; -c concurrency. Keep-alive is allowed
        # (Stage 9); do not assert specific throughput deltas here.
        cmd = [
            "hey",
            "-z",
            f"{duration_s:.3f}s",
            "-c",
            str(connections),
            url,
        ]
    elif tool == "ab":
        # ab uses fixed request count; approximate duration via a large -n with
        # -t timelimit (seconds).
        timelimit = max(1, int(round(duration_s)))
        cmd = [
            "ab",
            "-q",
            "-t",
            str(timelimit),
            "-c",
            str(connections),
            "-n",
            "10000000",
            url,
        ]
    else:
        raise ValueError(f"unknown tool {tool}")

    completed = subprocess.run(
        cmd,
        capture_output=True,
        text=True,
        encoding="utf-8",
        errors="replace",
        check=False,
    )
    combined = (completed.stdout or "") + ("\n" + completed.stderr if completed.stderr else "")
    return combined, completed.returncode


def parsed_to_run_result(
    workers: int,
    iteration: int,
    parsed: Dict[str, Any],
    tool_rc: int,
    raw: str,
    expect_success: bool,
) -> RunResult:
    errors = int(parsed.get("errors") or 0)
    non_2xx = int(parsed.get("non_2xx") or 0)
    notes = list(parsed.get("notes") or [])
    valid = tool_rc == 0 and errors == 0 and (non_2xx == 0 if expect_success else True)
    if tool_rc != 0:
        notes.append(f"benchmark tool exited with status {tool_rc}")
    if errors:
        notes.append(f"tool reported {errors} socket/failed requests")
    if non_2xx and expect_success:
        notes.append(f"tool reported {non_2xx} non-2xx responses")
    if parsed.get("requests_per_second") is None:
        valid = False
        notes.append("missing requests/sec in tool output")

    excerpt = raw if len(raw) <= 4000 else raw[:4000] + "\n...[truncated]..."
    return RunResult(
        workers=workers,
        iteration=iteration,
        requests_per_second=parsed.get("requests_per_second"),
        average_latency_ms=parsed.get("average_latency_ms"),
        p95_latency_ms=parsed.get("p95_latency_ms"),
        max_latency_ms=parsed.get("max_latency_ms"),
        total_requests=parsed.get("total_requests"),
        transfer_bytes_per_sec=parsed.get("transfer_bytes_per_sec"),
        errors=errors,
        non_2xx=non_2xx,
        valid=valid,
        notes=notes,
        raw_excerpt=excerpt,
    )


def parse_tool_output(tool: str, output: str) -> Dict[str, Any]:
    if tool == "wrk":
        return parse_wrk_output(output)
    if tool == "hey":
        return parse_hey_output(output)
    if tool == "ab":
        return parse_ab_output(output)
    raise ValueError(tool)


# ---------------------------------------------------------------------------
# Reporting
# ---------------------------------------------------------------------------


def format_number(value: Optional[float], digits: int = 2) -> str:
    if value is None:
        return "N/A"
    return f"{value:,.{digits}f}"


def print_summary(tool: str, path: str, connections: int, duration: float, iterations: int,
                  aggregates: Sequence[Tuple[int, Dict[str, Any]]]) -> None:
    print()
    print("CinderHTTP Benchmark Summary")
    print(f"Tool: {tool}")
    print(f"Path: {path}")
    print(f"Connections: {connections}")
    print(f"Duration: {duration:g}s")
    print(f"Iterations: {iterations}")
    print()
    header = f"{'Workers':<9}{'Req/sec':<14}{'Avg Latency':<14}{'P95 Latency':<14}"
    print(header)
    print("-" * len(header))
    for workers, agg in aggregates:
        rps = format_number(agg.get("requests_per_second_mean"), 2)
        avg = agg.get("average_latency_ms_mean")
        p95 = agg.get("p95_latency_ms_mean")
        avg_s = "N/A" if avg is None else f"{avg:.2f} ms"
        p95_s = "N/A" if p95 is None else f"{p95:.2f} ms"
        print(f"{workers:<9}{rps:<14}{avg_s:<14}{p95_s:<14}")
    print()


def write_json(path: Path, payload: Dict[str, Any]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("w", encoding="utf-8") as fh:
        json.dump(payload, fh, indent=2, sort_keys=False)
        fh.write("\n")


def write_csv(path: Path, rows: Sequence[Tuple[int, Dict[str, Any]]]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("w", encoding="utf-8", newline="") as fh:
        writer = csv.DictWriter(
            fh,
            fieldnames=[
                "workers",
                "requests_per_second_mean",
                "requests_per_second_stdev",
                "average_latency_ms",
                "p95_latency_ms",
            ],
        )
        writer.writeheader()
        for workers, agg in rows:
            writer.writerow(
                {
                    "workers": workers,
                    "requests_per_second_mean": agg.get("requests_per_second_mean"),
                    "requests_per_second_stdev": agg.get("requests_per_second_stdev"),
                    "average_latency_ms": agg.get("average_latency_ms_mean"),
                    "p95_latency_ms": agg.get("p95_latency_ms_mean"),
                }
            )


# ---------------------------------------------------------------------------
# CLI / orchestration
# ---------------------------------------------------------------------------


def project_root_from_script() -> Path:
    return Path(__file__).resolve().parent.parent


def build_arg_parser() -> argparse.ArgumentParser:
    p = argparse.ArgumentParser(
        description="Benchmark CinderHTTP worker scaling (Stage 8)."
    )
    p.add_argument("--tool", default="auto", help="auto|wrk|hey|ab (default: auto)")
    p.add_argument("--host", default="127.0.0.1")
    p.add_argument("--port", type=int, default=DEFAULT_PORT)
    p.add_argument("--path", default=DEFAULT_PATH, help=f"default: {DEFAULT_PATH}")
    p.add_argument("--duration", type=float, default=DEFAULT_DURATION, help="seconds")
    p.add_argument("--connections", type=int, default=DEFAULT_CONNECTIONS)
    p.add_argument("--iterations", type=int, default=DEFAULT_ITERATIONS)
    p.add_argument(
        "--workers",
        default=",".join(str(w) for w in DEFAULT_WORKERS),
        help="comma-separated worker counts (default: 1,2,4,8)",
    )
    p.add_argument("--queue-size", type=int, default=DEFAULT_QUEUE_SIZE)
    p.add_argument(
        "--output",
        default=str(Path("benchmarks") / "results" / "latest.json"),
        help="JSON output path (relative to project root unless absolute)",
    )
    p.add_argument(
        "--csv",
        default=str(Path("benchmarks") / "results" / "latest.csv"),
        help="CSV aggregate output path",
    )
    p.add_argument(
        "--binary",
        default=None,
        help="path to cinderhttp (default: <repo>/bin/cinderhttp)",
    )
    p.add_argument(
        "--root",
        default=None,
        help="document root for the server (default: <repo>/public)",
    )
    p.add_argument(
        "--warmup-duration",
        type=float,
        default=WARMUP_DURATION,
        help="warm-up seconds discarded before measured iterations",
    )
    p.add_argument(
        "--generate-fixtures",
        action="store_true",
        help="generate benchmarks/fixtures and use that directory as --root "
        "(implies static workload unless --path is set)",
    )
    p.add_argument(
        "--fixture-size",
        choices=["4k", "64k", "1m", "8m"],
        default=None,
        help="with --generate-fixtures, set --path to /benchmark-<size>.bin",
    )
    return p


def validate_args(args: argparse.Namespace) -> List[int]:
    if args.port <= 0 or args.port > 65535:
        raise ValueError("--port must be in 1..65535")
    if args.duration <= 0:
        raise ValueError("--duration must be positive")
    if args.connections <= 0:
        raise ValueError("--connections must be positive")
    if args.iterations <= 0:
        raise ValueError("--iterations must be positive")
    if args.queue_size <= 0:
        raise ValueError("--queue-size must be positive")
    if args.warmup_duration < 0:
        raise ValueError("--warmup-duration must be >= 0")
    if not args.path.startswith("/"):
        raise ValueError("--path must start with /")
    return parse_workers_list(args.workers)


def measure_once(
    tool: str,
    url: str,
    workers: int,
    iteration: int,
    connections: int,
    duration: float,
    script_dir: Path,
    expect_success: bool,
) -> RunResult:
    raw, rc = run_external_tool(tool, url, connections, duration, script_dir)
    parsed = parse_tool_output(tool, raw)
    return parsed_to_run_result(workers, iteration, parsed, rc, raw, expect_success)


def run_benchmark(args: argparse.Namespace) -> int:
    root = project_root_from_script()
    workers_list = validate_args(args)
    tool = detect_tool(args.tool)

    binary = Path(args.binary) if args.binary else root / "bin" / "cinderhttp"
    if not binary.is_file():
        raise FileNotFoundError(
            f"CinderHTTP binary not found at {binary}. Run `make` (release) first."
        )

    if args.generate_fixtures:
        from generate_fixtures import generate

        fixture_dir = root / "benchmarks" / "fixtures"
        generate(fixture_dir)
        doc_root = fixture_dir
        if args.fixture_size:
            args.path = f"/benchmark-{args.fixture_size}.bin"
        elif args.path == DEFAULT_PATH:
            args.path = "/benchmark-1m.bin"
    else:
        doc_root = Path(args.root) if args.root else root / "public"

    if not doc_root.is_dir():
        raise FileNotFoundError(f"document root not found: {doc_root}")

    out_json = Path(args.output)
    if not out_json.is_absolute():
        out_json = root / out_json
    out_csv = Path(args.csv)
    if not out_csv.is_absolute():
        out_csv = root / out_csv

    script_dir = Path(__file__).resolve().parent
    expect_success = (
        args.path == "/api/health"
        or args.path == "/"
        or args.path.startswith("/benchmark-")
    )

    print(
        f"Benchmark tool: {tool}\n"
        f"Binary: {binary}\n"
        f"Document root: {doc_root}\n"
        f"Target: http://{args.host}:{args.port}{args.path}\n"
        f"Workers: {workers_list}\n"
        f"Connections: {args.connections}  Duration: {args.duration:g}s  "
        f"Iterations: {args.iterations}  Warm-up: {args.warmup_duration:g}s\n"
        f"Queue size: {args.queue_size}\n"
        "Note: CinderHTTP uses Linux sendfile()-based static transfer with a "
        "bounded-buffer POSIX fallback; persistent connections are enabled."
    )

    results_blocks: List[Dict[str, Any]] = []
    aggregate_rows: List[Tuple[int, Dict[str, Any]]] = []

    for workers in workers_list:
        print(f"\n=== workers={workers} ===")
        server = ServerProcess(
            binary=binary,
            host=args.host,
            port=args.port,
            workers=workers,
            queue_size=args.queue_size,
            root=doc_root,
        )
        runs: List[RunResult] = []
        try:
            server.start()
            server.wait_ready("/api/health")
            url = f"{server.base_url}{args.path}"

            if args.warmup_duration > 0:
                print(f"  warm-up ({args.warmup_duration:g}s)...")
                _raw, _rc = run_external_tool(
                    tool, url, args.connections, args.warmup_duration, script_dir
                )
                # Discard warm-up entirely.

            for i in range(1, args.iterations + 1):
                print(f"  iteration {i}/{args.iterations}...")
                run = measure_once(
                    tool,
                    url,
                    workers,
                    i,
                    args.connections,
                    args.duration,
                    script_dir,
                    expect_success,
                )
                runs.append(run)
                status = "ok" if run.valid else "INVALID"
                rps = format_number(run.requests_per_second, 2)
                print(
                    f"    [{status}] req/s={rps} "
                    f"avg={format_number(run.average_latency_ms)}ms "
                    f"p95={format_number(run.p95_latency_ms)}ms "
                    f"errors={run.errors + run.non_2xx}"
                )
                if run.notes:
                    for note in run.notes:
                        print(f"      note: {note}")

            # Sanity: server still responsive (stats query is not used as a total).
            try:
                with urllib.request.urlopen(
                    f"{server.base_url}/api/stats", timeout=2.0
                ) as resp:
                    if resp.status != 200:
                        print("  warning: /api/stats did not return 200 after runs")
            except (urllib.error.URLError, TimeoutError, ConnectionError) as exc:
                print(f"  warning: post-run /api/stats check failed: {exc}")

        finally:
            server.stop()

        agg = aggregate_runs(runs)
        aggregate_rows.append((workers, agg))
        results_blocks.append(
            {
                "workers": workers,
                "runs": [asdict(r) for r in runs],
                "aggregate": agg,
            }
        )

    print_summary(
        tool, args.path, args.connections, args.duration, args.iterations, aggregate_rows
    )

    payload = {
        "metadata": {
            "timestamp": datetime.now(timezone.utc).isoformat(),
            "tool": tool,
            "host": args.host,
            "port": args.port,
            "path": args.path,
            "connections": args.connections,
            "duration_seconds": args.duration,
            "warmup_duration_seconds": args.warmup_duration,
            "iterations": args.iterations,
            "queue_size": args.queue_size,
            "binary": str(binary),
            "keep_alive": True,
            "environment": {
                "platform": platform.platform(),
                "python": sys.version.split()[0],
                "cpu_count": os.cpu_count(),
            },
        },
        "results": results_blocks,
    }
    write_json(out_json, payload)
    write_csv(out_csv, aggregate_rows)
    print(f"Wrote {out_json}")
    print(f"Wrote {out_csv}")

    # Fail the process if any measured iteration was invalid.
    if any(not run["valid"] for block in results_blocks for run in block["runs"]):
        print("Benchmark completed with INVALID iterations (see notes above).", file=sys.stderr)
        return 2
    return 0


def main(argv: Optional[Sequence[str]] = None) -> int:
    parser = build_arg_parser()
    args = parser.parse_args(argv)
    try:
        return run_benchmark(args)
    except (ValueError, FileNotFoundError, RuntimeError) as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 1
    except KeyboardInterrupt:
        print("\ninterrupted", file=sys.stderr)
        return 130


if __name__ == "__main__":
    sys.exit(main())
