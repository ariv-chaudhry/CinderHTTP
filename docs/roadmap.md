# Roadmap

CinderHTTP is being built in controlled stages rather than all at once.
Checkboxes are only marked complete once the corresponding behavior has
actually been implemented *and* exercised (compiled, run, and/or tested).

Current status: **Stage 5 complete** - application router, `/api/health`,
`/api/echo`, `/api/stats`, and thread-safe runtime statistics.

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

- [ ] Graceful shutdown, full version (drain/stop worker pool, join threads,
      destroy synchronization primitives, free all allocations)
- [ ] Malformed-request handling audit
- [ ] Memory cleanup audit
- [ ] AddressSanitizer / UndefinedBehaviorSanitizer clean run — *Stage 2–5
      unit tests were run under ASan/UBSan; broader process audit remains*
- [ ] Valgrind clean run

## Phase 7 - Testing

- [x] Parser tests
- [x] Queue tests
- [x] Router tests
- [x] Security tests (path traversal, encoded traversal, symlink escape,
      malformed percent-encoding, embedded NUL)
- [x] Integration tests (curl/nc; Stage 2–5 concurrency + API subset)

## Phase 8 - Performance

- [ ] Benchmark script (`wrk`/`ab`/`hey`, whichever is available)
- [ ] Worker-count comparison (1/2/4/8 workers)
- [ ] Latency measurements (avg, p95)

---

### Notes on Stage 5

- `/api/` is an application-owned namespace and never falls through to static
  files (even if a matching path exists under `--root`).
- Route matching uses exact path equality after stripping the query string;
  trailing slashes are significant (`/api/health/` ≠ `/api/health`).
- Wrong methods on known API paths return **405** with an `Allow` header;
  unknown API paths return JSON **404**.
- Temporary Stage 2 “POST always succeeds” behavior is removed; non-API POST
  returns **405**.
- Stats counters are mutex-protected; snapshots are copied before JSON is
  formatted so the lock is not held during I/O.
- Phase 6 still tracks a fuller reliability/audit pass.
