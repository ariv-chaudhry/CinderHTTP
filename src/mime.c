#include "mime.h"

#include <string.h>

typedef struct {
    const char *extension; /* without leading dot, lowercase */
    const char *mime_type;
} mime_entry_t;

static const mime_entry_t MIME_TABLE[] = {
    {"html", "text/html; charset=utf-8"},
    {"htm", "text/html; charset=utf-8"},
    {"css", "text/css; charset=utf-8"},
    {"js", "application/javascript"},
    {"json", "application/json"},
    {"txt", "text/plain; charset=utf-8"},
    {"csv", "text/csv; charset=utf-8"},
    {"xml", "application/xml"},
    {"png", "image/png"},
    {"jpg", "image/jpeg"},
    {"jpeg", "image/jpeg"},
    {"gif", "image/gif"},
    {"svg", "image/svg+xml"},
    {"ico", "image/x-icon"},
    {"webp", "image/webp"},
    {"pdf", "application/pdf"},
    {"woff", "font/woff"},
    {"woff2", "font/woff2"},
};

static int ascii_tolower(unsigned char c) {
    if (c >= 'A' && c <= 'Z') {
        return (int)(c - 'A' + 'a');
    }
    return (int)c;
}

/* Case-insensitive compare of extension (no leading dot) to a table key. */
static int extension_equal(const char *ext, const char *key) {
    while (*ext != '\0' && *key != '\0') {
        if (ascii_tolower((unsigned char)*ext) != ascii_tolower((unsigned char)*key)) {
            return 0;
        }
        ext++;
        key++;
    }
    return *ext == '\0' && *key == '\0';
}

const char *mime_type_from_path(const char *path) {
    if (path == NULL || path[0] == '\0') {
        return "application/octet-stream";
    }

    /* Find the final path segment, then the last '.' in that segment only so
     * that names like ".gitignore" or "archive.tar.gz" behave predictably:
     * we use the substring after the last dot in the basename. */
    const char *base = path;
    for (const char *p = path; *p != '\0'; p++) {
        if (*p == '/') {
            base = p + 1;
        }
    }

    const char *dot = NULL;
    for (const char *p = base; *p != '\0'; p++) {
        if (*p == '.') {
            dot = p;
        }
    }

    /* No extension, or a leading-dot hidden file with no further extension. */
    if (dot == NULL || dot == base || dot[1] == '\0') {
        return "application/octet-stream";
    }

    const char *ext = dot + 1;
    for (size_t i = 0; i < sizeof(MIME_TABLE) / sizeof(MIME_TABLE[0]); i++) {
        if (extension_equal(ext, MIME_TABLE[i].extension)) {
            return MIME_TABLE[i].mime_type;
        }
    }

    return "application/octet-stream";
}
