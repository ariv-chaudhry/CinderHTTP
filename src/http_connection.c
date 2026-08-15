#include "http_connection.h"

#include <stdlib.h>
#include <string.h>

static int ascii_tolower_char(unsigned char c) {
    if (c >= 'A' && c <= 'Z') {
        return (int)(c - 'A' + 'a');
    }
    return (int)c;
}

static int token_equal_ci(const char *a, size_t a_len, const char *b) {
    size_t b_len = strlen(b);
    if (a_len != b_len) {
        return 0;
    }
    for (size_t i = 0; i < a_len; i++) {
        if (ascii_tolower_char((unsigned char)a[i]) != ascii_tolower_char((unsigned char)b[i])) {
            return 0;
        }
    }
    return 1;
}

int http_header_value_has_token(const char *value, const char *token) {
    if (value == NULL || token == NULL || token[0] == '\0') {
        return 0;
    }

    const char *p = value;
    while (*p != '\0') {
        while (*p == ' ' || *p == '\t') {
            p++;
        }
        if (*p == '\0') {
            break;
        }

        const char *start = p;
        while (*p != '\0' && *p != ',') {
            p++;
        }
        const char *end = p;
        while (end > start && (end[-1] == ' ' || end[-1] == '\t')) {
            end--;
        }

        if (token_equal_ci(start, (size_t)(end - start), token)) {
            return 1;
        }

        if (*p == ',') {
            p++;
        }
    }
    return 0;
}

int http_request_wants_keep_alive(const http_request_t *request) {
    if (request == NULL || request->version == NULL) {
        return 0;
    }

    const char *connection = http_request_get_header(request, "Connection");
    int has_close = http_header_value_has_token(connection, "close");
    int has_keep_alive = http_header_value_has_token(connection, "keep-alive");

    /* Conflicting tokens → close. */
    if (has_close) {
        return 0;
    }

    if (strcmp(request->version, "HTTP/1.1") == 0) {
        return 1; /* default persistent; close already handled */
    }
    if (strcmp(request->version, "HTTP/1.0") == 0) {
        return has_keep_alive ? 1 : 0;
    }
    return 0;
}

int http_response_apply_connection_policy(http_response_t *response, int keep_alive,
                                          const char *http_version) {
    if (response == NULL) {
        return -1;
    }

    if (!keep_alive) {
        response->suppress_auto_connection_close = 0;
        return http_response_set_header(response, "Connection", "close");
    }

    if (http_version != NULL && strcmp(http_version, "HTTP/1.0") == 0) {
        response->suppress_auto_connection_close = 0;
        return http_response_set_header(response, "Connection", "keep-alive");
    }

    /* HTTP/1.1 persistent: omit Connection header (do not auto-insert close). */
    (void)http_response_remove_header(response, "Connection");
    response->suppress_auto_connection_close = 1;
    return 0;
}
