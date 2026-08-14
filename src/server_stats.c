#include "server_stats.h"

#include <stdio.h>
#include <string.h>

int server_stats_init(server_stats_t *stats) {
    if (stats == NULL) {
        return -1;
    }

    memset(stats, 0, sizeof(*stats));
    int err = pthread_mutex_init(&stats->mutex, NULL);
    if (err != 0) {
        fprintf(stderr, "cinderhttp: pthread_mutex_init(stats): %s\n", strerror(err));
        return -1;
    }
    stats->initialized = 1;
    return 0;
}

void server_stats_destroy(server_stats_t *stats) {
    if (stats == NULL || !stats->initialized) {
        return;
    }
    pthread_mutex_destroy(&stats->mutex);
    stats->initialized = 0;
}

static void lock_stats(server_stats_t *stats) {
    if (stats == NULL || !stats->initialized) {
        return;
    }
    pthread_mutex_lock(&stats->mutex);
}

static void unlock_stats(server_stats_t *stats) {
    if (stats == NULL || !stats->initialized) {
        return;
    }
    pthread_mutex_unlock(&stats->mutex);
}

void server_stats_connection_accepted(server_stats_t *stats) {
    if (stats == NULL || !stats->initialized) {
        return;
    }
    lock_stats(stats);
    stats->connections_accepted++;
    unlock_stats(stats);
}

void server_stats_connection_started(server_stats_t *stats) {
    if (stats == NULL || !stats->initialized) {
        return;
    }
    lock_stats(stats);
    stats->active_connections++;
    unlock_stats(stats);
}

void server_stats_connection_finished(server_stats_t *stats) {
    if (stats == NULL || !stats->initialized) {
        return;
    }
    lock_stats(stats);
    if (stats->active_connections > 0) {
        stats->active_connections--;
    }
    unlock_stats(stats);
}

void server_stats_request_received(server_stats_t *stats) {
    if (stats == NULL || !stats->initialized) {
        return;
    }
    lock_stats(stats);
    stats->requests_total++;
    unlock_stats(stats);
}

void server_stats_response_sent(server_stats_t *stats, int status) {
    if (stats == NULL || !stats->initialized) {
        return;
    }
    lock_stats(stats);
    if (status >= 200 && status <= 299) {
        stats->responses_2xx++;
    } else if (status >= 400 && status <= 499) {
        stats->responses_4xx++;
    } else if (status >= 500 && status <= 599) {
        stats->responses_5xx++;
    }
    unlock_stats(stats);
}

void server_stats_snapshot(server_stats_t *stats, server_stats_snapshot_t *out) {
    if (out == NULL) {
        return;
    }
    memset(out, 0, sizeof(*out));
    if (stats == NULL || !stats->initialized) {
        return;
    }
    lock_stats(stats);
    out->connections_accepted = stats->connections_accepted;
    out->requests_total = stats->requests_total;
    out->responses_2xx = stats->responses_2xx;
    out->responses_4xx = stats->responses_4xx;
    out->responses_5xx = stats->responses_5xx;
    out->active_connections = stats->active_connections;
    unlock_stats(stats);
}
