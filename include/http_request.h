/*
 * http_request.h - structured representation of a parsed HTTP request.
 *
 * Ownership: after a successful parse, http_request_t owns every dynamically
 * allocated field (target, version, each header name/value, the header array,
 * and the body). Callers must release that memory with http_request_destroy().
 * Pointers returned by http_request_get_header() are owned by the request and
 * remain valid only until destroy.
 */
#ifndef CINDERHTTP_HTTP_REQUEST_H
#define CINDERHTTP_HTTP_REQUEST_H

#include <stddef.h>

typedef enum {
    HTTP_METHOD_GET = 0,
    HTTP_METHOD_HEAD,
    HTTP_METHOD_POST,
    HTTP_METHOD_UNKNOWN
} http_method_t;

typedef struct {
    char *name;
    char *value;
} http_header_t;

typedef struct {
    http_method_t method;

    char *target;
    char *version;

    http_header_t *headers;
    size_t header_count;

    /* Body is arbitrary bytes of length body_length. A trailing NUL may be
     * present for convenience, but consumers must use body_length, not
     * strlen(), because bodies can contain embedded zero bytes. */
    unsigned char *body;
    size_t body_length;
} http_request_t;

void http_request_init(http_request_t *request);
void http_request_destroy(http_request_t *request);

const char *http_method_to_string(http_method_t method);
http_method_t http_method_from_string(const char *method);

/* Case-insensitive header lookup. Returns NULL if the header is absent.
 * If multiple headers share the same name, returns the first match. */
const char *http_request_get_header(const http_request_t *request, const char *name);

#endif /* CINDERHTTP_HTTP_REQUEST_H */
