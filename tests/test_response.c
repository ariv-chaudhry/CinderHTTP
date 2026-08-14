/*
 * test_response.c - unit tests for HTTP response building and serialization.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "http_response.h"

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

static void test_reason_phrases(void) {
    ASSERT_STR_EQ(http_reason_phrase(200), "OK");
    ASSERT_STR_EQ(http_reason_phrase(204), "No Content");
    ASSERT_STR_EQ(http_reason_phrase(400), "Bad Request");
    ASSERT_STR_EQ(http_reason_phrase(403), "Forbidden");
    ASSERT_STR_EQ(http_reason_phrase(404), "Not Found");
    ASSERT_STR_EQ(http_reason_phrase(405), "Method Not Allowed");
    ASSERT_STR_EQ(http_reason_phrase(413), "Payload Too Large");
    ASSERT_STR_EQ(http_reason_phrase(500), "Internal Server Error");
    ASSERT_STR_EQ(http_reason_phrase(501), "Not Implemented");
    ASSERT_STR_EQ(http_reason_phrase(505), "HTTP Version Not Supported");
}

static void test_serialize_get_like(void) {
    http_response_t response;
    http_response_init(&response);
    ASSERT_EQ(http_response_build_text(&response, 200, "CinderHTTP works!\n"), 0);

    unsigned char *wire = NULL;
    size_t wire_len = 0;
    ASSERT_EQ(http_response_serialize(&response, 0, &wire, &wire_len), 0);
    ASSERT_TRUE(wire != NULL);

    /* Must use CRLF, not bare LF. */
    ASSERT_TRUE(strstr((char *)wire, "\r\n") != NULL);
    ASSERT_TRUE(strstr((char *)wire, "HTTP/1.1 200 OK\r\n") == (char *)wire);
    ASSERT_TRUE(strstr((char *)wire, "Content-Length: 18\r\n") != NULL);
    ASSERT_TRUE(strstr((char *)wire, "Server: CinderHTTP/1.0\r\n") != NULL);
    ASSERT_TRUE(strstr((char *)wire, "Connection: close\r\n") != NULL);
    ASSERT_TRUE(strstr((char *)wire, "Content-Type: text/plain; charset=utf-8\r\n") != NULL);

    const char *body = strstr((char *)wire, "\r\n\r\n");
    ASSERT_TRUE(body != NULL);
    body += 4;
    ASSERT_EQ(wire_len, (size_t)(body - (char *)wire) + 18);
    ASSERT_TRUE(memcmp(body, "CinderHTTP works!\n", 18) == 0);

    free(wire);
    http_response_destroy(&response);
}

static void test_head_omits_body(void) {
    http_response_t response;
    http_response_init(&response);
    ASSERT_EQ(http_response_build_text(&response, 200, "CinderHTTP works!\n"), 0);

    unsigned char *wire = NULL;
    size_t wire_len = 0;
    ASSERT_EQ(http_response_serialize(&response, 1 /* omit body */, &wire, &wire_len), 0);

    ASSERT_TRUE(strstr((char *)wire, "Content-Length: 18\r\n") != NULL);
    const char *sep = strstr((char *)wire, "\r\n\r\n");
    ASSERT_TRUE(sep != NULL);
    size_t header_bytes = (size_t)(sep + 4 - (char *)wire);
    ASSERT_EQ(wire_len, header_bytes); /* no body bytes after headers */

    free(wire);
    http_response_destroy(&response);
}

static void test_content_length_matches_body(void) {
    http_response_t response;
    http_response_init(&response);
    const char *text = "POST request parsed successfully.\n";
    ASSERT_EQ(http_response_build_text(&response, 200, text), 0);
    ASSERT_EQ(response.body_length, strlen(text));

    unsigned char *wire = NULL;
    size_t wire_len = 0;
    ASSERT_EQ(http_response_serialize(&response, 0, &wire, &wire_len), 0);

    char expected[64];
    snprintf(expected, sizeof(expected), "Content-Length: %zu\r\n", strlen(text));
    ASSERT_TRUE(strstr((char *)wire, expected) != NULL);

    free(wire);
    http_response_destroy(&response);
}

static void test_does_not_duplicate_manual_headers(void) {
    http_response_t response;
    http_response_init(&response);
    http_response_set_status(&response, 200);
    ASSERT_EQ(http_response_set_body_text(&response, "x"), 0);
    ASSERT_EQ(http_response_add_header(&response, "Server", "Custom/1.0"), 0);
    ASSERT_EQ(http_response_add_header(&response, "Connection", "close"), 0);
    ASSERT_EQ(http_response_add_header(&response, "Content-Length", "1"), 0);

    unsigned char *wire = NULL;
    size_t wire_len = 0;
    ASSERT_EQ(http_response_serialize(&response, 0, &wire, &wire_len), 0);

    /* Exactly one Server header. */
    int server_count = 0;
    const char *p = (const char *)wire;
    while ((p = strstr(p, "Server:")) != NULL) {
        server_count++;
        p += 7;
    }
    ASSERT_EQ(server_count, 1);
    ASSERT_TRUE(strstr((char *)wire, "Server: Custom/1.0\r\n") != NULL);

    free(wire);
    http_response_destroy(&response);
}

static void test_build_json_and_body_copy(void) {
    http_response_t response;
    http_response_init(&response);
    ASSERT_EQ(http_response_build_json(&response, 200, "{\"status\":\"ok\"}"), 0);
    ASSERT_EQ(response.status_code, 200);
    ASSERT_EQ(response.body_length, strlen("{\"status\":\"ok\"}"));
    ASSERT_TRUE(memcmp(response.body, "{\"status\":\"ok\"}", response.body_length) == 0);

    int found_ct = 0;
    for (size_t i = 0; i < response.header_count; i++) {
        if (strcmp(response.headers[i].name, "Content-Type") == 0) {
            ASSERT_STR_EQ(response.headers[i].value, "application/json");
            found_ct = 1;
        }
    }
    ASSERT_TRUE(found_ct);
    http_response_destroy(&response);

    unsigned char bytes[] = {0x41, 0x00, 0x42, 0xFF};
    http_response_init(&response);
    ASSERT_EQ(http_response_set_body_copy(&response, bytes, 4), 0);
    ASSERT_EQ(response.body_length, 4);
    ASSERT_EQ(response.body[0], 0x41);
    ASSERT_EQ(response.body[1], 0x00);
    ASSERT_EQ(response.body[2], 0x42);
    ASSERT_EQ(response.body[3], 0xFF);
    ASSERT_EQ(http_response_set_body_copy(&response, NULL, 0), 0);
    ASSERT_EQ(response.body_length, 0);
    ASSERT_EQ(http_response_set_body_copy(&response, NULL, 3), -1);
    http_response_destroy(&response);
}

static void test_error_status_serialization(void) {
    const struct {
        int code;
        const char *phrase;
    } cases[] = {
        {400, "Bad Request"},
        {404, "Not Found"},
        {405, "Method Not Allowed"},
        {413, "Payload Too Large"},
        {500, "Internal Server Error"},
        {501, "Not Implemented"},
        {505, "HTTP Version Not Supported"},
    };

    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        http_response_t response;
        http_response_init(&response);
        ASSERT_EQ(http_response_build_text(&response, cases[i].code, "err"), 0);

        unsigned char *wire = NULL;
        size_t wire_len = 0;
        ASSERT_EQ(http_response_serialize(&response, 0, &wire, &wire_len), 0);

        char status[64];
        snprintf(status, sizeof(status), "HTTP/1.1 %d %s\r\n", cases[i].code, cases[i].phrase);
        ASSERT_TRUE(strstr((char *)wire, status) == (char *)wire);
        ASSERT_TRUE(strstr((char *)wire, "Content-Length: 3\r\n") != NULL);
        ASSERT_TRUE(strstr((char *)wire, "Content-Type:") != NULL);
        ASSERT_TRUE(strstr((char *)wire, "Connection: close\r\n") != NULL);
        const char *body = strstr((char *)wire, "\r\n\r\n");
        ASSERT_TRUE(body != NULL);
        ASSERT_TRUE(memcmp(body + 4, "err", 3) == 0);

        if (cases[i].code == 405) {
            ASSERT_EQ(http_response_add_header(&response, "Allow", "GET, HEAD"), 0);
            free(wire);
            ASSERT_EQ(http_response_serialize(&response, 0, &wire, &wire_len), 0);
            ASSERT_TRUE(strstr((char *)wire, "Allow: GET, HEAD\r\n") != NULL);
        }

        free(wire);
        http_response_destroy(&response);
    }
}

int main(void) {
    test_reason_phrases();
    test_serialize_get_like();
    test_head_omits_body();
    test_content_length_matches_body();
    test_does_not_duplicate_manual_headers();
    test_build_json_and_body_copy();
    test_error_status_serialization();

    printf("test_response: %d assertions passed, %d failed\n", g_passed, g_failures);
    return g_failures == 0 ? 0 : 1;
}
