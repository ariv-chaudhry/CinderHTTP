# CinderHTTP

CinderHTTP is an HTTP/1.1 server written from scratch in C using POSIX sockets
and pthreads. It uses a bounded producer–consumer connection queue and a fixed
worker pool so the accept thread never processes HTTP itself.

It implements manual HTTP parsing, an application router, secure static file
serving, MIME detection, thread-safe verbose logging, runtime statistics, and
graceful signal-driven shutdown — without an HTTP framework or parsing library.

**Status: Stage 8 of a staged build-out.** See
[`docs/roadmap.md`](docs/roadmap.md) for what is implemented versus planned.

## Features

Implemented so far:

- Command-line configuration (`--port`, `--workers`, `--queue-size`, `--root`,
  `--verbose`, `--help`)
- TCP listening socket with `SO_REUSEADDR` and signal-driven shutdown
- Bounded connection queue + fixed pthread worker pool (backpressure when full)
- Graceful queue-draining shutdown; SIGPIPE isolation for broken clients
- Thread-safe logging and runtime statistics
- Buffered multi-`recv()` HTTP request framing and manual HTTP/1.0–1.1 parsing
- Application router: `GET /api/health`, `POST /api/echo`, `GET /api/stats`
- Secure static file serving with MIME detection and binary-safe responses
- Path traversal protection (literal and percent-encoded) + symlink escape checks
- Custom `404.html`, correct HEAD metadata without sending a body
- Hardened unit/integration suite: framing, fuzz-style parser stress, raw
  malformed clients, ASan/UBSan, optional Valgrind/coverage
- Reproducible Stage 8 benchmarking across worker counts (`make benchmark`)

Planned:

- Keep-alive, chunked encoding, TLS
- Further optimizations guided by measured bottlenecks

## Architecture

```text
accept thread → bounded queue → workers → client_handle()
                                              |
                                    read / parse
                                              |
                               +--------------+--------------+
                               |                             |
                            /api/*                        other
                               |                             |
                            router                      static GET/HEAD
                               |
                    health / echo / stats
```

Full detail: [`docs/architecture.md`](docs/architecture.md).

## How It Works

1. Parse configuration and install signals.
2. Create / bind / listen; init stats, queue, and worker pool.
3. Accept thread: `accept()` → enqueue fd (backpressure when full).
4. Workers: `pop(fd)` → frame → parse → `/api` router **or** static fallback →
   send → close. Queue/stats mutexes are **not** held during request I/O.
5. `SIGINT`/`SIGTERM` drains the queue, joins workers, then destroys sync
   objects and stats.

## API Endpoints

```bash
curl http://localhost:8080/api/health

curl -X POST -H "Content-Type: text/plain" --data-binary "hello" \
  http://localhost:8080/api/echo

curl http://localhost:8080/api/stats
```

| Method | Path | Notes |
|--------|------|-------|
| `GET`/`HEAD` | `/api/health` | `{"status":"ok"}` |
| `POST` | `/api/echo` | Binary-safe body echo |
| `GET`/`HEAD` | `/api/stats` | Runtime counters JSON |

## Concurrency Model

Classic bounded producer–consumer:

- Producer: accept thread
- Buffer: ring of client fds (`--queue-size`)
- Consumers: fixed worker pool (`--workers`)

A full queue blocks the producer (intentional backpressure). See
[`docs/architecture.md`](docs/architecture.md).

## HTTP Support

Stage 5 supports HTTP/1.0–1.1 parsing, the `/api/*` routes above, `GET`/`HEAD`
static files, MIME detection, and path security. Keep-alive, chunked encoding,
and TLS are **not** implemented. Full detail:
[`docs/http_support.md`](docs/http_support.md).

## Project Structure

```text
CinderHTTP/
|-- include/   config, server, connection_queue, thread_pool, logger,
|              client_handler, router, server_stats, http_*, mime, static_files, utils
|-- src/       matching .c files + main.c
|-- public/    static demo site
|-- tests/     unit tests + integration/test_server.sh
|-- docs/      roadmap, architecture, http_support, memory_model
`-- Makefile
```

## Building

Requires GCC (or another C11 compiler), GNU Make, and a POSIX environment
(Linux or macOS; on Windows, use WSL).

```bash
make            # optimized build -> bin/cinderhttp
make debug      # ASan/UBSan debug build
make debug test # sanitizer build + unit tests in one make run
make sanitize   # convenience alias for ASan/UBSan unit tests
make valgrind   # unit tests under Valgrind (skipped if not installed)
make clean
```

Flags: `-std=c11 -Wall -Wextra -Wpedantic -Werror`.

## Running

```bash
./bin/cinderhttp --port 8080 --workers 4 --queue-size 64 --root ./public --verbose
```

Stop with `Ctrl+C`.

## Configuration

| Option           | Default      | Description                          |
|-------------------|--------------|--------------------------------------|
| `--port <n>`       | `8080`       | TCP port                             |
| `--workers <n>`    | `4`          | Worker thread pool size              |
| `--queue-size <n>` | `64`         | Bounded connection queue capacity    |
| `--root <path>`    | `./public`   | Static document root                 |
| `--verbose`        | off          | Verbose logging                      |
| `--help`           | -            | Usage                                |

## Testing

```bash
make test          # unit tests (includes a modest deterministic parser fuzz pass)
make integration   # live server checks, including raw malformed clients
make sanitize      # ASan/UBSan unit tests
make fuzz          # deeper deterministic parser fuzz (override with CINDERHTTP_FUZZ_ITERS)
make valgrind      # optional Valgrind sweep
make coverage      # optional gcov summary
```

Details: [`docs/testing.md`](docs/testing.md). The parser is exercised against
deterministic random and mutated malformed inputs; this is regression hardening,
not formal verification.

## Benchmarking / Performance

```bash
make benchmark
make benchmark BENCH_ARGS="--connections 128 --duration 15"
```

Compares worker configurations (`1/2/4/8` by default) on a release build,
records throughput and latency (including p95 when the selected tool supports
it), and writes `benchmarks/results/latest.json` (+ CSV). Requires `wrk`,
`hey`, or `ab` on `PATH`. See [`benchmarks/README.md`](benchmarks/README.md).

Localhost results are environment-dependent and are **not** production capacity
claims. CinderHTTP still closes each connection after one request.

## Memory Safety

`make debug test` runs unit tests under AddressSanitizer and
UndefinedBehaviorSanitizer. Ownership: [`docs/memory_model.md`](docs/memory_model.md).

## Limitations

- No keep-alive, chunked encoding, TLS, directory listings, or `sendfile()`
- No client read-timeout architecture (slow clients can occupy a worker)
- Static files buffered in memory up to 16 MiB
- No dynamic thread-pool resizing
- Query strings are not parsed into key/value maps

## Future Work

Possible next steps include keep-alive, evented I/O, and other optimizations
guided by Stage 8 measurements. See [`docs/roadmap.md`](docs/roadmap.md).

## License

[MIT](LICENSE)
