# Roadmap

CinderHTTP is being built in controlled stages rather than all at once (see
the development log in the project history for the exact stage boundaries).
This roadmap tracks progress by feature area. Checkboxes are only marked
complete once the corresponding behavior has actually been implemented *and*
exercised (compiled, run, and/or tested) - not when it is merely planned.

Current status: **Stage 1 complete** - TCP core (configuration, listening
socket, accept loop, hardcoded response, basic signal-driven shutdown).

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

- [ ] Request model (`http_request_t`, `http_header_t`)
- [ ] Parser (request line, headers, `Content-Length` body)
- [ ] Response generation (`http_response_t`, status/reason mapping)
- [ ] GET
- [ ] HEAD

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
- [ ] Malformed-request handling (400/405/413/505 paths)
- [ ] Memory cleanup audit
- [ ] AddressSanitizer / UndefinedBehaviorSanitizer clean run
- [ ] Valgrind clean run

## Phase 7 - Testing

- [ ] Parser tests
- [ ] Queue tests
- [ ] Router tests
- [ ] Security tests (path traversal, oversized input)
- [ ] Integration tests (curl-driven, plus raw malformed requests via `nc`)

## Phase 8 - Performance

- [ ] Benchmark script (`wrk`/`ab`/`hey`, whichever is available)
- [ ] Worker-count comparison (1/2/4/8 workers)
- [ ] Latency measurements (avg, p95)

---

### Notes on Stage 1

- `--workers`, `--queue-size`, and `--root` are parsed and validated now, but
  do not yet influence server behavior - there is no thread pool, connection
  queue, or static file server yet for them to configure. They are accepted
  early so the CLI surface does not change shape later.
- The server currently sends one fixed, hardcoded response to every
  connection regardless of method or path, purely to prove the socket
  lifecycle (`socket` -> `bind` -> `listen` -> `accept` -> respond -> close)
  works correctly. Real HTTP parsing begins in Phase 2.
- Shutdown today only has to stop an accept loop and close one socket, so
  the "basic" bullet above is complete. It is listed separately from Phase
  6's full graceful shutdown because that version also has to drain the
  connection queue and join worker threads, neither of which exist yet.
