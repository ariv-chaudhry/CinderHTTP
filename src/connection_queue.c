#include "connection_queue.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static void timespec_add_ms(struct timespec *ts, long ms) {
    ts->tv_sec += ms / 1000;
    ts->tv_nsec += (ms % 1000) * 1000000L;
    if (ts->tv_nsec >= 1000000000L) {
        ts->tv_sec += 1;
        ts->tv_nsec -= 1000000000L;
    }
}

int connection_queue_init(connection_queue_t *queue, size_t capacity) {
    if (queue == NULL || capacity == 0) {
        return -1;
    }

    memset(queue, 0, sizeof(*queue));
    queue->items = calloc(capacity, sizeof(int));
    if (queue->items == NULL) {
        return -1;
    }
    queue->capacity = capacity;

    int err = pthread_mutex_init(&queue->mutex, NULL);
    if (err != 0) {
        fprintf(stderr, "cinderhttp: pthread_mutex_init: %s\n", strerror(err));
        free(queue->items);
        queue->items = NULL;
        return -1;
    }
    queue->mutex_initialized = 1;

    err = pthread_cond_init(&queue->not_empty, NULL);
    if (err != 0) {
        fprintf(stderr, "cinderhttp: pthread_cond_init(not_empty): %s\n", strerror(err));
        connection_queue_destroy(queue);
        return -1;
    }
    queue->not_empty_initialized = 1;

    err = pthread_cond_init(&queue->not_full, NULL);
    if (err != 0) {
        fprintf(stderr, "cinderhttp: pthread_cond_init(not_full): %s\n", strerror(err));
        connection_queue_destroy(queue);
        return -1;
    }
    queue->not_full_initialized = 1;

    return 0;
}

void connection_queue_destroy(connection_queue_t *queue) {
    if (queue == NULL) {
        return;
    }

    /* Defensive: close any leftover fds if destroy runs without a full drain. */
    if (queue->items != NULL && queue->count > 0) {
        for (size_t i = 0; i < queue->count; i++) {
            size_t idx = (queue->head + i) % queue->capacity;
            int fd = queue->items[idx];
            if (fd >= 0) {
                close(fd);
                queue->items[idx] = -1;
            }
        }
        queue->count = 0;
    }

    if (queue->not_full_initialized) {
        pthread_cond_destroy(&queue->not_full);
        queue->not_full_initialized = 0;
    }
    if (queue->not_empty_initialized) {
        pthread_cond_destroy(&queue->not_empty);
        queue->not_empty_initialized = 0;
    }
    if (queue->mutex_initialized) {
        pthread_mutex_destroy(&queue->mutex);
        queue->mutex_initialized = 0;
    }

    free(queue->items);
    queue->items = NULL;
    queue->capacity = 0;
    queue->head = 0;
    queue->tail = 0;
    queue->count = 0;
    queue->shutting_down = 0;
}

connection_queue_status_t connection_queue_push(connection_queue_t *queue, int client_fd,
                                                const volatile sig_atomic_t *abort_flag) {
    if (queue == NULL || client_fd < 0) {
        return CONNECTION_QUEUE_ERROR;
    }

    int err = pthread_mutex_lock(&queue->mutex);
    if (err != 0) {
        return CONNECTION_QUEUE_ERROR;
    }

    /*
     * Timed waits let the accept thread notice an external abort flag (set by
     * a signal handler) without requiring pthread calls inside that handler.
     * Spurious wakeups and timeouts are handled by re-checking predicates.
     */
    while (queue->count >= queue->capacity && !queue->shutting_down) {
        if (abort_flag != NULL && *abort_flag != 0) {
            pthread_mutex_unlock(&queue->mutex);
            return CONNECTION_QUEUE_SHUTDOWN;
        }

        struct timespec ts;
        if (clock_gettime(CLOCK_REALTIME, &ts) != 0) {
            pthread_mutex_unlock(&queue->mutex);
            return CONNECTION_QUEUE_ERROR;
        }
        timespec_add_ms(&ts, 100);

        err = pthread_cond_timedwait(&queue->not_full, &queue->mutex, &ts);
        if (err != 0 && err != ETIMEDOUT) {
            pthread_mutex_unlock(&queue->mutex);
            return CONNECTION_QUEUE_ERROR;
        }
    }

    if (queue->shutting_down) {
        pthread_mutex_unlock(&queue->mutex);
        return CONNECTION_QUEUE_SHUTDOWN;
    }

    queue->items[queue->tail] = client_fd;
    queue->tail = (queue->tail + 1) % queue->capacity;
    queue->count++;

    /* Ownership transferred; do not return ERROR after this point. */
    (void)pthread_cond_signal(&queue->not_empty);
    pthread_mutex_unlock(&queue->mutex);
    return CONNECTION_QUEUE_OK;
}

connection_queue_status_t connection_queue_pop(connection_queue_t *queue, int *client_fd) {
    if (queue == NULL || client_fd == NULL) {
        return CONNECTION_QUEUE_ERROR;
    }

    int err = pthread_mutex_lock(&queue->mutex);
    if (err != 0) {
        return CONNECTION_QUEUE_ERROR;
    }

    while (queue->count == 0 && !queue->shutting_down) {
        err = pthread_cond_wait(&queue->not_empty, &queue->mutex);
        if (err != 0) {
            pthread_mutex_unlock(&queue->mutex);
            return CONNECTION_QUEUE_ERROR;
        }
    }

    /* Drain remaining work after shutdown; only stop when empty. */
    if (queue->count == 0 && queue->shutting_down) {
        pthread_mutex_unlock(&queue->mutex);
        return CONNECTION_QUEUE_SHUTDOWN;
    }

    *client_fd = queue->items[queue->head];
    queue->head = (queue->head + 1) % queue->capacity;
    queue->count--;

    /* Caller owns *client_fd; do not return ERROR after this point. */
    (void)pthread_cond_signal(&queue->not_full);
    pthread_mutex_unlock(&queue->mutex);
    return CONNECTION_QUEUE_OK;
}

void connection_queue_shutdown(connection_queue_t *queue) {
    if (queue == NULL || !queue->mutex_initialized) {
        return;
    }

    pthread_mutex_lock(&queue->mutex);
    queue->shutting_down = 1;
    if (queue->not_empty_initialized) {
        pthread_cond_broadcast(&queue->not_empty);
    }
    if (queue->not_full_initialized) {
        pthread_cond_broadcast(&queue->not_full);
    }
    pthread_mutex_unlock(&queue->mutex);
}
