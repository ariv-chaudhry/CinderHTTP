/*
 * http_reader.h - connection-level HTTP request framing over a TCP byte stream.
 *
 * Stage 9: a stateful reader accumulates recv() bytes, extracts exactly one
 * complete request at a time, and retains any leftover bytes for the next call.
 *
 * Ownership:
 *   - http_reader_t owns its internal accumulation buffer.
 *   - On HTTP_READ_OK, *request_data is a newly malloc'd copy of one framed
 *     message; the caller must free() it. Leftovers stay inside the reader.
 *   - The reader never closes client_fd.
 *
 * Idle keep-alive (no bytes yet for the next request) + SO_RCVTIMEO →
 * HTTP_READ_TIMEOUT with no buffer. Partial request + timeout → also
 * HTTP_READ_TIMEOUT (caller may send 408).
 */
#ifndef CINDERHTTP_HTTP_READER_H
#define CINDERHTTP_HTTP_READER_H

#include <stddef.h>

typedef enum {
    HTTP_READ_OK = 0,
    HTTP_READ_CLIENT_CLOSED,
    HTTP_READ_TIMEOUT,
    HTTP_READ_TOO_LARGE,
    HTTP_READ_IO_ERROR,
    HTTP_READ_BAD_REQUEST,
    HTTP_READ_UNSUPPORTED_TRANSFER_ENCODING,
    HTTP_READ_INVALID_CONTENT_LENGTH,
    HTTP_READ_OUT_OF_MEMORY
} http_read_result_t;

typedef struct {
    unsigned char *data;
    size_t length;
    size_t capacity;
} http_reader_t;

void http_reader_init(http_reader_t *reader);
void http_reader_destroy(http_reader_t *reader);

/*
 * Extract the next complete HTTP request from the connection.
 * May return immediately from buffered leftovers without calling recv().
 */
http_read_result_t http_reader_next_request(http_reader_t *reader, int client_fd,
                                            unsigned char **request_data,
                                            size_t *request_length);

/*
 * One-shot helper: read a single request then discard any leftovers.
 * Prefer http_reader_next_request for persistent connections.
 */
http_read_result_t http_read_request(int client_fd, unsigned char **buffer,
                                     size_t *buffer_length);

#endif /* CINDERHTTP_HTTP_READER_H */
