# Architecture

CinderHTTP is built in stages. Through Stage 3 the server is still
**single-threaded**: the accept loop handles one client connection at a time.
The worker pool and connection queue are planned for Stage 4.

## Overview

```text
                       CLIENTS
                          |
                          | TCP
                          v
                 +----------------+
                 | Listening Socket|
                 +-------+--------+
                         |
                         | accept()  (single-threaded today)
                         v
                  handle_client()
                         |
                         v
                 http_read_request()
                 (multi-recv framing)
                         |
                         v
                 http_parse_request()
                 (syntax only)
                         |
                         v
                  Request dispatch
                 +--------+--------+
                 |                 |
                 v                 v
           GET / HEAD            POST
                 |                 |
                 v                 v
         static_files_serve   temporary text
                 |             response
                 v
         +-------+--------+
         |                |
         v                v
   URL decode      MIME from path
   normalize
   realpath
   root check
         |
         v
   load file (GET) or size only (HEAD)
         |
         v
   http_response_t
         |
         v
   serialize + send_all()
         |
         v
   close connection
```

## Module responsibilities

| Module | Responsibility |
|--------|----------------|
| `config` | CLI defaults and validation |
| `server` | listen/accept loop, signal shutdown, dispatch |
| `http_reader` | Reconstruct one HTTP message from a TCP stream |
| `http_parser` | Parse a complete message into `http_request_t` |
| `http_request` | Request model + case-insensitive header lookup |
| `http_response` | Response model + CRLF serialization |
| `static_files` | Safe path resolution, file load, 404 page |
| `mime` | Extension → Content-Type |
| `utils` | `send_all()` |

## Separation of concerns

The HTTP parser knows methods, targets, versions, headers, and bodies. It does
**not** know about `./public`, `realpath()`, `stat()`, or MIME types.

Filesystem concerns live exclusively in `static_files` (+ `mime`).

## Shutdown

`SIGINT` / `SIGTERM` set a `volatile sig_atomic_t` flag. `SA_RESTART` is
omitted so `accept()` returns `EINTR` and the loop exits. Listening socket
cleanup remains in `main()`.

## Future concurrency (not yet implemented)

```text
accept loop → bounded connection queue → worker threads → same request pipeline
```

Introducing threads around a known-correct Stage 3 pipeline is the intent of
Stage 4.
