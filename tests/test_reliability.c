/*
 * test_reliability.c - Stage 6 lifecycle / shutdown / ownership invariants.
 */
#include "connection_queue.h"

#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "http_parser.h"
#include "http_request.h"
#include "utils.h"

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

static void sleep_ms(long ms) {
    struct timespec ts;
    ts.tv_sec = ms / 1000;
    ts.tv_nsec = (ms % 1000) * 1000000L;
    nanosleep(&ts, NULL);
}

static void test_queue_drain_on_shutdown(void) {
    connection_queue_t q;
    ASSERT_EQ(connection_queue_init(&q, 8), 0);
    ASSERT_EQ(connection_queue_push(&q, 11, NULL), CONNECTION_QUEUE_OK);
    ASSERT_EQ(connection_queue_push(&q, 22, NULL), CONNECTION_QUEUE_OK);
    ASSERT_EQ(connection_queue_push(&q, 33, NULL), CONNECTION_QUEUE_OK);

    connection_queue_shutdown(&q);

    int fd = -1;
    ASSERT_EQ(connection_queue_pop(&q, &fd), CONNECTION_QUEUE_OK);
    ASSERT_EQ(fd, 11);
    ASSERT_EQ(connection_queue_pop(&q, &fd), CONNECTION_QUEUE_OK);
    ASSERT_EQ(fd, 22);
    ASSERT_EQ(connection_queue_pop(&q, &fd), CONNECTION_QUEUE_OK);
    ASSERT_EQ(fd, 33);
    ASSERT_EQ(connection_queue_pop(&q, &fd), CONNECTION_QUEUE_SHUTDOWN);

    connection_queue_destroy(&q);
}

typedef struct {
    connection_queue_t *queue;
    connection_queue_status_t result;
    pthread_mutex_t *gate_mu;
    pthread_cond_t *gate_cv;
    int *entered;
} blocked_args_t;

static void *blocked_consumer(void *arg) {
    blocked_args_t *a = (blocked_args_t *)arg;
    pthread_mutex_lock(a->gate_mu);
    *a->entered = 1;
    pthread_cond_signal(a->gate_cv);
    pthread_mutex_unlock(a->gate_mu);

    int fd = -1;
    a->result = connection_queue_pop(a->queue, &fd);
    return NULL;
}

static void *blocked_producer(void *arg) {
    blocked_args_t *a = (blocked_args_t *)arg;
    pthread_mutex_lock(a->gate_mu);
    *a->entered = 1;
    pthread_cond_signal(a->gate_cv);
    pthread_mutex_unlock(a->gate_mu);

    a->result = connection_queue_push(a->queue, 99, NULL);
    return NULL;
}

static void test_shutdown_wakes_blocked_consumer(void) {
    connection_queue_t q;
    ASSERT_EQ(connection_queue_init(&q, 4), 0);

    pthread_mutex_t gate_mu = PTHREAD_MUTEX_INITIALIZER;
    pthread_cond_t gate_cv = PTHREAD_COND_INITIALIZER;
    int entered = 0;
    blocked_args_t args = {.queue = &q,
                           .result = CONNECTION_QUEUE_OK,
                           .gate_mu = &gate_mu,
                           .gate_cv = &gate_cv,
                           .entered = &entered};

    pthread_t tid;
    ASSERT_EQ(pthread_create(&tid, NULL, blocked_consumer, &args), 0);
    pthread_mutex_lock(&gate_mu);
    while (!entered) {
        pthread_cond_wait(&gate_cv, &gate_mu);
    }
    pthread_mutex_unlock(&gate_mu);
    sleep_ms(50);

    connection_queue_shutdown(&q);
    ASSERT_EQ(pthread_join(tid, NULL), 0);
    ASSERT_EQ(args.result, CONNECTION_QUEUE_SHUTDOWN);

    pthread_cond_destroy(&gate_cv);
    pthread_mutex_destroy(&gate_mu);
    connection_queue_destroy(&q);
}

static void test_shutdown_wakes_blocked_producer(void) {
    connection_queue_t q;
    ASSERT_EQ(connection_queue_init(&q, 1), 0);
    ASSERT_EQ(connection_queue_push(&q, 1, NULL), CONNECTION_QUEUE_OK);

    pthread_mutex_t gate_mu = PTHREAD_MUTEX_INITIALIZER;
    pthread_cond_t gate_cv = PTHREAD_COND_INITIALIZER;
    int entered = 0;
    blocked_args_t args = {.queue = &q,
                           .result = CONNECTION_QUEUE_OK,
                           .gate_mu = &gate_mu,
                           .gate_cv = &gate_cv,
                           .entered = &entered};

    pthread_t tid;
    ASSERT_EQ(pthread_create(&tid, NULL, blocked_producer, &args), 0);
    pthread_mutex_lock(&gate_mu);
    while (!entered) {
        pthread_cond_wait(&gate_cv, &gate_mu);
    }
    pthread_mutex_unlock(&gate_mu);
    sleep_ms(50);

    connection_queue_shutdown(&q);
    ASSERT_EQ(pthread_join(tid, NULL), 0);
    ASSERT_EQ(args.result, CONNECTION_QUEUE_SHUTDOWN);

    int fd = -1;
    ASSERT_EQ(connection_queue_pop(&q, &fd), CONNECTION_QUEUE_OK);
    ASSERT_EQ(fd, 1);
    ASSERT_EQ(connection_queue_pop(&q, &fd), CONNECTION_QUEUE_SHUTDOWN);

    pthread_cond_destroy(&gate_cv);
    pthread_mutex_destroy(&gate_mu);
    connection_queue_destroy(&q);
}

static void test_destroy_closes_leftover_fds_safe(void) {
    /* Destroy with empty queue after shutdown is the normal path. */
    connection_queue_t q;
    ASSERT_EQ(connection_queue_init(&q, 2), 0);
    connection_queue_shutdown(&q);
    connection_queue_destroy(&q);
    /* Second destroy must be a no-op / safe on zeroed state. */
    connection_queue_destroy(&q);
}

static void test_malformed_parser_regressions(void) {
    http_request_t req;
    http_request_init(&req);

    ASSERT_EQ(http_parse_request((const unsigned char *)"", 0, &req), HTTP_PARSE_BAD_REQUEST);

    const char *lf_only = "GET / HTTP/1.1\n\n";
    ASSERT_EQ(http_parse_request((const unsigned char *)lf_only, strlen(lf_only), &req),
              HTTP_PARSE_BAD_REQUEST);

    const char *no_version = "GET /\r\n\r\n";
    ASSERT_EQ(http_parse_request((const unsigned char *)no_version, strlen(no_version), &req),
              HTTP_PARSE_BAD_REQUEST);

    const char *chunked = "POST / HTTP/1.1\r\nTransfer-Encoding: chunked\r\n\r\n";
    ASSERT_EQ(http_parse_request((const unsigned char *)chunked, strlen(chunked), &req),
              HTTP_PARSE_UNSUPPORTED_TRANSFER_ENCODING);

    const char *dup_cl = "POST / HTTP/1.1\r\nContent-Length: 5\r\nContent-Length: 6\r\n\r\nhello!";
    ASSERT_EQ(http_parse_request((const unsigned char *)dup_cl, strlen(dup_cl), &req),
              HTTP_PARSE_INVALID_CONTENT_LENGTH);

    /* Body shorter than Content-Length is rejected when presented as complete. */
    const char *short_body = "POST / HTTP/1.1\r\nContent-Length: 10\r\n\r\nshort";
    ASSERT_EQ(http_parse_request((const unsigned char *)short_body, strlen(short_body), &req),
              HTTP_PARSE_BAD_REQUEST);

    http_request_destroy(&req);
}

static void test_send_all_zero_length(void) {
    /* length 0 must succeed without touching the socket. */
    ASSERT_EQ(send_all(-1, "", 0), 0);
    ASSERT_EQ(send_all(-1, NULL, 0), 0);
}

int main(void) {
    test_queue_drain_on_shutdown();
    test_shutdown_wakes_blocked_consumer();
    test_shutdown_wakes_blocked_producer();
    test_destroy_closes_leftover_fds_safe();
    test_malformed_parser_regressions();
    test_send_all_zero_length();

    printf("test_reliability: %d assertions passed, %d failed\n", g_passed, g_failures);
    return g_failures == 0 ? 0 : 1;
}
