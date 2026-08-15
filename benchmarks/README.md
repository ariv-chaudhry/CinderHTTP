# Benchmarking

CinderHTTP’s load harness is for **measurement**, not premature optimization.

The harness starts fresh **release-mode** (`-O2`) server processes, compares
worker-pool sizes, warms up, repeats measurements, and writes structured
results. It does **not** enable sanitizer builds or invent numbers.

CinderHTTP supports HTTP/1.0–1.1 persistent connections and file-backed static
transfer (Linux `sendfile()` with a bounded-buffer POSIX fallback). Load
generators may reuse connections; treat throughput as environment-specific
measurement, not a committed performance claim.

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

## Workloads

| Workload | How to run | Target |
|----------|------------|--------|
| API (default) | `make benchmark` or `make benchmark-api` | `GET /api/health` |
| Static | `make benchmark-static` | generated `GET /benchmark-1m.bin` |

API workload avoids disk variability when comparing worker counts. Static
workload exercises the file-backed send path (`sendfile` / fallback).

### Static fixtures

Fixtures are **not** committed. Generate them with:

```bash
python3 benchmarks/generate_fixtures.py
# or via the harness:
python3 benchmarks/benchmark.py --generate-fixtures --fixture-size 1m
make benchmark-static
```

Sizes: `4k`, `64k`, `1m`, `8m` → `benchmarks/fixtures/benchmark-<size>.bin`
(gitignored).

**Page-cache caveat:** repeated localhost GETs of the same fixture will often
hit the OS page cache. Results measure end-to-end server + tool behavior under
that condition, not cold-disk throughput. Do not treat a single run as a
storage-bound claim.

**sendfile note:** on Linux, static bodies prefer `sendfile()`; other platforms
(or sendfile failure into the portable path) use a fixed-size read/send chunk.
The harness prints a reminder when it starts; it does not assert which path
was taken.

## Quick start

```bash
make benchmark
make benchmark-api
make benchmark-static
```

Forward extra flags:

```bash
make benchmark BENCH_ARGS="--connections 128 --duration 15"
make benchmark-static BENCH_ARGS="--fixture-size 8m --workers 2,4"
```

Or call the script directly from the repo root:

```bash
python3 benchmarks/benchmark.py --workers 1,2,4,8
python3 benchmarks/benchmark.py --path /api/health
python3 benchmarks/benchmark.py --generate-fixtures --fixture-size 1m
python3 benchmarks/benchmark.py --tool wrk --iterations 5
```

## What the harness does

For each worker count in the list (default `1,2,4,8`):

1. Ensure the benchmark port is free (refuse to hit a foreign listener).
2. Start `bin/cinderhttp` with `--workers N`, `--queue-size` (default 256), no `--verbose`.
3. Poll `/api/health` until ready (bounded timeout; API routes work regardless
   of `--root`, including fixture directories).
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

Generated files under `benchmarks/results/` and `benchmarks/fixtures/` are
gitignored (results directory kept via `.gitkeep`).

## Parser tests

```bash
make benchmark-test
```

These unittest checks parse fixtures only; they do **not** require wrk/hey/ab
and do **not** run load.

## Interpreting results

- Localhost numbers are **not** Internet-scale production claims.
- Results vary with CPU, OS scheduler, power settings, page cache, and
  background load.
- Persistent connections may yield higher throughput than one-shot baselines
  when the load tool reuses sockets — measure before comparing eras.
- The harness primarily varies **worker count**; queue size defaults high
  enough not to be the intended bottleneck unless you override it downward.
- Do not invent or paste “expected” req/s figures into docs or PRs; record
  what your machine actually produced under `benchmarks/results/`.
