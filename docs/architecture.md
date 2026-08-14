# Architecture

CinderHTTP Stage 6 hardens lifecycle and shutdown on top of the Stage 4–5
producer–consumer worker pool, application router, and runtime stats.

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
                  namespace dispatch
                    /            \
                   /              \
            /api/* path        other path
                   |                  |
                   v                  v
            router_dispatch    static_files_serve
                   |            (GET/HEAD only)
                   |
         +---------+---------+
         |         |         |
         v         v         v
   /api/health /api/echo /api/stats
         |         |         |
         +---------+---------+
                   |
                   v
            http_response_t
                   |
                   v
               send_all()
                   |
                   v
                 close
```

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
| `config` | CLI defaults and validation |
| `server` | Listen/accept, stats ownership, orchestration |
| `connection_queue` | Bounded ring buffer + mutex/condvars |
| `thread_pool` | Fixed joinable worker lifecycle |
| `client_handler` | One-connection read/parse/dispatch/send/close |
| `router` | `/api/*` method+path matching and handlers |
| `server_stats` | Thread-safe counters + snapshots |
| `logger` | Mutex-protected stderr logging |
| `http_reader` / `http_parser` | Framing and syntax |
| `http_response` | Response model + serialization |
| `static_files` / `mime` | Secure static serving |
| `utils` | `send_all()` |

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
workers drain already-queued clients
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

## Stats lock scope

The stats mutex protects only counter reads/writes. Workers do not hold it
while reading requests, serving files, formatting JSON, or sending responses.
`/api/stats` copies a snapshot under the lock, then formats unlocked.
