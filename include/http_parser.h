/*
 * http_parser.h - parse a complete reconstructed HTTP request message.
 *
 * The parser operates on an already-framed byte buffer (headers + optional
 * body). Network framing / incremental recv() logic lives in http_reader;
 * keeping those concerns separate means the parser can be unit-tested with
 * plain memory buffers and never touches sockets.
 *
 * http_inspect_message_framing() is a small shared helper used by the reader
 * so Content-Length / Transfer-Encoding validation is not duplicated.
 */
#ifndef CINDERHTTP_HTTP_PARSER_H
#define CINDERHTTP_HTTP_PARSER_H

#include <stddef.h>

#include "http_limits.h"
#include "http_request.h"

typedef enum {
    HTTP_PARSE_OK = 0,
    HTTP_PARSE_BAD_REQUEST,
    HTTP_PARSE_UNSUPPORTED_METHOD,
    HTTP_PARSE_UNSUPPORTED_VERSION,
    HTTP_PARSE_TOO_LARGE,
    HTTP_PARSE_TOO_MANY_HEADERS,
    HTTP_PARSE_INVALID_CONTENT_LENGTH,
    HTTP_PARSE_UNSUPPORTED_TRANSFER_ENCODING,
    HTTP_PARSE_OUT_OF_MEMORY
} http_parse_result_t;

/*
 * Parses a complete HTTP request message in `data` into `request`.
 * On success, `request` owns all allocations and must be destroyed by the
 * caller. On failure, `request` is left in a destroy-safe empty state
 * (any partial allocations have already been cleaned up).
 */
http_parse_result_t http_parse_request(const unsigned char *data, size_t data_length,
                                       http_request_t *request);

/*
 * Shared Content-Length syntax check. Accepts only a non-empty sequence of
 * ASCII digits with no leading sign, no trailing garbage, and a value that
 * fits in size_t and does not exceed HTTP_MAX_BODY_SIZE.
 *
 * Returns HTTP_PARSE_OK and writes *out_length on success; otherwise returns
 * HTTP_PARSE_INVALID_CONTENT_LENGTH or HTTP_PARSE_TOO_LARGE.
 */
http_parse_result_t http_parse_content_length_value(const char *value, size_t *out_length);

/*
 * Inspects a complete header section (bytes from the start of the message
 * through and including the terminating "\r\n\r\n") to determine the body
 * length the reader must still obtain. Used by http_reader so framing and
 * full parsing agree on Content-Length / Transfer-Encoding rules.
 *
 * header_length must include the final "\r\n\r\n".
 * On success, *out_body_length is set (0 if no body is expected).
 */
http_parse_result_t http_inspect_message_framing(const unsigned char *data, size_t header_length,
                                                 size_t *out_body_length);

/* Locate the first occurrence of "\r\n\r\n" in `data`. Returns the index of
 * the first '\r' of the terminator, or (size_t)-1 if not found. */
size_t http_find_header_terminator(const unsigned char *data, size_t length);

#endif /* CINDERHTTP_HTTP_PARSER_H */
