# Roadmap

CinderHTTP is being built in controlled stages rather than all at once.
Checkboxes are only marked complete once the corresponding behavior has
actually been implemented *and* exercised (compiled, run, and/or tested).

Current status: **Stage 2 complete** - HTTP core (buffered request reading,
manual parser, request/response models, GET/HEAD/POST handling, unit +
integration tests).

## Phase 1 - TCP Core

- [x] Configuration (CLI parsing + validation for `--port`, `--workers`,
      `--queue-size`, `--root`, `--verbose`, `--help`)
- [x] Listening socket (`socket()` + `SO_REUSEADDR` + `bind()` + `listen()`)
- [x] Connection acceptance (`accept()` loop, tolerant of `EINTR` and
      per-client errors)
- [x] Clean shutdown, basic version (`SIGINT`/`SIGTERM` set a
      `volatile sig_atomic_t` flag; the accept loop notices it via `EINTR`
      and exits; listening socket is closed)

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

- [ ] Document root resolution
- [ ] MIME detection
- [ ] 404 handling (custom `public/404.html`)
- [ ] Path traversal protection

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
- [ ] Malformed-request handling (400/405/413/505 paths) — *basic mapping
      exists in Stage 2; full audit remains*
- [ ] Memory cleanup audit
- [ ] AddressSanitizer / UndefinedBehaviorSanitizer clean run — *Stage 2 unit
      tests were run under ASan/UBSan; broader process audit remains*
- [ ] Valgrind clean run

## Phase 7 - Testing

- [x] Parser tests
- [ ] Queue tests
- [ ] Router tests
- [ ] Security tests (path traversal, oversized input)
- [x] Integration tests (curl-driven, plus raw malformed requests via `nc`)
      — Stage 2 subset

## Phase 8 - Performance

- [ ] Benchmark script (`wrk`/`ab`/`hey`, whichever is available)
- [ ] Worker-count comparison (1/2/4/8 workers)
- [ ] Latency measurements (avg, p95)

---

### Notes on Stage 2

- The temporary request handler returns fixed success text for any valid
  GET/HEAD/POST. There is still no router and no static file serving.
- `Connection: close` is always sent; keep-alive is deferred.
- Duplicate `Content-Length` headers are rejected even when identical.
- `Transfer-Encoding` (including `chunked`) returns `501 Not Implemented`.
