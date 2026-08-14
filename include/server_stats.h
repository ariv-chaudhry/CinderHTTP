/*
 * server_stats.h - thread-safe runtime counters for CinderHTTP.
 *
 * Ownership: server_run() owns the stats object for the process lifetime.
 * Workers and the router borrow a non-owning pointer. Destroy only after all
 * workers have been joined.
 *
 * The mutex protects only this object. It is never held across I/O, logging,
 * JSON formatting, or request processing.
 */
#ifndef CINDERHTTP_SERVER_STATS_H
#define CINDERHTTP_SERVER_STATS_H

#include <pthread.h>
#include <stdint.h>

typedef struct {
    uint64_t connections_accepted;
    uint64_t requests_total;
    uint64_t responses_2xx;
    uint64_t responses_4xx;
    uint64_t responses_5xx;
    uint64_t active_connections;
} server_stats_snapshot_t;

typedef struct {
    pthread_mutex_t mutex;
    int initialized;

    uint64_t connections_accepted;
    uint64_t requests_total;
    uint64_t responses_2xx;
    uint64_t responses_4xx;
    uint64_t responses_5xx;
    uint64_t active_connections;
} server_stats_t;

/* Returns 0 on success, -1 on failure. */
int server_stats_init(server_stats_t *stats);

void server_stats_destroy(server_stats_t *stats);

/* After accept() succeeds. */
void server_stats_connection_accepted(server_stats_t *stats);

/*
 * active_connections +1 after the fd is successfully enqueued for workers.
 * Pair with server_stats_connection_finished() exactly once per such fd.
 */
void server_stats_connection_started(server_stats_t *stats);

/* active_connections -1 when client_handle() finishes (never underflows). */
void server_stats_connection_finished(server_stats_t *stats);

/*
 * After http_read_request() returns HTTP_READ_OK (enough bytes to attempt
 * parsing). Parse errors still count as requests.
 */
void server_stats_request_received(server_stats_t *stats);

/*
 * After one HTTP response has been selected/sent. Classifies by status:
 * 2xx / 4xx / 5xx. Other statuses are ignored for class counters.
 */
void server_stats_response_sent(server_stats_t *stats, int status);

/* Copies counters under the lock, then unlocks. Safe to format from snapshot. */
void server_stats_snapshot(server_stats_t *stats, server_stats_snapshot_t *out);

#endif /* CINDERHTTP_SERVER_STATS_H */
