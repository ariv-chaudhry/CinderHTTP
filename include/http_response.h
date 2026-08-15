/*
 * http_response.h - structured HTTP response and wire serialization.
 *
 * Body kinds:
 *   HTTP_BODY_NONE    — empty body (Content-Length 0)
 *   HTTP_BODY_MEMORY  — heap buffer; freed by destroy when body_owned
 *   HTTP_BODY_FILE    — open file descriptor; closed by destroy
 *
 * After http_response_set_file_body() succeeds, the response owns `fd` and
 * http_response_destroy() closes it. On failure, the caller retains ownership.
 *
 * Reason phrase pointers from http_reason_phrase() are static literals.
 */
#ifndef CINDERHTTP_HTTP_RESPONSE_H
#define CINDERHTTP_HTTP_RESPONSE_H

#include <stddef.h>
#include <sys/types.h>

#include "http_request.h"

typedef enum {
    HTTP_BODY_NONE = 0,
    HTTP_BODY_MEMORY,
    HTTP_BODY_FILE
} http_body_kind_t;

typedef struct {
    int status_code;
    const char *reason_phrase;

    http_header_t *headers;
    size_t header_count;

    http_body_kind_t body_kind;
    unsigned char *body; /* MEMORY only */
    size_t body_length;  /* Content-Length / expected transfer size */
    int body_owned;      /* MEMORY: destroy frees body */
    int file_fd;         /* FILE: owned fd, or -1 */

    /*
     * When non-zero, serialize will not auto-insert Connection: close if the
     * response has no Connection header (used for HTTP/1.1 keep-alive).
     */
    int suppress_auto_connection_close;
} http_response_t;

void http_response_init(http_response_t *response);
void http_response_destroy(http_response_t *response);

const char *http_reason_phrase(int status_code);

void http_response_set_status(http_response_t *response, int status_code);

/* Copies name and value into owned storage. Returns 0 on success, -1 on OOM. */
int http_response_add_header(http_response_t *response, const char *name, const char *value);

/*
 * Case-insensitive replace-or-add for a header name. Avoids duplicates.
 * Returns 0 on success, -1 on error.
 */
int http_response_set_header(http_response_t *response, const char *name, const char *value);

/* Removes all headers matching name (case-insensitive). Returns count removed. */
int http_response_remove_header(http_response_t *response, const char *name);

/* Takes ownership of `body` (must be malloc'd). body may be NULL if length 0. */
int http_response_set_body_owned(http_response_t *response, unsigned char *body, size_t length);

/* Copies text into a newly owned body buffer. Returns 0 on success, -1 on OOM. */
int http_response_set_body_text(http_response_t *response, const char *text);

/*
 * Binary-safe copy of `length` bytes into a newly owned body buffer.
 * length == 0 clears the body (body may be NULL). Rejects body == NULL with
 * length > 0. Preserves embedded zero bytes. Returns 0 on success, -1 on error.
 */
int http_response_set_body_copy(http_response_t *response, const unsigned char *body,
                                size_t length);

/*
 * Attach an open regular-file descriptor as the response body.
 * On success, ownership of fd transfers to the response (destroy closes it).
 * On failure, ownership remains with the caller. size becomes Content-Length.
 */
int http_response_set_file_body(http_response_t *response, int fd, off_t size);

/*
 * Builds a simple text/plain response with status, optional body, and the
 * standard Server / Connection / Content-Type / Content-Length headers.
 * Returns 0 on success, -1 on OOM.
 */
int http_response_build_text(http_response_t *response, int status_code, const char *body_text);

/*
 * Builds an application/json response. `json` is copied as the body (may be
 * empty). Content-Length is derived from the body; Server/Connection are added
 * at serialize time. Returns 0 on success, -1 on OOM.
 */
int http_response_build_json(http_response_t *response, int status_code, const char *json);

/*
 * Serialize response headers only (status line + headers + final CRLF).
 * Content-Length reflects body_length. Never includes body bytes.
 * Caller owns *out_buffer on success.
 */
int http_response_serialize_headers(const http_response_t *response, unsigned char **out_buffer,
                                    size_t *out_length);

/*
 * Serializes headers, and for MEMORY bodies optionally appends body bytes.
 * FILE-backed responses never embed file bytes in the buffer (headers only).
 * If omit_body is non-zero (HEAD), Content-Length still reflects body_length
 * but no body bytes are appended.
 *
 * On success returns 0 and transfers ownership of *out_buffer to the caller.
 */
int http_response_serialize(const http_response_t *response, int omit_body,
                            unsigned char **out_buffer, size_t *out_length);

/*
 * Send headers, then MEMORY body via send_all, or FILE body via sendfile()
 * (Linux) / bounded read+send fallback. HEAD (omit_body) sends headers only.
 * Returns 0 on success, -1 on failure.
 */
int http_response_send(int client_fd, const http_response_t *response, int omit_body);

#endif /* CINDERHTTP_HTTP_RESPONSE_H */
