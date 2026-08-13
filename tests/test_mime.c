/*
 * test_mime.c - MIME type lookup tests.
 */
#include <stdio.h>
#include <string.h>

#include "mime.h"

static int g_failures = 0;
static int g_passed = 0;

#define ASSERT_STR_EQ(a, b)                                                                        \
    do {                                                                                           \
        const char *_a = (a);                                                                      \
        const char *_b = (b);                                                                      \
        if (_a == NULL || _b == NULL || strcmp(_a, _b) != 0) {                                     \
            fprintf(stderr, "FAIL %s:%d: \"%s\" != \"%s\"\n", __FILE__, __LINE__,                  \
                    _a ? _a : "(null)", _b ? _b : "(null)");                                       \
            g_failures++;                                                                          \
        } else {                                                                                   \
            g_passed++;                                                                            \
        }                                                                                          \
    } while (0)

int main(void) {
    ASSERT_STR_EQ(mime_type_from_path("index.html"), "text/html; charset=utf-8");
    ASSERT_STR_EQ(mime_type_from_path("INDEX.HTML"), "text/html; charset=utf-8");
    ASSERT_STR_EQ(mime_type_from_path("/css/style.css"), "text/css; charset=utf-8");
    ASSERT_STR_EQ(mime_type_from_path("script.js"), "application/javascript");
    ASSERT_STR_EQ(mime_type_from_path("data.json"), "application/json");
    ASSERT_STR_EQ(mime_type_from_path("image.png"), "image/png");
    ASSERT_STR_EQ(mime_type_from_path("photo.jpg"), "image/jpeg");
    ASSERT_STR_EQ(mime_type_from_path("photo.jpeg"), "image/jpeg");
    ASSERT_STR_EQ(mime_type_from_path("PHOTO.JPEG"), "image/jpeg");
    ASSERT_STR_EQ(mime_type_from_path("icon.gif"), "image/gif");
    ASSERT_STR_EQ(mime_type_from_path("graphic.svg"), "image/svg+xml");
    ASSERT_STR_EQ(mime_type_from_path("file.txt"), "text/plain; charset=utf-8");
    ASSERT_STR_EQ(mime_type_from_path("document.pdf"), "application/pdf");
    ASSERT_STR_EQ(mime_type_from_path("unknown.xyz"), "application/octet-stream");
    ASSERT_STR_EQ(mime_type_from_path("filename-without-extension"), "application/octet-stream");
    ASSERT_STR_EQ(mime_type_from_path(".hiddenfile"), "application/octet-stream");
    ASSERT_STR_EQ(mime_type_from_path("assets/app.min.js"), "application/javascript");
    ASSERT_STR_EQ(mime_type_from_path(NULL), "application/octet-stream");

    printf("test_mime: %d assertions passed, %d failed\n", g_passed, g_failures);
    return g_failures == 0 ? 0 : 1;
}
