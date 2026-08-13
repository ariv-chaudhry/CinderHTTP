# Roadmap

CinderHTTP is being built in controlled stages rather than all at once.
Checkboxes are only marked complete once the corresponding behavior has
actually been implemented *and* exercised (compiled, run, and/or tested).

Current status: **Stage 3 complete** - secure static file serving, MIME
detection, path traversal protection, custom 404, binary-safe file responses.

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

- [ ] Bounded connection queue (mutex + condition variables)
- [ ] Worker pool
- [ ] Thread-safe logging

## Phase 5 - Application Layer

- [ ] Router (method + path -> handler)
- [ ] `/api/health`
- [ ] `/api/echo`
- [ ] `/api/stats`

## Phase 6 - Reliability

- [ ] Graceful shutdown, full version (drain/stop worker pool, join threads,
      destroy synchronization primitives, free all allocations)
- [ ] Malformed-request handling audit
- [ ] Memory cleanup audit
- [ ] AddressSanitizer / UndefinedBehaviorSanitizer clean run — *Stage 2/3
      unit tests were run under ASan/UBSan; broader process audit remains*
- [ ] Valgrind clean run

## Phase 7 - Testing

- [x] Parser tests
- [ ] Queue tests
- [ ] Router tests
- [x] Security tests (path traversal, encoded traversal, symlink escape,
      malformed percent-encoding, embedded NUL)
- [x] Integration tests (curl/nc; Stage 2–3 subset)

## Phase 8 - Performance

- [ ] Benchmark script (`wrk`/`ab`/`hey`, whichever is available)
- [ ] Worker-count comparison (1/2/4/8 workers)
- [ ] Latency measurements (avg, p95)

---

### Notes on Stage 3

- Request handling is still single-threaded (accept loop processes one client
  at a time). Multithreading is Stage 4.
- Static files are buffered in memory up to `HTTP_MAX_STATIC_FILE_SIZE`
  (16 MiB). `sendfile()` / streaming is future work.
- Directory access without `index.html` returns **403 Forbidden** (no
  directory listings).
- Percent-decoding is performed **once**; `%252e` is not recursively decoded.
- TOCTOU between `realpath`/`stat` and `fopen` is acknowledged as an
  educational-server limitation, not production sandboxing.
