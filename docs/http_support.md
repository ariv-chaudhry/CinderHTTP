# HTTP Support

This document describes what CinderHTTP implements today (Stage 2) and what is
intentionally out of scope for now.

## Supported

- HTTP/1.0 request parsing
- HTTP/1.1 request parsing
- Methods: `GET`, `HEAD`, `POST`
- Origin-form request targets (including query strings), parsed and preserved
- Header parsing with optional whitespace trimming around values
- Case-insensitive header name lookup
- Request bodies via `Content-Length`
- Binary-safe bodies (length-based; embedded NUL bytes are allowed)
- One HTTP request per TCP connection
- Responses always include `Connection: close` and close the socket afterward
- Status mapping for common errors:
  - `400 Bad Request` (malformed syntax)
  - `405 Method Not Allowed` (well-formed but unsupported method)
  - `413 Payload Too Large` (over limit)
  - `501 Not Implemented` (`Transfer-Encoding`, including `chunked`)
  - `505 HTTP Version Not Supported` (e.g. `HTTP/2.0`)
- Correct `HEAD` behavior: same headers/`Content-Length` as the corresponding
  GET-style response, but zero body bytes on the wire

## Request limits

Centralized in `include/http_limits.h`:

| Limit | Value | Purpose |
|-------|-------|---------|
| `HTTP_MAX_REQUEST_LINE` | 8192 | Cap request-line length |
| `HTTP_MAX_HEADER_BYTES` | 32768 | Cap header section size |
| `HTTP_MAX_HEADERS` | 100 | Cap header field count |
| `HTTP_MAX_BODY_SIZE` | 1 MiB | Cap request body size |

## Content-Length policy

- Values must be a non-empty digit sequence only (no sign, no trailing junk).
- Values larger than `HTTP_MAX_BODY_SIZE` yield `413`.
- **Duplicate `Content-Length` headers are always rejected**, even when the
  values are identical. This avoids ambiguous framing and keeps the reader and
  parser aligned.

## Transfer-Encoding policy

Chunked (and any other) `Transfer-Encoding` is **not** implemented. Presence of
the header yields `501 Not Implemented` rather than being ignored.

## Not supported (yet)

- Persistent connections / keep-alive request loops
- HTTP pipelining
- Chunked request bodies
- Multipart form parsing
- Static file serving / MIME types / path traversal checks
- Application routing (`/api/*`)
- Multithreading / worker pool / connection queue
- TLS / HTTPS
- HTTP/2 / HTTP/3
- Compression, WebSockets, range requests

## Architecture note

Network framing (`http_reader`) is separate from syntax parsing
(`http_parser`). The reader reconstructs one complete message across multiple
`recv()` calls; the parser then turns that buffer into an `http_request_t`.
Both share `http_parse_content_length_value()` /
`http_inspect_message_framing()` so Content-Length rules cannot drift apart.
