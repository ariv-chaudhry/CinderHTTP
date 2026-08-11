# CinderHTTP

CinderHTTP is a multithreaded HTTP/1.1 server written from scratch in C using
POSIX sockets and pthreads. It implements manual HTTP parsing, a bounded
producer-consumer connection queue, a fixed worker pool, static file
serving, application routing, server statistics, and graceful shutdown
without relying on an HTTP framework or parsing library.

**Status: Stage 1 of a staged build-out.** The project is being developed in
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
- A reliable, partial-write-safe socket send helper (`send_all`)

Planned (see [`docs/roadmap.md`](docs/roadmap.md) for the full breakdown):

- Manual HTTP/1.1 request parsing (request line, headers, `Content-Length` body)
- Static file serving with MIME detection and path-traversal protection
- A bounded, thread-safe connection queue feeding a fixed worker pool
- A lightweight router with `/api/health`, `/api/echo`, `/api/stats`
- Thread-safe request logging and runtime statistics
- Full graceful shutdown (drain queue, join workers, free all resources)
- Unit tests, integration tests, sanitizer/Valgrind verification, and benchmarks

## Architecture

Target architecture (worker pool and connection queue are not wired in yet -
see [How It Works](#how-it-works) for what Stage 1 actually does today):

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

Right now the listening thread does everything itself, synchronously, per
connection:

1. `main()` parses configuration and installs signal handlers.
2. `server_create_listening_socket()` creates, binds, and listens on the
   configured TCP port.
3. `server_run()` loops on `accept()`. For each accepted connection it sends
   one fixed, correctly framed HTTP response (see `src/server.c`) and closes
   the socket - there is no request parsing yet, so the response does not
   depend on what the client sent.
4. On `SIGINT`/`SIGTERM`, `accept()` returns `EINTR`, the loop notices the
   shutdown flag and exits, and `main()` closes the listening socket.

This exists to validate the socket lifecycle end-to-end before anything else
is layered on top of it. The connection queue and worker pool described in
the architecture diagram above are the next major pieces of work.

## Concurrency Model

Not yet implemented - the server is currently single-threaded (the accept
loop handles each client inline). The bounded connection queue and worker
thread pool are tracked in [`docs/roadmap.md`](docs/roadmap.md) Phase 4.
Once implemented, the design and its synchronization reasoning will be
documented in `docs/concurrency.md`.

## HTTP Support

Not yet implemented - no bytes are read from the client and no HTTP parsing
occurs. Every connection receives the same hardcoded response regardless of
method or path. Real parsing begins in Phase 2 of the roadmap; scope will be
documented in `docs/http_support.md` once it exists.

## Project Structure

```text
CinderHTTP/
|-- README.md
|-- LICENSE
|-- Makefile
|-- .gitignore
|-- .clang-format
|
|-- include/            # Public headers, one per module
|   |-- config.h
|   |-- server.h
|   `-- utils.h
|
|-- src/                # Implementation, one .c per header above
|   |-- main.c
|   |-- config.c
|   |-- server.c
|   `-- utils.c
|
|-- public/             # Static site served once static_files.c exists
|   |-- css/
|   `-- assets/
|
|-- tests/               # Unit + integration tests (from Stage 2 onward)
|   `-- integration/
|
|-- benchmarks/          # Load-testing scripts and (real, measured) results
|   `-- results/
|
|-- docs/
|   `-- roadmap.md
|
`-- scripts/
    |-- run.sh
    `-- format.sh
```

Headers/sources for modules that do not exist yet (`http_parser`,
`http_request`, `http_response`, `router`, `static_files`, `mime`,
`connection_queue`, `thread_pool`, `logger`, `stats`) are intentionally not
present as empty stubs. An empty `.c` file is rejected by `-Wpedantic
-Werror` (`ISO C forbids an empty translation unit`) and an empty header
would carry no information, so each file is added in the stage that actually
implements it rather than committed as a placeholder. The same applies to
`docs/architecture.md`, `docs/concurrency.md`, `docs/memory_model.md`, and
`docs/http_support.md`, and to `benchmarks/benchmark.sh`/`benchmarks/README.md`.

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

No automated tests yet - there is not yet enough behavior (parsing, routing,
file serving) to meaningfully test beyond "does the process start." Unit
tests begin in Phase 2 (`tests/test_parser.c`) once the HTTP parser exists.
`make test` already exists and currently reports that no tests are present
yet; it will pick up `tests/test_*.c` files automatically as they are added,
with no further Makefile changes required.

You can currently smoke-test the server manually:

```bash
make run &
curl -i http://localhost:8080/
curl -i http://localhost:8080/anything    # same fixed response; no routing yet
kill %1                                    # or Ctrl+C in the foreground terminal
```

## Memory Safety

`make debug` builds with AddressSanitizer and UndefinedBehaviorSanitizer.
There is minimal dynamic memory allocation yet (Stage 1 uses stack buffers
and one file descriptor per connection), so there is little to catch today,
but the sanitizer build is wired up now so every subsequent stage is checked
from the moment it lands. Once heap allocation is introduced (starting with
the HTTP request/response structures), this section will document concrete
Valgrind usage, e.g.:

```bash
valgrind --leak-check=full --show-leak-kinds=all ./bin/cinderhttp
```

## Benchmarking

Not applicable yet - there is no concurrency to benchmark. See
[`docs/roadmap.md`](docs/roadmap.md) Phase 8. `benchmarks/results/` will
hold real, measured output once benchmarks exist; it will never contain
fabricated numbers.

## Limitations

Current, real limitations of Stage 1 (not an exhaustive list of the
project's eventual scope - see `docs/http_support.md` once it exists for
that):

- No HTTP parsing: the response is identical for every request.
- Single-threaded: one client is handled at a time, synchronously, inline in
  the accept loop.
- No static file serving, routing, logging, or statistics.
- `--workers`, `--queue-size`, and `--root` are accepted but not yet
  functional.

## Future Work

Tracked in detail in [`docs/roadmap.md`](docs/roadmap.md). At a high level:
HTTP parsing and responses, static file serving with MIME detection and
path-traversal protection, a bounded connection queue and worker pool,
routing and API endpoints, statistics, structured logging, full graceful
shutdown, tests, sanitizer/Valgrind verification, and benchmarking.

## What I Learned / Engineering Decisions

- **Manual `argv` parsing over `getopt_long`.** The option set is small and
  fixed, and hand-rolling it keeps error messages exactly as specified and
  keeps the parsing logic in one obviously readable place rather than
  behind `getopt`'s global `optarg`/`optind` state.
- **`strtol` over `atoi` for numeric CLI options.** `atoi` has no way to
  report failure; `strtol` combined with checking `errno` and the end
  pointer lets `--port abc` be rejected outright instead of silently
  becoming `0`.
- **`sig_atomic_t` flag + no `SA_RESTART`, instead of a self-pipe or
  `signalfd`.** For a single blocking `accept()` call, a flag that the
  signal handler sets and the accept loop polls is the simplest correct
  option: the handler does no I/O or allocation (required for
  async-signal-safety), and omitting `SA_RESTART` guarantees `accept()`
  actually returns `EINTR` instead of transparently retrying and never
  observing the flag.
- **`SO_REUSEADDR` on the listening socket.** Without it, restarting the
  server immediately after stopping it (common during development) often
  fails with "Address already in use" while the previous socket's
  connections are still in `TIME_WAIT`.
- **Computing `Content-Length` from `strlen()` instead of writing the number
  by hand.** A hand-counted length is an easy way to send a subtly broken
  response; deriving it from the actual body at runtime makes the two
  impossible to desynchronize.
- **No stub files for unimplemented modules.** See
  [Project Structure](#project-structure) - files are added when the stage
  that implements them lands, not as empty placeholders, partly because
  `-Wpedantic -Werror` rejects empty translation units and partly because an
  empty file documents nothing.

## License

[MIT](LICENSE)
