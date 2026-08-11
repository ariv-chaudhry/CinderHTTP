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

#include "utils.h"

/*
 * Kernel-level backlog of fully-established connections waiting for
 * accept(). This is distinct from the application-level, bounded
 * connection_queue_t introduced once worker threads exist: the backlog
 * below only protects the TCP handshake side of things, before our code
 * ever sees the socket.
 */
#define SERVER_LISTEN_BACKLOG 128

/*
 * Fixed response body sent for every connection in this stage. There is no
 * HTTP parsing yet - server_run() below exists only to prove the accept
 * loop and socket plumbing work end-to-end. Real per-request responses
 * begin once the HTTP parser and router exist (see docs/roadmap.md).
 */
#define STAGE1_RESPONSE_BODY "CinderHTTP works!\n"

/*
 * Set by the signal handler, polled by the accept loop. sig_atomic_t is the
 * only type ISO C guarantees can be assigned atomically with respect to
 * asynchronous signal delivery; `volatile` prevents the compiler from
 * hoisting reads of this flag out of the accept loop.
 */
static volatile sig_atomic_t g_shutdown_requested = 0;

static void handle_shutdown_signal(int signum) {
    /* Signal handlers must be async-signal-safe: no I/O, no malloc, nothing
     * beyond simple flag updates. Setting an existing sig_atomic_t is safe. */
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
     * SA_RESTART is deliberately omitted. If it were set, the kernel would
     * silently restart accept() after the handler ran, and the accept loop
     * would never observe g_shutdown_requested until the *next* client
     * connected - potentially never, if traffic has stopped. Leaving
     * SA_RESTART off makes accept() fail with EINTR instead, so the loop
     * below always rechecks the shutdown flag immediately.
     */
    action.sa_flags = 0;

    if (sigaction(SIGINT, &action, NULL) < 0) {
        perror("cinderhttp: sigaction(SIGINT)");
    }
    if (sigaction(SIGTERM, &action, NULL) < 0) {
        perror("cinderhttp: sigaction(SIGTERM)");
    }
}

int server_create_listening_socket(const server_config_t *config) {
    int listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (listen_fd < 0) {
        perror("cinderhttp: socket");
        return -1;
    }

    /*
     * Without SO_REUSEADDR, restarting the server shortly after a previous
     * run can fail with "Address already in use" while the old socket's
     * connections linger in TIME_WAIT.
     */
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

/*
 * Sends the fixed Stage 1 response and closes the connection. The
 * Content-Length header is computed from strlen(STAGE1_RESPONSE_BODY)
 * rather than written as a separate literal, so the header can never drift
 * out of sync with the body it describes.
 */
static void handle_client_stage1(int client_fd) {
    char response[256];
    int response_len = snprintf(response, sizeof(response),
                                 "HTTP/1.1 200 OK\r\n"
                                 "Server: CinderHTTP/1.0\r\n"
                                 "Content-Type: text/plain\r\n"
                                 "Content-Length: %zu\r\n"
                                 "Connection: close\r\n"
                                 "\r\n"
                                 "%s",
                                 strlen(STAGE1_RESPONSE_BODY), STAGE1_RESPONSE_BODY);

    if (response_len > 0 && (size_t)response_len < sizeof(response)) {
        if (send_all(client_fd, response, (size_t)response_len) < 0) {
            perror("cinderhttp: send_all");
        }
    } else {
        fprintf(stderr, "cinderhttp: stage-1 response did not fit in its buffer\n");
    }

    close(client_fd);
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

    fprintf(stderr, "[verbose] connection from %s:%u\n", ip, ntohs(addr->sin_port));
}

int server_run(const server_config_t *config, int listen_fd) {
    printf("CinderHTTP listening on port %d (press Ctrl+C to stop)\n", config->port);

    while (!shutdown_was_requested()) {
        struct sockaddr_in client_addr;
        socklen_t client_len = sizeof(client_addr);
        memset(&client_addr, 0, sizeof(client_addr));

        int client_fd = accept(listen_fd, (struct sockaddr *)&client_addr, &client_len);
        if (client_fd < 0) {
            if (errno == EINTR) {
                /* Either the shutdown signal or something benign; let the
                 * while-condition above decide whether to keep looping. */
                continue;
            }
            /* A single misbehaving client/kernel hiccup must not take down
             * the whole server. */
            perror("cinderhttp: accept");
            continue;
        }

        log_connection_if_verbose(config, &client_addr);
        handle_client_stage1(client_fd);
    }

    return 0;
}

void server_close_listening_socket(int listen_fd) {
    if (listen_fd >= 0) {
        close(listen_fd);
    }
}
