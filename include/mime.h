/*
 * mime.h - extension-based Content-Type lookup for static files.
 *
 * Stage 3 does not sniff file contents; the extension alone selects the type.
 * Unknown or missing extensions map to application/octet-stream.
 */
#ifndef CINDERHTTP_MIME_H
#define CINDERHTTP_MIME_H

/*
 * Returns a static string suitable for a Content-Type header value.
 * Extension matching is case-insensitive. The returned pointer must not be
 * freed. `path` may be NULL (treated as unknown).
 */
const char *mime_type_from_path(const char *path);

#endif /* CINDERHTTP_MIME_H */
