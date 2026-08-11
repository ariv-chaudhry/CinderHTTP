#include "http_parser.h"

#include <errno.h>
#include <limits.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

static int is_token_char(unsigned char c) {
    /* RFC 7230 tchar subset used for method tokens: ALPHA / DIGIT and a
     * few punctuation marks. Rejecting everything else keeps "G@T" etc.
     * classified as malformed (400) rather than "unsupported" (405). */
    if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9')) {
        return 1;
    }
    switch (c) {
        case '!':
        case '#':
        case '$':
        case '%':
        case '&':
        case '\'':
        case '*':
        case '+':
        case '-':
        case '.':
        case '^':
        case '_':
        case '`':
        case '|':
        case '~':
            return 1;
        default:
            return 0;
    }
}

static int is_supported_method_token(const char *method) {
    return strcmp(method, "GET") == 0 || strcmp(method, "HEAD") == 0 ||
           strcmp(method, "POST") == 0;
}

static int is_known_unsupported_method(const char *method) {
    static const char *const known[] = {"PUT",     "DELETE", "PATCH", "OPTIONS",
                                        "TRACE",   "CONNECT", "PRI",  NULL};
    for (size_t i = 0; known[i] != NULL; i++) {
        if (strcmp(method, known[i]) == 0) {
            return 1;
        }
    }
    /* Any other well-formed method token we simply do not implement. */
    return 1;
}

static int method_token_is_well_formed(const char *method) {
    if (method == NULL || method[0] == '\0') {
        return 0;
    }
    for (const char *p = method; *p != '\0'; p++) {
        if (!is_token_char((unsigned char)*p)) {
            return 0;
        }
    }
    return 1;
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

static char *dup_range(const char *start, size_t length) {
    char *copy = malloc(length + 1);
    if (copy == NULL) {
        return NULL;
    }
    memcpy(copy, start, length);
    copy[length] = '\0';
    return copy;
}

static const char *skip_ows(const char *start, const char *end) {
    while (start < end && (*start == ' ' || *start == '\t')) {
        start++;
    }
    return start;
}

static const char *trim_ows_end(const char *start, const char *end) {
    while (end > start && (end[-1] == ' ' || end[-1] == '\t')) {
        end--;
    }
    return end;
}

size_t http_find_header_terminator(const unsigned char *data, size_t length) {
    if (data == NULL || length < 4) {
        return (size_t)-1;
    }
    for (size_t i = 0; i + 3 < length; i++) {
        if (data[i] == '\r' && data[i + 1] == '\n' && data[i + 2] == '\r' &&
            data[i + 3] == '\n') {
            return i;
        }
    }
    return (size_t)-1;
}

http_parse_result_t http_parse_content_length_value(const char *value, size_t *out_length) {
    if (value == NULL || out_length == NULL || value[0] == '\0') {
        return HTTP_PARSE_INVALID_CONTENT_LENGTH;
    }

    /* Reject signs and anything that is not a pure digit sequence. */
    for (const char *p = value; *p != '\0'; p++) {
        if (*p < '0' || *p > '9') {
            return HTTP_PARSE_INVALID_CONTENT_LENGTH;
        }
    }

    errno = 0;
    char *end = NULL;
    unsigned long long parsed = strtoull(value, &end, 10);
    if (end == value || *end != '\0' || errno == ERANGE) {
        return HTTP_PARSE_INVALID_CONTENT_LENGTH;
    }

    if (parsed > (unsigned long long)HTTP_MAX_BODY_SIZE) {
        return HTTP_PARSE_TOO_LARGE;
    }

#if SIZE_MAX < ULLONG_MAX
    if (parsed > (unsigned long long)SIZE_MAX) {
        return HTTP_PARSE_INVALID_CONTENT_LENGTH;
    }
#endif

    *out_length = (size_t)parsed;
    return HTTP_PARSE_OK;
}

/*
 * Walk raw header lines (after the request line) looking for Content-Length
 * and Transfer-Encoding. Duplicate Content-Length values with differing
 * numbers are rejected; identical duplicates are also rejected for
 * simplicity and predictability (documented in docs/http_support.md).
 */
http_parse_result_t http_inspect_message_framing(const unsigned char *data, size_t header_length,
                                                 size_t *out_body_length) {
    if (data == NULL || out_body_length == NULL || header_length < 4) {
        return HTTP_PARSE_BAD_REQUEST;
    }
    if (!(data[header_length - 4] == '\r' && data[header_length - 3] == '\n' &&
          data[header_length - 2] == '\r' && data[header_length - 1] == '\n')) {
        return HTTP_PARSE_BAD_REQUEST;
    }

    *out_body_length = 0;

    const char *cursor = (const char *)data;
    const char *end = (const char *)data + header_length;

    /* Skip request line. */
    const char *line_end = memchr(cursor, '\n', (size_t)(end - cursor));
    if (line_end == NULL || line_end == cursor || line_end[-1] != '\r') {
        return HTTP_PARSE_BAD_REQUEST;
    }
    cursor = line_end + 1;

    int saw_content_length = 0;
    size_t content_length = 0;
    int saw_chunked = 0;

    while (cursor < end) {
        if (cursor + 1 < end && cursor[0] == '\r' && cursor[1] == '\n') {
            break; /* final empty line */
        }

        const char *hdr_end = memchr(cursor, '\n', (size_t)(end - cursor));
        if (hdr_end == NULL || hdr_end == cursor || hdr_end[-1] != '\r') {
            return HTTP_PARSE_BAD_REQUEST;
        }

        const char *line = cursor;
        size_t line_len = (size_t)((hdr_end - 1) - cursor); /* exclude CR */

        const char *colon = memchr(line, ':', line_len);
        if (colon == NULL || colon == line) {
            return HTTP_PARSE_BAD_REQUEST;
        }

        char *name = dup_range(line, (size_t)(colon - line));
        if (name == NULL) {
            return HTTP_PARSE_OUT_OF_MEMORY;
        }

        const char *value_start = skip_ows(colon + 1, line + line_len);
        const char *value_end = trim_ows_end(value_start, line + line_len);
        char *value = dup_range(value_start, (size_t)(value_end - value_start));
        if (value == NULL) {
            free(name);
            return HTTP_PARSE_OUT_OF_MEMORY;
        }

        if (header_name_equal(name, "Transfer-Encoding")) {
            /* Any Transfer-Encoding is unsupported in Stage 2; chunked is
             * the common case we must not silently ignore. */
            if (header_name_equal(value, "chunked") || strstr(value, "chunked") != NULL) {
                saw_chunked = 1;
            } else {
                free(name);
                free(value);
                return HTTP_PARSE_UNSUPPORTED_TRANSFER_ENCODING;
            }
        } else if (header_name_equal(name, "Content-Length")) {
            size_t parsed_len = 0;
            http_parse_result_t cl_result = http_parse_content_length_value(value, &parsed_len);
            if (cl_result != HTTP_PARSE_OK) {
                free(name);
                free(value);
                return cl_result;
            }
            if (saw_content_length) {
                /* Reject all duplicates, even identical ones. */
                free(name);
                free(value);
                return HTTP_PARSE_INVALID_CONTENT_LENGTH;
            }
            saw_content_length = 1;
            content_length = parsed_len;
        }

        free(name);
        free(value);
        cursor = hdr_end + 1;
    }

    if (saw_chunked) {
        return HTTP_PARSE_UNSUPPORTED_TRANSFER_ENCODING;
    }

    if (saw_content_length) {
        *out_body_length = content_length;
    }
    return HTTP_PARSE_OK;
}

static http_parse_result_t parse_request_line(const char *line, size_t line_len,
                                              http_request_t *request) {
    if (line_len == 0 || line_len > HTTP_MAX_REQUEST_LINE) {
        return line_len > HTTP_MAX_REQUEST_LINE ? HTTP_PARSE_TOO_LARGE : HTTP_PARSE_BAD_REQUEST;
    }

    /* METHOD SP TARGET SP VERSION — exactly two spaces separating three
     * non-empty fields. No leading/trailing spaces. */
    const char *first_sp = memchr(line, ' ', line_len);
    if (first_sp == NULL || first_sp == line) {
        return HTTP_PARSE_BAD_REQUEST;
    }

    size_t method_len = (size_t)(first_sp - line);
    char *method = dup_range(line, method_len);
    if (method == NULL) {
        return HTTP_PARSE_OUT_OF_MEMORY;
    }

    if (!method_token_is_well_formed(method)) {
        free(method);
        return HTTP_PARSE_BAD_REQUEST;
    }

    const char *after_method = first_sp + 1;
    size_t remaining = line_len - method_len - 1;
    if (remaining == 0 || *after_method == ' ') {
        free(method);
        return HTTP_PARSE_BAD_REQUEST;
    }

    const char *second_sp = memchr(after_method, ' ', remaining);
    if (second_sp == NULL || second_sp == after_method) {
        free(method);
        return HTTP_PARSE_BAD_REQUEST;
    }

    size_t target_len = (size_t)(second_sp - after_method);
    const char *version_start = second_sp + 1;
    size_t version_len = line_len - method_len - 1 - target_len - 1;
    if (version_len == 0 || memchr(version_start, ' ', version_len) != NULL) {
        free(method);
        return HTTP_PARSE_BAD_REQUEST;
    }

    if (!is_supported_method_token(method)) {
        http_parse_result_t result =
            is_known_unsupported_method(method) ? HTTP_PARSE_UNSUPPORTED_METHOD
                                                : HTTP_PARSE_BAD_REQUEST;
        free(method);
        return result;
    }

    request->method = http_method_from_string(method);
    free(method);

    request->target = dup_range(after_method, target_len);
    request->version = dup_range(version_start, version_len);
    if (request->target == NULL || request->version == NULL) {
        return HTTP_PARSE_OUT_OF_MEMORY;
    }

    if (strcmp(request->version, "HTTP/1.0") == 0 || strcmp(request->version, "HTTP/1.1") == 0) {
        return HTTP_PARSE_OK;
    }

    /* Syntactically HTTP-like but unsupported (HTTP/2.0, HTTP/3, ...). */
    if (strncmp(request->version, "HTTP/", 5) == 0 && request->version[5] != '\0') {
        int all_rest_ok = 1;
        for (const char *p = request->version + 5; *p != '\0'; p++) {
            if (!((*p >= '0' && *p <= '9') || *p == '.')) {
                all_rest_ok = 0;
                break;
            }
        }
        if (all_rest_ok) {
            return HTTP_PARSE_UNSUPPORTED_VERSION;
        }
    }

    return HTTP_PARSE_BAD_REQUEST;
}

static http_parse_result_t append_header(http_request_t *request, char *name, char *value) {
    if (request->header_count >= HTTP_MAX_HEADERS) {
        free(name);
        free(value);
        return HTTP_PARSE_TOO_MANY_HEADERS;
    }

    http_header_t *grown =
        realloc(request->headers, (request->header_count + 1) * sizeof(http_header_t));
    if (grown == NULL) {
        free(name);
        free(value);
        return HTTP_PARSE_OUT_OF_MEMORY;
    }

    request->headers = grown;
    request->headers[request->header_count].name = name;
    request->headers[request->header_count].value = value;
    request->header_count++;
    return HTTP_PARSE_OK;
}

static http_parse_result_t parse_header_line(const char *line, size_t line_len,
                                             http_request_t *request) {
    if (line_len == 0) {
        return HTTP_PARSE_BAD_REQUEST;
    }

    const char *colon = memchr(line, ':', line_len);
    if (colon == NULL || colon == line) {
        return HTTP_PARSE_BAD_REQUEST;
    }

    /* Header names must be non-empty and must not contain spaces. */
    size_t name_len = (size_t)(colon - line);
    for (size_t i = 0; i < name_len; i++) {
        if (line[i] == ' ' || line[i] == '\t') {
            return HTTP_PARSE_BAD_REQUEST;
        }
    }

    char *name = dup_range(line, name_len);
    if (name == NULL) {
        return HTTP_PARSE_OUT_OF_MEMORY;
    }

    const char *value_start = skip_ows(colon + 1, line + line_len);
    const char *value_end = trim_ows_end(value_start, line + line_len);
    char *value = dup_range(value_start, (size_t)(value_end - value_start));
    if (value == NULL) {
        free(name);
        return HTTP_PARSE_OUT_OF_MEMORY;
    }

    return append_header(request, name, value);
}

http_parse_result_t http_parse_request(const unsigned char *data, size_t data_length,
                                       http_request_t *request) {
    if (request == NULL) {
        return HTTP_PARSE_BAD_REQUEST;
    }
    http_request_init(request);

    if (data == NULL || data_length == 0) {
        return HTTP_PARSE_BAD_REQUEST;
    }

    size_t term = http_find_header_terminator(data, data_length);
    if (term == (size_t)-1) {
        return HTTP_PARSE_BAD_REQUEST;
    }

    size_t header_bytes = term + 4; /* include \r\n\r\n */
    if (header_bytes > HTTP_MAX_HEADER_BYTES) {
        return HTTP_PARSE_TOO_LARGE;
    }

    size_t expected_body = 0;
    http_parse_result_t frame = http_inspect_message_framing(data, header_bytes, &expected_body);
    if (frame != HTTP_PARSE_OK) {
        return frame;
    }

    if (data_length < header_bytes + expected_body) {
        return HTTP_PARSE_BAD_REQUEST; /* incomplete message handed to parser */
    }
    if (data_length > header_bytes + expected_body) {
        /* Extra trailing bytes after a complete message are not accepted in
         * the one-request-per-connection model (could be pipelining). */
        return HTTP_PARSE_BAD_REQUEST;
    }

    const char *cursor = (const char *)data;
    /*
     * `term` indexes the first '\r' of the blank-line terminator "\r\n\r\n".
     * That first CRLF is also the line ending of the previous line (the last
     * header, or the request line when there are no headers). Including those
     * two bytes lets the line-splitting loop below see a terminating '\n' for
     * the final header.
     */
    const char *headers_end = (const char *)data + term + 2;

    const char *line_end = memchr(cursor, '\n', (size_t)(headers_end - cursor));
    if (line_end == NULL || line_end == cursor || line_end[-1] != '\r') {
        http_request_destroy(request);
        return HTTP_PARSE_BAD_REQUEST;
    }

    size_t req_line_len = (size_t)((line_end - 1) - cursor);
    http_parse_result_t rl = parse_request_line(cursor, req_line_len, request);
    if (rl != HTTP_PARSE_OK) {
        http_request_destroy(request);
        return rl;
    }

    cursor = line_end + 1;
    while (cursor < headers_end) {
        const char *hdr_nl = memchr(cursor, '\n', (size_t)(headers_end - cursor));
        if (hdr_nl == NULL || hdr_nl == cursor || hdr_nl[-1] != '\r') {
            http_request_destroy(request);
            return HTTP_PARSE_BAD_REQUEST;
        }

        size_t line_len = (size_t)((hdr_nl - 1) - cursor);
        http_parse_result_t hr = parse_header_line(cursor, line_len, request);
        if (hr != HTTP_PARSE_OK) {
            http_request_destroy(request);
            return hr;
        }
        cursor = hdr_nl + 1;
    }

    /* Re-validate Content-Length against stored headers for consistency with
     * framing, and reject duplicates the structured model also saw. */
    const char *cl = http_request_get_header(request, "Content-Length");
    const char *te = http_request_get_header(request, "Transfer-Encoding");
    if (te != NULL) {
        http_request_destroy(request);
        return HTTP_PARSE_UNSUPPORTED_TRANSFER_ENCODING;
    }

    size_t body_length = 0;
    if (cl != NULL) {
        /* Count Content-Length occurrences for duplicate detection. */
        size_t cl_count = 0;
        for (size_t i = 0; i < request->header_count; i++) {
            if (header_name_equal(request->headers[i].name, "Content-Length")) {
                cl_count++;
            }
        }
        if (cl_count > 1) {
            http_request_destroy(request);
            return HTTP_PARSE_INVALID_CONTENT_LENGTH;
        }

        http_parse_result_t cl_result = http_parse_content_length_value(cl, &body_length);
        if (cl_result != HTTP_PARSE_OK) {
            http_request_destroy(request);
            return cl_result;
        }
    }

    if (body_length != expected_body) {
        http_request_destroy(request);
        return HTTP_PARSE_BAD_REQUEST;
    }

    if (body_length > 0) {
        /* Allocate body_length + 1 so callers can treat the buffer as a C
         * string when the body is textual, but always trust body_length. */
        request->body = malloc(body_length + 1);
        if (request->body == NULL) {
            http_request_destroy(request);
            return HTTP_PARSE_OUT_OF_MEMORY;
        }
        memcpy(request->body, data + header_bytes, body_length);
        request->body[body_length] = '\0';
        request->body_length = body_length;
    }

    return HTTP_PARSE_OK;
}
