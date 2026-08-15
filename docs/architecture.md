# Architecture

CinderHTTP Stage 10 completes the implementation roadmap: a producer–consumer
worker pool, application router, HTTP/1.x persistent connections, file-backed
static streaming, and separate keep-alive vs request deadlines.

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
            |   (keep-alive vs request    |
            |    timeout)                 |
            | parse                       |
            | route / static              |
            | apply Connection policy     |
            | send response               |
            |   MEMORY → send_all         |
            |   FILE → sendfile|fallback  |
            |   HEAD → headers only       |
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
| `config` | CLI defaults/validation (`--keep-alive-timeout`, `--request-timeout`, …) |
| `server` | Listen/accept, stats ownership, orchestration |
| `connection_queue` | Bounded ring buffer + mutex/condvars (**connections**, not requests) |
| `thread_pool` | Fixed joinable worker lifecycle |
| `client_handler` | Connection loop: read/parse/dispatch/send; close once |
| `http_connection` | Connection-token parsing + keep-alive / response policy |
| `http_reader` | Stateful framing; leftover preservation; dual timeout model |
| `http_parser` | Single framed-message syntax |
| `router` | `/api/*` method+path matching and handlers |
| `server_stats` | Thread-safe counters + snapshots |
| `logger` | Mutex-protected stderr logging |
| `http_response` | Response model; MEMORY / FILE body kinds; serialize + send |
| `static_files` / `mime` | Secure resolve → open/`fstat` → file-backed response |
| `utils` | `send_all()` |

## File-backed static path

```text
request target
  → strip query / URL-decode / lexical normalize
  → realpath + document-root confinement
  → open(O_RDONLY) + fstat
  → http_response_set_file_body(fd, size)   # response owns fd
  → http_response_send:
        send headers
        if HEAD (omit_body): stop
        else: Linux sendfile()  —or—  fixed chunk read+send fallback
  → http_response_destroy() closes fd
```

API routes and small error pages still use MEMORY bodies. Static success paths
do **not** load the whole file into the heap.

## Persistent-connection tradeoff

Because each keep-alive connection pins a worker:

```text
workers = 4
4 idle persistent clients  →  all workers occupied until idle timeout
```

`--keep-alive-timeout`, `--request-timeout`, and
`HTTP_MAX_REQUESTS_PER_CONNECTION` bound that occupancy. This is inherent to
the blocking worker-pool design; event-driven multiplexing is intentionally
out of scope (see optional extensions in [`roadmap.md`](roadmap.md)).

The queue stores **accepted connections**, not individual keep-alive requests.

## Timeout model

Two complementary deadlines:

| Timeout | When it applies | On expiry |
|---------|-----------------|-----------|
| `--keep-alive-timeout` | Idle wait with empty reader buffer (next request has not begun) | Silent close |
| `--request-timeout` | Wall-clock (`CLOCK_MONOTONIC`) once any bytes of the current request are present | `408` + `Connection: close` |

A socket receive timeout is also set as a backstop (the larger of the two
configured values) so a worker blocked in `recv` cannot hang forever through
shutdown.

Shutdown remains bounded: a worker returns from `recv` within the backstop,
then joins the normal drain path.

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
  (recv backstop bounds idle / in-flight waits)
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
