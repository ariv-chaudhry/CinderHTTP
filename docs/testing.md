# Testing

CinderHTTP’s tests focus on observable contracts and safety invariants:
boundary conditions, untrusted input, resource ownership, concurrency, and
shutdown behavior — not internal implementation trivia.

## Test categories

| Category | What it covers |
| --- | --- |
| Unit | Parser, response (incl. file body), MIME, static files, router, stats, queue, reliability, HTTP reader, connection policy |
| Integration | Live server via curl + `raw_http.py` + `test_keep_alive.py` + `test_static_stream.py` |
| Reliability | SIGPIPE/`send_all`, queue drain, malformed regressions, client disconnect during transfer |
| Fuzz-style | Deterministic random + mutated inputs to `http_parse_request` |
| Sanitizer | ASan + UBSan via `make debug test` / `make sanitize` |
| Valgrind | Optional leak/origin checks via `make valgrind` |
| Coverage | Optional gcov summary via `make coverage` |
| Performance | Optional load harness via `make benchmark` / `benchmark-api` / `benchmark-static` |
| Profiling build | `make profile` (`-O2 -g` binary for external profilers) |

## Commands

```bash
make test                 # unit suite (includes ~2000 fuzz iterations + bench parser tests)
make benchmark-test       # Python unittest for wrk/hey/ab parsers only
make integration          # live checks through Stage 10 (keep-alive + static stream)
make sanitize             # ASan/UBSan unit tests
make debug test           # same, explicit debug flags in one make run
make fuzz                 # deeper parser fuzz (default 50,000 iterations)
CINDERHTTP_FUZZ_ITERS=100000 make fuzz
make valgrind             # skipped cleanly if Valgrind is missing
make coverage             # clean rebuild with --coverage + gcov summary
make profile              # -O2 -g binary suitable for external profilers
make benchmark            # release build + load harness (requires wrk, hey, or ab)
make benchmark-api        # API workload shortcut (GET /api/health)
make benchmark-static     # generate fixtures + 1 MiB static workload
make benchmark BENCH_ARGS="--connections 128 --duration 15"
```

## Performance testing

Performance benchmarks are intentionally separate from correctness tests.

```bash
make benchmark
make benchmark-api
make benchmark-static
```

The harness starts fresh release-mode server instances across the selected
worker counts, performs a warm-up, runs repeated measurements, and stores
structured results under `benchmarks/results/`.

`make test` remains deterministic and does **not** run load generators. Host
performance numbers must not be asserted in unit tests.

Details: [`benchmarks/README.md`](../benchmarks/README.md).

## Fuzzing

`tests/test_parser_fuzz.c` uses a fixed xorshift seed (`0xC1D3`) so runs are
reproducible.

- **Random strategy:** arbitrary bytes (including `NUL`, CR, LF) at varied lengths
- **Mutation strategy:** start from valid seed requests and apply deterministic
  mutations (replace/delete/insert/truncate/corrupt digits)
- Every iteration calls `http_request_init` → `http_parse_request` →
  `http_request_destroy`, whether parse succeeds or fails
- Override iterations with `CINDERHTTP_FUZZ_ITERS` (validated; capped)

This is lightweight adversarial regression coverage, not a production fuzzer
or formal verification claim.

## Raw HTTP helper

`tests/integration/raw_http.py` (Python 3 stdlib only) sends exact request
bytes, optional write half-close, chunked sends with pauses, and a bounded
socket timeout so the integration suite cannot hang forever.

## Stage 9 keep-alive coverage

`tests/integration/test_keep_alive.py` uses raw sockets and parses responses by
`Content-Length` (never “read until close” for persistent replies). It covers:

- HTTP/1.1 persistence and `Connection: close`
- HTTP/1.0 default close and explicit keep-alive
- Multi-request framing on one TCP connection
- Coalesced TCP data and fragmented requests (via unit reader tests)
- POST body followed by next request
- HEAD followed by GET (no body bytes stolen from the next response)
- Idle timeout (`--keep-alive-timeout 1` in the harness)
- Malformed second / invalid request → error + close
- Connection counters vs request counters
- Concurrent clients each sending multiple keep-alive requests (8×5)

Unit coverage also includes connection-token parsing, persistence policy,
response `Connection` headers, coalesced/fragmented reader extraction, and
POST+GET body framing leftovers.

## Stage 10 static streaming + deadline coverage

`tests/integration/test_static_stream.py` exercises the file-backed path and
request deadline. Coverage includes:

- Static streaming GET with binary integrity (byte-exact body)
- Large and zero-byte files (`Content-Length` correctness)
- HEAD static metadata without body bytes
- Persistent connection: static → API → static on one TCP session
- Client disconnect mid-transfer does not kill the server
- Request deadline / 408 on incomplete framing
- Slow trickle that stays under per-recv idle but exceeds `--request-timeout`

Unit coverage includes file-body send + HEAD (`tests/test_response.c`), static
open/`fstat`/file-backed attach (`tests/test_static_files.c`), and reader
timeout / disconnect paths (`tests/test_http_reader.c`).

## Philosophy

Prefer tests that would catch real regressions: fragmented framing, limit±1
sizes, malformed metadata, filesystem confinement, HEAD length correctness,
keep-alive stream framing, file-backed transfer integrity, request deadlines,
queue/stats concurrency, and live-server survival after bad clients.
