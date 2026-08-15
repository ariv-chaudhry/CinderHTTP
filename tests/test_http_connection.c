/*
 * test_http_connection.c - Connection token parsing and keep-alive policy.
 */
#include "http_connection.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "http_request.h"
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

static void test_tokens(void) {
    ASSERT_TRUE(http_header_value_has_token("close", "close"));
    ASSERT_TRUE(http_header_value_has_token("CLOSE", "close"));
    ASSERT_TRUE(http_header_value_has_token(" close ", "close"));
    ASSERT_TRUE(http_header_value_has_token("foo, close", "close"));
    ASSERT_TRUE(http_header_value_has_token("keep-alive", "keep-alive"));
    ASSERT_TRUE(http_header_value_has_token("Keep-Alive", "keep-alive"));
    ASSERT_TRUE(http_header_value_has_token("foo, keep-alive, bar", "keep-alive"));
    ASSERT_TRUE(!http_header_value_has_token("close-ish", "close"));
    ASSERT_TRUE(!http_header_value_has_token("keep-alive-extra", "keep-alive"));
    ASSERT_TRUE(!http_header_value_has_token("", "close"));
    ASSERT_TRUE(!http_header_value_has_token(NULL, "close"));
}

static http_request_t make_req(const char *version, const char *connection) {
    http_request_t req;
    http_request_init(&req);
    req.method = HTTP_METHOD_GET;
    req.target = strdup("/");
    req.version = strdup(version);
    if (connection != NULL) {
        req.headers = calloc(1, sizeof(http_header_t));
        req.headers[0].name = strdup("Connection");
        req.headers[0].value = strdup(connection);
        req.header_count = 1;
    }
    return req;
}

static void test_policy(void) {
    http_request_t r;

    r = make_req("HTTP/1.1", NULL);
    ASSERT_EQ(http_request_wants_keep_alive(&r), 1);
    http_request_destroy(&r);

    r = make_req("HTTP/1.1", "close");
    ASSERT_EQ(http_request_wants_keep_alive(&r), 0);
    http_request_destroy(&r);

    r = make_req("HTTP/1.1", "CLOSE");
    ASSERT_EQ(http_request_wants_keep_alive(&r), 0);
    http_request_destroy(&r);

    r = make_req("HTTP/1.1", "keep-alive, close");
    ASSERT_EQ(http_request_wants_keep_alive(&r), 0);
    http_request_destroy(&r);

    r = make_req("HTTP/1.0", NULL);
    ASSERT_EQ(http_request_wants_keep_alive(&r), 0);
    http_request_destroy(&r);

    r = make_req("HTTP/1.0", "keep-alive");
    ASSERT_EQ(http_request_wants_keep_alive(&r), 1);
    http_request_destroy(&r);

    r = make_req("HTTP/1.0", "Keep-Alive");
    ASSERT_EQ(http_request_wants_keep_alive(&r), 1);
    http_request_destroy(&r);
}

static const char *hdr(const http_response_t *resp, const char *name) {
    for (size_t i = 0; i < resp->header_count; i++) {
        if (strcasecmp(resp->headers[i].name, name) == 0) {
            return resp->headers[i].value;
        }
    }
    return NULL;
}

static void test_response_connection_headers(void) {
    http_response_t resp;
    unsigned char *wire = NULL;
    size_t wire_len = 0;

    http_response_init(&resp);
    ASSERT_EQ(http_response_build_text(&resp, 200, "ok"), 0);
    ASSERT_EQ(http_response_apply_connection_policy(&resp, 0, "HTTP/1.1"), 0);
    ASSERT_EQ(http_response_serialize(&resp, 0, &wire, &wire_len), 0);
    ASSERT_TRUE(strstr((char *)wire, "Connection: close\r\n") != NULL);
    free(wire);
    http_response_destroy(&resp);

    http_response_init(&resp);
    ASSERT_EQ(http_response_build_text(&resp, 200, "ok"), 0);
    ASSERT_EQ(http_response_apply_connection_policy(&resp, 1, "HTTP/1.0"), 0);
    ASSERT_TRUE(hdr(&resp, "Connection") != NULL);
    ASSERT_TRUE(strcasecmp(hdr(&resp, "Connection"), "keep-alive") == 0);
    ASSERT_EQ(http_response_serialize(&resp, 0, &wire, &wire_len), 0);
    ASSERT_TRUE(strstr((char *)wire, "Connection: keep-alive\r\n") != NULL);
    free(wire);
    http_response_destroy(&resp);

    http_response_init(&resp);
    ASSERT_EQ(http_response_build_text(&resp, 200, "ok"), 0);
    ASSERT_EQ(http_response_apply_connection_policy(&resp, 1, "HTTP/1.1"), 0);
    ASSERT_TRUE(hdr(&resp, "Connection") == NULL);
    ASSERT_EQ(resp.suppress_auto_connection_close, 1);
    ASSERT_EQ(http_response_serialize(&resp, 0, &wire, &wire_len), 0);
    ASSERT_TRUE(strstr((char *)wire, "Connection:") == NULL);
    free(wire);
    http_response_destroy(&resp);
}

static void test_set_header_replace(void) {
    http_response_t resp;
    http_response_init(&resp);
    ASSERT_EQ(http_response_set_header(&resp, "Connection", "close"), 0);
    ASSERT_EQ(http_response_set_header(&resp, "connection", "keep-alive"), 0);
    ASSERT_EQ(resp.header_count, 1);
    ASSERT_TRUE(strcasecmp(resp.headers[0].value, "keep-alive") == 0);
    http_response_destroy(&resp);
}

int main(void) {
    test_tokens();
    test_policy();
    test_response_connection_headers();
    test_set_header_replace();
    printf("test_http_connection: %d assertions passed, %d failed\n", g_passed, g_failures);
    return g_failures == 0 ? 0 : 1;
}
