#include "http_reader.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>

#include "http_limits.h"
#include "http_parser.h"

#define HTTP_READER_INITIAL_CAPACITY 4096
#define HTTP_READER_RECV_CHUNK 4096

static http_read_result_t map_framing_error(http_parse_result_t result) {
    switch (result) {
        case HTTP_PARSE_OK:
            return HTTP_READ_OK;
        case HTTP_PARSE_TOO_LARGE:
            return HTTP_READ_TOO_LARGE;
        case HTTP_PARSE_UNSUPPORTED_TRANSFER_ENCODING:
            return HTTP_READ_UNSUPPORTED_TRANSFER_ENCODING;
        case HTTP_PARSE_INVALID_CONTENT_LENGTH:
            return HTTP_READ_INVALID_CONTENT_LENGTH;
        case HTTP_PARSE_OUT_OF_MEMORY:
            return HTTP_READ_OUT_OF_MEMORY;
        default:
            return HTTP_READ_BAD_REQUEST;
    }
}

static int ensure_capacity(unsigned char **buffer, size_t *capacity, size_t needed) {
    if (needed <= *capacity) {
        return 1;
    }
    if (needed > HTTP_MAX_MESSAGE_BYTES) {
        return 0;
    }

    size_t new_capacity = *capacity;
    if (new_capacity == 0) {
        new_capacity = HTTP_READER_INITIAL_CAPACITY;
    }
    while (new_capacity < needed) {
        if (new_capacity > HTTP_MAX_MESSAGE_BYTES / 2) {
            new_capacity = HTTP_MAX_MESSAGE_BYTES;
            break;
        }
        new_capacity *= 2;
    }
    if (new_capacity < needed || new_capacity > HTTP_MAX_MESSAGE_BYTES) {
        return 0;
    }

    unsigned char *grown = realloc(*buffer, new_capacity);
    if (grown == NULL) {
        return 0;
    }
    *buffer = grown;
    *capacity = new_capacity;
    return 1;
}

http_read_result_t http_read_request(int client_fd, unsigned char **buffer,
                                     size_t *buffer_length) {
    if (buffer == NULL || buffer_length == NULL) {
        return HTTP_READ_BAD_REQUEST;
    }
    *buffer = NULL;
    *buffer_length = 0;

    unsigned char *data = NULL;
    size_t capacity = 0;
    size_t length = 0;
    size_t header_end = (size_t)-1; /* index of first \r of \r\n\r\n */
    size_t body_length = 0;
    int headers_complete = 0;

    for (;;) {
        size_t need = length + HTTP_READER_RECV_CHUNK;
        if (!headers_complete) {
            /* While hunting for the header terminator, refuse to grow past
             * the header-byte ceiling (plus a little room for the recv). */
            if (length >= HTTP_MAX_HEADER_BYTES) {
                free(data);
                return HTTP_READ_TOO_LARGE;
            }
            if (need > HTTP_MAX_HEADER_BYTES) {
                need = HTTP_MAX_HEADER_BYTES;
            }
            if (need <= length) {
                free(data);
                return HTTP_READ_TOO_LARGE;
            }
        } else {
            if (body_length > HTTP_MAX_MESSAGE_BYTES - (header_end + 4)) {
                free(data);
                return HTTP_READ_TOO_LARGE;
            }
            size_t total_needed = header_end + 4 + body_length;
            if (length >= total_needed) {
                break;
            }
            need = total_needed;
        }

        if (!ensure_capacity(&data, &capacity, need)) {
            free(data);
            if (need > HTTP_MAX_MESSAGE_BYTES) {
                return HTTP_READ_TOO_LARGE;
            }
            return HTTP_READ_OUT_OF_MEMORY;
        }

        size_t space = capacity - length;
        if (space == 0) {
            free(data);
            return HTTP_READ_TOO_LARGE;
        }

        ssize_t n = recv(client_fd, data + length, space, 0);
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            free(data);
            return HTTP_READ_IO_ERROR;
        }
        if (n == 0) {
            /* Peer closed. No bytes at all → clean close with no response.
             * Partial message → also treated as client closed (Stage 2 does
             * not attempt to answer incomplete requests). */
            free(data);
            return HTTP_READ_CLIENT_CLOSED;
        }

        length += (size_t)n;

        if (!headers_complete) {
            header_end = http_find_header_terminator(data, length);
            if (header_end == (size_t)-1) {
                if (length >= HTTP_MAX_HEADER_BYTES) {
                    free(data);
                    return HTTP_READ_TOO_LARGE;
                }
                continue;
            }

            size_t header_bytes = header_end + 4;
            if (header_bytes > HTTP_MAX_HEADER_BYTES) {
                free(data);
                return HTTP_READ_TOO_LARGE;
            }

            http_parse_result_t framing =
                http_inspect_message_framing(data, header_bytes, &body_length);
            if (framing != HTTP_PARSE_OK) {
                free(data);
                return map_framing_error(framing);
            }

            /* Overflow-safe: body_length is untrusted until compared this way. */
            if (body_length > HTTP_MAX_MESSAGE_BYTES - header_bytes) {
                free(data);
                return HTTP_READ_TOO_LARGE;
            }

            headers_complete = 1;

            if (length >= header_bytes + body_length) {
                /* Headers and body arrived together (common with small POSTs). */
                length = header_bytes + body_length;
                break;
            }
        } else {
            /* headers_complete: total_needed was validated against message max. */
            if (body_length > HTTP_MAX_MESSAGE_BYTES - (header_end + 4)) {
                free(data);
                return HTTP_READ_TOO_LARGE;
            }
            size_t total_needed = header_end + 4 + body_length;
            if (length >= total_needed) {
                length = total_needed;
                break;
            }
        }
    }

    *buffer = data;
    *buffer_length = length;
    return HTTP_READ_OK;
}
