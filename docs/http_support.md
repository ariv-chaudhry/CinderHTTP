# HTTP Support

This document describes what CinderHTTP implements today (Stage 3) and what is
intentionally out of scope.

## Supported

- HTTP/1.0 and HTTP/1.1 request parsing
- Methods: `GET`, `HEAD`, `POST` (POST still uses Stage 2 temporary text
  handling — not routed to the filesystem)
- Origin-form request targets (including query strings), parsed and preserved
- Header parsing with case-insensitive lookup
- Request bodies via `Content-Length`
- Binary-safe bodies (length-based)
- One HTTP request per TCP connection (`Connection: close`)
- Static file `GET` / `HEAD` from a configurable document root (`--root`)
- MIME type detection from file extension
- Custom `404.html` when present under the document root
- Query-string stripping for filesystem lookup (`/index.html?x=1` → `index.html`)
- Correct `HEAD` behavior (same `Content-Length` as GET, zero body bytes)

### Status codes in use

| Code | Meaning |
|------|---------|
| 200 | Successful static file or temporary POST response |
| 400 | Malformed HTTP or malformed percent-encoding / path |
| 403 | Path traversal, symlink escape, or directory without index |
| 404 | Safe path that does not resolve to a regular file |
| 405 | Unsupported method (e.g. `DELETE`) |
| 413 | Oversized request body or oversized static file |
| 501 | `Transfer-Encoding` (including chunked) |
| 505 | Unsupported HTTP version |
| 500 | Unexpected I/O / allocation failures |

## Security behavior (static files)

- One-pass URL decoding (`%2e` → `.`); no recursive decode loops
- Lexical path normalization rejecting `..` that would escape the URL root
- `realpath()` of document root and resolved target, with **directory-boundary**
  prefix checks (so `/public` does not match `/publicity`)
- Symlinks that resolve outside the document root are denied (403)
- Embedded decoded NUL (`%00`) is rejected (400)
- Malformed percent escapes (`%2`, `%GG`) are rejected (400)
- Absolute filesystem paths are never returned in HTTP error bodies

## Request / static limits

| Limit | Value |
|-------|-------|
| `HTTP_MAX_REQUEST_LINE` | 8192 |
| `HTTP_MAX_HEADER_BYTES` | 32768 |
| `HTTP_MAX_HEADERS` | 100 |
| `HTTP_MAX_BODY_SIZE` | 1 MiB |
| `HTTP_MAX_STATIC_FILE_SIZE` | 16 MiB |

## Not supported

- Multithreading / connection queue / worker pool
- General application router and `/api/*` endpoints
- Keep-alive / pipelining
- Chunked transfer encoding
- TLS / HTTPS
- HTTP/2 / HTTP/3
- Directory listings
- Range requests / ETag / compression
- `sendfile()` zero-copy

## Architecture note

```text
HTTP parser          (syntax only — no filesystem)
       │
       ▼
Request handler
       │
       ├── POST → temporary text response
       │
       └── GET/HEAD → static resolver
              ├── URL decode (one pass)
              ├── lexical normalize
              ├── realpath + root confinement
              ├── MIME detection
              └── binary-safe load (GET) / metadata only (HEAD)
                     │
                     ▼
              http_response_t → send_all()
```
