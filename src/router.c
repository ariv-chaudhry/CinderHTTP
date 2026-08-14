#include "router.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef int (*route_handler_fn)(const router_context_t *ctx, const http_request_t *request,
                                http_response_t *response);

typedef struct {
    http_method_t method;
    const char *path;
    route_handler_fn handler;
} route_t;

/*
 * Compare request target path (before '?') to an exact route path.
 * Trailing slashes are significant: /api/health/ does not match /api/health.
 */
static int path_equals(const char *target, const char *route_path) {
    if (target == NULL || route_path == NULL) {
        return 0;
    }

    size_t i = 0;
    while (target[i] != '\0' && target[i] != '?' && route_path[i] != '\0') {
        if (target[i] != route_path[i]) {
            return 0;
        }
        i++;
    }

    if (route_path[i] != '\0') {
        return 0;
    }
    return target[i] == '\0' || target[i] == '?';
}

static int path_has_api_prefix(const char *target) {
    if (target == NULL) {
        return 0;
    }

    /* Exact "/api" or anything under "/api/..." (query ignored). */
    if (path_equals(target, "/api")) {
        return 1;
    }

    const char *prefix = "/api/";
    size_t i = 0;
    while (prefix[i] != '\0') {
        if (target[i] == '\0' || target[i] == '?' || target[i] != prefix[i]) {
            return 0;
        }
        i++;
    }
    return 1;
}

int router_is_api_request(const http_request_t *request) {
    if (request == NULL) {
        return 0;
    }
    return path_has_api_prefix(request->target);
}

static int method_compatible(http_method_t route_method, http_method_t request_method) {
    if (route_method == request_method) {
        return 1;
    }
    /* HEAD may invoke a GET handler; body omission happens at send time. */
    if (route_method == HTTP_METHOD_GET && request_method == HTTP_METHOD_HEAD) {
        return 1;
    }
    return 0;
}

static int handle_health(const router_context_t *ctx, const http_request_t *request,
                         http_response_t *response) {
    (void)ctx;
    (void)request;
    return http_response_build_json(response, 200, "{\"status\":\"ok\"}");
}

static int handle_echo(const router_context_t *ctx, const http_request_t *request,
                       http_response_t *response) {
    (void)ctx;
    if (request == NULL || response == NULL) {
        return -1;
    }

    http_response_destroy(response);
    http_response_init(response);
    http_response_set_status(response, 200);

    if (http_response_set_body_copy(response, request->body, request->body_length) != 0) {
        return -1;
    }

    const char *content_type = http_request_get_header(request, "Content-Type");
    if (content_type == NULL || content_type[0] == '\0') {
        content_type = "application/octet-stream";
    }
    if (http_response_add_header(response, "Content-Type", content_type) != 0) {
        return -1;
    }
    return 0;
}

static int handle_stats(const router_context_t *ctx, const http_request_t *request,
                        http_response_t *response) {
    (void)request;
    if (ctx == NULL || ctx->stats == NULL || response == NULL) {
        return -1;
    }

    server_stats_snapshot_t snap;
    server_stats_snapshot(ctx->stats, &snap);

    char json[512];
    int n = snprintf(json, sizeof(json),
                     "{\"connections_accepted\":%" PRIu64 ",\"requests_total\":%" PRIu64
                     ",\"responses_2xx\":%" PRIu64 ",\"responses_4xx\":%" PRIu64
                     ",\"responses_5xx\":%" PRIu64 ",\"active_connections\":%" PRIu64 "}",
                     snap.connections_accepted, snap.requests_total, snap.responses_2xx,
                     snap.responses_4xx, snap.responses_5xx, snap.active_connections);
    if (n < 0 || (size_t)n >= sizeof(json)) {
        return -1;
    }

    return http_response_build_json(response, 200, json);
}

static const route_t ROUTES[] = {
    {HTTP_METHOD_GET, "/api/health", handle_health},
    {HTTP_METHOD_POST, "/api/echo", handle_echo},
    {HTTP_METHOD_GET, "/api/stats", handle_stats},
};

static const size_t ROUTE_COUNT = sizeof(ROUTES) / sizeof(ROUTES[0]);

static int build_allow_header(const char *path, char *out, size_t out_size) {
    if (path == NULL || out == NULL || out_size == 0) {
        return -1;
    }

    int has_get = 0;
    int has_post = 0;
    for (size_t i = 0; i < ROUTE_COUNT; i++) {
        if (strcmp(ROUTES[i].path, path) != 0) {
            continue;
        }
        if (ROUTES[i].method == HTTP_METHOD_GET) {
            has_get = 1;
        } else if (ROUTES[i].method == HTTP_METHOD_POST) {
            has_post = 1;
        }
    }

    out[0] = '\0';
    size_t used = 0;
    if (has_get) {
        int n = snprintf(out + used, out_size - used, "GET, HEAD");
        if (n < 0 || (size_t)n >= out_size - used) {
            return -1;
        }
        used += (size_t)n;
    }
    if (has_post) {
        int n = snprintf(out + used, out_size - used, "%sPOST", used > 0 ? ", " : "");
        if (n < 0 || (size_t)n >= out_size - used) {
            return -1;
        }
    }
    return (out[0] != '\0') ? 0 : -1;
}

static int build_api_error(http_response_t *response, int status, const char *json_error,
                           const char *allow) {
    if (http_response_build_json(response, status, json_error) != 0) {
        return -1;
    }
    if (allow != NULL && allow[0] != '\0') {
        if (http_response_add_header(response, "Allow", allow) != 0) {
            return -1;
        }
    }
    return 0;
}

router_result_t router_dispatch(const router_context_t *ctx, const http_request_t *request,
                                http_response_t *response) {
    if (ctx == NULL || request == NULL || response == NULL || request->target == NULL) {
        return ROUTER_ERROR;
    }

    int path_exists = 0;
    const char *matched_path = NULL;

    for (size_t i = 0; i < ROUTE_COUNT; i++) {
        if (!path_equals(request->target, ROUTES[i].path)) {
            continue;
        }
        path_exists = 1;
        matched_path = ROUTES[i].path;

        if (method_compatible(ROUTES[i].method, request->method)) {
            if (ROUTES[i].handler(ctx, request, response) != 0) {
                return ROUTER_ERROR;
            }
            return ROUTER_HANDLED;
        }
    }

    if (path_exists) {
        char allow[64];
        if (build_allow_header(matched_path, allow, sizeof(allow)) != 0) {
            allow[0] = '\0';
        }
        if (build_api_error(response, 405, "{\"error\":\"method not allowed\"}", allow) != 0) {
            return ROUTER_ERROR;
        }
        return ROUTER_HANDLED;
    }

    if (build_api_error(response, 404, "{\"error\":\"not found\"}", NULL) != 0) {
        return ROUTER_ERROR;
    }
    return ROUTER_HANDLED;
}
