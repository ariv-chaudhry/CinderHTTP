# HTTP Support

This document describes what CinderHTTP implements today (Stage 9) and what is
intentionally out of scope.

## Supported

- HTTP/1.0 and HTTP/1.1 request parsing
- Methods: `GET`, `HEAD`, `POST`
- Origin-form request targets (including query strings), parsed and preserved
- Header parsing with case-insensitive lookup
- Request bodies via `Content-Length`
- Binary-safe bodies (length-based); embedded NUL is allowed **in bodies only**
- Protocol metadata (method, target, version, header names/values) rejects
  embedded NUL bytes
- Duplicate `Content-Length` headers are rejected (identical or conflicting)
- HTTP/1.1 persistent connections by default
- HTTP/1.1 `Connection: close`
- HTTP/1.0 default-close semantics
- HTTP/1.0 explicit `Connection: keep-alive`
- Bounded keep-alive / read timeout (`--keep-alive-timeout`, default 5s)
- Maximum requests per connection (`HTTP_MAX_REQUESTS_PER_CONNECTION`, 100)
- Sequential handling of buffered requests (TCP may deliver multiple messages
  in one `recv()`; they are processed one at a time in order)
- Multithreaded accept → bounded queue → fixed worker pool
- Application router for the reserved `/api/` namespace
- Static file `GET` / `HEAD` from a configurable document root (`--root`)
- MIME type detection from file extension
- Custom `404.html` when present under the document root
- Query-string stripping for filesystem lookup (`/index.html?x=1` → `index.html`)
- Correct `HEAD` behavior (same `Content-Length` as GET, zero body bytes)

### Persistence policy

| Version | Default | Explicit override |
|---------|---------|-------------------|
| HTTP/1.1 | keep-alive | `Connection: close` |
| HTTP/1.0 | close | `Connection: keep-alive` |

Token matching is case-insensitive and comma-list aware (`foo, close`).
If both `keep-alive` and `close` appear, **close** wins.

### Response `Connection` header policy

| Situation | Response header |
|-----------|-----------------|
| HTTP/1.1 persistent | omit `Connection` (optional keep-alive not sent) |
| HTTP/1.1 closing | `Connection: close` |
| HTTP/1.0 persistent | `Connection: keep-alive` |
| HTTP/1.0 closing | `Connection: close` |
| Protocol errors (400/408/413/501/505, …) | `Connection: close` |

Every response includes a correct `Content-Length` so keep-alive clients can
frame the next message without relying on socket closure.

### Application routes

| Method | Path | Behavior |
|--------|------|----------|
| `GET` / `HEAD` | `/api/health` | `{"status":"ok"}` (`application/json`) |
| `POST` | `/api/echo` | Echo request body bytes (binary-safe) |
| `GET` / `HEAD` | `/api/stats` | JSON runtime counters |

Routing notes:

- Query strings do not affect matching (`/api/health?x=1` → health).
- Exact paths only (`/api/health/` is **not** `/api/health`).
- `/api/` never falls through to static files.
- Wrong method on a known API path → **405** with route-specific `Allow`.
- Unknown `/api/*` → JSON **404** (`{"error":"not found"}`).
- Non-API `POST` (e.g. `POST /`) → **405** (no temporary success body).

### `/api/echo` details

- Body length uses `request->body_length` (never `strlen`).
- Copies request `Content-Type` when present; otherwise
  `application/octet-stream`.
- Empty body (`Content-Length: 0`) returns `200` with `Content-Length: 0`.

### `/api/stats` counters

| Field | Meaning |
|-------|---------|
| `connections_accepted` | Successful `accept()` count |
| `active_connections` | Enqueued/in-flight **connections** (started after successful queue push; finished when `client_handle` ends — not per request) |
| `requests_total` | Incremented after a complete framed message is read (`HTTP_READ_OK`), before/despite parse errors |
| `responses_2xx` / `4xx` / `5xx` | Incremented once per successfully sent HTTP response |

One TCP connection carrying five requests increments `connections_accepted` by
1 and `requests_total` by 5.

`/api/stats` itself increments `requests_total` when read; its own `2xx` count
is updated only after that response is sent (so the returned snapshot may not
include its own response class yet).

### Status codes in use

| Code | Meaning |
|------|---------|
| 200 | Successful static file or API response |
| 400 | Malformed HTTP or malformed percent-encoding / path |
| 403 | Path traversal, symlink escape, or directory without index |
| 404 | Static miss or unknown `/api/*` route |
| 405 | Unsupported method (static or API) |
| 408 | Partial request timed out before completion |
| 413 | Oversized request body or oversized static file |
| 501 | `Transfer-Encoding` (including chunked) |
| 505 | Unsupported HTTP version |
| 500 | Unexpected I/O / allocation failures |

### Connection close vs continue

**Close** after: client EOF; HTTP close policy; max requests; idle/read timeout;
socket/framing/parser errors; 400 / 408 / 413 / 501 / 505 (and similar framing
uncertainty); failed send.

**May remain open** after valid framing with application outcomes such as
200 / 403 / 404 / 405 (when the request itself was syntactically complete).

Idle keep-alive timeout (no bytes for the next request) closes **silently**
(no 408). Partial request + timeout → **408** + `Connection: close`.

## Security behavior (static files)

- One-pass URL decoding (`%2e` → `.`); no recursive decode loops
- Lexical path normalization rejecting `..` that would escape the URL root
- `realpath()` of document root and resolved target, with **directory-boundary**
  prefix checks (so `/public` does not match `/publicity`)
- Symlinks that resolve outside the document root are denied (403)
- Embedded decoded NUL (`%00`) is rejected (400)
- Malformed percent escapes (`%2`, `%GG`) are rejected (400)
- Absolute filesystem paths are never returned in HTTP error bodies

## Request / connection limits

| Limit | Value |
|-------|-------|
| `HTTP_MAX_REQUEST_LINE` | 8192 |
| `HTTP_MAX_HEADER_BYTES` | 32768 |
| `HTTP_MAX_HEADERS` | 100 |
| `HTTP_MAX_BODY_SIZE` | 1 MiB |
| `HTTP_MAX_MESSAGE_BYTES` | headers + body (per request) |
| `HTTP_MAX_STATIC_FILE_SIZE` | 16 MiB |
| `HTTP_MAX_REQUESTS_PER_CONNECTION` | 100 |
| `--keep-alive-timeout` | default 5s (range 1–300) |

Per-request size limits apply to each framed message, not the lifetime sum of
a persistent connection.

## Not supported

- Concurrent HTTP pipelining (buffered requests are handled sequentially)
- Chunked transfer encoding
- TLS / HTTPS
- HTTP/2 / HTTP/3
- Directory listings
- Range requests / ETag / compression
- `sendfile()` zero-copy
- Query-string key/value parsing beyond path extraction
- Absolute whole-request deadline (timeout is per blocking `recv` via
  `SO_RCVTIMEO`, not a total Slowloris deadline)

## Architecture note

```text
HTTP reader/framer   (connection buffer; one framed message at a time)
       │
       ▼
HTTP parser          (syntax only — no filesystem)
       │
       ▼
client_handle (may loop while keep-alive)
       │
       ├── /api/* → router (health / echo / stats)
       │
       └── other → GET/HEAD static resolver
              ├── URL decode (one pass)
              ├── lexical normalize
              ├── realpath + root confinement
              ├── MIME detection
              └── binary-safe load (GET) / metadata only (HEAD)
                     │
                     ▼
              http_response_t → send_all()
                     │
                     └── close or next request
```

### Worker pinning tradeoff

A persistent connection remains assigned to the same worker until it closes.
Idle keep-alive clients can occupy workers until `--keep-alive-timeout` fires.
The bounded queue stores **accepted connections**, not individual keep-alive
requests. Event-driven multiplexing is intentionally out of scope for Stage 9.
