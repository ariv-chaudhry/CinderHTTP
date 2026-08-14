/*
 * client_handler.h - per-connection HTTP request pipeline.
 *
 * Ownership: client_handle() takes ownership of client_fd and closes it
 * exactly once before returning. The caller must not use or close the fd
 * afterwards. stats is borrowed (non-owning) for the duration of the call.
 */
#ifndef CINDERHTTP_CLIENT_HANDLER_H
#define CINDERHTTP_CLIENT_HANDLER_H

#include "config.h"
#include "server_stats.h"

/*
 * Read one framed HTTP message, parse it, dispatch via the /api router or
 * static-file fallback, send the response, update stats, then close client_fd.
 *
 * worker_id is optional for verbose logging (-1 to omit).
 */
void client_handle(const server_config_t *config, server_stats_t *stats, int client_fd,
                   int worker_id);

#endif /* CINDERHTTP_CLIENT_HANDLER_H */
