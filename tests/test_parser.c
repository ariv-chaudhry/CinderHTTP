/*
 * test_parser.c - unit tests for HTTP request parsing and framing helpers.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "http_parser.h"
#include "http_request.h"

static int g_failures = 0;
static int g_passed = 0;

#define ASSERT_TRUE(expr)                                                                          \
    do {                                                                                           \
        if (!(expr)) {                                                                             \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #expr);                        \
            g_failures++;                                                                          \
        } else {                                                                                   \
            g_passed++;                                                                            \
        }                                                                                          \
    } while (0)

#define ASSERT_EQ(a, b)                                                                            \
    do {                                                                                           \
        long long _a = (long long)(a);                                                             \
        long long _b = (long long)(b);                                                             \
        if (_a != _b) {                                                                            \
            fprintf(stderr, "FAIL %s:%d: %s (%lld) != %s (%lld)\n", __FILE__, __LINE__, #a, _a,   \
                    #b, _b);                                                                       \
            g_failures++;                                                                          \
        } else {                                                                                   \
            g_passed++;                                                                            \
        }                                                                                          \
    } while (0)

#define ASSERT_STR_EQ(a, b)                                                                        \
    do {                                                                                           \
        const char *_a = (a);                                                                      \
        const char *_b = (b);                                                                      \
        if (_a == NULL || _b == NULL || strcmp(_a, _b) != 0) {                                     \
            fprintf(stderr, "FAIL %s:%d: \"%s\" != \"%s\"\n", __FILE__, __LINE__,                  \
                    _a ? _a : "(null)", _b ? _b : "(null)");                                       \
            g_failures++;                                                                          \
        } else {                                                                                   \
            g_passed++;                                                                            \
        }                                                                                          \
    } while (0)

static http_parse_result_t parse_cstr(const char *msg, http_request_t *req) {
    return http_parse_request((const unsigned char *)msg, strlen(msg), req);
}

static void test_valid_get(void) {
    http_request_t req;
    http_request_init(&req);
    const char *msg = "GET /index.html HTTP/1.1\r\n"
                      "Host: localhost:8080\r\n"
                      "User-Agent: curl/8.0\r\n"
                      "\r\n";
    ASSERT_EQ(parse_cstr(msg, &req), HTTP_PARSE_OK);
    ASSERT_EQ(req.method, HTTP_METHOD_GET);
    ASSERT_STR_EQ(req.target, "/index.html");
    ASSERT_STR_EQ(req.version, "HTTP/1.1");
    ASSERT_EQ(req.header_count, 2);
    ASSERT_EQ(req.body_length, 0);
    ASSERT_STR_EQ(http_request_get_header(&req, "Host"), "localhost:8080");
    http_request_destroy(&req);
}

static void test_valid_head(void) {
    http_request_t req;
    http_request_init(&req);
    const char *msg = "HEAD / HTTP/1.1\r\nHost: localhost\r\n\r\n";
    ASSERT_EQ(parse_cstr(msg, &req), HTTP_PARSE_OK);
    ASSERT_EQ(req.method, HTTP_METHOD_HEAD);
    ASSERT_STR_EQ(req.target, "/");
    http_request_destroy(&req);
}

static void test_valid_post_body(void) {
    http_request_t req;
    http_request_init(&req);
    const char *msg = "POST /echo HTTP/1.1\r\n"
                      "Host: localhost\r\n"
                      "Content-Length: 5\r\n"
                      "\r\n"
                      "hello";
    ASSERT_EQ(parse_cstr(msg, &req), HTTP_PARSE_OK);
    ASSERT_EQ(req.method, HTTP_METHOD_POST);
    ASSERT_EQ(req.body_length, 5);
    ASSERT_TRUE(req.body != NULL && memcmp(req.body, "hello", 5) == 0);
    http_request_destroy(&req);
}

static void test_http10(void) {
    http_request_t req;
    http_request_init(&req);
    const char *msg = "GET / HTTP/1.0\r\n\r\n";
    ASSERT_EQ(parse_cstr(msg, &req), HTTP_PARSE_OK);
    ASSERT_STR_EQ(req.version, "HTTP/1.0");
    http_request_destroy(&req);
}

static void test_query_string(void) {
    http_request_t req;
    http_request_init(&req);
    const char *msg = "GET /search?q=test&x=1 HTTP/1.1\r\n\r\n";
    ASSERT_EQ(parse_cstr(msg, &req), HTTP_PARSE_OK);
    ASSERT_STR_EQ(req.target, "/search?q=test&x=1");
    http_request_destroy(&req);
}

static void test_header_case_insensitive(void) {
    http_request_t req;
    http_request_init(&req);
    const char *msg = "GET / HTTP/1.1\r\n"
                      "Content-Type: text/plain\r\n"
                      "\r\n";
    ASSERT_EQ(parse_cstr(msg, &req), HTTP_PARSE_OK);
    ASSERT_STR_EQ(http_request_get_header(&req, "content-type"), "text/plain");
    ASSERT_STR_EQ(http_request_get_header(&req, "CONTENT-TYPE"), "text/plain");
    ASSERT_STR_EQ(http_request_get_header(&req, "Content-Type"), "text/plain");
    http_request_destroy(&req);
}

static void test_content_length_case(void) {
    http_request_t req;
    http_request_init(&req);
    const char *msg = "POST / HTTP/1.1\r\n"
                      "CONTENT-LENGTH: 3\r\n"
                      "\r\n"
                      "abc";
    ASSERT_EQ(parse_cstr(msg, &req), HTTP_PARSE_OK);
    ASSERT_EQ(req.body_length, 3);
    http_request_destroy(&req);
}

static void test_empty_body_zero_length(void) {
    http_request_t req;
    http_request_init(&req);
    const char *msg = "POST / HTTP/1.1\r\nContent-Length: 0\r\n\r\n";
    ASSERT_EQ(parse_cstr(msg, &req), HTTP_PARSE_OK);
    ASSERT_EQ(req.body_length, 0);
    ASSERT_TRUE(req.body == NULL);
    http_request_destroy(&req);
}

static void test_body_with_zero_byte(void) {
    http_request_t req;
    http_request_init(&req);
    unsigned char msg[] = "POST / HTTP/1.1\r\nContent-Length: 3\r\n\r\n"
                          "a\0b";
    /* strlen would stop at \0 in the body, so use explicit length:
     * headers end at index of body start. */
    size_t header_len = sizeof("POST / HTTP/1.1\r\nContent-Length: 3\r\n\r\n") - 1;
    size_t total = header_len + 3;
    ASSERT_EQ(http_parse_request(msg, total, &req), HTTP_PARSE_OK);
    ASSERT_EQ(req.body_length, 3);
    ASSERT_TRUE(req.body[0] == 'a' && req.body[1] == '\0' && req.body[2] == 'b');
    http_request_destroy(&req);
}

static void test_header_value_trimming(void) {
    http_request_t req;
    http_request_init(&req);
    const char *msg = "GET / HTTP/1.1\r\nHost:   localhost  \r\n\r\n";
    ASSERT_EQ(parse_cstr(msg, &req), HTTP_PARSE_OK);
    ASSERT_STR_EQ(http_request_get_header(&req, "Host"), "localhost");
    http_request_destroy(&req);
}

static void test_missing_fields(void) {
    http_request_t req;
    http_request_init(&req);
    ASSERT_EQ(parse_cstr("GET HTTP/1.1\r\n\r\n", &req), HTTP_PARSE_BAD_REQUEST);
    http_request_destroy(&req);

    ASSERT_EQ(parse_cstr("GET / \r\n\r\n", &req), HTTP_PARSE_BAD_REQUEST);
    http_request_destroy(&req);

    ASSERT_EQ(parse_cstr("/ HTTP/1.1\r\n\r\n", &req), HTTP_PARSE_BAD_REQUEST);
    http_request_destroy(&req);
}

static void test_malformed_spacing(void) {
    http_request_t req;
    http_request_init(&req);
    ASSERT_EQ(parse_cstr("GET  / HTTP/1.1\r\n\r\n", &req), HTTP_PARSE_BAD_REQUEST);
    http_request_destroy(&req);
    ASSERT_EQ(parse_cstr("GET /  HTTP/1.1\r\n\r\n", &req), HTTP_PARSE_BAD_REQUEST);
    http_request_destroy(&req);
}

static void test_unsupported_method(void) {
    http_request_t req;
    http_request_init(&req);
    ASSERT_EQ(parse_cstr("DELETE / HTTP/1.1\r\n\r\n", &req), HTTP_PARSE_UNSUPPORTED_METHOD);
    http_request_destroy(&req);
    ASSERT_EQ(parse_cstr("PUT / HTTP/1.1\r\n\r\n", &req), HTTP_PARSE_UNSUPPORTED_METHOD);
    http_request_destroy(&req);
}

static void test_malformed_method(void) {
    http_request_t req;
    http_request_init(&req);
    ASSERT_EQ(parse_cstr("G@T / HTTP/1.1\r\n\r\n", &req), HTTP_PARSE_BAD_REQUEST);
    http_request_destroy(&req);
}

static void test_unsupported_version(void) {
    http_request_t req;
    http_request_init(&req);
    ASSERT_EQ(parse_cstr("GET / HTTP/2.0\r\n\r\n", &req), HTTP_PARSE_UNSUPPORTED_VERSION);
    http_request_destroy(&req);
    ASSERT_EQ(parse_cstr("GET / HTTP/3\r\n\r\n", &req), HTTP_PARSE_UNSUPPORTED_VERSION);
    http_request_destroy(&req);
}

static void test_malformed_version(void) {
    http_request_t req;
    http_request_init(&req);
    ASSERT_EQ(parse_cstr("GET / HTP/1.1\r\n\r\n", &req), HTTP_PARSE_BAD_REQUEST);
    http_request_destroy(&req);
    ASSERT_EQ(parse_cstr("GET / HTTP/1.1x\r\n\r\n", &req), HTTP_PARSE_BAD_REQUEST);
    http_request_destroy(&req);
}

static void test_header_without_colon(void) {
    http_request_t req;
    http_request_init(&req);
    ASSERT_EQ(parse_cstr("GET / HTTP/1.1\r\nHost localhost\r\n\r\n", &req),
              HTTP_PARSE_BAD_REQUEST);
    http_request_destroy(&req);
}

static void test_empty_header_name(void) {
    http_request_t req;
    http_request_init(&req);
    ASSERT_EQ(parse_cstr("GET / HTTP/1.1\r\n: value\r\n\r\n", &req), HTTP_PARSE_BAD_REQUEST);
    http_request_destroy(&req);
}

static void test_too_many_headers(void) {
    http_request_t req;
    http_request_init(&req);

    size_t capacity = 64 * 1024;
    char *msg = malloc(capacity);
    ASSERT_TRUE(msg != NULL);
    if (msg == NULL) {
        return;
    }

    size_t offset = 0;
    offset += (size_t)snprintf(msg + offset, capacity - offset, "GET / HTTP/1.1\r\n");
    for (int i = 0; i < HTTP_MAX_HEADERS + 1; i++) {
        offset +=
            (size_t)snprintf(msg + offset, capacity - offset, "X-H-%d: v\r\n", i);
    }
    offset += (size_t)snprintf(msg + offset, capacity - offset, "\r\n");

    ASSERT_EQ(http_parse_request((unsigned char *)msg, offset, &req), HTTP_PARSE_TOO_MANY_HEADERS);
    http_request_destroy(&req);
    free(msg);
}

static void test_content_length_errors(void) {
    http_request_t req;
    http_request_init(&req);

    ASSERT_EQ(parse_cstr("POST / HTTP/1.1\r\nContent-Length: abc\r\n\r\n", &req),
              HTTP_PARSE_INVALID_CONTENT_LENGTH);
    http_request_destroy(&req);

    ASSERT_EQ(parse_cstr("POST / HTTP/1.1\r\nContent-Length: -1\r\n\r\n", &req),
              HTTP_PARSE_INVALID_CONTENT_LENGTH);
    http_request_destroy(&req);

    ASSERT_EQ(parse_cstr("POST / HTTP/1.1\r\nContent-Length: +5\r\n\r\n", &req),
              HTTP_PARSE_INVALID_CONTENT_LENGTH);
    http_request_destroy(&req);

    ASSERT_EQ(parse_cstr("POST / HTTP/1.1\r\nContent-Length: 5x\r\n\r\n", &req),
              HTTP_PARSE_INVALID_CONTENT_LENGTH);
    http_request_destroy(&req);

    ASSERT_EQ(parse_cstr("POST / HTTP/1.1\r\nContent-Length: 5hello\r\n\r\n", &req),
              HTTP_PARSE_INVALID_CONTENT_LENGTH);
    http_request_destroy(&req);

    ASSERT_EQ(parse_cstr("POST / HTTP/1.1\r\nContent-Length:\r\n\r\n", &req),
              HTTP_PARSE_INVALID_CONTENT_LENGTH);
    http_request_destroy(&req);

    ASSERT_EQ(parse_cstr("POST / HTTP/1.1\r\nContent-Length: \r\n\r\n", &req),
              HTTP_PARSE_INVALID_CONTENT_LENGTH);
    http_request_destroy(&req);

    ASSERT_EQ(parse_cstr("POST / HTTP/1.1\r\n"
                         "Content-Length: 99999999999999999999999999999\r\n\r\n",
                         &req),
              HTTP_PARSE_INVALID_CONTENT_LENGTH);
    http_request_destroy(&req);
}

static void test_content_length_leading_zeros(void) {
    http_request_t req;
    http_request_init(&req);
    ASSERT_EQ(parse_cstr("POST / HTTP/1.1\r\nContent-Length: 0005\r\n\r\nhello", &req),
              HTTP_PARSE_OK);
    ASSERT_EQ(req.body_length, 5);
    ASSERT_TRUE(memcmp(req.body, "hello", 5) == 0);
    http_request_destroy(&req);
}

static void test_content_length_and_transfer_encoding(void) {
    http_request_t req;
    http_request_init(&req);
    const char *msg = "POST / HTTP/1.1\r\n"
                      "Content-Length: 5\r\n"
                      "Transfer-Encoding: chunked\r\n"
                      "\r\n"
                      "hello";
    ASSERT_EQ(parse_cstr(msg, &req), HTTP_PARSE_UNSUPPORTED_TRANSFER_ENCODING);
    http_request_destroy(&req);
}

static void test_nul_in_metadata(void) {
    http_request_t req;
    http_request_init(&req);

    /* Embedded NUL in method. */
    unsigned char bad_method[] = {'G', 'E', '\0', 'T', ' ', '/', ' ', 'H', 'T', 'T', 'P',
                                  '/', '1', '.', '1', '\r', '\n', '\r', '\n'};
    ASSERT_EQ(http_parse_request(bad_method, sizeof(bad_method), &req), HTTP_PARSE_BAD_REQUEST);
    http_request_destroy(&req);

    /* Embedded NUL in target. */
    unsigned char bad_target[] = {'G', 'E', 'T', ' ', '/', 'a', '\0', 'b', ' ', 'H', 'T',
                                  'T', 'P', '/', '1', '.', '1', '\r', '\n', '\r', '\n'};
    ASSERT_EQ(http_parse_request(bad_target, sizeof(bad_target), &req), HTTP_PARSE_BAD_REQUEST);
    http_request_destroy(&req);

    /* Embedded NUL in header name. */
    unsigned char bad_hname[] = {'G', 'E', 'T', ' ', '/', ' ', 'H', 'T', 'T', 'P', '/', '1',
                                 '.', '1', '\r', '\n', 'H', 'o', '\0', 's', 't', ':', ' ',
                                 'x', '\r', '\n', '\r', '\n'};
    ASSERT_EQ(http_parse_request(bad_hname, sizeof(bad_hname), &req), HTTP_PARSE_BAD_REQUEST);
    http_request_destroy(&req);

    /* Embedded NUL in header value. */
    unsigned char bad_hval[] = {'G', 'E', 'T', ' ', '/', ' ', 'H', 'T', 'T', 'P', '/', '1',
                                '.', '1', '\r', '\n', 'H', 'o', 's', 't', ':', ' ', 'x',
                                '\0', 'y', '\r', '\n', '\r', '\n'};
    ASSERT_EQ(http_parse_request(bad_hval, sizeof(bad_hval), &req), HTTP_PARSE_BAD_REQUEST);
    http_request_destroy(&req);
}

static void test_extra_body_bytes_rejected(void) {
    http_request_t req;
    http_request_init(&req);
    /* Content-Length: 5 but trailing EXTRA after the declared body. */
    const char *msg = "POST / HTTP/1.1\r\nContent-Length: 5\r\n\r\nhelloEXTRA";
    ASSERT_EQ(parse_cstr(msg, &req), HTTP_PARSE_BAD_REQUEST);
    http_request_destroy(&req);
}

static void test_body_size_boundaries(void) {
    http_request_t req;
    http_request_init(&req);

    char hdr[96];
    int n = snprintf(hdr, sizeof(hdr), "POST / HTTP/1.1\r\nContent-Length: %d\r\n\r\n",
                     HTTP_MAX_BODY_SIZE - 1);
    ASSERT_TRUE(n > 0);
    size_t hdr_len = (size_t)n;
    size_t body_len = (size_t)(HTTP_MAX_BODY_SIZE - 1);
    unsigned char *msg = malloc(hdr_len + body_len);
    ASSERT_TRUE(msg != NULL);
    if (msg == NULL) {
        return;
    }
    memcpy(msg, hdr, hdr_len);
    memset(msg + hdr_len, 'a', body_len);
    ASSERT_EQ(http_parse_request(msg, hdr_len + body_len, &req), HTTP_PARSE_OK);
    ASSERT_EQ(req.body_length, body_len);
    http_request_destroy(&req);
    free(msg);

    n = snprintf(hdr, sizeof(hdr), "POST / HTTP/1.1\r\nContent-Length: %d\r\n\r\n",
                 HTTP_MAX_BODY_SIZE);
    ASSERT_TRUE(n > 0);
    hdr_len = (size_t)n;
    body_len = (size_t)HTTP_MAX_BODY_SIZE;
    msg = malloc(hdr_len + body_len);
    ASSERT_TRUE(msg != NULL);
    if (msg == NULL) {
        return;
    }
    memcpy(msg, hdr, hdr_len);
    memset(msg + hdr_len, 'b', body_len);
    ASSERT_EQ(http_parse_request(msg, hdr_len + body_len, &req), HTTP_PARSE_OK);
    ASSERT_EQ(req.body_length, body_len);
    http_request_destroy(&req);
    free(msg);
}

static void test_colon_in_header_value(void) {
    http_request_t req;
    http_request_init(&req);
    ASSERT_EQ(parse_cstr("GET / HTTP/1.1\r\nAuthorization: abc:def:ghi\r\n\r\n", &req),
              HTTP_PARSE_OK);
    ASSERT_EQ(req.header_count, 1);
    ASSERT_STR_EQ(req.headers[0].value, "abc:def:ghi");
    http_request_destroy(&req);
}

static void test_body_above_limit(void) {
    http_request_t req;
    http_request_init(&req);
    char msg[128];
    snprintf(msg, sizeof(msg), "POST / HTTP/1.1\r\nContent-Length: %d\r\n\r\n",
             HTTP_MAX_BODY_SIZE + 1);
    ASSERT_EQ(parse_cstr(msg, &req), HTTP_PARSE_TOO_LARGE);
    http_request_destroy(&req);
}

static void test_duplicate_content_length(void) {
    http_request_t req;
    http_request_init(&req);
    const char *msg = "POST / HTTP/1.1\r\n"
                      "Content-Length: 5\r\n"
                      "Content-Length: 10\r\n"
                      "\r\n";
    ASSERT_EQ(parse_cstr(msg, &req), HTTP_PARSE_INVALID_CONTENT_LENGTH);
    http_request_destroy(&req);

    /* Identical duplicates are also rejected. */
    const char *msg2 = "POST / HTTP/1.1\r\n"
                       "Content-Length: 5\r\n"
                       "Content-Length: 5\r\n"
                       "\r\n"
                       "hello";
    ASSERT_EQ(parse_cstr(msg2, &req), HTTP_PARSE_INVALID_CONTENT_LENGTH);
    http_request_destroy(&req);
}

static void test_chunked_rejected(void) {
    http_request_t req;
    http_request_init(&req);
    const char *msg = "POST / HTTP/1.1\r\n"
                      "Transfer-Encoding: chunked\r\n"
                      "\r\n"
                      "5\r\nhello\r\n0\r\n\r\n";
    ASSERT_EQ(parse_cstr(msg, &req), HTTP_PARSE_UNSUPPORTED_TRANSFER_ENCODING);
    http_request_destroy(&req);
}

static void test_content_length_helper(void) {
    size_t out = 0;
    ASSERT_EQ(http_parse_content_length_value("0", &out), HTTP_PARSE_OK);
    ASSERT_EQ(out, 0);
    ASSERT_EQ(http_parse_content_length_value("42", &out), HTTP_PARSE_OK);
    ASSERT_EQ(out, 42);
    ASSERT_EQ(http_parse_content_length_value("", &out), HTTP_PARSE_INVALID_CONTENT_LENGTH);
    ASSERT_EQ(http_parse_content_length_value("01", &out), HTTP_PARSE_OK); /* leading zeros ok */
}

static void test_find_terminator(void) {
    const unsigned char *msg = (const unsigned char *)"GET / HTTP/1.1\r\nHost: x\r\n\r\nbody";
    size_t idx = http_find_header_terminator(msg, 30);
    ASSERT_TRUE(idx != (size_t)-1);
    ASSERT_EQ(msg[idx], '\r');
    ASSERT_EQ(http_find_header_terminator(msg, 10), (size_t)-1);
}

static void test_oversized_request_line(void) {
    http_request_t req;
    http_request_init(&req);

    size_t target_len = HTTP_MAX_REQUEST_LINE;
    char *msg = malloc(target_len + 64);
    ASSERT_TRUE(msg != NULL);
    if (msg == NULL) {
        return;
    }

    memcpy(msg, "GET ", 4);
    memset(msg + 4, 'a', target_len);
    memcpy(msg + 4 + target_len, " HTTP/1.1\r\n\r\n", 13);
    size_t total = 4 + target_len + 13;
    ASSERT_EQ(http_parse_request((unsigned char *)msg, total, &req), HTTP_PARSE_TOO_LARGE);
    http_request_destroy(&req);
    free(msg);
}

int main(void) {
    test_valid_get();
    test_valid_head();
    test_valid_post_body();
    test_http10();
    test_query_string();
    test_header_case_insensitive();
    test_content_length_case();
    test_empty_body_zero_length();
    test_body_with_zero_byte();
    test_header_value_trimming();
    test_missing_fields();
    test_malformed_spacing();
    test_unsupported_method();
    test_malformed_method();
    test_unsupported_version();
    test_malformed_version();
    test_header_without_colon();
    test_empty_header_name();
    test_too_many_headers();
    test_content_length_errors();
    test_content_length_leading_zeros();
    test_content_length_and_transfer_encoding();
    test_nul_in_metadata();
    test_extra_body_bytes_rejected();
    test_body_size_boundaries();
    test_colon_in_header_value();
    test_body_above_limit();
    test_duplicate_content_length();
    test_chunked_rejected();
    test_content_length_helper();
    test_find_terminator();
    test_oversized_request_line();

    printf("test_parser: %d assertions passed, %d failed\n", g_passed, g_failures);
    return g_failures == 0 ? 0 : 1;
}
