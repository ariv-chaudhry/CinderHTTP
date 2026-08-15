/*
 * http_connection.h - Connection header tokens and keep-alive policy.
 *
 * Persistence rules (Stage 9):
 *   HTTP/1.1 → keep-alive unless Connection contains token "close"
 *   HTTP/1.0 → close unless Connection contains token "keep-alive"
 * If both keep-alive and close appear, prefer close (safer).
 */
#ifndef CINDERHTTP_HTTP_CONNECTION_H
#define CINDERHTTP_HTTP_CONNECTION_H

#include "http_request.h"
#include "http_response.h"

/* Case-insensitive exact token match within a comma-separated header value. */
int http_header_value_has_token(const char *value, const char *token);

/*
 * Returns 1 if the connection should remain open after a successful response
 * for this request, 0 if it should close. Protocol-error callers typically
 * force close regardless.
 */
int http_request_wants_keep_alive(const http_request_t *request);

/*
 * Sets response Connection headers / suppress flags for the chosen policy.
 * keep_alive non-zero → persist; zero → Connection: close.
 * http_version may be NULL (treated as close-safe HTTP/1.1 with close).
 */
int http_response_apply_connection_policy(http_response_t *response, int keep_alive,
                                          const char *http_version);

#endif /* CINDERHTTP_HTTP_CONNECTION_H */
