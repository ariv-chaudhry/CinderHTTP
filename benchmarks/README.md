# Benchmarking (Stage 8)

CinderHTTP’s Stage 8 goal is **measurement**, not premature optimization.

The harness starts fresh **release-mode** (`-O2`) server processes, compares
worker-pool sizes, warms up, repeats measurements, and writes structured
results. It does **not** enable keep-alive (unsupported), sanitizer builds, or
fabricated numbers.

## Prerequisites

- Python 3 (standard library only)
- Built binary: `bin/cinderhttp` (`make` / `make benchmark`)
- At least one of:

```text
wrk   (preferred)
hey
ab    (ApacheBench, often via apache2-utils)
```

Detection order for `--tool auto`: `wrk` → `hey` → `ab`.

## Default workload

```text
GET /api/health
```

This is a small JSON API response and avoids static-file / disk variability when
comparing worker counts. Override with `--path /` or another existing GET route
if needed.

## Quick start

```bash
make benchmark
```

Forward extra flags:

```bash
make benchmark BENCH_ARGS="--connections 128 --duration 15"
```

Or call the script directly from the repo root:

```bash
python3 benchmarks/benchmark.py --workers 1,2,4,8
python3 benchmarks/benchmark.py --path /
python3 benchmarks/benchmark.py --tool wrk --iterations 5
```

## What the harness does

For each worker count in the list (default `1,2,4,8`):

1. Ensure the benchmark port is free (refuse to hit a foreign listener).
2. Start `bin/cinderhttp` with `--workers N`, `--queue-size` (default 256), no `--verbose`.
3. Poll `/api/health` until ready (bounded timeout).
4. Run a short **warm-up** (discarded).
5. Run `--iterations` measured loads (default 3).
6. Optionally sanity-check `/api/stats` (not used as an exact request total).
7. SIGTERM the server and wait for exit before the next worker count.

Warm-up results are never included in averages.

## Metrics

Normalized fields:

| Field | Unit |
| --- | --- |
| requests/sec | req/s |
| average latency | milliseconds |
| p95 latency | milliseconds (when the tool truly provides it) |

**p95 support**

- `wrk`: true p95 via `benchmarks/wrk_report.lua` (`latency:percentile(95)`). Plain
  `wrk --latency` only prints 50/75/90/99 — those are **not** relabeled as p95.
- `hey`: 95% line from latency distribution.
- `ab`: 95% line from the percentile table. Average latency uses the
  `Time per request ... (mean)` line, **not** the “across all concurrent” line.

Failed socket requests / non-2xx responses invalidate a measured iteration for
the default successful workloads.

## Outputs

```text
benchmarks/results/latest.json   # full metadata + per-iteration runs + aggregates
benchmarks/results/latest.csv    # aggregate table for plotting
```

Generated files under `benchmarks/results/` are gitignored (directory kept via
`.gitkeep`).

## Parser tests

```bash
make benchmark-test
```

These unittest checks parse fixtures only; they do **not** require wrk/hey/ab
and do **not** run load.

## Interpreting results

- Localhost numbers are **not** Internet-scale production claims.
- Results vary with CPU, OS scheduler, power settings, and background load.
- CinderHTTP uses **one request per connection** (`Connection: close`); tools
  that open many concurrent connections still pay full handshake cost per
  request relative to keep-alive servers.
- Stage 8 primarily varies **worker count**; queue size defaults high enough
  not to be the intended bottleneck unless you override it downward.
- Use this baseline before considering later optimizations (`sendfile`, epoll,
  keep-alive, etc.).
