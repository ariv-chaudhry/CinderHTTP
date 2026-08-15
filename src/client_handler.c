#include "client_handler.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include "http_connection.h"
#include "http_limits.h"
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
        case HTTP_READ_TIMEOUT:
            return 408;
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
        case 408:
            return "Request Timeout";
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
 * Protocol / framing errors make the remaining stream unsafe → must close.
 * Application errors (404/405/…) may keep the connection when framing is OK.
 */
static int status_requires_close(int status) {
    switch (status) {
        case 400:
        case 408:
        case 413:
        case 501:
        case 505:
            return 1;
        case 500:
            return 1; /* prefer safety when internal state may be uncertain */
        default:
            return 0;
    }
}

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

static int set_recv_timeout(int client_fd, int timeout_sec) {
    struct timeval tv;
    tv.tv_sec = timeout_sec;
    tv.tv_usec = 0;
    if (setsockopt(client_fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv)) != 0) {
        return -1;
    }
    return 0;
}

/*
 * Per-connection lifecycle (Stage 9):
 *   active_connections already +1 after successful queue push
 *   set SO_RCVTIMEO
 *   loop:
 *     read next framed request (may use leftover buffer bytes)
 *     parse → route/static → apply Connection policy → send
 *     close or continue
 *   finish active accounting → close fd exactly once
 *
 * Ownership of client_fd transfers here and is closed on every exit path.
 * The reader never closes the socket.
 */
void client_handle(const server_config_t *config, server_stats_t *stats, int client_fd,
                   int worker_id) {
    http_reader_t reader;
    http_reader_init(&reader);

    int requests_on_connection = 0;

    if (worker_id >= 0) {
        logger_verbose(config->verbose, "worker %d handling fd %d", worker_id, client_fd);
    }

    if (set_recv_timeout(client_fd, config->keep_alive_timeout_sec) != 0) {
        perror("cinderhttp: SO_RCVTIMEO");
        /* Continue with default (possibly infinite) timeout rather than abort. */
    }

    for (;;) {
        unsigned char *raw = NULL;
        size_t raw_len = 0;
        http_request_t request;
        http_response_t response;
        http_request_init(&request);
        http_response_init(&response);

        int omit_body = 0;
        int status = -1;
        int response_ready = 0;
        int keep_alive = 0;
        int force_close = 0;
        size_t buffered_before = reader.length;

        http_read_result_t read_result =
            http_reader_next_request(&reader, client_fd, &raw, &raw_len);

        if (read_result == HTTP_READ_CLIENT_CLOSED) {
            if (worker_id >= 0) {
                logger_verbose(config->verbose, "fd %d closed: client closed", client_fd);
            }
            http_response_destroy(&response);
            http_request_destroy(&request);
            free(raw);
            break;
        }

        if (read_result == HTTP_READ_TIMEOUT) {
            if (buffered_before == 0 && reader.length == 0) {
                /* Idle keep-alive wait: silent close. */
                if (worker_id >= 0) {
                    logger_verbose(config->verbose, "fd %d closed: idle timeout", client_fd);
                }
                http_response_destroy(&response);
                http_request_destroy(&request);
                free(raw);
                break;
            }
            /* Partial request timed out → 408 + close. */
            status = build_static_error_response(config, &response, 408);
            if (status >= 0) {
                response_ready = 1;
                force_close = 1;
            }
            goto send_one;
        }

        if (read_result == HTTP_READ_IO_ERROR) {
            perror("cinderhttp: recv");
            http_response_destroy(&response);
            http_request_destroy(&request);
            free(raw);
            break;
        }

        if (read_result != HTTP_READ_OK) {
            status = build_static_error_response(config, &response,
                                                 status_for_read_result(read_result));
            if (status >= 0) {
                response_ready = 1;
                force_close = 1;
            }
            goto send_one;
        }

        server_stats_request_received(stats);

        http_parse_result_t parse_result = http_parse_request(raw, raw_len, &request);
        if (parse_result != HTTP_PARSE_OK) {
            status = status_for_parse_result(parse_result);
            logger_verbose(config->verbose, "parse error -> %d", status);
            if (build_static_error_response(config, &response, status) >= 0) {
                response_ready = 1;
                force_close = 1;
            } else {
                status = -1;
                force_close = 1;
            }
            goto send_one;
        }

        omit_body = (request.method == HTTP_METHOD_HEAD) ? 1 : 0;
        keep_alive = http_request_wants_keep_alive(&request);

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
                force_close = 1;
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
            status = build_static_error_response(config, &response, 405);
            if (status >= 0) {
                response_ready = 1;
            } else {
                status = -1;
            }
        }

        if (status >= 0 && status_requires_close(status)) {
            force_close = 1;
            keep_alive = 0;
        }

    send_one:
        requests_on_connection++;
        if (requests_on_connection >= HTTP_MAX_REQUESTS_PER_CONNECTION) {
            force_close = 1;
            keep_alive = 0;
            if (worker_id >= 0) {
                logger_verbose(config->verbose, "fd %d: max requests reached", client_fd);
            }
        }

        if (response_ready && status >= 0) {
            int persist = keep_alive && !force_close;
            if (http_response_apply_connection_policy(&response, persist, request.version) != 0) {
                /* OOM applying Connection — force close after best-effort send. */
                force_close = 1;
                persist = 0;
                (void)http_response_apply_connection_policy(&response, 0, request.version);
            }

            log_request_if_verbose(config, &request, status);
            if (http_response_send(client_fd, &response, omit_body) != 0) {
                perror("cinderhttp: send response");
                force_close = 1;
            } else {
                server_stats_response_sent(stats, status);
            }
        } else {
            force_close = 1;
        }

        http_response_destroy(&response);
        http_request_destroy(&request);
        free(raw);

        if (force_close || !keep_alive) {
            if (worker_id >= 0 && force_close) {
                logger_verbose(config->verbose, "fd %d closed: connection close policy", client_fd);
            }
            break;
        }
        /* Persist: loop for next request; leftovers remain in reader. */
    }

    http_reader_destroy(&reader);
    close(client_fd);
    server_stats_connection_finished(stats);
}
