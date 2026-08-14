#include "server_stats.h"

#include <pthread.h>
#include <stdio.h>
#include <string.h>

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

static void test_initial_and_increments(void) {
    server_stats_t stats;
    ASSERT_EQ(server_stats_init(&stats), 0);

    server_stats_snapshot_t snap;
    server_stats_snapshot(&stats, &snap);
    ASSERT_EQ(snap.connections_accepted, 0);
    ASSERT_EQ(snap.requests_total, 0);
    ASSERT_EQ(snap.responses_2xx, 0);
    ASSERT_EQ(snap.responses_4xx, 0);
    ASSERT_EQ(snap.responses_5xx, 0);
    ASSERT_EQ(snap.active_connections, 0);

    server_stats_connection_accepted(&stats);
    server_stats_connection_accepted(&stats);
    server_stats_request_received(&stats);
    server_stats_connection_started(&stats);
    server_stats_connection_started(&stats);
    server_stats_connection_finished(&stats);

    server_stats_response_sent(&stats, 200);
    server_stats_response_sent(&stats, 204);
    server_stats_response_sent(&stats, 400);
    server_stats_response_sent(&stats, 404);
    server_stats_response_sent(&stats, 500);
    server_stats_response_sent(&stats, 503);
    server_stats_response_sent(&stats, 100); /* ignored class */

    server_stats_snapshot(&stats, &snap);
    ASSERT_EQ(snap.connections_accepted, 2);
    ASSERT_EQ(snap.requests_total, 1);
    ASSERT_EQ(snap.active_connections, 1);
    ASSERT_EQ(snap.responses_2xx, 2);
    ASSERT_EQ(snap.responses_4xx, 2);
    ASSERT_EQ(snap.responses_5xx, 2);

    server_stats_connection_finished(&stats);
    server_stats_snapshot(&stats, &snap);
    ASSERT_EQ(snap.active_connections, 0);

    /* Underflow guard */
    server_stats_connection_finished(&stats);
    server_stats_snapshot(&stats, &snap);
    ASSERT_EQ(snap.active_connections, 0);

    server_stats_destroy(&stats);
}

typedef struct {
    server_stats_t *stats;
    int iterations;
} concurrent_args_t;

static void *concurrent_worker(void *arg) {
    concurrent_args_t *a = (concurrent_args_t *)arg;
    for (int i = 0; i < a->iterations; i++) {
        server_stats_request_received(a->stats);
        server_stats_response_sent(a->stats, 200);
    }
    return NULL;
}

static void test_concurrent_updates(void) {
    server_stats_t stats;
    ASSERT_EQ(server_stats_init(&stats), 0);

    enum { THREADS = 8, ITERS = 10000 };
    pthread_t threads[THREADS];
    concurrent_args_t args = {.stats = &stats, .iterations = ITERS};

    for (int i = 0; i < THREADS; i++) {
        ASSERT_EQ(pthread_create(&threads[i], NULL, concurrent_worker, &args), 0);
    }
    for (int i = 0; i < THREADS; i++) {
        ASSERT_EQ(pthread_join(threads[i], NULL), 0);
    }

    server_stats_snapshot_t snap;
    server_stats_snapshot(&stats, &snap);
    ASSERT_EQ(snap.requests_total, (uint64_t)THREADS * ITERS);
    ASSERT_EQ(snap.responses_2xx, (uint64_t)THREADS * ITERS);

    server_stats_destroy(&stats);
}

int main(void) {
    test_initial_and_increments();
    test_concurrent_updates();

    printf("test_server_stats: %d assertions passed, %d failed\n", g_passed, g_failures);
    return g_failures == 0 ? 0 : 1;
}
