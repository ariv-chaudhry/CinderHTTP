#include "thread_pool.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "client_handler.h"

typedef struct {
    thread_pool_t *pool;
    int worker_id;
} worker_arg_t;

static void *worker_main(void *arg) {
    worker_arg_t *warg = (worker_arg_t *)arg;
    thread_pool_t *pool = warg->pool;
    int worker_id = warg->worker_id;
    free(warg);

    for (;;) {
        int client_fd = -1;
        connection_queue_status_t status = connection_queue_pop(pool->queue, &client_fd);
        if (status != CONNECTION_QUEUE_OK) {
            break;
        }
        /* Queue mutex is not held across request processing. */
        client_handle(pool->config, pool->stats, client_fd, worker_id);
    }

    return NULL;
}

int thread_pool_init(thread_pool_t *pool, size_t worker_count, connection_queue_t *queue,
                     const server_config_t *config, server_stats_t *stats) {
    if (pool == NULL || worker_count == 0 || queue == NULL || config == NULL || stats == NULL) {
        return -1;
    }

    memset(pool, 0, sizeof(*pool));
    pool->threads = calloc(worker_count, sizeof(pthread_t));
    if (pool->threads == NULL) {
        return -1;
    }

    pool->worker_count = worker_count;
    pool->started_count = 0;
    pool->queue = queue;
    pool->config = config;
    pool->stats = stats;
    return 0;
}

int thread_pool_start(thread_pool_t *pool) {
    if (pool == NULL || pool->threads == NULL) {
        return -1;
    }

    for (size_t i = 0; i < pool->worker_count; i++) {
        worker_arg_t *warg = malloc(sizeof(*warg));
        if (warg == NULL) {
            fprintf(stderr, "cinderhttp: out of memory creating worker %zu\n", i);
            connection_queue_shutdown(pool->queue);
            for (size_t j = 0; j < pool->started_count; j++) {
                pthread_join(pool->threads[j], NULL);
            }
            pool->started_count = 0;
            return -1;
        }
        warg->pool = pool;
        warg->worker_id = (int)i;

        int err = pthread_create(&pool->threads[i], NULL, worker_main, warg);
        if (err != 0) {
            free(warg);
            fprintf(stderr, "cinderhttp: pthread_create(worker %zu): %s\n", i, strerror(err));
            connection_queue_shutdown(pool->queue);
            for (size_t j = 0; j < pool->started_count; j++) {
                pthread_join(pool->threads[j], NULL);
            }
            pool->started_count = 0;
            return -1;
        }
        pool->started_count++;
    }

    return 0;
}

void thread_pool_join(thread_pool_t *pool) {
    if (pool == NULL || pool->threads == NULL) {
        return;
    }

    for (size_t i = 0; i < pool->started_count; i++) {
        int err = pthread_join(pool->threads[i], NULL);
        if (err != 0) {
            fprintf(stderr, "cinderhttp: pthread_join(worker %zu): %s\n", i, strerror(err));
        }
    }
    pool->started_count = 0;
}

void thread_pool_destroy(thread_pool_t *pool) {
    if (pool == NULL) {
        return;
    }
    free(pool->threads);
    pool->threads = NULL;
    pool->worker_count = 0;
    pool->queue = NULL;
    pool->config = NULL;
    pool->stats = NULL;
}
