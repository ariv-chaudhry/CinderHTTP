# CinderHTTP

CinderHTTP is an HTTP/1.1 server written from scratch in C using POSIX sockets
(and, eventually, pthreads). The **intended** architecture includes a bounded
producer-consumer connection queue and a fixed worker pool; the current
implementation through Stage 3 is still synchronous and single-threaded while
the request pipeline (parsing + secure static files) is proven correct.

It implements manual HTTP parsing, secure static file serving, MIME detection,
and graceful signal-driven shutdown without relying on an HTTP framework or
parsing library.

**Status: Stage 3 of a staged build-out.** See
[`docs/roadmap.md`](docs/roadmap.md) for what is implemented versus planned.

## Features

Implemented so far:

- Command-line configuration (`--port`, `--workers`, `--queue-size`, `--root`,
  `--verbose`, `--help`) — `--root` selects the static document root
- TCP listening socket with `SO_REUSEADDR` and signal-driven shutdown
- Buffered multi-`recv()` HTTP request framing and manual HTTP/1.0–1.1 parsing
- `GET` / `HEAD` / `POST` (POST still temporary text; not filesystem-routed)
- Secure static file serving with MIME detection and binary-safe responses
- Path traversal protection (literal and percent-encoded) + symlink escape checks
- Custom `404.html`, correct HEAD metadata without sending a body
- Unit tests and curl/nc integration checks

Planned:

- Bounded connection queue + worker thread pool (Stage 4)
- Application router (`/api/health`, `/api/echo`, `/api/stats`)
- Thread-safe request logging and runtime statistics
- Keep-alive, chunked encoding, TLS, benchmarks

## Architecture

Target architecture (worker pool not wired yet). Current Stage 3 flow is
documented in [`docs/architecture.md`](docs/architecture.md).

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

The accept loop still handles each client inline (no worker pool yet):

1. Parse configuration (`--root` selects the document root) and install signals.
2. Create / bind / listen on the configured TCP port.
3. For each connection: frame one HTTP request → parse → dispatch:
   - `GET`/`HEAD` → secure static resolver (decode, normalize, `realpath`,
     MIME, load or metadata-only) → response
   - `POST` → temporary Stage 2 text confirmation (never writes files)
4. Serialize with CRLF, send via `send_all()`, destroy owned buffers, close
   the socket (`Connection: close`).
5. `SIGINT`/`SIGTERM` stops the accept loop cleanly.

Details: [`docs/http_support.md`](docs/http_support.md),
[`docs/memory_model.md`](docs/memory_model.md).

## Concurrency Model

**The current implementation is synchronous.** One client is handled at a
time in the accept loop. The bounded connection queue and worker thread pool
are Stage 4 (`docs/roadmap.md`).

## HTTP Support

Stage 3 supports HTTP/1.0–1.1 parsing, `GET`/`HEAD` static files, MIME
detection, path security, and temporary `POST` text responses. Keep-alive,
chunked encoding, routing, and TLS are **not** implemented. Full detail:
[`docs/http_support.md`](docs/http_support.md).

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
|   |-- http_response.h
|   |-- mime.h
|   `-- static_files.h
|
|-- src/
|   |-- main.c
|   |-- config.c
|   |-- server.c
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

Modules that do not exist yet (`router`, `connection_queue`, `thread_pool`,
`logger`, `stats`) are added when the stage that implements them lands.

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
make test                         # unit tests (parser, response, mime, static)
make integration                  # curl/nc checks against a live server
```

Manual smoke checks:

```bash
./bin/cinderhttp --port 8080 --root ./public --verbose
curl -i http://localhost:8080/
curl -i http://localhost:8080/css/style.css
curl -I http://localhost:8080/
curl -i http://localhost:8080/not-found
curl --path-as-is -i http://localhost:8080/../../outside.txt   # expect 403
curl -i -X POST --data hello http://localhost:8080/test
```

## Memory Safety

`make debug` builds with AddressSanitizer and UndefinedBehaviorSanitizer.
Stage 2–3 unit tests were run under those sanitizers. Ownership rules:
[`docs/memory_model.md`](docs/memory_model.md).

## Limitations

Current Stage 3 limitations:

- **Synchronous / single-threaded** — one client at a time
- No application router or `/api/*` endpoints yet
- No keep-alive, chunked encoding, TLS, directory listings, or `sendfile()`
- Static files buffered in memory up to 16 MiB
- `--workers` / `--queue-size` accepted but not yet functional
- POST never writes to the filesystem (temporary text response only)

## Benchmarking

Deferred until concurrency exists (Stage 4+). See `docs/roadmap.md` Phase 8.
`benchmarks/results/` will hold measured output only — never fabricated numbers.

## Future Work

Next up is **Stage 4 — bounded connection queue and worker thread pool**.
See [`docs/roadmap.md`](docs/roadmap.md).

## What I Learned / Engineering Decisions

- **Separate framing from parsing.** `http_reader` reconstructs complete
  messages; `http_parser` never touches sockets.
- **Separate parsing from the filesystem.** Static resolution lives in
  `static_files`, not in the HTTP parser.
- **One-pass URL decode + lexical normalize + `realpath` boundary check.**
  Rejects literal/encoded traversal and symlink escapes without fragile
  substring searches for `..` alone.
- **HEAD uses `stat` size without loading the file.** `Content-Length` stays
  correct while zero body bytes go on the wire.
- **Directory without index → 403.** No directory listings by design.
- **Reject duplicate `Content-Length` and any `Transfer-Encoding`.** Clear
  framing beats silent guessing.
- **`--root` is live; workers/queue are still reserved.** Keeps the CLI
  stable as concurrency lands.

## License

[MIT](LICENSE)
