#include "http_request.h"

#include <stdlib.h>
#include <string.h>

void http_request_init(http_request_t *request) {
    if (request == NULL) {
        return;
    }
    request->method = HTTP_METHOD_UNKNOWN;
    request->target = NULL;
    request->version = NULL;
    request->headers = NULL;
    request->header_count = 0;
    request->body = NULL;
    request->body_length = 0;
}

void http_request_destroy(http_request_t *request) {
    if (request == NULL) {
        return;
    }

    free(request->target);
    free(request->version);

    if (request->headers != NULL) {
        for (size_t i = 0; i < request->header_count; i++) {
            free(request->headers[i].name);
            free(request->headers[i].value);
        }
        free(request->headers);
    }

    free(request->body);
    http_request_init(request);
}

const char *http_method_to_string(http_method_t method) {
    switch (method) {
        case HTTP_METHOD_GET:
            return "GET";
        case HTTP_METHOD_HEAD:
            return "HEAD";
        case HTTP_METHOD_POST:
            return "POST";
        case HTTP_METHOD_UNKNOWN:
        default:
            return "UNKNOWN";
    }
}

http_method_t http_method_from_string(const char *method) {
    if (method == NULL) {
        return HTTP_METHOD_UNKNOWN;
    }
    if (strcmp(method, "GET") == 0) {
        return HTTP_METHOD_GET;
    }
    if (strcmp(method, "HEAD") == 0) {
        return HTTP_METHOD_HEAD;
    }
    if (strcmp(method, "POST") == 0) {
        return HTTP_METHOD_POST;
    }
    return HTTP_METHOD_UNKNOWN;
}

static int ascii_tolower(unsigned char c) {
    if (c >= 'A' && c <= 'Z') {
        return (int)(c - 'A' + 'a');
    }
    return (int)c;
}

/* Case-insensitive ASCII compare; HTTP header names are case-insensitive. */
static int header_name_equal(const char *a, const char *b) {
    while (*a != '\0' && *b != '\0') {
        if (ascii_tolower((unsigned char)*a) != ascii_tolower((unsigned char)*b)) {
            return 0;
        }
        a++;
        b++;
    }
    return *a == '\0' && *b == '\0';
}

const char *http_request_get_header(const http_request_t *request, const char *name) {
    if (request == NULL || name == NULL) {
        return NULL;
    }

    for (size_t i = 0; i < request->header_count; i++) {
        if (header_name_equal(request->headers[i].name, name)) {
            return request->headers[i].value;
        }
    }
    return NULL;
}
