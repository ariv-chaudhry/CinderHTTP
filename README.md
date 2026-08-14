# CinderHTTP

CinderHTTP is an HTTP/1.1 server written from scratch in C using POSIX sockets
and pthreads. It uses a bounded producer–consumer connection queue and a fixed
worker pool so the accept thread never processes HTTP itself.

It implements manual HTTP parsing, an application router, secure static file
serving, MIME detection, thread-safe verbose logging, runtime statistics, and
graceful signal-driven shutdown — without an HTTP framework or parsing library.

**Status: Stage 5 of a staged build-out.** See
[`docs/roadmap.md`](docs/roadmap.md) for what is implemented versus planned.

## Features

Implemented so far:

- Command-line configuration (`--port`, `--workers`, `--queue-size`, `--root`,
  `--verbose`, `--help`)
- TCP listening socket with `SO_REUSEADDR` and signal-driven shutdown
- Bounded connection queue + fixed pthread worker pool (backpressure when full)
- Thread-safe logging and runtime statistics
- Buffered multi-`recv()` HTTP request framing and manual HTTP/1.0–1.1 parsing
- Application router: `GET /api/health`, `POST /api/echo`, `GET /api/stats`
- Secure static file serving with MIME detection and binary-safe responses
- Path traversal protection (literal and percent-encoded) + symlink escape checks
- Custom `404.html`, correct HEAD metadata without sending a body
- Unit tests (queue, router, stats) and curl/nc integration checks

Planned:

- Keep-alive, chunked encoding, TLS, benchmarks
- Broader reliability / sanitizer audit (Stage 6)

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
make test
make integration
```

## Memory Safety

`make debug test` runs unit tests under AddressSanitizer and
UndefinedBehaviorSanitizer. Ownership: [`docs/memory_model.md`](docs/memory_model.md).

## Limitations

- No keep-alive, chunked encoding, TLS, directory listings, or `sendfile()`
- Static files buffered in memory up to 16 MiB
- No dynamic thread-pool resizing
- Query strings are not parsed into key/value maps

## Future Work

Next up is **Stage 6 — Reliability** (shutdown/audit hardening). See
[`docs/roadmap.md`](docs/roadmap.md).

## License

[MIT](LICENSE)
