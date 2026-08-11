/*
 * server.h - listening socket lifecycle and the accept loop.
 *
 * Stage 2: the accept loop still handles each client inline (no worker pool
 * yet). Per connection it reads one framed HTTP request, parses it, builds a
 * response, and closes the socket (Connection: close). Starting with the
 * connection queue and worker pool (see docs/roadmap.md), server_run() will
 * instead push accepted file descriptors into a bounded queue; that change
 * is confined to server.c.
 */
#ifndef CINDERHTTP_SERVER_H
#define CINDERHTTP_SERVER_H

#include "config.h"

/*
 * Installs SIGINT/SIGTERM handlers that request a graceful shutdown. Must
 * be called once during startup, before server_run(). See server.c for the
 * async-signal-safety reasoning behind the handler's design.
 */
void server_install_signal_handlers(void);

/*
 * Creates a TCP listening socket bound to config->port and starts listening
 * for connections. On success, returns a non-negative file descriptor. On
 * failure, returns -1; a diagnostic has already been printed to stderr.
 */
int server_create_listening_socket(const server_config_t *config);

/*
 * Runs the accept loop until a shutdown signal (SIGINT/SIGTERM) is
 * observed. Ownership of `listen_fd` remains with the caller: this function
 * never closes it, so the caller can always call
 * server_close_listening_socket() afterwards regardless of how the loop
 * exited. Returns 0 after a clean, signal-driven shutdown.
 */
int server_run(const server_config_t *config, int listen_fd);

/* Closes the listening socket. Safe to call with a negative fd. */
void server_close_listening_socket(int listen_fd);

#endif /* CINDERHTTP_SERVER_H */
