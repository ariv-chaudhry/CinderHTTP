/*
 * http_response.h - structured HTTP response and wire serialization.
 *
 * Ownership: http_response_t owns its header array and each header name/value,
 * plus the body buffer when body_owned is set. Reason phrase pointers returned
 * by http_reason_phrase() are static string literals and must not be freed.
 */
#ifndef CINDERHTTP_HTTP_RESPONSE_H
#define CINDERHTTP_HTTP_RESPONSE_H

#include <stddef.h>

#include "http_request.h"

typedef struct {
    int status_code;
    const char *reason_phrase;

    http_header_t *headers;
    size_t header_count;

    unsigned char *body;
    size_t body_length;
    int body_owned; /* 1 if destroy() should free(body) */
} http_response_t;

void http_response_init(http_response_t *response);
void http_response_destroy(http_response_t *response);

const char *http_reason_phrase(int status_code);

void http_response_set_status(http_response_t *response, int status_code);

/* Copies name and value into owned storage. Returns 0 on success, -1 on OOM. */
int http_response_add_header(http_response_t *response, const char *name, const char *value);

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
 * Serializes the response into a newly allocated buffer suitable for send().
 * Always uses CRLF line endings. If omit_body is non-zero (HEAD responses),
 * Content-Length still reflects body_length but the body bytes are omitted.
 *
 * On success returns 0 and transfers ownership of *out_buffer to the caller.
 * On failure returns -1 and leaves *out_buffer NULL.
 */
int http_response_serialize(const http_response_t *response, int omit_body,
                            unsigned char **out_buffer, size_t *out_length);

/* Serialize and send via send_all(). Returns 0 on success, -1 on failure. */
int http_response_send(int client_fd, const http_response_t *response, int omit_body);

#endif /* CINDERHTTP_HTTP_RESPONSE_H */
