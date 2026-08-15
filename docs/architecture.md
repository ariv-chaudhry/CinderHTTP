# Architecture

CinderHTTP Stage 9 adds HTTP/1.x persistent connections and bounded read
timeouts on top of the Stage 4–8 producer–consumer worker pool, application
router, runtime stats, and benchmarking harness.

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
              initialize http_reader_t
                          |
            +-------------↺---------------+
            | read next HTTP request      |
            | parse                       |
            | route / static              |
            | apply Connection policy     |
            | send response               |
            | determine close / continue  |
            +-------------+---------------+
                          |
                keep-alive│
                          └──── loop (same worker, same fd)
                          |
                        close
```

A worker that owns a persistent client processes that client's sequential
requests itself. Keep-alive requests are **not** re-enqueued.

Shared synchronized state (does **not** serialize request handling):

```text
                 server_stats_t
                 (mutex-protected counters)
                        ^
                        |
        workers update / /api/stats snapshots
```

## Module responsibilities

| Module | Responsibility |
|--------|----------------|
| `config` | CLI defaults and validation (incl. `--keep-alive-timeout`) |
| `server` | Listen/accept, stats ownership, orchestration |
| `connection_queue` | Bounded ring buffer + mutex/condvars (**connections**, not requests) |
| `thread_pool` | Fixed joinable worker lifecycle |
| `client_handler` | Connection loop: read/parse/dispatch/send; close once |
| `http_connection` | Connection-token parsing + keep-alive / response policy |
| `http_reader` | Stateful framing; leftover-byte preservation |
| `http_parser` | Single framed-message syntax |
| `router` | `/api/*` method+path matching and handlers |
| `server_stats` | Thread-safe counters + snapshots |
| `logger` | Mutex-protected stderr logging |
| `http_response` | Response model + serialization (`Content-Length` framing) |
| `static_files` / `mime` | Secure static serving |
| `utils` | `send_all()` |

## Persistent-connection tradeoff

Because each keep-alive connection pins a worker:

```text
workers = 4
4 idle persistent clients  →  all workers occupied until idle timeout
```

`--keep-alive-timeout` and `HTTP_MAX_REQUESTS_PER_CONNECTION` bound that
occupancy. This is inherent to the blocking worker-pool design; Stage 9 does
not introduce event-driven multiplexing.

## Timeout model

`SO_RCVTIMEO` is set on each accepted client socket to
`config->keep_alive_timeout_sec` (default 5, range 1–300).

- **Idle keep-alive** (no bytes yet for the next request) → silent close
- **Partial request** + timeout → `408 Request Timeout` + `Connection: close`
- Timeout is per blocking `recv`, not an absolute whole-request deadline

Shutdown remains bounded: a worker blocked in `recv` returns within the
configured receive timeout, then joins the normal drain path.

## Routing rules

- Match on **method + path** (query string ignored for matching).
- Exact path match; trailing slash is a different route.
- Known path + wrong method → **405** + `Allow`.
- Unknown `/api/*` → JSON **404** (never static fallback).
- Non-`/api` paths → static GET/HEAD; other methods → **405**.

## Graceful shutdown

```text
SIGINT / SIGTERM
      |
      v
shutdown flag (sig_atomic_t only)
      |
      v
accept loop exits (accept EINTR / push abort)
      |
      v
connection_queue_shutdown + broadcast
      |
      v
workers finish current connection
  (recv timeout bounds idle keep-alive waits)
      |
      v
workers exit (empty && shutting_down)
      |
      v
pthread_join all started workers
      |
      v
destroy pool → queue → stats → logger
      |
      v
main closes listening socket
```

Policy: **drain connections that successfully entered the application queue**.
Do not discard queued fds during normal shutdown. SIGPIPE is ignored so a
worker writing to a disconnected client cannot terminate the process.

Signal handlers only set flags / interrupt accept — they do not close
worker-owned sockets.

## Stats lock scope

The stats mutex protects only counter reads/writes. Workers do not hold it
while reading requests, serving files, formatting JSON, or sending responses.
`/api/stats` copies a snapshot under the lock, then formats unlocked.

`active_connections` tracks TCP connections, not HTTP requests.
