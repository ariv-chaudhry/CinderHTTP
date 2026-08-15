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

void http_reader_init(http_reader_t *reader) {
    if (reader == NULL) {
        return;
    }
    reader->data = NULL;
    reader->length = 0;
    reader->capacity = 0;
}

void http_reader_destroy(http_reader_t *reader) {
    if (reader == NULL) {
        return;
    }
    free(reader->data);
    http_reader_init(reader);
}

static int ensure_capacity(http_reader_t *reader, size_t needed) {
    if (needed <= reader->capacity) {
        return 1;
    }
    /* Connection buffer may hold at most one in-flight message plus a modest
     * amount of lookahead; reject growth past the single-message ceiling. */
    if (needed > HTTP_MAX_MESSAGE_BYTES) {
        return 0;
    }

    size_t new_capacity = reader->capacity;
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

    unsigned char *grown = realloc(reader->data, new_capacity);
    if (grown == NULL) {
        return 0;
    }
    reader->data = grown;
    reader->capacity = new_capacity;
    return 1;
}

/*
 * If a complete request is already buffered, set *message_length and return
 * HTTP_READ_OK. If more data is needed, return HTTP_READ_IO_ERROR as a sentinel
 * meaning "need recv" (not a real I/O error). Other returns are fatal framing.
 */
static http_read_result_t try_extract_length(const http_reader_t *reader, size_t *message_length) {
    *message_length = 0;
    if (reader->length == 0) {
        return HTTP_READ_IO_ERROR; /* need data */
    }

    size_t header_end = http_find_header_terminator(reader->data, reader->length);
    if (header_end == (size_t)-1) {
        if (reader->length >= HTTP_MAX_HEADER_BYTES) {
            return HTTP_READ_TOO_LARGE;
        }
        return HTTP_READ_IO_ERROR; /* need data */
    }

    size_t header_bytes = header_end + 4;
    if (header_bytes > HTTP_MAX_HEADER_BYTES) {
        return HTTP_READ_TOO_LARGE;
    }

    size_t body_length = 0;
    http_parse_result_t framing =
        http_inspect_message_framing(reader->data, header_bytes, &body_length);
    if (framing != HTTP_PARSE_OK) {
        return map_framing_error(framing);
    }

    if (body_length > HTTP_MAX_MESSAGE_BYTES - header_bytes) {
        return HTTP_READ_TOO_LARGE;
    }

    size_t total = header_bytes + body_length;
    if (reader->length < total) {
        return HTTP_READ_IO_ERROR; /* need data */
    }

    *message_length = total;
    return HTTP_READ_OK;
}

static http_read_result_t take_message(http_reader_t *reader, size_t message_length,
                                       unsigned char **request_data, size_t *request_length) {
    unsigned char *copy = malloc(message_length);
    if (copy == NULL) {
        return HTTP_READ_OUT_OF_MEMORY;
    }
    memcpy(copy, reader->data, message_length);

    size_t remaining = reader->length - message_length;
    if (remaining > 0) {
        memmove(reader->data, reader->data + message_length, remaining);
    }
    reader->length = remaining;

    *request_data = copy;
    *request_length = message_length;
    return HTTP_READ_OK;
}

http_read_result_t http_reader_next_request(http_reader_t *reader, int client_fd,
                                            unsigned char **request_data,
                                            size_t *request_length) {
    if (reader == NULL || request_data == NULL || request_length == NULL) {
        return HTTP_READ_BAD_REQUEST;
    }
    *request_data = NULL;
    *request_length = 0;

    for (;;) {
        size_t message_length = 0;
        http_read_result_t ready = try_extract_length(reader, &message_length);
        if (ready == HTTP_READ_OK) {
            return take_message(reader, message_length, request_data, request_length);
        }
        if (ready != HTTP_READ_IO_ERROR) {
            /* Framing error: drop buffered bytes; connection must close. */
            reader->length = 0;
            return ready;
        }

        /* Need more socket bytes. Cap growth for header-hunting phase. */
        size_t need = reader->length + HTTP_READER_RECV_CHUNK;
        size_t header_end = http_find_header_terminator(reader->data, reader->length);
        if (header_end == (size_t)-1) {
            if (reader->length >= HTTP_MAX_HEADER_BYTES) {
                reader->length = 0;
                return HTTP_READ_TOO_LARGE;
            }
            if (need > HTTP_MAX_HEADER_BYTES) {
                need = HTTP_MAX_HEADER_BYTES;
            }
            if (need <= reader->length) {
                reader->length = 0;
                return HTTP_READ_TOO_LARGE;
            }
        } else {
            size_t header_bytes = header_end + 4;
            size_t body_length = 0;
            http_parse_result_t framing =
                http_inspect_message_framing(reader->data, header_bytes, &body_length);
            if (framing != HTTP_PARSE_OK) {
                reader->length = 0;
                return map_framing_error(framing);
            }
            if (body_length > HTTP_MAX_MESSAGE_BYTES - header_bytes) {
                reader->length = 0;
                return HTTP_READ_TOO_LARGE;
            }
            need = header_bytes + body_length;
        }

        if (!ensure_capacity(reader, need)) {
            if (need > HTTP_MAX_MESSAGE_BYTES) {
                reader->length = 0;
                return HTTP_READ_TOO_LARGE;
            }
            return HTTP_READ_OUT_OF_MEMORY;
        }

        size_t space = reader->capacity - reader->length;
        if (space == 0) {
            reader->length = 0;
            return HTTP_READ_TOO_LARGE;
        }

        ssize_t n = recv(client_fd, reader->data + reader->length, space, 0);
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
#if defined(EAGAIN)
            if (errno == EAGAIN) {
                return HTTP_READ_TIMEOUT;
            }
#endif
#if defined(EWOULDBLOCK) && (!defined(EAGAIN) || EWOULDBLOCK != EAGAIN)
            if (errno == EWOULDBLOCK) {
                return HTTP_READ_TIMEOUT;
            }
#endif
            return HTTP_READ_IO_ERROR;
        }
        if (n == 0) {
            if (reader->length == 0) {
                return HTTP_READ_CLIENT_CLOSED;
            }
            /* Partial request then EOF. */
            reader->length = 0;
            return HTTP_READ_CLIENT_CLOSED;
        }

        reader->length += (size_t)n;
    }
}

http_read_result_t http_read_request(int client_fd, unsigned char **buffer,
                                     size_t *buffer_length) {
    http_reader_t reader;
    http_reader_init(&reader);
    http_read_result_t rc = http_reader_next_request(&reader, client_fd, buffer, buffer_length);
    http_reader_destroy(&reader);
    return rc;
}
