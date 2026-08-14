/*
 * thread_pool.h - fixed-size POSIX worker pool consuming a connection queue.
 *
 * Lifecycle:
 *   thread_pool_init  -> allocate thread handles
 *   thread_pool_start -> create workers (partial failure joins created ones)
 *   (accept thread pushes work)
 *   connection_queue_shutdown -> workers drain then exit
 *   thread_pool_join  -> wait for every worker
 *   thread_pool_destroy -> free thread array (does not destroy queue or stats)
 *
 * config, queue, and stats are borrowed (non-owning).
 */
#ifndef CINDERHTTP_THREAD_POOL_H
#define CINDERHTTP_THREAD_POOL_H

#include <pthread.h>
#include <stddef.h>

#include "config.h"
#include "connection_queue.h"
#include "server_stats.h"

typedef struct {
    pthread_t *threads;
    size_t worker_count;
    size_t started_count;

    connection_queue_t *queue;
    const server_config_t *config;
    server_stats_t *stats;
} thread_pool_t;

/* Allocates thread handle storage. Does not create threads yet.
 * Returns 0 on success, -1 on failure. */
int thread_pool_init(thread_pool_t *pool, size_t worker_count, connection_queue_t *queue,
                     const server_config_t *config, server_stats_t *stats);

/*
 * Creates worker_count joinable threads. On partial failure, shuts down the
 * queue, joins already-created workers, and returns -1.
 */
int thread_pool_start(thread_pool_t *pool);

/* Joins every successfully created worker. */
void thread_pool_join(thread_pool_t *pool);

/* Frees thread handle storage. Call after join. Does not destroy queue/stats. */
void thread_pool_destroy(thread_pool_t *pool);

#endif /* CINDERHTTP_THREAD_POOL_H */
