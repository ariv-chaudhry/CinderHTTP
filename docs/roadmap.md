# Roadmap

CinderHTTP is being built in controlled stages rather than all at once.
Checkboxes are only marked complete once the corresponding behavior has
actually been implemented *and* exercised (compiled, run, and/or tested).

Current status: **Stage 9 complete** - HTTP/1.0–1.1 persistent connections with
stateful request framing, bounded keep-alive/read timeouts, and maximum
requests per connection, on top of the Stage 8 benchmarking harness.

## Phase 1 - TCP Core

- [x] Configuration (CLI parsing + validation for `--port`, `--workers`,
      `--queue-size`, `--root`, `--verbose`, `--help`)
- [x] Listening socket (`socket()` + `SO_REUSEADDR` + `bind()` + `listen()`)
- [x] Connection acceptance (`accept()` loop, tolerant of `EINTR` and
      per-client errors)
- [x] Clean shutdown, basic version (`SIGINT`/`SIGTERM`)

## Phase 2 - HTTP Core

- [x] Request model (`http_request_t`, `http_header_t`)
- [x] Parser (request line, headers, `Content-Length` body)
- [x] Response generation (`http_response_t`, status/reason mapping)
- [x] GET
- [x] HEAD
- [x] POST body parsing via `Content-Length`
- [x] Buffered multi-`recv()` request framing (`http_reader`)
- [x] Error responses: 400 / 405 / 413 / 501 / 505

## Phase 3 - Static Serving

- [x] Static document root (`--root`, default `./public`)
- [x] GET static file serving
- [x] HEAD static file handling (metadata without loading body)
- [x] MIME detection (extension-based, case-insensitive)
- [x] Binary file support
- [x] Custom 404 handling (`public/404.html` when available)
- [x] URL decoding (single pass)
- [x] Query-string stripping for filesystem lookup
- [x] Traversal protection (lexical `..` normalization)
- [x] Encoded traversal protection (`%2e%2e`, etc.)
- [x] Symlink escape protection (`realpath` + root boundary check)
- [x] Static-file / MIME / security unit tests
- [x] Integration checks for HTML/CSS/404/traversal

## Phase 4 - Concurrency

- [x] Bounded connection queue (mutex + condition variables)
- [x] Worker pool
- [x] Thread-safe logging

## Phase 5 - Application Layer

- [x] Router (method + path -> handler)
- [x] `/api/health`
- [x] `/api/echo`
- [x] `/api/stats`

## Phase 6 - Reliability

- [x] Graceful shutdown, full version (drain/stop worker pool, join threads,
      destroy synchronization primitives, free all allocations)
- [x] Malformed-request handling audit
- [x] Memory cleanup audit
- [x] AddressSanitizer / UndefinedBehaviorSanitizer clean run
- [x] Valgrind support target added (`make valgrind`); clean unit-test run
      completed in this environment

## Phase 7 - Testing

- [x] Parser tests
- [x] Queue tests
- [x] Router tests
- [x] Security tests
- [x] Integration tests
- [x] HTTP reader/framing tests
- [x] Boundary-value regression tests
- [x] Deterministic parser fuzz-style testing
- [x] Malformed live-request testing
- [x] Concurrent malformed-client survival testing
- [x] Sanitizer-backed regression suite

## Phase 8 - Performance

- [x] Reproducible benchmark harness
- [x] Automatic wrk / hey / ab detection
- [x] Worker-count comparison (1 / 2 / 4 / 8)
- [x] Repeated measurements with warm-up
- [x] Throughput measurements
- [x] Average latency measurements
- [x] p95 latency where supported by the selected tool
- [x] JSON benchmark output
- [x] Human-readable aggregate report

## Phase 9 - Persistent Connections

- [x] HTTP/1.1 keep-alive by default
- [x] HTTP/1.1 `Connection: close`
- [x] HTTP/1.0 default-close semantics
- [x] HTTP/1.0 explicit keep-alive
- [x] Stateful multi-request connection reader
- [x] Buffered next-request preservation
- [x] Bounded keep-alive/read timeout
- [x] Maximum requests per connection
- [x] Multi-request integration coverage

---

### Notes on Stage 7

- Framing is tested via `socketpair()` fragmentation and CRLFCRLF splits.
- Duplicate `Content-Length` (identical or conflicting) is rejected.
- Embedded NUL bytes are rejected in protocol metadata; bodies may still be binary.
- See [`docs/testing.md`](testing.md) for commands and fuzz details.

### Notes on Stage 8

- Benchmarks use the **release** (`-O2`) binary on localhost, defaulting to
  `GET /api/health`.
- Each worker count starts a **fresh** server; warm-up is discarded; iterations
  are aggregated (mean / stdev) into JSON and CSV under `benchmarks/results/`.
- Results are machine- and environment-dependent; they are a baseline for later
  optimization decisions, not production capacity claims.
- See [`benchmarks/README.md`](../benchmarks/README.md).

### Notes on Stage 9

- Stateful `http_reader_t` extracts one framed request at a time and retains
  leftovers (`memmove` compaction). The parser still sees a single message.
- Persistence is version-aware; `Connection` tokens are case-insensitive and
  comma-list aware; conflicting tokens prefer **close**.
- Idle timeout uses `SO_RCVTIMEO` (per-recv, not an absolute whole-request
  deadline). Idle silence closes without 408; partial request + timeout → 408.
- A keep-alive connection pins its worker until close; timeout and
  `HTTP_MAX_REQUESTS_PER_CONNECTION` bound occupancy.
- Load tools may now reuse connections; Stage 8 methodology is unchanged.
  Do not treat throughput deltas as committed claims without fresh measurements.
