/*
 * test_static_files.c - path security, resolution, and static serve tests.
 *
 * Uses a temporary document root under the system temp directory so tests
 * never depend on /etc/passwd or the live ./public tree.
 */
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "http_request.h"
#include "http_response.h"
#include "static_files.h"

static int g_failures = 0;
static int g_passed = 0;

#define ASSERT_TRUE(expr)                                                                          \
    do {                                                                                           \
        if (!(expr)) {                                                                             \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #expr);                        \
            g_failures++;                                                                          \
        } else {                                                                                   \
            g_passed++;                                                                            \
        }                                                                                          \
    } while (0)

#define ASSERT_EQ(a, b)                                                                            \
    do {                                                                                           \
        long long _a = (long long)(a);                                                             \
        long long _b = (long long)(b);                                                             \
        if (_a != _b) {                                                                            \
            fprintf(stderr, "FAIL %s:%d: %s (%lld) != %s (%lld)\n", __FILE__, __LINE__, #a, _a,   \
                    #b, _b);                                                                       \
            g_failures++;                                                                          \
        } else {                                                                                   \
            g_passed++;                                                                            \
        }                                                                                          \
    } while (0)

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

static char g_root[512];
static char g_outside[512];

/* Read response body bytes from MEMORY or FILE-backed responses. */
static int read_body_bytes(const http_response_t *resp, unsigned char *out, size_t len) {
    if (resp == NULL || (len > 0 && out == NULL)) {
        return -1;
    }
    if (resp->body_length != len) {
        return -1;
    }
    if (len == 0) {
        return 0;
    }
    if (resp->body_kind == HTTP_BODY_MEMORY) {
        if (resp->body == NULL) {
            return -1;
        }
        memcpy(out, resp->body, len);
        return 0;
    }
    if (resp->body_kind == HTTP_BODY_FILE) {
        if (resp->file_fd < 0) {
            return -1;
        }
        if (lseek(resp->file_fd, 0, SEEK_SET) < 0) {
            return -1;
        }
        size_t got = 0;
        while (got < len) {
            ssize_t n = read(resp->file_fd, out + got, len - got);
            if (n < 0) {
                if (errno == EINTR) {
                    continue;
                }
                return -1;
            }
            if (n == 0) {
                return -1;
            }
            got += (size_t)n;
        }
        (void)lseek(resp->file_fd, 0, SEEK_SET);
        return 0;
    }
    return -1;
}

static int write_file(const char *path, const void *data, size_t len) {
    FILE *fp = fopen(path, "wb");
    if (fp == NULL) {
        return -1;
    }
    if (len > 0 && fwrite(data, 1, len, fp) != len) {
        fclose(fp);
        return -1;
    }
    fclose(fp);
    return 0;
}

static int setup_fixture(void) {
    const char *tmpdir = getenv("TMPDIR");
    if (tmpdir == NULL) {
        tmpdir = "/tmp";
    }

    snprintf(g_root, sizeof(g_root), "%s/cinderhttp_static_test_XXXXXX", tmpdir);
    if (mkdtemp(g_root) == NULL) {
        perror("mkdtemp root");
        return -1;
    }

    snprintf(g_outside, sizeof(g_outside), "%s/cinderhttp_outside_XXXXXX", tmpdir);
    if (mkdtemp(g_outside) == NULL) {
        perror("mkdtemp outside");
        return -1;
    }

    char path[640];
    snprintf(path, sizeof(path), "%s/index.html", g_root);
    ASSERT_TRUE(write_file(path, "<html>home</html>", 16) == 0);

    snprintf(path, sizeof(path), "%s/404.html", g_root);
    ASSERT_TRUE(write_file(path, "<html>missing</html>", 19) == 0);

    snprintf(path, sizeof(path), "%s/css", g_root);
    if (mkdir(path, 0755) != 0) {
        perror("mkdir css");
        return -1;
    }
    snprintf(path, sizeof(path), "%s/css/style.css", g_root);
    ASSERT_TRUE(write_file(path, "body{}", 6) == 0);

    snprintf(path, sizeof(path), "%s/empty.txt", g_root);
    ASSERT_TRUE(write_file(path, "", 0) == 0);

    unsigned char bin[] = {0x00, 0x01, 0x02, 0xff, 0x10};
    snprintf(path, sizeof(path), "%s/blob.bin", g_root);
    ASSERT_TRUE(write_file(path, bin, sizeof(bin)) == 0);

    snprintf(path, sizeof(path), "%s/app.min.js", g_root);
    ASSERT_TRUE(write_file(path, "x=1;", 4) == 0);

    snprintf(path, sizeof(path), "%s/docs", g_root);
    if (mkdir(path, 0755) != 0) {
        return -1;
    }
    snprintf(path, sizeof(path), "%s/docs/index.html", g_root);
    ASSERT_TRUE(write_file(path, "docs-index", 10) == 0);

    snprintf(path, sizeof(path), "%s/empty_dir", g_root);
    if (mkdir(path, 0755) != 0) {
        return -1;
    }

    snprintf(path, sizeof(path), "%s/file..txt", g_root);
    ASSERT_TRUE(write_file(path, "dots-ok", 7) == 0);

    snprintf(path, sizeof(path), "%s/secret.txt", g_outside);
    ASSERT_TRUE(write_file(path, "TOPSECRET", 9) == 0);

    return 0;
}

static void cleanup_fixture(void) {
    char path[640];
    snprintf(path, sizeof(path), "%s/index.html", g_root);
    unlink(path);
    snprintf(path, sizeof(path), "%s/404.html", g_root);
    unlink(path);
    snprintf(path, sizeof(path), "%s/css/style.css", g_root);
    unlink(path);
    snprintf(path, sizeof(path), "%s/css", g_root);
    rmdir(path);
    snprintf(path, sizeof(path), "%s/empty.txt", g_root);
    unlink(path);
    snprintf(path, sizeof(path), "%s/blob.bin", g_root);
    unlink(path);
    snprintf(path, sizeof(path), "%s/app.min.js", g_root);
    unlink(path);
    snprintf(path, sizeof(path), "%s/docs/index.html", g_root);
    unlink(path);
    snprintf(path, sizeof(path), "%s/docs", g_root);
    rmdir(path);
    snprintf(path, sizeof(path), "%s/empty_dir", g_root);
    rmdir(path);
    snprintf(path, sizeof(path), "%s/file..txt", g_root);
    unlink(path);
    snprintf(path, sizeof(path), "%s/link", g_root);
    unlink(path);
    rmdir(g_root);

    snprintf(path, sizeof(path), "%s/secret.txt", g_outside);
    unlink(path);
    rmdir(g_outside);
}

static http_request_t make_get(const char *target) {
    http_request_t req;
    http_request_init(&req);
    req.method = HTTP_METHOD_GET;
    req.target = (char *)target;
    req.version = (char *)"HTTP/1.1";
    return req;
}

static void clear_borrowed(http_request_t *req) {
    req->target = NULL;
    req->version = NULL;
    http_request_destroy(req);
}

static void test_helpers(void) {
    char *out = NULL;
    ASSERT_EQ(static_files_extract_path("/index.html?x=1", &out), STATIC_FILE_OK);
    ASSERT_STR_EQ(out, "/index.html");
    free(out);

    ASSERT_EQ(static_files_extract_path("/css/style.css?v=2", &out), STATIC_FILE_OK);
    ASSERT_STR_EQ(out, "/css/style.css");
    free(out);

    ASSERT_EQ(static_files_url_decode("/%2e%2e/etc/passwd", &out), STATIC_FILE_OK);
    ASSERT_STR_EQ(out, "/../etc/passwd");
    free(out);

    ASSERT_EQ(static_files_url_decode("/test%2", &out), STATIC_FILE_BAD_TARGET);
    ASSERT_EQ(static_files_url_decode("/test%GG", &out), STATIC_FILE_BAD_TARGET);
    ASSERT_EQ(static_files_url_decode("/file.txt%00.html", &out), STATIC_FILE_BAD_TARGET);

    ASSERT_EQ(static_files_normalize_path("/../secret", &out), STATIC_FILE_FORBIDDEN);
    ASSERT_EQ(static_files_normalize_path("/foo/../../../etc/passwd", &out),
              STATIC_FILE_FORBIDDEN);
    ASSERT_EQ(static_files_normalize_path("/css/style.css", &out), STATIC_FILE_OK);
    ASSERT_STR_EQ(out, "css/style.css");
    free(out);

    ASSERT_EQ(static_files_normalize_path("/assets/app.min.js", &out), STATIC_FILE_OK);
    ASSERT_STR_EQ(out, "assets/app.min.js");
    free(out);

    /* Double-encoding is NOT recursively decoded: %252e stays "%2e". */
    ASSERT_EQ(static_files_url_decode("/%252e%252e/x", &out), STATIC_FILE_OK);
    ASSERT_STR_EQ(out, "/%2e%2e/x");
    free(out);
}

static void test_serve_ok(void) {
    http_response_t resp;
    http_response_init(&resp);

    http_request_t req = make_get("/");
    ASSERT_EQ(static_files_serve(g_root, &req, &resp, 1), STATIC_FILE_OK);
    ASSERT_EQ(resp.status_code, 200);
    ASSERT_EQ(resp.body_length, 16);
    ASSERT_EQ(resp.body_kind, HTTP_BODY_FILE);
    {
        unsigned char buf[32];
        ASSERT_TRUE(read_body_bytes(&resp, buf, 16) == 0);
        ASSERT_TRUE(memcmp(buf, "<html>home</html>", 16) == 0);
    }
    clear_borrowed(&req);
    http_response_destroy(&resp);

    req = make_get("/css/style.css");
    ASSERT_EQ(static_files_serve(g_root, &req, &resp, 1), STATIC_FILE_OK);
    ASSERT_EQ(resp.body_length, 6);
    clear_borrowed(&req);
    http_response_destroy(&resp);

    req = make_get("/index.html?theme=dark");
    ASSERT_EQ(static_files_serve(g_root, &req, &resp, 1), STATIC_FILE_OK);
    ASSERT_EQ(resp.body_length, 16);
    clear_borrowed(&req);
    http_response_destroy(&resp);

    req = make_get("/docs/");
    ASSERT_EQ(static_files_serve(g_root, &req, &resp, 1), STATIC_FILE_OK);
    ASSERT_EQ(resp.body_length, 10);
    clear_borrowed(&req);
    http_response_destroy(&resp);

    req = make_get("/app.min.js");
    ASSERT_EQ(static_files_serve(g_root, &req, &resp, 1), STATIC_FILE_OK);
    ASSERT_EQ(resp.body_length, 4);
    clear_borrowed(&req);
    http_response_destroy(&resp);
}

static void test_empty_and_binary(void) {
    http_response_t resp;
    http_response_init(&resp);

    http_request_t req = make_get("/empty.txt");
    ASSERT_EQ(static_files_serve(g_root, &req, &resp, 1), STATIC_FILE_OK);
    ASSERT_EQ(resp.status_code, 200);
    ASSERT_EQ(resp.body_length, 0);
    ASSERT_EQ(resp.body_kind, HTTP_BODY_FILE);
    clear_borrowed(&req);
    http_response_destroy(&resp);

    unsigned char expected[] = {0x00, 0x01, 0x02, 0xff, 0x10};
    req = make_get("/blob.bin");
    ASSERT_EQ(static_files_serve(g_root, &req, &resp, 1), STATIC_FILE_OK);
    ASSERT_EQ(resp.body_length, 5);
    {
        unsigned char buf[8];
        ASSERT_TRUE(read_body_bytes(&resp, buf, 5) == 0);
        ASSERT_TRUE(memcmp(buf, expected, 5) == 0);
    }
    clear_borrowed(&req);
    http_response_destroy(&resp);
}

static void test_head_metadata(void) {
    http_response_t get_resp;
    http_response_t head_resp;
    http_response_init(&get_resp);
    http_response_init(&head_resp);

    http_request_t req = make_get("/blob.bin");
    ASSERT_EQ(static_files_serve(g_root, &req, &get_resp, 1), STATIC_FILE_OK);
    ASSERT_EQ(static_files_serve(g_root, &req, &head_resp, 0), STATIC_FILE_OK);
    ASSERT_EQ(get_resp.status_code, head_resp.status_code);
    ASSERT_EQ(get_resp.body_length, head_resp.body_length);
    ASSERT_EQ(head_resp.body_length, 5);
    ASSERT_EQ(head_resp.body_kind, HTTP_BODY_FILE);
    ASSERT_TRUE(head_resp.body == NULL);
    ASSERT_TRUE(head_resp.file_fd >= 0);
    clear_borrowed(&req);
    http_response_destroy(&get_resp);
    http_response_destroy(&head_resp);
}

static void test_missing_and_dir(void) {
    http_response_t resp;
    http_response_init(&resp);

    http_request_t req = make_get("/nope.html");
    ASSERT_EQ(static_files_serve(g_root, &req, &resp, 1), STATIC_FILE_NOT_FOUND);
    clear_borrowed(&req);

    req = make_get("/empty_dir/");
    ASSERT_EQ(static_files_serve(g_root, &req, &resp, 1), STATIC_FILE_FORBIDDEN);
    clear_borrowed(&req);
    http_response_destroy(&resp);
}

static void test_traversal(void) {
    http_response_t resp;
    http_response_init(&resp);

    const char *attacks[] = {
        "/../secret.txt",
        "/../../etc/passwd",
        "/foo/../../../etc/passwd",
        "/%2e%2e/etc/passwd",
        "/%2E%2E/etc/passwd",
        "/%2e%2e/%2e%2e/etc/passwd",
        "/foo/%2e%2e/%2e%2e/etc/passwd",
        NULL,
    };

    for (int i = 0; attacks[i] != NULL; i++) {
        http_request_t req = make_get(attacks[i]);
        static_file_result_t rc = static_files_serve(g_root, &req, &resp, 1);
        ASSERT_TRUE(rc == STATIC_FILE_FORBIDDEN || rc == STATIC_FILE_BAD_TARGET ||
                    rc == STATIC_FILE_NOT_FOUND);
        /* Must never return OK with outside content. */
        if (rc == STATIC_FILE_OK) {
            unsigned char buf[64];
            if (resp.body_length >= 9 && read_body_bytes(&resp, buf, resp.body_length) == 0) {
                ASSERT_TRUE(memcmp(buf, "TOPSECRET", 9) != 0);
            }
        }
        clear_borrowed(&req);
        http_response_destroy(&resp);
    }

    http_request_t bad = make_get("/foo%2");
    ASSERT_EQ(static_files_serve(g_root, &bad, &resp, 1), STATIC_FILE_BAD_TARGET);
    clear_borrowed(&bad);

    bad = make_get("/foo%GG");
    ASSERT_EQ(static_files_serve(g_root, &bad, &resp, 1), STATIC_FILE_BAD_TARGET);
    clear_borrowed(&bad);

    bad = make_get("/file.txt%00.html");
    ASSERT_EQ(static_files_serve(g_root, &bad, &resp, 1), STATIC_FILE_BAD_TARGET);
    clear_borrowed(&bad);
    http_response_destroy(&resp);
}

static void test_symlink_escape(void) {
    char linkpath[640];
    char target[640];
    snprintf(linkpath, sizeof(linkpath), "%s/link", g_root);
    snprintf(target, sizeof(target), "%s/secret.txt", g_outside);

    if (symlink(target, linkpath) != 0) {
        fprintf(stderr, "SKIP symlink test: %s\n", strerror(errno));
        return;
    }

    http_response_t resp;
    http_response_init(&resp);
    http_request_t req = make_get("/link");
    static_file_result_t rc = static_files_serve(g_root, &req, &resp, 1);
    ASSERT_EQ(rc, STATIC_FILE_FORBIDDEN);
    clear_borrowed(&req);
    http_response_destroy(&resp);
    unlink(linkpath);
}

static void test_dots_in_filename(void) {
    http_response_t resp;
    http_response_init(&resp);
    http_request_t req = make_get("/file..txt");
    ASSERT_EQ(static_files_serve(g_root, &req, &resp, 1), STATIC_FILE_OK);
    ASSERT_EQ(resp.body_length, 7);
    {
        unsigned char buf[16];
        ASSERT_TRUE(read_body_bytes(&resp, buf, 7) == 0);
        ASSERT_TRUE(memcmp(buf, "dots-ok", 7) == 0);
    }
    clear_borrowed(&req);
    http_response_destroy(&resp);
}

static void test_symlink_dir_escape(void) {
    char linkpath[640];
    snprintf(linkpath, sizeof(linkpath), "%s/out_dir", g_root);
    if (symlink(g_outside, linkpath) != 0) {
        fprintf(stderr, "SKIP symlink dir test: %s\n", strerror(errno));
        return;
    }

    http_response_t resp;
    http_response_init(&resp);
    http_request_t req = make_get("/out_dir/secret.txt");
    static_file_result_t rc = static_files_serve(g_root, &req, &resp, 1);
    ASSERT_EQ(rc, STATIC_FILE_FORBIDDEN);
    clear_borrowed(&req);
    http_response_destroy(&resp);
    unlink(linkpath);
}

static void test_custom_404(void) {
    http_response_t resp;
    http_response_init(&resp);
    ASSERT_EQ(static_files_build_not_found(g_root, &resp), 0);
    ASSERT_EQ(resp.status_code, 404);
    ASSERT_EQ(resp.body_length, 19);
    {
        unsigned char buf[32];
        ASSERT_TRUE(read_body_bytes(&resp, buf, 19) == 0);
        ASSERT_TRUE(memcmp(buf, "<html>missing</html>", 19) == 0);
    }
    http_response_destroy(&resp);
}

int main(void) {
    if (setup_fixture() != 0) {
        fprintf(stderr, "fixture setup failed\n");
        return 1;
    }

    test_helpers();
    test_serve_ok();
    test_empty_and_binary();
    test_head_metadata();
    test_missing_and_dir();
    test_traversal();
    test_symlink_escape();
    test_dots_in_filename();
    test_symlink_dir_escape();
    test_custom_404();

    cleanup_fixture();

    printf("test_static_files: %d assertions passed, %d failed\n", g_passed, g_failures);
    return g_failures == 0 ? 0 : 1;
}
