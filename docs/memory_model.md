# Memory Model

Ownership rules through Stage 3. Every allocation has a clear owner and a
matching release path.

## Raw receive buffer

- Allocated by `http_read_request()` via `malloc`/`realloc`.
- On success, ownership is transferred to the caller through `*buffer`.
- `handle_client()` in `server.c` frees this buffer after parsing (including
  error paths).

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

## Client sockets

- Accepted in `server_run()`, owned by `handle_client()` for the request.
- Always closed at the end of `handle_client()`.
- Listening socket remains owned by `main()`.

## Configuration

- `server_config_t` is a stack object in `main()` with no dynamic allocations.
  `document_root` is a fixed-size array inside the struct.

## Known limitation (TOCTOU)

Path validation uses `realpath`/`stat` then later `fopen`. A race between
those calls is possible on a shared filesystem. This educational server does
not claim production-grade sandboxing; descriptor-based hardening can be
explored later without changing the Stage 3 API shape.
