#include "client_handler.h"

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#include "http_parser.h"
#include "http_reader.h"
#include "http_request.h"
#include "http_response.h"
#include "logger.h"
#include "static_files.h"

#define STAGE2_POST_BODY "POST request parsed successfully.\n"

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

static void send_error_response(const server_config_t *config, int client_fd, int status,
                                int omit_body) {
    http_response_t response;
    http_response_init(&response);

    int built = -1;
    if (status == 404) {
        built = static_files_build_not_found(config->document_root, &response);
    } else {
        built = static_files_build_error(&response, status, error_heading_for_status(status));
    }

    if (built != 0) {
        http_response_destroy(&response);
        if (http_response_build_text(&response, status, error_heading_for_status(status)) != 0) {
            http_response_destroy(&response);
            return;
        }
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
    logger_verbose(1, "%s %s -> %d", http_method_to_string(request->method),
                   request->target != NULL ? request->target : "?", status);
}

static int build_post_response(http_response_t *response) {
    return http_response_build_text(response, 200, STAGE2_POST_BODY);
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

    http_response_destroy(response);
    if (status == 404) {
        if (static_files_build_not_found(config->document_root, response) != 0) {
            return 500;
        }
    } else {
        if (static_files_build_error(response, status, error_heading_for_status(status)) != 0) {
            return 500;
        }
    }
    return status;
}

void client_handle(const server_config_t *config, int client_fd, int worker_id) {
    unsigned char *raw = NULL;
    size_t raw_len = 0;
    http_request_t request;
    http_response_t response;
    http_request_init(&request);
    http_response_init(&response);

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
        send_error_response(config, client_fd, status_for_read_result(read_result), 0);
        goto cleanup;
    }

    http_parse_result_t parse_result = http_parse_request(raw, raw_len, &request);
    if (parse_result != HTTP_PARSE_OK) {
        int status = status_for_parse_result(parse_result);
        logger_verbose(config->verbose, "parse error -> %d", status);
        send_error_response(config, client_fd, status, 0);
        goto cleanup;
    }

    int omit_body = (request.method == HTTP_METHOD_HEAD) ? 1 : 0;
    int status = 500;

    if (request.method == HTTP_METHOD_GET || request.method == HTTP_METHOD_HEAD) {
        int load_body = (request.method == HTTP_METHOD_GET) ? 1 : 0;
        status = handle_static_get_head(config, &request, &response, load_body);
        if (status == 500 && response.body == NULL) {
            send_error_response(config, client_fd, 500, omit_body);
            goto cleanup;
        }
    } else if (request.method == HTTP_METHOD_POST) {
        if (build_post_response(&response) != 0) {
            send_error_response(config, client_fd, 500, 0);
            goto cleanup;
        }
        status = 200;
    } else {
        send_error_response(config, client_fd, 405, 0);
        goto cleanup;
    }

    log_request_if_verbose(config, &request, status);
    if (http_response_send(client_fd, &response, omit_body) != 0) {
        perror("cinderhttp: send response");
    }

cleanup:
    http_response_destroy(&response);
    http_request_destroy(&request);
    free(raw);
    close(client_fd);
}
