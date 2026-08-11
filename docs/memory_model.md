# Memory Model

Ownership rules for Stage 2. Every allocation has a clear owner and a matching
release path.

## Raw receive buffer

- Allocated by `http_read_request()` via `malloc`/`realloc`.
- On success, ownership is transferred to the caller through `*buffer`.
- The per-connection handler in `server.c` frees this buffer after parsing
  (including on error paths).

## `http_request_t`

Owns:

- `target`
- `version`
- header array (`headers`)
- each header `name` and `value`
- `body` (optional; may contain embedded NUL bytes)

`http_request_destroy()` releases all of the above and re-initializes the
struct to an empty state.

Pointers returned by `http_request_get_header()` are owned by the request and
become invalid after `http_request_destroy()`.

On parse failure, `http_parse_request()` destroys any partial allocations
before returning, so the caller's `http_request_t` is always destroy-safe.

## `http_response_t`

Owns:

- header array and each header name/value
- `body` when `body_owned` is set (the usual case for text responses)

Does **not** own:

- `reason_phrase` (points at a static string from `http_reason_phrase()`)

`http_response_destroy()` frees owned headers/body and resets the struct.

## Serialized response bytes

- `http_response_serialize()` allocates a wire buffer (with a trailing NUL for
  convenience; reported length excludes the NUL).
- `http_response_send()` frees that buffer after `send_all()`.
- Callers of `serialize` directly must `free()` the buffer themselves.

## Client sockets

- Accepted in `server_run()`, owned by `handle_client()` for the duration of
  the request.
- Always closed at the end of `handle_client()`, including error/early-exit
  paths.
- The listening socket remains owned by `main()` and is closed after
  `server_run()` returns.

## Configuration

- `server_config_t` is a stack object in `main()` with no dynamic allocations.

## Not yet present

Queue storage, worker thread arrays, static-file buffers, and logger/stats
state will document their ownership when those modules are introduced.
