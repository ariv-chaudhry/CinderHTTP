/*
 * http_reader.h - reconstruct exactly one HTTP request from a TCP byte stream.
 *
 * Separates network framing (incremental recv, finding \r\n\r\n, reading the
 * Content-Length body) from HTTP syntax parsing. The returned buffer is a
 * complete message owned by the caller and must be freed with free().
 *
 * Client disconnect before any bytes: HTTP_READ_CLIENT_CLOSED, no buffer.
 * Client disconnect mid-message: HTTP_READ_CLIENT_CLOSED, no buffer (no
 * partial response is generated for incomplete requests in Stage 2).
 */
#ifndef CINDERHTTP_HTTP_READER_H
#define CINDERHTTP_HTTP_READER_H

#include <stddef.h>

typedef enum {
    HTTP_READ_OK = 0,
    HTTP_READ_CLIENT_CLOSED,
    HTTP_READ_TOO_LARGE,
    HTTP_READ_IO_ERROR,
    HTTP_READ_BAD_REQUEST,
    HTTP_READ_UNSUPPORTED_TRANSFER_ENCODING,
    HTTP_READ_INVALID_CONTENT_LENGTH,
    HTTP_READ_OUT_OF_MEMORY
} http_read_result_t;

/*
 * Reads one complete HTTP request from client_fd into a newly allocated
 * buffer. On HTTP_READ_OK, *buffer is non-NULL, *buffer_length is the full
 * message size, and the caller owns *buffer (free it). On any error,
 * *buffer is NULL and *buffer_length is 0.
 */
http_read_result_t http_read_request(int client_fd, unsigned char **buffer,
                                     size_t *buffer_length);

#endif /* CINDERHTTP_HTTP_READER_H */
