/*
 * http_limits.h - centralized bounds for HTTP request intake.
 *
 * These limits exist so a client cannot force unbounded memory allocation by
 * sending an arbitrarily large request line, header block, or body. Every
 * module that grows a receive buffer or allocates parser storage should
 * consult these constants rather than inventing its own magic numbers.
 */
#ifndef CINDERHTTP_HTTP_LIMITS_H
#define CINDERHTTP_HTTP_LIMITS_H

/* Maximum length of the request-line alone (METHOD SP TARGET SP VERSION). */
#define HTTP_MAX_REQUEST_LINE 8192

/* Maximum size of the header section including the request line and the
 * terminating CRLF CRLF. Protects against header-bombing. */
#define HTTP_MAX_HEADER_BYTES 32768

/* Maximum number of individual header fields after the request line. */
#define HTTP_MAX_HEADERS 100

/* Maximum request body size (1 MiB). Bodies larger than this yield 413. */
#define HTTP_MAX_BODY_SIZE (1024 * 1024)

/* Absolute ceiling for a single reconstructed message: headers + body. */
#define HTTP_MAX_MESSAGE_BYTES (HTTP_MAX_HEADER_BYTES + HTTP_MAX_BODY_SIZE)

/*
 * Hard cap on sequential requests on one TCP connection. After this many
 * responses the server sends Connection: close and releases the worker.
 */
#define HTTP_MAX_REQUESTS_PER_CONNECTION 100

/*
 * Upper bound on a single static file served by one worker. Files are streamed
 * (Linux sendfile / bounded-buffer fallback), so this is no longer a heap-size
 * guard — it limits how long one connection can monopolize a worker.
 * Oversized static files yield 413 Payload Too Large.
 */
#define HTTP_MAX_STATIC_FILE_SIZE (64 * 1024 * 1024)

#endif /* CINDERHTTP_HTTP_LIMITS_H */
