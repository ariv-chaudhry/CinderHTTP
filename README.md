# CinderHTTP

CinderHTTP is an HTTP/1.1 server written from scratch in C using POSIX sockets
and pthreads. It uses a bounded producer–consumer connection queue and a fixed
worker pool so the accept thread never processes HTTP itself.

It implements manual HTTP parsing, secure static file serving, MIME detection,
thread-safe verbose logging, and graceful signal-driven shutdown without an
HTTP framework or parsing library.

**Status: Stage 4 of a staged build-out.** See
[`docs/roadmap.md`](docs/roadmap.md) for what is implemented versus planned.

## Features

Implemented so far:

- Command-line configuration (`--port`, `--workers`, `--queue-size`, `--root`,
  `--verbose`, `--help`) — workers and queue size are live
- TCP listening socket with `SO_REUSEADDR` and signal-driven shutdown
- Bounded connection queue + fixed pthread worker pool (backpressure when full)
- Thread-safe logging for concurrent workers
- Buffered multi-`recv()` HTTP request framing and manual HTTP/1.0–1.1 parsing
- `GET` / `HEAD` / `POST` (POST still temporary text; not filesystem-routed)
- Secure static file serving with MIME detection and binary-safe responses
- Path traversal protection (literal and percent-encoded) + symlink escape checks
- Custom `404.html`, correct HEAD metadata without sending a body
- Unit tests (including queue concurrency) and curl/nc integration checks

Planned:

- Application router (`/api/health`, `/api/echo`, `/api/stats`)
- Runtime statistics endpoint
- Keep-alive, chunked encoding, TLS, benchmarks

## Architecture

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
               +----------+-----------+
                          |
              +-----------+-----------+
              |           |           |
              v           v           v
          Worker 1    Worker 2    Worker N
                          |
                          v
                    client_handle()
                          |
                   HTTP read/parse
                          |
                   static / POST
                          |
                      response
```

Full detail: [`docs/architecture.md`](docs/architecture.md).

## How It Works

1. Parse configuration and install signals.
2. Create / bind / listen on the configured TCP port.
3. Initialize the connection queue and start `--workers` joinable threads.
4. Accept thread: `accept()` → log → `connection_queue_push(fd)`.
5. Workers: `pop(fd)` → `client_handle()` (frame → parse → static/POST → send
   → close). The queue mutex is **not** held during request processing.
6. `SIGINT`/`SIGTERM` sets a `sig_atomic_t` flag; accept loop exits, queue
   shutdown drains queued work, workers are joined, then primitives are
   destroyed.

Details: [`docs/http_support.md`](docs/http_support.md),
[`docs/memory_model.md`](docs/memory_model.md).

## Concurrency Model

Classic bounded producer–consumer:

- Producer: accept thread
- Buffer: ring of client fds (`--queue-size`)
- Consumers: fixed worker pool (`--workers`)

A full queue blocks the producer (intentional backpressure). See
[`docs/architecture.md`](docs/architecture.md).

## HTTP Support

Stage 4 preserves Stage 3 HTTP semantics: HTTP/1.0–1.1 parsing, `GET`/`HEAD`
static files, MIME detection, path security, and temporary `POST` text
responses. Keep-alive, chunked encoding, routing, and TLS are **not**
implemented. Full detail: [`docs/http_support.md`](docs/http_support.md).

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
|   |-- connection_queue.h
|   |-- thread_pool.h
|   |-- logger.h
|   |-- client_handler.h
|   |-- utils.h
|   |-- http_limits.h
|   |-- http_request.h
|   |-- http_parser.h
|   |-- http_reader.h
|   |-- http_response.h
|   |-- mime.h
|   `-- static_files.h
|
|-- src/
|   |-- main.c
|   |-- config.c
|   |-- server.c
|   |-- connection_queue.c
|   |-- thread_pool.c
|   |-- logger.c
|   |-- client_handler.c
|   |-- utils.c
|   |-- http_request.c
|   |-- http_parser.c
|   |-- http_reader.c
|   |-- http_response.c
|   |-- mime.c
|   `-- static_files.c
|
|-- public/
|   |-- index.html
|   |-- 404.html
|   |-- css/style.css
|   `-- assets/
|
|-- tests/
|   |-- test_parser.c
|   |-- test_response.c
|   |-- test_mime.c
|   |-- test_static_files.c
|   |-- test_connection_queue.c
|   `-- integration/test_server.sh
|
|-- docs/
|   |-- roadmap.md
|   |-- architecture.md
|   |-- http_support.md
|   `-- memory_model.md
|
|-- benchmarks/
|   `-- results/
|
`-- scripts/
    |-- run.sh
    `-- format.sh
```

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
./bin/cinderhttp --port 9000 --workers 4 --queue-size 64 --verbose
scripts/run.sh --port 9000
```

Stop the server with `Ctrl+C` (`SIGINT`); it drains the queue, joins workers,
prints a shutdown message, and exits cleanly.

## Configuration

| Option           | Default      | Description                                   |
|-------------------|--------------|------------------------------------------------|
| `--port <n>`       | `8080`       | TCP port to listen on                          |
| `--workers <n>`    | `4`          | Fixed worker thread pool size                  |
| `--queue-size <n>` | `64`         | Bounded connection queue capacity              |
| `--root <path>`    | `./public`   | Document root for static files                 |
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
make test                         # unit tests (parser, response, mime, static, queue)
make integration                  # curl/nc checks including concurrent Stage 4
```

Manual smoke checks:

```bash
./bin/cinderhttp --port 8080 --workers 4 --queue-size 64 --root ./public --verbose
curl -i http://localhost:8080/
curl -i http://localhost:8080/css/style.css
curl -I http://localhost:8080/
curl -i http://localhost:8080/not-found
curl --path-as-is -i http://localhost:8080/../../outside.txt   # expect 403
curl -i -X POST --data hello http://localhost:8080/test

for i in $(seq 1 20); do curl -sS http://127.0.0.1:8080/ >/dev/null & done
wait
```

## Memory Safety

`make debug` builds with AddressSanitizer and UndefinedBehaviorSanitizer.
Stage 2–4 unit tests are run under those sanitizers. Ownership rules:
[`docs/memory_model.md`](docs/memory_model.md).

## Limitations

Current Stage 4 limitations:

- No application router or `/api/*` endpoints yet
- No keep-alive, chunked encoding, TLS, directory listings, or `sendfile()`
- Static files buffered in memory up to 16 MiB
- POST never writes to the filesystem (temporary text response only)
- No dynamic thread-pool resizing

## Benchmarking

See `docs/roadmap.md` Phase 8. `benchmarks/results/` will hold measured
output only — never fabricated numbers.

## Future Work

Next up is **Stage 5 — router and `/api/health`, `/api/echo`, `/api/stats`**.
See [`docs/roadmap.md`](docs/roadmap.md).

## What I Learned / Engineering Decisions

- **Separate framing from parsing.** `http_reader` reconstructs complete
  messages; `http_parser` never touches sockets.
- **Separate parsing from the filesystem.** Static resolution lives in
  `static_files`, not in the HTTP parser.
- **Accept thread never handles HTTP.** Queue + workers keep orchestration
  distinct from the request pipeline.
- **Bounded queue = backpressure.** Prefer blocking the producer over
  unbounded fd accumulation.
- **No pthread in signal handlers.** Shutdown uses `sig_atomic_t` plus timed
  waits in `push()` so a full-queue producer can still exit.
- **Drain after shutdown.** Queued clients are processed; workers exit only
  when empty and shutting down.
- **One-pass URL decode + lexical normalize + `realpath` boundary check.**
  Rejects literal/encoded traversal and symlink escapes.
- **HEAD uses `stat` size without loading the file.**
- **Directory without index → 403.** No directory listings by design.
- **Reject duplicate `Content-Length` and any `Transfer-Encoding`.**

## License

[MIT](LICENSE)
