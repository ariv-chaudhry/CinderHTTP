#include "server.h"

#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include "connection_queue.h"
#include "logger.h"
#include "server_stats.h"
#include "thread_pool.h"

/*
 * Kernel-level backlog of fully-established connections waiting for
 * accept(). Distinct from the application-level bounded connection_queue_t.
 */
#define SERVER_LISTEN_BACKLOG 128

/*
 * Set by the signal handler, polled by the accept loop. Also passed into
 * connection_queue_push() so a producer blocked on a full queue can notice
 * shutdown without pthread calls inside the signal handler.
 */
static volatile sig_atomic_t g_shutdown_requested = 0;

static void handle_shutdown_signal(int signum) {
    (void)signum;
    g_shutdown_requested = 1;
}

static int shutdown_was_requested(void) {
    return g_shutdown_requested != 0;
}

void server_install_signal_handlers(void) {
    struct sigaction action;
    memset(&action, 0, sizeof(action));
    action.sa_handler = handle_shutdown_signal;
    sigemptyset(&action.sa_mask);

    /*
     * SA_RESTART is deliberately omitted so accept() fails with EINTR after
     * the handler runs, and the loop rechecks g_shutdown_requested promptly.
     */
    action.sa_flags = 0;

    if (sigaction(SIGINT, &action, NULL) < 0) {
        perror("cinderhttp: sigaction(SIGINT)");
    }
    if (sigaction(SIGTERM, &action, NULL) < 0) {
        perror("cinderhttp: sigaction(SIGTERM)");
    }

    /*
     * Ignore SIGPIPE so a worker writing to a client that already disconnected
     * cannot terminate the whole process. send_all() also uses MSG_NOSIGNAL
     * where available; SIG_IGN is the portable baseline.
     */
    struct sigaction pipe_action;
    memset(&pipe_action, 0, sizeof(pipe_action));
    pipe_action.sa_handler = SIG_IGN;
    sigemptyset(&pipe_action.sa_mask);
    pipe_action.sa_flags = 0;
    if (sigaction(SIGPIPE, &pipe_action, NULL) < 0) {
        perror("cinderhttp: sigaction(SIGPIPE)");
    }
}

int server_create_listening_socket(const server_config_t *config) {
    int listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (listen_fd < 0) {
        perror("cinderhttp: socket");
        return -1;
    }

    int reuse = 1;
    if (setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse)) < 0) {
        perror("cinderhttp: setsockopt(SO_REUSEADDR)");
        close(listen_fd);
        return -1;
    }

    struct sockaddr_in address;
    memset(&address, 0, sizeof(address));
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_ANY);
    address.sin_port = htons((uint16_t)config->port);

    if (bind(listen_fd, (const struct sockaddr *)&address, sizeof(address)) < 0) {
        perror("cinderhttp: bind");
        close(listen_fd);
        return -1;
    }

    if (listen(listen_fd, SERVER_LISTEN_BACKLOG) < 0) {
        perror("cinderhttp: listen");
        close(listen_fd);
        return -1;
    }

    return listen_fd;
}

static void log_connection_if_verbose(const server_config_t *config,
                                      const struct sockaddr_in *addr) {
    if (!config->verbose) {
        return;
    }

    char ip[INET_ADDRSTRLEN];
    if (inet_ntop(AF_INET, &addr->sin_addr, ip, sizeof(ip)) == NULL) {
        (void)snprintf(ip, sizeof(ip), "?");
    }

    logger_verbose(1, "accepted connection from %s:%u", ip, ntohs(addr->sin_port));
}

int server_run(const server_config_t *config, int listen_fd) {
    /*
     * Initialization order and reverse cleanup on failure:
     *   logger → stats → queue → pool alloc → workers
     * Shutdown (happy path) drains the queue before destroy:
     *   queue_shutdown → join workers → destroy pool → queue → stats → logger
     */
    int logger_ready = 0;
    int stats_ready = 0;
    int queue_ready = 0;
    int pool_ready = 0;
    int workers_running = 0;
    int result = -1;

    server_stats_t stats;
    connection_queue_t queue;
    thread_pool_t pool;
    memset(&stats, 0, sizeof(stats));
    memset(&queue, 0, sizeof(queue));
    memset(&pool, 0, sizeof(pool));

    if (logger_init() != 0) {
        fprintf(stderr, "cinderhttp: logger_init failed\n");
        goto cleanup;
    }
    logger_ready = 1;

    if (server_stats_init(&stats) != 0) {
        fprintf(stderr, "cinderhttp: failed to initialize server stats\n");
        goto cleanup;
    }
    stats_ready = 1;

    if (connection_queue_init(&queue, (size_t)config->queue_capacity) != 0) {
        fprintf(stderr, "cinderhttp: failed to initialize connection queue\n");
        goto cleanup;
    }
    queue_ready = 1;

    if (thread_pool_init(&pool, (size_t)config->worker_count, &queue, config, &stats) != 0) {
        fprintf(stderr, "cinderhttp: failed to initialize thread pool\n");
        goto cleanup;
    }
    pool_ready = 1;

    if (thread_pool_start(&pool) != 0) {
        fprintf(stderr, "cinderhttp: failed to start worker threads\n");
        /* thread_pool_start already shut down the queue and joined partial workers. */
        goto cleanup;
    }
    workers_running = 1;

    printf("CinderHTTP listening on port %d (root=%s, workers=%d, queue=%d)\n", config->port,
           config->document_root, config->worker_count, config->queue_capacity);
    fflush(stdout);

    while (!shutdown_was_requested()) {
        struct sockaddr_in client_addr;
        socklen_t client_len = sizeof(client_addr);
        memset(&client_addr, 0, sizeof(client_addr));

        int client_fd = accept(listen_fd, (struct sockaddr *)&client_addr, &client_len);
        if (client_fd < 0) {
            if (errno == EINTR) {
                continue;
            }
            perror("cinderhttp: accept");
            continue;
        }

        server_stats_connection_accepted(&stats);
        log_connection_if_verbose(config, &client_addr);

        /*
         * Ownership: accept thread owns client_fd until push succeeds.
         * On shutdown/abort/error, push does not take ownership — close here.
         */
        connection_queue_status_t push_status =
            connection_queue_push(&queue, client_fd, &g_shutdown_requested);
        if (push_status != CONNECTION_QUEUE_OK) {
            close(client_fd);
            if (push_status == CONNECTION_QUEUE_SHUTDOWN || shutdown_was_requested()) {
                break;
            }
            logger_log("cinderhttp: failed to enqueue client connection");
            continue;
        }

        /* Fd entered the worker subsystem (queued or soon handled). */
        server_stats_connection_started(&stats);
    }

    /*
     * Graceful shutdown: stop accepting, wake waiters, drain already-queued
     * clients, join workers, then destroy subsystems in reverse order.
     */
    result = 0;

cleanup:
    if (workers_running || (pool_ready && queue_ready)) {
        /* Ensure workers cannot block forever if we failed after start, or
         * during normal shutdown after the accept loop. */
        if (queue_ready) {
            connection_queue_shutdown(&queue);
        }
        if (pool_ready) {
            thread_pool_join(&pool);
            workers_running = 0;
        }
    }
    if (pool_ready) {
        thread_pool_destroy(&pool);
        pool_ready = 0;
    }
    if (queue_ready) {
        connection_queue_destroy(&queue);
        queue_ready = 0;
    }
    if (stats_ready) {
        server_stats_destroy(&stats);
        stats_ready = 0;
    }
    if (logger_ready) {
        logger_destroy();
        logger_ready = 0;
    }
    return result;
}

void server_close_listening_socket(int listen_fd) {
    if (listen_fd >= 0) {
        close(listen_fd);
    }
}
