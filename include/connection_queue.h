/*
 * connection_queue.h - bounded producer-consumer queue of client sockets.
 *
 * The accept thread pushes file descriptors; workers pop them. A mutex and
 * two condition variables protect the ring buffer and provide backpressure
 * when the queue is full.
 *
 * Ownership: after a successful push, the queue (and eventually the worker
 * that pops) owns the descriptor. On push failure due to shutdown, ownership
 * remains with the caller, who must close the fd.
 */
#ifndef CINDERHTTP_CONNECTION_QUEUE_H
#define CINDERHTTP_CONNECTION_QUEUE_H

#include <pthread.h>
#include <signal.h>
#include <stddef.h>

typedef struct {
    int *items;
    size_t capacity;
    size_t head;
    size_t tail;
    size_t count;

    int shutting_down;

    pthread_mutex_t mutex;
    pthread_cond_t not_empty;
    pthread_cond_t not_full;
} connection_queue_t;

typedef enum {
    CONNECTION_QUEUE_OK = 0,
    CONNECTION_QUEUE_SHUTDOWN, /* shutdown in progress / no more work */
    CONNECTION_QUEUE_ERROR
} connection_queue_status_t;

/*
 * Allocates the ring buffer and initializes synchronization primitives.
 * Returns 0 on success, -1 on failure (partial state is cleaned up).
 */
int connection_queue_init(connection_queue_t *queue, size_t capacity);

/* Destroys mutex/condvars and frees the ring buffer. Call only after all
 * threads that use the queue have been joined. */
void connection_queue_destroy(connection_queue_t *queue);

/*
 * Enqueues client_fd. Blocks while the queue is full unless shutting down.
 *
 * If abort_flag is non-NULL, push periodically rechecks *abort_flag (via
 * timed waits) so a signal-set flag can unblock a producer stuck on a full
 * queue without calling pthread APIs from a signal handler. When abort_flag
 * becomes non-zero, returns CONNECTION_QUEUE_SHUTDOWN without enqueueing;
 * the caller still owns client_fd and must close it.
 */
connection_queue_status_t connection_queue_push(connection_queue_t *queue, int client_fd,
                                                const volatile sig_atomic_t *abort_flag);

/*
 * Dequeues into *client_fd. Blocks while empty unless shutting down.
 * After shutdown, continues returning already-queued descriptors; once the
 * queue is empty and shutting_down is set, returns CONNECTION_QUEUE_SHUTDOWN.
 */
connection_queue_status_t connection_queue_pop(connection_queue_t *queue, int *client_fd);

/* Sets shutting_down and broadcasts both condition variables. */
void connection_queue_shutdown(connection_queue_t *queue);

#endif /* CINDERHTTP_CONNECTION_QUEUE_H */
