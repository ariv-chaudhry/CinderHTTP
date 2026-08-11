#include "server.h"

#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include "http_parser.h"
#include "http_reader.h"
#include "http_request.h"
#include "http_response.h"

/*
 * Kernel-level backlog of fully-established connections waiting for
 * accept(). This is distinct from the application-level, bounded
 * connection_queue_t introduced once worker threads exist: the backlog
 * below only protects the TCP handshake side of things, before our code
 * ever sees the socket.
 */
#define SERVER_LISTEN_BACKLOG 128

#define STAGE2_GET_BODY "CinderHTTP request parsed successfully.\n"
#define STAGE2_POST_BODY "POST request parsed successfully.\n"

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

static int status_for_read_result(http_read_result_t result) {
    switch (result) {
        case HTTP_READ_TOO_LARGE:
            return 413;
        case HTTP_READ_UNSUPPORTED_TRANSFER_ENCODING:
            return 501;
        case HTTP_READ_INVALID_CONTENT_LENGTH:
        case HTTP_READ_BAD_REQUEST:
            return 400;
        case HTTP_READ_OUT_OF_MEMORY:
            return 500;
        default:
            return 400;
    }
}

static int status_for_parse_result(http_parse_result_t result) {
    switch (result) {
        case HTTP_PARSE_OK:
            return 200;
        case HTTP_PARSE_UNSUPPORTED_METHOD:
            return 405;
        case HTTP_PARSE_UNSUPPORTED_VERSION:
            return 505;
        case HTTP_PARSE_TOO_LARGE:
            return 413;
        case HTTP_PARSE_TOO_MANY_HEADERS:
            return 400;
        case HTTP_PARSE_INVALID_CONTENT_LENGTH:
            return 400;
        case HTTP_PARSE_UNSUPPORTED_TRANSFER_ENCODING:
            return 501;
        case HTTP_PARSE_OUT_OF_MEMORY:
            return 500;
        case HTTP_PARSE_BAD_REQUEST:
        default:
            return 400;
    }
}

static const char *error_body_for_status(int status) {
    switch (status) {
        case 400:
            return "Bad Request\n";
        case 405:
            return "Method Not Allowed\n";
        case 413:
            return "Payload Too Large\n";
        case 501:
            return "Not Implemented\n";
        case 505:
            return "HTTP Version Not Supported\n";
        case 500:
            return "Internal Server Error\n";
        default:
            return "Error\n";
    }
}

/*
 * Temporary Stage 2 handler: no router yet. GET/HEAD share one body string;
 * POST gets a different confirmation body. HEAD omits the body on the wire
 * but keeps Content-Length equal to the GET body size.
 */
static int build_success_response(const http_request_t *request, http_response_t *response) {
    const char *body = STAGE2_GET_BODY;
    if (request->method == HTTP_METHOD_POST) {
        body = STAGE2_POST_BODY;
    }
    return http_response_build_text(response, 200, body);
}

static void send_error_response(int client_fd, int status, int omit_body) {
    http_response_t response;
    http_response_init(&response);
    if (http_response_build_text(&response, status, error_body_for_status(status)) != 0) {
        http_response_destroy(&response);
        return;
    }
    if (http_response_send(client_fd, &response, omit_body) != 0) {
        perror("cinderhttp: send response");
    }
    http_response_destroy(&response);
}

static void log_request_if_verbose(const server_config_t *config, const http_request_t *request,
                                   int status) {
    if (!config->verbose || request == NULL) {
        return;
    }
    fprintf(stderr, "[verbose] %s %s -> %d\n", http_method_to_string(request->method),
            request->target != NULL ? request->target : "?", status);
}

/*
 * Per-connection Stage 2 lifecycle:
 *   read one framed message -> parse -> build response -> send -> cleanup.
 * Connection: close after exactly one request (keep-alive deferred).
 */
static void handle_client(const server_config_t *config, int client_fd) {
    unsigned char *raw = NULL;
    size_t raw_len = 0;
    http_request_t request;
    http_response_t response;
    http_request_init(&request);
    http_response_init(&response);

    http_read_result_t read_result = http_read_request(client_fd, &raw, &raw_len);
    if (read_result == HTTP_READ_CLIENT_CLOSED) {
        /* No complete request (including zero-byte connects). Close quietly. */
        goto cleanup;
    }
    if (read_result == HTTP_READ_IO_ERROR) {
        perror("cinderhttp: recv");
        goto cleanup;
    }
    if (read_result != HTTP_READ_OK) {
        send_error_response(client_fd, status_for_read_result(read_result), 0);
        goto cleanup;
    }

    http_parse_result_t parse_result = http_parse_request(raw, raw_len, &request);
    if (parse_result != HTTP_PARSE_OK) {
        int status = status_for_parse_result(parse_result);
        if (config->verbose) {
            fprintf(stderr, "[verbose] parse error -> %d\n", status);
        }
        send_error_response(client_fd, status, 0);
        goto cleanup;
    }

    if (build_success_response(&request, &response) != 0) {
        send_error_response(client_fd, 500, 0);
        goto cleanup;
    }

    int omit_body = (request.method == HTTP_METHOD_HEAD) ? 1 : 0;
    log_request_if_verbose(config, &request, response.status_code);
    if (http_response_send(client_fd, &response, omit_body) != 0) {
        perror("cinderhttp: send response");
    }

cleanup:
    http_response_destroy(&response);
    http_request_destroy(&request);
    free(raw);
    close(client_fd);
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
        handle_client(config, client_fd);
    }

    return 0;
}

void server_close_listening_socket(int listen_fd) {
    if (listen_fd >= 0) {
        close(listen_fd);
    }
}
