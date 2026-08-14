# Memory Model

Ownership rules through Stage 4. Every allocation and socket has a clear owner
and a matching release path.

## Queue lifecycle

```text
init → active → shutdown → drained → destroyed
```

- `connection_queue_init()` allocates the ring and initializes mutex/condvars.
- `connection_queue_shutdown()` only sets a flag and wakes waiters; it does
  **not** destroy synchronization objects.
- Workers may still pop queued descriptors after shutdown until the queue is
  empty.
- `connection_queue_destroy()` runs only after all workers are joined.

## Thread-pool lifecycle

```text
allocate thread handles
→ create workers
→ workers run
→ queue shutdown
→ workers drain
→ workers exit
→ join
→ free thread handles
```

Partial `pthread_create` failure: shut down the queue, join already-created
workers, free handles, report failure. Startup either leaves a valid pool or
fails cleanly.

Destruction order (required):

```text
shutdown queue → join workers → destroy pool handles → destroy queue
```

Never destroy the queue mutex/condvars while a worker might still wait on them.

## Socket / file-descriptor ownership

| Stage | Owner | Close responsibility |
|-------|-------|----------------------|
| Before `accept()` | — | No client fd exists |
| After successful `accept()` | Accept / server thread | Must close if not successfully queued |
| After successful `connection_queue_push()` | Queue / worker subsystem | Accept thread must **not** close |
| After `connection_queue_pop()` | That worker | Passes to `client_handle()` |
| Inside `client_handle()` | Client handler | Closes exactly once on all paths |
| Push fails (shutdown / abort) | Accept thread | Close exactly once; ownership never transferred |

No descriptor should be leaked, processed twice, or closed twice.

Listening socket remains owned by `main()` for the whole process lifetime.

## Raw receive buffer

- Allocated by `http_read_request()` via `malloc`/`realloc`.
- On success, ownership is transferred to the caller through `*buffer`.
- `client_handle()` frees this buffer after parsing (including error paths).

## `http_request_t`

Owns: `target`, `version`, header array, each header name/value, and `body`.

`http_request_destroy()` releases all of the above.

Pointers from `http_request_get_header()` are owned by the request and become
invalid after destroy.

## `http_response_t`

Owns: header array / name / value strings, and `body` when `body_owned` is set.

Does **not** own: `reason_phrase` (static string from `http_reason_phrase()`).

`http_response_destroy()` frees owned storage.

## Serialized response bytes

- `http_response_serialize()` allocates a wire buffer (trailing NUL for
  convenience; reported length excludes it).
- `http_response_send()` frees that buffer after `send_all()`.

## Static-file path lifetime

All of the following are local to `static_files_serve()` (or helpers) and are
freed before the function returns — including on error:

| Buffer | Created by | Freed by |
|--------|------------|----------|
| Path-only target (query stripped) | `static_files_extract_path` | `static_files_serve` cleanup |
| URL-decoded path | `static_files_url_decode` | `static_files_serve` cleanup |
| Lexically normalized relative path | `static_files_normalize_path` | `static_files_serve` cleanup |
| Canonical resolved filesystem path | `resolve_under_root` | `static_files_serve` cleanup |

Stack buffers (`PATH_MAX` arrays) hold temporary `realpath` / join results and
need no free.

## Static file body ownership

- On successful GET, `read_entire_file()` allocates the body buffer.
- Ownership is transferred into `http_response_t` via
  `http_response_set_body_owned()` (`body_owned = 1`).
- `http_response_destroy()` releases the body afterward.
- On HEAD (`load_body = 0`), no file body is allocated; `body_length` is set
  from `stat` metadata only.

## Custom 404 page

`static_files_build_not_found()` may call `static_files_serve()` for
`/404.html`. On success, that response (including owned body) is moved into
the caller's `http_response_t` and retagged as status 404. On failure, a
generated HTML error body is allocated instead.

## Logger

`logger` uses a process-wide mutex around each complete log line. It must not
be used from signal handlers. Workers may call it concurrently; lines do not
interleave.

## Configuration

- `server_config_t` is a stack object in `main()` with no dynamic allocations.
  `document_root` is a fixed-size array inside the struct.
- After startup, config is treated as read-only and shared by all workers
  without locking.

## Known limitation (TOCTOU)

Path validation uses `realpath`/`stat` then later `fopen`. A race between
those calls is possible on a shared filesystem. This educational server does
not claim production-grade sandboxing; descriptor-based hardening can be
explored later without changing the Stage 4 API shape.
