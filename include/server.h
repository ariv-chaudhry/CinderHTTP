/*
 * server.h - listening socket lifecycle and the accept/orchestration loop.
 *
 * The accept thread only accepts connections and enqueues client file
 * descriptors. A fixed worker pool dequeues and runs client_handle().
 *
 * Shutdown policy (Stage 6): SIGINT/SIGTERM set a sig_atomic_t flag only.
 * The accept loop exits, the queue enters shutdown, workers drain already
 * queued clients, then workers are joined and subsystems are destroyed.
 * SIGPIPE is ignored so a disconnected client cannot kill the process.
 */
#ifndef CINDERHTTP_SERVER_H
#define CINDERHTTP_SERVER_H

#include "config.h"

/*
 * Installs SIGINT/SIGTERM handlers that request a graceful shutdown, and
 * ignores SIGPIPE. Must be called once during startup, before server_run().
 * See server.c for async-signal-safety and shutdown sequencing.
 */
void server_install_signal_handlers(void);

/*
 * Creates a TCP listening socket bound to config->port and starts listening
 * for connections. On success, returns a non-negative file descriptor. On
 * failure, returns -1; a diagnostic has already been printed to stderr.
 */
int server_create_listening_socket(const server_config_t *config);

/*
 * Initializes logger/stats/queue/worker pool, then runs the accept loop until
 * SIGINT/SIGTERM. Ownership of listen_fd remains with the caller.
 * Returns 0 after a clean, signal-driven shutdown, or -1 if startup failed
 * (partial initialization is rolled back before returning).
 */
int server_run(const server_config_t *config, int listen_fd);

/* Closes the listening socket. Safe to call with a negative fd. */
void server_close_listening_socket(int listen_fd);

#endif /* CINDERHTTP_SERVER_H */
