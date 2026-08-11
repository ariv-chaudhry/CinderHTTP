# CinderHTTP

CinderHTTP is a multithreaded HTTP/1.1 server written from scratch in C using
POSIX sockets and pthreads. It implements manual HTTP parsing, a bounded
producer-consumer connection queue, a fixed worker pool, static file
serving, application routing, server statistics, and graceful shutdown
without relying on an HTTP framework or parsing library.

**Status: Stage 2 of a staged build-out.** The project is being developed in
deliberate, verifiable stages rather than committed as one large drop; see
[`docs/roadmap.md`](docs/roadmap.md) for exactly what is implemented today
versus what is planned. This README describes the project's target shape
and is explicit about which parts exist right now.

## Features

Implemented so far:

- Command-line configuration with validation (`--port`, `--workers`,
  `--queue-size`, `--root`, `--verbose`, `--help`)
- A manually constructed TCP listening socket (`socket`/`setsockopt`/`bind`/`listen`)
- An `accept()` loop that tolerates interrupted system calls and per-client
  errors without crashing
- Signal-driven shutdown on `SIGINT`/`SIGTERM`
- Reliable, partial-write-safe `send_all()`
- Buffered request reading across multiple `recv()` calls (TCP byte-stream framing)
- Manual HTTP/1.0 and HTTP/1.1 request parsing (no HTTP libraries)
- `GET`, `HEAD`, and `POST` with `Content-Length` bodies
- Structured request/response models with explicit memory ownership
- Correct `HEAD` responses (headers/`Content-Length` without a body)
- Error responses: 400 / 405 / 413 / 501 / 505
- Unit tests (`make test`) and curl/nc integration checks

Planned (see [`docs/roadmap.md`](docs/roadmap.md) for the full breakdown):

- Static file serving with MIME detection and path-traversal protection
- A bounded, thread-safe connection queue feeding a fixed worker pool
- A lightweight router with `/api/health`, `/api/echo`, `/api/stats`
- Thread-safe request logging and runtime statistics
- Full graceful shutdown (drain queue, join workers, free all resources)
- Keep-alive, chunked encoding, TLS, benchmarks

## Architecture

Target architecture (worker pool and connection queue are not wired in yet -
see [How It Works](#how-it-works) for what Stage 2 actually does today):

```text
                       CLIENTS
                          |
                          | TCP
                          v
                 +----------------+
                 | Listening Socket|
                 +-------+--------+
                         |
                         | accept()
                         v
                +-----------------+
                | Connection Queue |
                | bounded + thread |
                | safe             |
                +--------+--------+
                         |
          +--------------+--------------+
          v              v              v
      Worker 1        Worker 2       Worker N
          |              |              |
          +--------------+--------------+
                         v
                  Request Handler
                         |
                         v
                   HTTP Parser
                         |
                         v
                      Router
                 +------+-------+
                 v              v
           Static Files       API Routes
                 |              |
                 +------+-------+
                        v
                  HTTP Response
                        |
                        v
                      Client
```

## How It Works (current state)

The listening loop still handles each client inline (no worker pool yet):

1. `main()` parses configuration and installs signal handlers.
2. `server_create_listening_socket()` creates, binds, and listens on the
   configured TCP port.
3. `server_run()` loops on `accept()`. For each connection:
   - `http_read_request()` accumulates TCP bytes until `\r\n\r\n` and any
     `Content-Length` body are complete
   - `http_parse_request()` builds an owned `http_request_t`
   - a temporary Stage 2 handler builds an `http_response_t` (fixed success
     text for GET/HEAD/POST; mapped error statuses otherwise)
   - the response is serialized with CRLF and sent via `send_all()`
   - request/response/raw buffers are destroyed and the socket is closed
     (`Connection: close`; one request per connection)
4. On `SIGINT`/`SIGTERM`, `accept()` returns `EINTR`, the loop notices the
   shutdown flag and exits, and `main()` closes the listening socket.

See [`docs/http_support.md`](docs/http_support.md) and
[`docs/memory_model.md`](docs/memory_model.md) for protocol scope and ownership.

## Concurrency Model

Not yet implemented - the server is currently single-threaded (the accept
loop handles each client inline). The bounded connection queue and worker
thread pool are tracked in [`docs/roadmap.md`](docs/roadmap.md) Phase 4.
Once implemented, the design and its synchronization reasoning will be
documented in `docs/concurrency.md`.

## HTTP Support

Stage 2 supports manual parsing of HTTP/1.0 and HTTP/1.1 requests for
`GET` / `HEAD` / `POST`, `Content-Length` bodies, and structured response
generation. Keep-alive, chunked encoding, routing, and static files are
**not** implemented. Full detail: [`docs/http_support.md`](docs/http_support.md).

## Project Structure

```text
CinderHTTP/
|-- README.md
|-- LICENSE
|-- Makefile
|-- .gitignore
|-- .clang-format
|
|-- include/
|   |-- config.h
|   |-- server.h
|   |-- utils.h
|   |-- http_limits.h
|   |-- http_request.h
|   |-- http_parser.h
|   |-- http_reader.h
|   `-- http_response.h
|
|-- src/
|   |-- main.c
|   |-- config.c
|   |-- server.c
|   |-- utils.c
|   |-- http_request.c
|   |-- http_parser.c
|   |-- http_reader.c
|   `-- http_response.c
|
|-- public/             # Static site served once static_files.c exists
|   |-- css/
|   `-- assets/
|
|-- tests/
|   |-- test_parser.c
|   |-- test_response.c
|   `-- integration/
|       `-- test_server.sh
|
|-- benchmarks/
|   `-- results/
|
|-- docs/
|   |-- roadmap.md
|   |-- http_support.md
|   `-- memory_model.md
|
`-- scripts/
    |-- run.sh
    `-- format.sh
```

Modules that do not exist yet (`router`, `static_files`, `mime`,
`connection_queue`, `thread_pool`, `logger`, `stats`) are added when the stage
that implements them lands — not as empty stubs (`-Wpedantic -Werror` rejects
empty translation units).

## Building

Requires GCC (or another C11 compiler), GNU Make, and a POSIX environment
(Linux or macOS; on Windows, use WSL).

```bash
make            # optimized build -> bin/cinderhttp
make debug      # -O0 -g build with AddressSanitizer + UndefinedBehaviorSanitizer
make clean      # remove build/ and bin/
```

The build uses `-std=c11 -Wall -Wextra -Wpedantic -Werror`; the project does
not currently build with any warnings suppressed.

## Running

```bash
make run
# or, equivalently:
./bin/cinderhttp
./bin/cinderhttp --port 9000 --verbose
scripts/run.sh --port 9000
```

Stop the server with `Ctrl+C` (`SIGINT`); it will print a shutdown message
and exit cleanly.

## Configuration

| Option           | Default      | Description                                   |
|-------------------|--------------|------------------------------------------------|
| `--port <n>`       | `8080`       | TCP port to listen on                          |
| `--workers <n>`    | `4`          | Worker thread pool size *(not yet active)*     |
| `--queue-size <n>` | `64`         | Connection queue capacity *(not yet active)*   |
| `--root <path>`    | `./public`   | Document root for static files *(not yet active)* |
| `--verbose`        | off          | Enable verbose diagnostic logging to stderr    |
| `--help`           | -            | Print usage and exit                           |

Invalid values (out-of-range ports, non-numeric counts, missing values,
unrecognized flags) are rejected with a message on stderr and a non-zero
exit code rather than being silently ignored or clamped.

## API Endpoints

Not yet implemented. Planned: `GET /api/health`, `POST /api/echo`,
`GET /api/stats` (see [`docs/roadmap.md`](docs/roadmap.md) Phase 5).

## Testing

```bash
make test                         # unit tests (parser + response)
make integration                  # curl/nc checks against a live server
```

Manual smoke checks:

```bash
./bin/cinderhttp --port 8080 --verbose
curl -i http://localhost:8080/
curl -I http://localhost:8080/                 # HEAD: headers only
curl -i -X POST --data hello http://localhost:8080/test
curl -i -X DELETE http://localhost:8080/       # expect 405
```

## Memory Safety

`make debug` builds with AddressSanitizer and UndefinedBehaviorSanitizer.
Stage 2 unit tests were also run under those sanitizers. Ownership rules are
documented in [`docs/memory_model.md`](docs/memory_model.md). Valgrind usage
(once heap-heavy paths grow further):

```bash
valgrind --leak-check=full --show-leak-kinds=all ./bin/cinderhttp
```

## Benchmarking

Not applicable yet - there is no concurrency to benchmark. See
[`docs/roadmap.md`](docs/roadmap.md) Phase 8. `benchmarks/results/` will
hold real, measured output once benchmarks exist; it will never contain
fabricated numbers.

## Limitations

Current, real limitations of Stage 2:

- Single-threaded: one client is handled at a time, inline in the accept loop
- No static file serving, routing, structured request logging, or statistics
- No keep-alive / pipelining / chunked transfer encoding / TLS
- Temporary handler returns fixed success text for any valid GET/HEAD/POST
  (path is parsed but not routed)
- `--workers`, `--queue-size`, and `--root` are accepted but not yet functional

## Future Work

Tracked in detail in [`docs/roadmap.md`](docs/roadmap.md). Next up is static
file serving with MIME detection and path-traversal protection, then the
bounded connection queue and worker pool, routing/API endpoints, statistics,
structured logging, full graceful shutdown, and benchmarking.

## What I Learned / Engineering Decisions

- **Separate framing from parsing.** `http_reader` reconstructs one complete
  message across multiple `recv()` calls; `http_parser` only sees complete
  buffers. That split matches how TCP actually works and makes the parser
  unit-testable without sockets.
- **Shared Content-Length validation.** The reader and parser both call
  `http_inspect_message_framing()` / `http_parse_content_length_value()` so
  framing rules cannot disagree.
- **Reject duplicate `Content-Length` always.** Even identical duplicates are
  refused; ambiguous framing is not worth supporting.
- **`Transfer-Encoding` → 501, not silent ignore.** Chunked bodies are a
  future feature; pretending they are absent would be incorrect.
- **HEAD omits body but keeps `Content-Length`.** Serialization takes an
  `omit_body` flag so GET and HEAD share one response object.
- **Manual `argv` parsing over `getopt_long`.** Small fixed option set; clear
  errors without `optarg`/`optind` globals.
- **`sig_atomic_t` flag + no `SA_RESTART`.** Async-signal-safe shutdown that
  unblocks `accept()` via `EINTR`.
- **`SO_REUSEADDR`.** Avoids restart failures while prior sockets sit in
  `TIME_WAIT`.
- **No stub files for unimplemented modules.** Added only when implemented
  (`-Wpedantic -Werror` rejects empty translation units).

## License

[MIT](LICENSE)
