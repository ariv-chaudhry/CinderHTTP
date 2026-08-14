# Architecture

CinderHTTP Stage 4 uses a classic **producer–consumer** design: one accept
thread enqueues client sockets into a bounded queue; a fixed worker pool
dequeues and runs the HTTP pipeline concurrently.

## Overview

```text
                       CLIENTS
                          |
                          | TCP
                          v
                  +---------------+
                  | listen socket |
                  +-------+-------+
                          |
                          | accept()
                          v
                  +---------------+
                  | accept thread |
                  +-------+-------+
                          |
                          | push(fd)
                          v
               +----------------------+
               | bounded connection   |
               | queue                |
               | mutex + condvars     |
               +----------+-----------+
                          |
              +-----------+-----------+
              |           |           |
              v           v           v
          Worker 1    Worker 2    Worker N
              |           |           |
              +-----------+-----------+
                          |
                          v
                    client_handle()
                          |
                          v
                 http_read_request()
                          |
                          v
                 http_parse_request()
                          |
                          v
                  request dispatch
                    /          \
                   /            \
              static GET/HEAD   POST
                   \            /
                    \          /
                     response
                          |
                          v
                     send_all()
                          |
                          v
                        close
```

## Producer–consumer design

| Role | Thread | Action |
|------|--------|--------|
| Producer | Accept / main thread | `accept()` then `connection_queue_push()` |
| Buffer | Bounded ring of fds | Capacity = `--queue-size`; full queue blocks the producer (backpressure) |
| Consumers | `--workers` pthread workers | `connection_queue_pop()` then `client_handle()` |

The queue mutex protects only queue metadata (`head`/`tail`/`count`/
`shutting_down`). Workers **do not** hold that mutex while processing HTTP
requests, so clients are handled in parallel. The logger mutex serializes only
brief log lines.

## Module responsibilities

| Module | Responsibility |
|--------|----------------|
| `config` | CLI defaults and validation |
| `server` | Listen socket, accept loop, orchestration, signal flag |
| `connection_queue` | Bounded ring buffer + mutex/condvars |
| `thread_pool` | Fixed joinable worker lifecycle |
| `client_handler` | One-connection HTTP read/parse/dispatch/send/close |
| `logger` | Mutex-protected stderr logging |
| `http_reader` | Reconstruct one HTTP message from a TCP stream |
| `http_parser` | Parse a complete message into `http_request_t` |
| `http_request` | Request model + case-insensitive header lookup |
| `http_response` | Response model + CRLF serialization |
| `static_files` | Safe path resolution, file load, 404 page |
| `mime` | Extension → Content-Type (read-only table) |
| `utils` | `send_all()` |

## Separation of concerns

The HTTP parser knows methods, targets, versions, headers, and bodies. It does
**not** know about `./public`, `realpath()`, `stat()`, or MIME types.

Filesystem concerns live exclusively in `static_files` (+ `mime`).

Concurrency lives in `connection_queue` / `thread_pool` / `server`; the HTTP
pipeline in `client_handler` is unchanged in semantics from Stage 3.

## Shutdown

`SIGINT` / `SIGTERM` set a `volatile sig_atomic_t` flag only (no pthread,
malloc, or I/O in the handler). `SA_RESTART` is omitted so `accept()` returns
`EINTR`.

When the accept loop exits:

1. `connection_queue_shutdown()` sets the flag and broadcasts both condvars
2. Workers finish already-queued connections, then exit when empty+shutdown
3. `thread_pool_join()` waits for every worker
4. Queue mutex/condvars and allocations are destroyed
5. Listening socket cleanup remains in `main()`

If the accept thread is blocked in `push()` on a full queue, timed waits
periodically observe the same shutdown flag so shutdown cannot deadlock
without calling pthread APIs from the signal handler. A fd that fails to
enqueue remains owned by the accept thread and is closed there.

## Thread-safety notes

Request modules use per-request heap/stack state. Shared mutable state is
limited to:

- the connection queue (mutex)
- the logger (mutex)
- `g_shutdown_requested` (`sig_atomic_t`, written only from the handler)

Read-only static tables (MIME map, reason phrases) need no locking.
