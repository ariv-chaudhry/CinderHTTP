#include "router.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "http_parser.h"
#include "http_request.h"
#include "http_response.h"
#include "server_stats.h"

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

static const char *response_header(const http_response_t *response, const char *name) {
    for (size_t i = 0; i < response->header_count; i++) {
        if (strcasecmp(response->headers[i].name, name) == 0) {
            return response->headers[i].value;
        }
    }
    return NULL;
}

static int parse_message(const char *raw, size_t raw_len, http_request_t *request) {
    return http_parse_request((const unsigned char *)raw, raw_len, request) == HTTP_PARSE_OK;
}

static void test_health_get(void) {
    server_stats_t stats;
    ASSERT_EQ(server_stats_init(&stats), 0);
    router_context_t ctx = {.config = NULL, .stats = &stats};

    const char *raw = "GET /api/health HTTP/1.1\r\nHost: localhost\r\n\r\n";
    http_request_t request;
    http_response_t response;
    http_request_init(&request);
    http_response_init(&response);
    ASSERT_TRUE(parse_message(raw, strlen(raw), &request));
    ASSERT_TRUE(router_is_api_request(&request));
    ASSERT_EQ(router_dispatch(&ctx, &request, &response), ROUTER_HANDLED);
    ASSERT_EQ(response.status_code, 200);
    ASSERT_STR_EQ(response_header(&response, "Content-Type"), "application/json");
    ASSERT_TRUE(response.body != NULL);
    ASSERT_TRUE(memcmp(response.body, "{\"status\":\"ok\"}", response.body_length) == 0);

    http_response_destroy(&response);
    http_request_destroy(&request);
    server_stats_destroy(&stats);
}

static void test_health_head_and_query(void) {
    server_stats_t stats;
    ASSERT_EQ(server_stats_init(&stats), 0);
    router_context_t ctx = {.config = NULL, .stats = &stats};

    const char *raw = "HEAD /api/health?foo=bar HTTP/1.1\r\nHost: localhost\r\n\r\n";
    http_request_t request;
    http_response_t response;
    http_request_init(&request);
    http_response_init(&response);
    ASSERT_TRUE(parse_message(raw, strlen(raw), &request));
    ASSERT_EQ(router_dispatch(&ctx, &request, &response), ROUTER_HANDLED);
    ASSERT_EQ(response.status_code, 200);
    ASSERT_EQ(response.body_length, strlen("{\"status\":\"ok\"}"));

    unsigned char *wire = NULL;
    size_t wire_len = 0;
    ASSERT_EQ(http_response_serialize(&response, 1 /* omit body */, &wire, &wire_len), 0);
    char expected_cl[64];
    snprintf(expected_cl, sizeof(expected_cl), "Content-Length: %zu\r\n", response.body_length);
    ASSERT_TRUE(strstr((char *)wire, expected_cl) != NULL);
    const char *sep = strstr((char *)wire, "\r\n\r\n");
    ASSERT_TRUE(sep != NULL);
    ASSERT_EQ(wire_len, (size_t)(sep + 4 - (char *)wire));
    free(wire);

    http_response_destroy(&response);
    http_request_destroy(&request);
    server_stats_destroy(&stats);
}

static void test_method_path_matrix(void) {
    server_stats_t stats;
    ASSERT_EQ(server_stats_init(&stats), 0);
    router_context_t ctx = {.config = NULL, .stats = &stats};

    struct {
        const char *raw;
        int status;
        const char *allow_needle; /* NULL if Allow not required */
    } cases[] = {
        {"GET /api/health HTTP/1.1\r\nHost: x\r\n\r\n", 200, NULL},
        {"HEAD /api/health HTTP/1.1\r\nHost: x\r\n\r\n", 200, NULL},
        {"POST /api/health HTTP/1.1\r\nHost: x\r\nContent-Length: 0\r\n\r\n", 405, "GET"},
        {"GET /api/echo HTTP/1.1\r\nHost: x\r\n\r\n", 405, "POST"},
        {"HEAD /api/echo HTTP/1.1\r\nHost: x\r\n\r\n", 405, "POST"},
        {"POST /api/echo HTTP/1.1\r\nHost: x\r\nContent-Length: 0\r\n\r\n", 200, NULL},
        {"GET /api/stats HTTP/1.1\r\nHost: x\r\n\r\n", 200, NULL},
        {"HEAD /api/stats HTTP/1.1\r\nHost: x\r\n\r\n", 200, NULL},
        {"POST /api/stats HTTP/1.1\r\nHost: x\r\nContent-Length: 0\r\n\r\n", 405, "GET"},
        {"GET /api/unknown HTTP/1.1\r\nHost: x\r\n\r\n", 404, NULL},
        {"POST /api/unknown HTTP/1.1\r\nHost: x\r\nContent-Length: 0\r\n\r\n", 404, NULL},
    };

    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        http_request_t request;
        http_response_t response;
        http_request_init(&request);
        http_response_init(&response);
        ASSERT_TRUE(parse_message(cases[i].raw, strlen(cases[i].raw), &request));
        ASSERT_EQ(router_dispatch(&ctx, &request, &response), ROUTER_HANDLED);
        ASSERT_EQ(response.status_code, cases[i].status);
        if (cases[i].allow_needle != NULL) {
            const char *allow = response_header(&response, "Allow");
            ASSERT_TRUE(allow != NULL);
            ASSERT_TRUE(strstr(allow, cases[i].allow_needle) != NULL);
        }
        http_response_destroy(&response);
        http_request_destroy(&request);
    }

    server_stats_destroy(&stats);
}

static void test_health_wrong_method(void) {
    server_stats_t stats;
    ASSERT_EQ(server_stats_init(&stats), 0);
    router_context_t ctx = {.config = NULL, .stats = &stats};

    const char *raw = "POST /api/health HTTP/1.1\r\nHost: localhost\r\nContent-Length: 0\r\n\r\n";
    http_request_t request;
    http_response_t response;
    http_request_init(&request);
    http_response_init(&response);
    ASSERT_TRUE(parse_message(raw, strlen(raw), &request));
    ASSERT_EQ(router_dispatch(&ctx, &request, &response), ROUTER_HANDLED);
    ASSERT_EQ(response.status_code, 405);
    const char *allow = response_header(&response, "Allow");
    ASSERT_TRUE(allow != NULL);
    ASSERT_TRUE(strstr(allow, "GET") != NULL);
    ASSERT_TRUE(strstr(allow, "HEAD") != NULL);

    http_response_destroy(&response);
    http_request_destroy(&request);
    server_stats_destroy(&stats);
}

static void test_echo_text(void) {
    server_stats_t stats;
    ASSERT_EQ(server_stats_init(&stats), 0);
    router_context_t ctx = {.config = NULL, .stats = &stats};

    const char *raw = "POST /api/echo HTTP/1.1\r\nHost: localhost\r\n"
                      "Content-Type: text/plain\r\nContent-Length: 11\r\n\r\nhello world";
    http_request_t request;
    http_response_t response;
    http_request_init(&request);
    http_response_init(&response);
    ASSERT_TRUE(parse_message(raw, strlen(raw), &request));
    ASSERT_EQ(router_dispatch(&ctx, &request, &response), ROUTER_HANDLED);
    ASSERT_EQ(response.status_code, 200);
    ASSERT_EQ(response.body_length, 11);
    ASSERT_TRUE(memcmp(response.body, "hello world", 11) == 0);
    ASSERT_STR_EQ(response_header(&response, "Content-Type"), "text/plain");

    http_response_destroy(&response);
    http_request_destroy(&request);
    server_stats_destroy(&stats);
}

static void test_echo_binary_and_empty(void) {
    server_stats_t stats;
    ASSERT_EQ(server_stats_init(&stats), 0);
    router_context_t ctx = {.config = NULL, .stats = &stats};

    unsigned char raw[128];
    const char *hdr = "POST /api/echo HTTP/1.1\r\nHost: localhost\r\nContent-Length: 4\r\n\r\n";
    size_t hdr_len = strlen(hdr);
    memcpy(raw, hdr, hdr_len);
    raw[hdr_len + 0] = 0x41;
    raw[hdr_len + 1] = 0x00;
    raw[hdr_len + 2] = 0x42;
    raw[hdr_len + 3] = 0xFF;

    http_request_t request;
    http_response_t response;
    http_request_init(&request);
    http_response_init(&response);
    ASSERT_TRUE(parse_message((const char *)raw, hdr_len + 4, &request));
    ASSERT_EQ(router_dispatch(&ctx, &request, &response), ROUTER_HANDLED);
    ASSERT_EQ(response.body_length, 4);
    ASSERT_EQ(response.body[0], 0x41);
    ASSERT_EQ(response.body[1], 0x00);
    ASSERT_EQ(response.body[2], 0x42);
    ASSERT_EQ(response.body[3], 0xFF);
    ASSERT_STR_EQ(response_header(&response, "Content-Type"), "application/octet-stream");

    http_response_destroy(&response);
    http_request_destroy(&request);

    const char *empty = "POST /api/echo?test=1 HTTP/1.1\r\nHost: localhost\r\n"
                        "Content-Length: 0\r\n\r\n";
    http_request_init(&request);
    http_response_init(&response);
    ASSERT_TRUE(parse_message(empty, strlen(empty), &request));
    ASSERT_EQ(router_dispatch(&ctx, &request, &response), ROUTER_HANDLED);
    ASSERT_EQ(response.status_code, 200);
    ASSERT_EQ(response.body_length, 0);

    http_response_destroy(&response);
    http_request_destroy(&request);
    server_stats_destroy(&stats);
}

static void test_echo_wrong_method(void) {
    server_stats_t stats;
    ASSERT_EQ(server_stats_init(&stats), 0);
    router_context_t ctx = {.config = NULL, .stats = &stats};

    const char *raw = "GET /api/echo HTTP/1.1\r\nHost: localhost\r\n\r\n";
    http_request_t request;
    http_response_t response;
    http_request_init(&request);
    http_response_init(&response);
    ASSERT_TRUE(parse_message(raw, strlen(raw), &request));
    ASSERT_EQ(router_dispatch(&ctx, &request, &response), ROUTER_HANDLED);
    ASSERT_EQ(response.status_code, 405);
    ASSERT_STR_EQ(response_header(&response, "Allow"), "POST");

    http_response_destroy(&response);
    http_request_destroy(&request);
    server_stats_destroy(&stats);
}

static void test_stats_and_unknown(void) {
    server_stats_t stats;
    ASSERT_EQ(server_stats_init(&stats), 0);
    server_stats_connection_accepted(&stats);
    server_stats_request_received(&stats);
    server_stats_response_sent(&stats, 200);
    server_stats_response_sent(&stats, 404);
    server_stats_connection_started(&stats);

    router_context_t ctx = {.config = NULL, .stats = &stats};

    const char *raw = "GET /api/stats?x=y HTTP/1.1\r\nHost: localhost\r\n\r\n";
    http_request_t request;
    http_response_t response;
    http_request_init(&request);
    http_response_init(&response);
    ASSERT_TRUE(parse_message(raw, strlen(raw), &request));
    ASSERT_EQ(router_dispatch(&ctx, &request, &response), ROUTER_HANDLED);
    ASSERT_EQ(response.status_code, 200);
    ASSERT_TRUE(response.body != NULL);
    ASSERT_TRUE(strstr((char *)response.body, "\"connections_accepted\":1") != NULL);
    ASSERT_TRUE(strstr((char *)response.body, "\"requests_total\":1") != NULL);
    ASSERT_TRUE(strstr((char *)response.body, "\"responses_2xx\":1") != NULL);
    ASSERT_TRUE(strstr((char *)response.body, "\"responses_4xx\":1") != NULL);
    ASSERT_TRUE(strstr((char *)response.body, "\"active_connections\":1") != NULL);

    http_response_destroy(&response);
    http_request_destroy(&request);

    const char *bad = "POST /api/stats HTTP/1.1\r\nHost: localhost\r\nContent-Length: 0\r\n\r\n";
    http_request_init(&request);
    http_response_init(&response);
    ASSERT_TRUE(parse_message(bad, strlen(bad), &request));
    ASSERT_EQ(router_dispatch(&ctx, &request, &response), ROUTER_HANDLED);
    ASSERT_EQ(response.status_code, 405);

    http_response_destroy(&response);
    http_request_destroy(&request);

    const char *nope = "GET /api/nope HTTP/1.1\r\nHost: localhost\r\n\r\n";
    http_request_init(&request);
    http_response_init(&response);
    ASSERT_TRUE(parse_message(nope, strlen(nope), &request));
    ASSERT_TRUE(router_is_api_request(&request));
    ASSERT_EQ(router_dispatch(&ctx, &request, &response), ROUTER_HANDLED);
    ASSERT_EQ(response.status_code, 404);
    ASSERT_TRUE(strstr((char *)response.body, "not found") != NULL);

    http_response_destroy(&response);
    http_request_destroy(&request);

    const char *slash = "GET /api/health/ HTTP/1.1\r\nHost: localhost\r\n\r\n";
    http_request_init(&request);
    http_response_init(&response);
    ASSERT_TRUE(parse_message(slash, strlen(slash), &request));
    ASSERT_EQ(router_dispatch(&ctx, &request, &response), ROUTER_HANDLED);
    ASSERT_EQ(response.status_code, 404);

    http_response_destroy(&response);
    http_request_destroy(&request);

    http_request_init(&request);
    request.method = HTTP_METHOD_GET;
    request.target = strdup("/index.html");
    ASSERT_TRUE(!router_is_api_request(&request));
    http_request_destroy(&request);

    server_stats_destroy(&stats);
}

int main(void) {
    test_health_get();
    test_health_head_and_query();
    test_health_wrong_method();
    test_echo_text();
    test_echo_binary_and_empty();
    test_echo_wrong_method();
    test_stats_and_unknown();
    test_method_path_matrix();

    printf("test_router: %d assertions passed, %d failed\n", g_passed, g_failures);
    return g_failures == 0 ? 0 : 1;
}
