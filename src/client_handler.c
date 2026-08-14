#include "client_handler.h"

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#include "http_parser.h"
#include "http_reader.h"
#include "http_request.h"
#include "http_response.h"
#include "logger.h"
#include "router.h"
#include "static_files.h"

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

static int status_for_static_result(static_file_result_t result) {
    switch (result) {
        case STATIC_FILE_OK:
            return 200;
        case STATIC_FILE_NOT_FOUND:
            return 404;
        case STATIC_FILE_FORBIDDEN:
            return 403;
        case STATIC_FILE_BAD_TARGET:
            return 400;
        case STATIC_FILE_TOO_LARGE:
            return 413;
        case STATIC_FILE_OUT_OF_MEMORY:
        case STATIC_FILE_IO_ERROR:
        default:
            return 500;
    }
}

static const char *error_heading_for_status(int status) {
    switch (status) {
        case 400:
            return "Bad Request";
        case 403:
            return "Forbidden";
        case 404:
            return "Not Found";
        case 405:
            return "Method Not Allowed";
        case 413:
            return "Payload Too Large";
        case 501:
            return "Not Implemented";
        case 505:
            return "HTTP Version Not Supported";
        case 500:
        default:
            return "Internal Server Error";
    }
}

/*
 * Build a non-API HTML/text error into *response. Returns the status code on
 * success, or -1 if construction failed (caller should not send).
 */
static int build_static_error_response(const server_config_t *config, http_response_t *response,
                                       int status) {
    http_response_destroy(response);
    http_response_init(response);

    int built = -1;
    if (status == 404) {
        built = static_files_build_not_found(config->document_root, response);
    } else {
        built = static_files_build_error(response, status, error_heading_for_status(status));
    }

    if (built != 0) {
        http_response_destroy(response);
        if (http_response_build_text(response, status, error_heading_for_status(status)) != 0) {
            return -1;
        }
    }
    return status;
}

static void log_request_if_verbose(const server_config_t *config, const http_request_t *request,
                                   int status) {
    if (!config->verbose || request == NULL) {
        return;
    }
    logger_verbose(1, "%s %s -> %d", http_method_to_string(request->method),
                   request->target != NULL ? request->target : "?", status);
}

static int handle_static_get_head(const server_config_t *config, const http_request_t *request,
                                  http_response_t *response, int load_body) {
    static_file_result_t result =
        static_files_serve(config->document_root, request, response, load_body);

    if (result == STATIC_FILE_OK) {
        return 200;
    }

    int status = status_for_static_result(result);
    logger_verbose(config->verbose, "static resolve -> %d", status);
    return build_static_error_response(config, response, status);
}

/*
 * Per-connection lifecycle:
 *   start active accounting -> read -> parse -> route/static -> send once ->
 *   response accounting -> finish active accounting -> close.
 *
 * Exactly one response class counter is incremented when an HTTP response is
 * successfully sent. I/O failures with no HTTP response do not invent a status.
 */
void client_handle(const server_config_t *config, server_stats_t *stats, int client_fd,
                   int worker_id) {
    unsigned char *raw = NULL;
    size_t raw_len = 0;
    http_request_t request;
    http_response_t response;
    http_request_init(&request);
    http_response_init(&response);

    int omit_body = 0;
    int status = -1;
    int response_ready = 0;
    int response_counted = 0;

    if (worker_id >= 0) {
        logger_verbose(config->verbose, "worker %d handling fd %d", worker_id, client_fd);
    }

    http_read_result_t read_result = http_read_request(client_fd, &raw, &raw_len);
    if (read_result == HTTP_READ_CLIENT_CLOSED) {
        goto cleanup;
    }
    if (read_result == HTTP_READ_IO_ERROR) {
        perror("cinderhttp: recv");
        goto cleanup;
    }
    if (read_result != HTTP_READ_OK) {
        status = build_static_error_response(config, &response, status_for_read_result(read_result));
        if (status >= 0) {
            response_ready = 1;
        }
        goto send_and_cleanup;
    }

    /* Preferred requests_total point: complete framed message ready to parse. */
    server_stats_request_received(stats);

    http_parse_result_t parse_result = http_parse_request(raw, raw_len, &request);
    if (parse_result != HTTP_PARSE_OK) {
        status = status_for_parse_result(parse_result);
        logger_verbose(config->verbose, "parse error -> %d", status);
        if (build_static_error_response(config, &response, status) >= 0) {
            response_ready = 1;
        } else {
            status = -1;
        }
        goto send_and_cleanup;
    }

    omit_body = (request.method == HTTP_METHOD_HEAD) ? 1 : 0;

    if (router_is_api_request(&request)) {
        router_context_t ctx = {.config = config, .stats = stats};
        router_result_t routed = router_dispatch(&ctx, &request, &response);
        if (routed != ROUTER_HANDLED) {
            status = build_static_error_response(config, &response, 500);
            if (status >= 0) {
                response_ready = 1;
            } else {
                status = -1;
            }
        } else {
            status = response.status_code;
            response_ready = 1;
        }
    } else if (request.method == HTTP_METHOD_GET || request.method == HTTP_METHOD_HEAD) {
        int load_body = (request.method == HTTP_METHOD_GET) ? 1 : 0;
        status = handle_static_get_head(config, &request, &response, load_body);
        if (status >= 0) {
            response_ready = 1;
        }
    } else {
        /* Non-API POST/other methods are not supported on static resources. */
        status = build_static_error_response(config, &response, 405);
        if (status >= 0) {
            response_ready = 1;
        } else {
            status = -1;
        }
    }

send_and_cleanup:
    if (response_ready && status >= 0) {
        log_request_if_verbose(config, &request, status);
        if (http_response_send(client_fd, &response, omit_body) != 0) {
            perror("cinderhttp: send response");
        } else {
            server_stats_response_sent(stats, status);
            response_counted = 1;
        }
    }
    (void)response_counted;

cleanup:
    http_response_destroy(&response);
    http_request_destroy(&request);
    free(raw);
    close(client_fd);
    server_stats_connection_finished(stats);
}
