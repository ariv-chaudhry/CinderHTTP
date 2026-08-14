/*
 * router.h - application-layer route matching for the /api/ namespace.
 *
 * The router matches method + path (query string ignored). It distinguishes
 * unknown paths (404) from known paths with unsupported methods (405 + Allow).
 *
 * Paths under /api/ never fall through to the static file server. Non-API
 * paths are detected by router_is_api_request() so client_handler can dispatch
 * to static_files_serve() instead.
 */
#ifndef CINDERHTTP_ROUTER_H
#define CINDERHTTP_ROUTER_H

#include "config.h"
#include "http_request.h"
#include "http_response.h"
#include "server_stats.h"

typedef struct {
    const server_config_t *config;
    server_stats_t *stats;
} router_context_t;

typedef enum {
    ROUTER_HANDLED = 0, /* response fully populated */
    ROUTER_ERROR        /* allocation / internal failure */
} router_result_t;

/* True if the request target's path is in the reserved /api namespace. */
int router_is_api_request(const http_request_t *request);

/*
 * Dispatch an API request. Caller must only invoke this when
 * router_is_api_request() is true. On ROUTER_HANDLED, *response is ready to
 * send (omit body for HEAD at send time).
 */
router_result_t router_dispatch(const router_context_t *ctx, const http_request_t *request,
                                http_response_t *response);

#endif /* CINDERHTTP_ROUTER_H */
