/*
 * static_files.h - secure static file resolution and response building.
 *
 * The HTTP parser never touches the filesystem. This module owns:
 *   untrusted request target
 *     -> path extraction / query stripping
 *     -> one-pass URL decoding
 *     -> lexical normalization
 *     -> document-root confinement (including symlink resolution)
 *     -> MIME lookup
 *     -> optional binary-safe file load into http_response_t
 *
 * Ownership of the response body is transferred to http_response_t on success
 * (body_owned=1); the caller destroys the response as usual.
 */
#ifndef CINDERHTTP_STATIC_FILES_H
#define CINDERHTTP_STATIC_FILES_H

#include "http_request.h"
#include "http_response.h"

typedef enum {
    STATIC_FILE_OK = 0,
    STATIC_FILE_NOT_FOUND,
    STATIC_FILE_FORBIDDEN,
    STATIC_FILE_BAD_TARGET,
    STATIC_FILE_TOO_LARGE,
    STATIC_FILE_IO_ERROR,
    STATIC_FILE_OUT_OF_MEMORY
} static_file_result_t;

/*
 * Serves a static file for a GET or HEAD request.
 *
 * document_root: configured document root (must exist and be a directory).
 * request: parsed request; original target is not modified.
 * response: on STATIC_FILE_OK, filled with status/headers/body (body may be
 *           empty when load_body is 0 for HEAD).
 * load_body: non-zero to read file contents; zero for HEAD (Content-Length
 *            still reflects the real file size).
 *
 * On any non-OK result, `response` is left in a destroy-safe empty state
 * (caller should build an error page separately, including custom 404).
 */
static_file_result_t static_files_serve(const char *document_root, const http_request_t *request,
                                        http_response_t *response, int load_body);

/*
 * Builds a 404 response, preferring <document_root>/404.html when that file
 * can be loaded safely. Falls back to a minimal HTML body. Returns 0 on
 * success, -1 on allocation failure.
 */
int static_files_build_not_found(const char *document_root, http_response_t *response);

/*
 * Builds a generic HTML error response (no filesystem paths in the body).
 * Returns 0 on success, -1 on allocation failure.
 */
int static_files_build_error(http_response_t *response, int status_code, const char *heading);

/*
 * Testable helpers (also used internally). These do not touch the network.
 * On success, *out is a newly allocated NUL-terminated string the caller
 * must free. On failure, *out is NULL.
 */
static_file_result_t static_files_extract_path(const char *request_target, char **out_path);
static_file_result_t static_files_url_decode(const char *input, char **out_decoded);
static_file_result_t static_files_normalize_path(const char *decoded_path, char **out_relative);

#endif /* CINDERHTTP_STATIC_FILES_H */
