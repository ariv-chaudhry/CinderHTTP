#include "http_response.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "utils.h"

void http_response_init(http_response_t *response) {
    if (response == NULL) {
        return;
    }
    response->status_code = 500;
    response->reason_phrase = http_reason_phrase(500);
    response->headers = NULL;
    response->header_count = 0;
    response->body = NULL;
    response->body_length = 0;
    response->body_owned = 0;
    response->suppress_auto_connection_close = 0;
}

void http_response_destroy(http_response_t *response) {
    if (response == NULL) {
        return;
    }

    if (response->headers != NULL) {
        for (size_t i = 0; i < response->header_count; i++) {
            free(response->headers[i].name);
            free(response->headers[i].value);
        }
        free(response->headers);
    }

    if (response->body_owned) {
        free(response->body);
    }

    http_response_init(response);
}

const char *http_reason_phrase(int status_code) {
    switch (status_code) {
        case 200:
            return "OK";
        case 201:
            return "Created";
        case 204:
            return "No Content";
        case 400:
            return "Bad Request";
        case 403:
            return "Forbidden";
        case 404:
            return "Not Found";
        case 405:
            return "Method Not Allowed";
        case 408:
            return "Request Timeout";
        case 413:
            return "Payload Too Large";
        case 500:
            return "Internal Server Error";
        case 501:
            return "Not Implemented";
        case 505:
            return "HTTP Version Not Supported";
        default:
            return "Unknown";
    }
}

void http_response_set_status(http_response_t *response, int status_code) {
    if (response == NULL) {
        return;
    }
    response->status_code = status_code;
    response->reason_phrase = http_reason_phrase(status_code);
}

static int ascii_tolower(unsigned char c) {
    if (c >= 'A' && c <= 'Z') {
        return (int)(c - 'A' + 'a');
    }
    return (int)c;
}

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

static char *dup_string(const char *s) {
    size_t len = strlen(s);
    char *copy = malloc(len + 1);
    if (copy == NULL) {
        return NULL;
    }
    memcpy(copy, s, len + 1);
    return copy;
}

int http_response_add_header(http_response_t *response, const char *name, const char *value) {
    if (response == NULL || name == NULL || value == NULL) {
        return -1;
    }

    char *name_copy = dup_string(name);
    char *value_copy = dup_string(value);
    if (name_copy == NULL || value_copy == NULL) {
        free(name_copy);
        free(value_copy);
        return -1;
    }

    http_header_t *grown =
        realloc(response->headers, (response->header_count + 1) * sizeof(http_header_t));
    if (grown == NULL) {
        free(name_copy);
        free(value_copy);
        return -1;
    }

    response->headers = grown;
    response->headers[response->header_count].name = name_copy;
    response->headers[response->header_count].value = value_copy;
    response->header_count++;
    return 0;
}

int http_response_remove_header(http_response_t *response, const char *name) {
    if (response == NULL || name == NULL || response->headers == NULL) {
        return 0;
    }
    int removed = 0;
    size_t i = 0;
    while (i < response->header_count) {
        if (header_name_equal(response->headers[i].name, name)) {
            free(response->headers[i].name);
            free(response->headers[i].value);
            size_t last = response->header_count - 1;
            if (i != last) {
                response->headers[i] = response->headers[last];
            }
            response->header_count--;
            removed++;
            continue;
        }
        i++;
    }
    return removed;
}

int http_response_set_header(http_response_t *response, const char *name, const char *value) {
    if (response == NULL || name == NULL || value == NULL) {
        return -1;
    }
    (void)http_response_remove_header(response, name);
    return http_response_add_header(response, name, value);
}

int http_response_set_body_owned(http_response_t *response, unsigned char *body, size_t length) {
    if (response == NULL) {
        return -1;
    }
    if (response->body_owned) {
        free(response->body);
    }
    response->body = body;
    response->body_length = length;
    response->body_owned = (body != NULL) ? 1 : 0;
    return 0;
}

int http_response_set_body_text(http_response_t *response, const char *text) {
    if (response == NULL) {
        return -1;
    }
    if (text == NULL) {
        return http_response_set_body_owned(response, NULL, 0);
    }

    size_t len = strlen(text);
    unsigned char *copy = malloc(len + 1);
    if (copy == NULL) {
        return -1;
    }
    memcpy(copy, text, len);
    copy[len] = '\0';
    return http_response_set_body_owned(response, copy, len);
}

int http_response_set_body_copy(http_response_t *response, const unsigned char *body,
                                size_t length) {
    if (response == NULL) {
        return -1;
    }
    if (length == 0) {
        return http_response_set_body_owned(response, NULL, 0);
    }
    if (body == NULL) {
        return -1;
    }

    unsigned char *copy = malloc(length);
    if (copy == NULL) {
        return -1;
    }
    memcpy(copy, body, length);
    return http_response_set_body_owned(response, copy, length);
}

static int response_has_header(const http_response_t *response, const char *name) {
    for (size_t i = 0; i < response->header_count; i++) {
        if (header_name_equal(response->headers[i].name, name)) {
            return 1;
        }
    }
    return 0;
}

int http_response_build_text(http_response_t *response, int status_code, const char *body_text) {
    if (response == NULL) {
        return -1;
    }

    http_response_destroy(response);
    http_response_init(response);
    http_response_set_status(response, status_code);

    if (body_text != NULL) {
        if (http_response_set_body_text(response, body_text) != 0) {
            return -1;
        }
    }

    if (http_response_add_header(response, "Content-Type", "text/plain; charset=utf-8") != 0) {
        return -1;
    }
    return 0;
}

int http_response_build_json(http_response_t *response, int status_code, const char *json) {
    if (response == NULL) {
        return -1;
    }

    http_response_destroy(response);
    http_response_init(response);
    http_response_set_status(response, status_code);

    if (json != NULL) {
        if (http_response_set_body_text(response, json) != 0) {
            return -1;
        }
    }

    if (http_response_add_header(response, "Content-Type", "application/json") != 0) {
        return -1;
    }
    return 0;
}

int http_response_serialize(const http_response_t *response, int omit_body,
                            unsigned char **out_buffer, size_t *out_length) {
    if (response == NULL || out_buffer == NULL || out_length == NULL) {
        return -1;
    }
    *out_buffer = NULL;
    *out_length = 0;

    char status_line[128];
    int status_len =
        snprintf(status_line, sizeof(status_line), "HTTP/1.1 %d %s\r\n", response->status_code,
                 response->reason_phrase != NULL ? response->reason_phrase
                                                 : http_reason_phrase(response->status_code));
    if (status_len < 0 || (size_t)status_len >= sizeof(status_line)) {
        return -1;
    }

    char length_hdr[64];
    int length_hdr_len =
        snprintf(length_hdr, sizeof(length_hdr), "Content-Length: %zu\r\n", response->body_length);
    if (length_hdr_len < 0 || (size_t)length_hdr_len >= sizeof(length_hdr)) {
        return -1;
    }

    const char *server_hdr = "Server: CinderHTTP/1.0\r\n";
    const char *connection_hdr = "Connection: close\r\n";

    int need_server = !response_has_header(response, "Server");
    int need_connection = !response_has_header(response, "Connection") &&
                          !response->suppress_auto_connection_close;
    int need_content_length = !response_has_header(response, "Content-Length");

    size_t total = (size_t)status_len;
    for (size_t i = 0; i < response->header_count; i++) {
        total += strlen(response->headers[i].name) + 2; /* ": " */
        total += strlen(response->headers[i].value) + 2; /* "\r\n" */
    }
    if (need_server) {
        total += strlen(server_hdr);
    }
    if (need_connection) {
        total += strlen(connection_hdr);
    }
    if (need_content_length) {
        total += (size_t)length_hdr_len;
    }
    total += 2; /* final \r\n */
    if (!omit_body) {
        total += response->body_length;
    }

    unsigned char *buffer = malloc(total + 1);
    if (buffer == NULL) {
        return -1;
    }

    size_t offset = 0;
    memcpy(buffer + offset, status_line, (size_t)status_len);
    offset += (size_t)status_len;

    for (size_t i = 0; i < response->header_count; i++) {
        size_t name_len = strlen(response->headers[i].name);
        size_t value_len = strlen(response->headers[i].value);
        memcpy(buffer + offset, response->headers[i].name, name_len);
        offset += name_len;
        buffer[offset++] = ':';
        buffer[offset++] = ' ';
        memcpy(buffer + offset, response->headers[i].value, value_len);
        offset += value_len;
        buffer[offset++] = '\r';
        buffer[offset++] = '\n';
    }

    if (need_server) {
        size_t n = strlen(server_hdr);
        memcpy(buffer + offset, server_hdr, n);
        offset += n;
    }
    if (need_connection) {
        size_t n = strlen(connection_hdr);
        memcpy(buffer + offset, connection_hdr, n);
        offset += n;
    }
    if (need_content_length) {
        memcpy(buffer + offset, length_hdr, (size_t)length_hdr_len);
        offset += (size_t)length_hdr_len;
    }

    buffer[offset++] = '\r';
    buffer[offset++] = '\n';

    if (!omit_body && response->body_length > 0 && response->body != NULL) {
        memcpy(buffer + offset, response->body, response->body_length);
        offset += response->body_length;
    }

    if (offset != total) {
        free(buffer);
        return -1;
    }

    /* Trailing NUL is for convenience (strstr in tests/debugging); wire length
     * excludes it. HTTP bodies are still length-based, not C-string-based. */
    buffer[total] = '\0';

    *out_buffer = buffer;
    *out_length = total;
    return 0;
}

int http_response_send(int client_fd, const http_response_t *response, int omit_body) {
    unsigned char *wire = NULL;
    size_t wire_len = 0;
    if (http_response_serialize(response, omit_body, &wire, &wire_len) != 0) {
        return -1;
    }

    ssize_t sent = send_all(client_fd, wire, wire_len);
    free(wire);
    return (sent < 0) ? -1 : 0;
}
