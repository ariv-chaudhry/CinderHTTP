#include "static_files.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "http_limits.h"
#include "mime.h"

#ifndef O_CLOEXEC
#define O_CLOEXEC 0
#endif

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

static int is_hex(unsigned char c) {
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
}

static int hex_value(unsigned char c) {
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    if (c >= 'A' && c <= 'F') {
        return c - 'A' + 10;
    }
    return -1;
}

/*
 * Strip query (?...) and fragment (#...) from an origin-form target.
 * Does not allocate; writes length of the path portion through *out_len.
 */
static static_file_result_t path_portion(const char *target, const char **out_start,
                                         size_t *out_len) {
    if (target == NULL || target[0] == '\0') {
        return STATIC_FILE_BAD_TARGET;
    }
    if (target[0] != '/') {
        return STATIC_FILE_BAD_TARGET;
    }

    const char *end = target;
    while (*end != '\0' && *end != '?' && *end != '#') {
        end++;
    }

    *out_start = target;
    *out_len = (size_t)(end - target);
    return STATIC_FILE_OK;
}

static_file_result_t static_files_extract_path(const char *request_target, char **out_path) {
    if (out_path == NULL) {
        return STATIC_FILE_BAD_TARGET;
    }
    *out_path = NULL;

    const char *start = NULL;
    size_t len = 0;
    static_file_result_t rc = path_portion(request_target, &start, &len);
    if (rc != STATIC_FILE_OK) {
        return rc;
    }

    char *copy = malloc(len + 1);
    if (copy == NULL) {
        return STATIC_FILE_OUT_OF_MEMORY;
    }
    memcpy(copy, start, len);
    copy[len] = '\0';
    *out_path = copy;
    return STATIC_FILE_OK;
}

/*
 * Single-pass percent-decoding. Does NOT recursively decode (%252e stays
 * "%2e"). Rejects truncated/invalid escapes and embedded NUL (%00).
 */
static_file_result_t static_files_url_decode(const char *input, char **out_decoded) {
    if (out_decoded == NULL || input == NULL) {
        return STATIC_FILE_BAD_TARGET;
    }
    *out_decoded = NULL;

    size_t in_len = strlen(input);
    char *out = malloc(in_len + 1);
    if (out == NULL) {
        return STATIC_FILE_OUT_OF_MEMORY;
    }

    size_t oi = 0;
    for (size_t i = 0; i < in_len;) {
        unsigned char c = (unsigned char)input[i];
        if (c == '%') {
            if (i + 2 >= in_len || !is_hex((unsigned char)input[i + 1]) ||
                !is_hex((unsigned char)input[i + 2])) {
                free(out);
                return STATIC_FILE_BAD_TARGET;
            }
            int hi = hex_value((unsigned char)input[i + 1]);
            int lo = hex_value((unsigned char)input[i + 2]);
            unsigned char decoded = (unsigned char)((hi << 4) | lo);
            /* Embedded NUL would truncate C string APIs and hide trailing
             * path data from validation — reject rather than pass through. */
            if (decoded == '\0') {
                free(out);
                return STATIC_FILE_BAD_TARGET;
            }
            out[oi++] = (char)decoded;
            i += 3;
        } else {
            out[oi++] = (char)c;
            i += 1;
        }
    }
    out[oi] = '\0';
    *out_decoded = out;
    return STATIC_FILE_OK;
}

/*
 * Lexically normalize an absolute decoded path into a relative path under the
 * document root (no leading slash). Resolves "." and ".." segments; if ".."
 * would escape above "/", returns FORBIDDEN. Empty path (from "/") becomes "".
 *
 * This pass alone is not sufficient security — symlink checks happen later —
 * but it rejects obvious traversal before any filesystem calls.
 */
static_file_result_t static_files_normalize_path(const char *decoded_path, char **out_relative) {
    if (out_relative == NULL || decoded_path == NULL || decoded_path[0] != '/') {
        return STATIC_FILE_BAD_TARGET;
    }
    *out_relative = NULL;

    /* Collect segments into a small stack of heap strings. */
    char **segments = NULL;
    size_t count = 0;
    size_t capacity = 0;

    const char *p = decoded_path + 1; /* skip leading '/' */
    while (*p != '\0') {
        const char *start = p;
        while (*p != '\0' && *p != '/') {
            p++;
        }
        size_t seglen = (size_t)(p - start);

        if (seglen == 0) {
            /* "//" — skip empty segment */
        } else if (seglen == 1 && start[0] == '.') {
            /* "." — skip */
        } else if (seglen == 2 && start[0] == '.' && start[1] == '.') {
            if (count == 0) {
                /* Would escape above document-root mount point. */
                for (size_t i = 0; i < count; i++) {
                    free(segments[i]);
                }
                free(segments);
                return STATIC_FILE_FORBIDDEN;
            }
            free(segments[count - 1]);
            count--;
        } else {
            if (count == capacity) {
                size_t new_cap = (capacity == 0) ? 8 : capacity * 2;
                char **grown = realloc(segments, new_cap * sizeof(char *));
                if (grown == NULL) {
                    for (size_t i = 0; i < count; i++) {
                        free(segments[i]);
                    }
                    free(segments);
                    return STATIC_FILE_OUT_OF_MEMORY;
                }
                segments = grown;
                capacity = new_cap;
            }
            char *seg = malloc(seglen + 1);
            if (seg == NULL) {
                for (size_t i = 0; i < count; i++) {
                    free(segments[i]);
                }
                free(segments);
                return STATIC_FILE_OUT_OF_MEMORY;
            }
            memcpy(seg, start, seglen);
            seg[seglen] = '\0';
            segments[count++] = seg;
        }

        if (*p == '/') {
            p++;
        }
    }

    size_t total = 0;
    for (size_t i = 0; i < count; i++) {
        total += strlen(segments[i]);
        if (i + 1 < count) {
            total += 1; /* slash */
        }
    }

    char *relative = malloc(total + 1);
    if (relative == NULL) {
        for (size_t i = 0; i < count; i++) {
            free(segments[i]);
        }
        free(segments);
        return STATIC_FILE_OUT_OF_MEMORY;
    }

    size_t off = 0;
    for (size_t i = 0; i < count; i++) {
        size_t n = strlen(segments[i]);
        memcpy(relative + off, segments[i], n);
        off += n;
        free(segments[i]);
        if (i + 1 < count) {
            relative[off++] = '/';
        }
    }
    relative[off] = '\0';
    free(segments);

    *out_relative = relative;
    return STATIC_FILE_OK;
}

/*
 * True if `path` is exactly `root` or a path under `root` with a directory
 * separator boundary (prevents /public matching /publicity).
 */
static int path_is_within_root(const char *path, const char *root) {
    size_t root_len = strlen(root);
    size_t path_len = strlen(path);
    if (path_len < root_len) {
        return 0;
    }
    if (strncmp(path, root, root_len) != 0) {
        return 0;
    }
    if (path_len == root_len) {
        return 1;
    }
    return path[root_len] == '/';
}

static static_file_result_t join_root_relative(const char *root, const char *relative,
                                               char *out, size_t out_size) {
    int written;
    if (relative[0] == '\0') {
        written = snprintf(out, out_size, "%s", root);
    } else {
        written = snprintf(out, out_size, "%s/%s", root, relative);
    }
    if (written < 0 || (size_t)written >= out_size) {
        return STATIC_FILE_BAD_TARGET;
    }
    return STATIC_FILE_OK;
}

/*
 * Open the resolved path, fstat the descriptor, and attach it as a file-backed
 * response body. `expected_size` is the earlier discovery size; fstat is
 * authoritative. load_body is unused for buffering — HEAD still attaches the
 * fd so Content-Length matches; the sender omits bytes for HEAD.
 */
static static_file_result_t fill_file_response(http_response_t *response, const char *fs_path,
                                               size_t expected_size, int load_body) {
    (void)load_body;
    (void)expected_size;

    http_response_destroy(response);
    http_response_init(response);
    http_response_set_status(response, 200);

    const char *mime = mime_type_from_path(fs_path);
    if (http_response_add_header(response, "Content-Type", mime) != 0) {
        return STATIC_FILE_OUT_OF_MEMORY;
    }

    int fd = open(fs_path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        http_response_destroy(response);
        if (errno == EACCES) {
            return STATIC_FILE_FORBIDDEN;
        }
        if (errno == ENOENT) {
            return STATIC_FILE_NOT_FOUND;
        }
        return STATIC_FILE_IO_ERROR;
    }

    struct stat st;
    if (fstat(fd, &st) != 0) {
        close(fd);
        http_response_destroy(response);
        return STATIC_FILE_IO_ERROR;
    }

    if (!S_ISREG(st.st_mode)) {
        close(fd);
        http_response_destroy(response);
        return STATIC_FILE_FORBIDDEN;
    }

    if (st.st_size < 0) {
        close(fd);
        http_response_destroy(response);
        return STATIC_FILE_IO_ERROR;
    }
    if ((unsigned long long)st.st_size > (unsigned long long)HTTP_MAX_STATIC_FILE_SIZE) {
        close(fd);
        http_response_destroy(response);
        return STATIC_FILE_TOO_LARGE;
    }
    if ((unsigned long long)st.st_size > (unsigned long long)SIZE_MAX) {
        close(fd);
        http_response_destroy(response);
        return STATIC_FILE_TOO_LARGE;
    }

    if (http_response_set_file_body(response, fd, st.st_size) != 0) {
        close(fd);
        http_response_destroy(response);
        return STATIC_FILE_OUT_OF_MEMORY;
    }
    /* Ownership of fd transferred to response. */
    return STATIC_FILE_OK;
}

/*
 * Resolve relative path under canonical_root. On success, *out_resolved is a
 * malloc'd canonical path the caller must free, *out_size is the file size,
 * and *out_is_dir indicates a directory.
 */
static static_file_result_t resolve_under_root(const char *canonical_root, const char *relative,
                                               char **out_resolved, size_t *out_size,
                                               int *out_is_dir) {
    *out_resolved = NULL;
    *out_size = 0;
    *out_is_dir = 0;

    char joined[PATH_MAX];
    static_file_result_t jr = join_root_relative(canonical_root, relative, joined, sizeof(joined));
    if (jr != STATIC_FILE_OK) {
        return jr;
    }

    char resolved[PATH_MAX];
    if (realpath(joined, resolved) == NULL) {
        if (errno == ENOENT) {
            return STATIC_FILE_NOT_FOUND;
        }
        if (errno == EACCES) {
            return STATIC_FILE_FORBIDDEN;
        }
        /* ENOTDIR etc. */
        return STATIC_FILE_NOT_FOUND;
    }

    if (!path_is_within_root(resolved, canonical_root)) {
        return STATIC_FILE_FORBIDDEN;
    }

    struct stat st;
    if (stat(resolved, &st) != 0) {
        if (errno == ENOENT) {
            return STATIC_FILE_NOT_FOUND;
        }
        if (errno == EACCES) {
            return STATIC_FILE_FORBIDDEN;
        }
        return STATIC_FILE_IO_ERROR;
    }

    if (S_ISDIR(st.st_mode)) {
        *out_is_dir = 1;
    } else if (!S_ISREG(st.st_mode)) {
        return STATIC_FILE_FORBIDDEN;
    }

    if (!*out_is_dir) {
        if (st.st_size < 0) {
            return STATIC_FILE_IO_ERROR;
        }
        if ((unsigned long long)st.st_size > (unsigned long long)HTTP_MAX_STATIC_FILE_SIZE) {
            return STATIC_FILE_TOO_LARGE;
        }
        if ((unsigned long long)st.st_size > (unsigned long long)SIZE_MAX) {
            return STATIC_FILE_TOO_LARGE;
        }
        *out_size = (size_t)st.st_size;
    }

    char *copy = malloc(strlen(resolved) + 1);
    if (copy == NULL) {
        return STATIC_FILE_OUT_OF_MEMORY;
    }
    memcpy(copy, resolved, strlen(resolved) + 1);
    *out_resolved = copy;
    return STATIC_FILE_OK;
}

static static_file_result_t try_index(const char *canonical_root, const char *dir_relative,
                                      char **out_resolved, size_t *out_size) {
    char index_rel[PATH_MAX];
    int n;
    if (dir_relative[0] == '\0') {
        n = snprintf(index_rel, sizeof(index_rel), "index.html");
    } else {
        n = snprintf(index_rel, sizeof(index_rel), "%s/index.html", dir_relative);
    }
    if (n < 0 || (size_t)n >= sizeof(index_rel)) {
        return STATIC_FILE_BAD_TARGET;
    }

    int is_dir = 0;
    static_file_result_t rc =
        resolve_under_root(canonical_root, index_rel, out_resolved, out_size, &is_dir);
    if (rc == STATIC_FILE_OK && is_dir) {
        free(*out_resolved);
        *out_resolved = NULL;
        return STATIC_FILE_FORBIDDEN;
    }
    return rc;
}

static_file_result_t static_files_serve(const char *document_root, const http_request_t *request,
                                        http_response_t *response, int load_body) {
    if (document_root == NULL || request == NULL || response == NULL || request->target == NULL) {
        return STATIC_FILE_BAD_TARGET;
    }

    http_response_destroy(response);
    http_response_init(response);

    char *path_only = NULL;
    char *decoded = NULL;
    char *relative = NULL;
    char *resolved = NULL;
    static_file_result_t result = STATIC_FILE_BAD_TARGET;

    result = static_files_extract_path(request->target, &path_only);
    if (result != STATIC_FILE_OK) {
        goto done;
    }

    result = static_files_url_decode(path_only, &decoded);
    if (result != STATIC_FILE_OK) {
        goto done;
    }

    result = static_files_normalize_path(decoded, &relative);
    if (result != STATIC_FILE_OK) {
        goto done;
    }

    char canonical_root[PATH_MAX];
    if (realpath(document_root, canonical_root) == NULL) {
        result = STATIC_FILE_IO_ERROR;
        goto done;
    }

    struct stat root_st;
    if (stat(canonical_root, &root_st) != 0 || !S_ISDIR(root_st.st_mode)) {
        result = STATIC_FILE_IO_ERROR;
        goto done;
    }

    size_t size = 0;
    int is_dir = 0;
    result = resolve_under_root(canonical_root, relative, &resolved, &size, &is_dir);
    if (result == STATIC_FILE_OK && is_dir) {
        /* Directory: only serve if index.html exists; otherwise 403 (no listing). */
        free(resolved);
        resolved = NULL;
        result = try_index(canonical_root, relative, &resolved, &size);
        if (result == STATIC_FILE_NOT_FOUND) {
            result = STATIC_FILE_FORBIDDEN;
            goto done;
        }
        if (result != STATIC_FILE_OK) {
            goto done;
        }
    } else if (result == STATIC_FILE_NOT_FOUND) {
        /*
         * If the request looked like a directory (trailing slash) we already
         * normalized away the slash; try index under that relative path when
         * the path itself is missing as a file. For "/" relative is "" and
         * resolve_under_root hits the root directory (is_dir), handled above.
         */
        goto done;
    } else if (result != STATIC_FILE_OK) {
        goto done;
    }

    result = fill_file_response(response, resolved, size, load_body);
    if (result != STATIC_FILE_OK) {
        http_response_destroy(response);
        goto done;
    }

done:
    free(path_only);
    free(decoded);
    free(relative);
    free(resolved);
    return result;
}

int static_files_build_error(http_response_t *response, int status_code, const char *heading) {
    if (response == NULL || heading == NULL) {
        return -1;
    }

    char body[512];
    int n = snprintf(body, sizeof(body),
                     "<!doctype html>\n"
                     "<html><head><title>%d %s</title></head>"
                     "<body><h1>%d %s</h1></body></html>\n",
                     status_code, heading, status_code, heading);
    if (n < 0 || (size_t)n >= sizeof(body)) {
        return -1;
    }

    http_response_destroy(response);
    http_response_init(response);
    http_response_set_status(response, status_code);
    if (http_response_set_body_text(response, body) != 0) {
        return -1;
    }
    if (http_response_add_header(response, "Content-Type", "text/html; charset=utf-8") != 0) {
        return -1;
    }
    return 0;
}

int static_files_build_not_found(const char *document_root, http_response_t *response) {
    if (response == NULL) {
        return -1;
    }

    /* Attempt to serve a safe <root>/404.html without recursing on failure. */
    if (document_root != NULL) {
        http_request_t fake;
        http_request_init(&fake);
        fake.method = HTTP_METHOD_GET;
        fake.target = "/404.html"; /* not owned; not destroyed */
        fake.version = "HTTP/1.1";

        http_response_t page;
        http_response_init(&page);
        static_file_result_t rc = static_files_serve(document_root, &fake, &page, 1);
        /* Clear non-owned pointers before destroy. */
        fake.target = NULL;
        fake.version = NULL;
        http_request_destroy(&fake);

        if (rc == STATIC_FILE_OK) {
            http_response_destroy(response);
            *response = page;
            /* Re-tag as 404 while keeping the custom body/headers. */
            http_response_set_status(response, 404);
            return 0;
        }
        http_response_destroy(&page);
    }

    return static_files_build_error(response, 404, "Not Found");
}
