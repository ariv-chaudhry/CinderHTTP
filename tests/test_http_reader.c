/*
 * test_http_reader.c - Stage 7 framing tests via AF_UNIX socketpair.
 *
 * Writes are fragmented deliberately so http_read_request() cannot assume
 * one recv() equals one HTTP message.
 */
#include "http_reader.h"

#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#include "http_limits.h"
#include "utils.h"

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

typedef struct {
    int fd;
    const unsigned char *data;
    size_t length;
    const size_t *chunks;
    size_t chunk_count;
    int close_after;
} writer_args_t;

static void *fragmented_writer(void *arg) {
    writer_args_t *a = (writer_args_t *)arg;
    size_t off = 0;
    for (size_t i = 0; i < a->chunk_count && off < a->length; i++) {
        size_t n = a->chunks[i];
        if (n > a->length - off) {
            n = a->length - off;
        }
        if (n == 0) {
            continue;
        }
        ssize_t sent = send_all(a->fd, a->data + off, n);
        if (sent < 0) {
            break;
        }
        off += (size_t)sent;
    }
    /* Flush any remainder if the schedule was shorter than the message. */
    if (off < a->length) {
        (void)send_all(a->fd, a->data + off, a->length - off);
        off = a->length;
    }
    if (a->close_after) {
        shutdown(a->fd, SHUT_WR);
        close(a->fd);
    }
    return NULL;
}

static int open_pair(int fds[2]) {
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, fds) != 0) {
        perror("socketpair");
        return -1;
    }
    return 0;
}

static void test_fragmented_get(void) {
    int fds[2];
    ASSERT_EQ(open_pair(fds), 0);

    const char *msg = "GET / HTTP/1.1\r\nHost: localhost\r\n\r\n";
    size_t chunks[] = {1, 2, 3, 8, 64};
    writer_args_t wargs = {.fd = fds[0],
                           .data = (const unsigned char *)msg,
                           .length = strlen(msg),
                           .chunks = chunks,
                           .chunk_count = sizeof(chunks) / sizeof(chunks[0]),
                           .close_after = 1};

    pthread_t tid;
    ASSERT_EQ(pthread_create(&tid, NULL, fragmented_writer, &wargs), 0);

    unsigned char *buf = NULL;
    size_t len = 0;
    http_read_result_t rc = http_read_request(fds[1], &buf, &len);
    ASSERT_EQ(rc, HTTP_READ_OK);
    if (rc == HTTP_READ_OK && buf != NULL) {
        ASSERT_TRUE(buf != NULL);
        ASSERT_EQ(len, strlen(msg));
        ASSERT_TRUE(memcmp(buf, msg, len) == 0);
        free(buf);
    }

    close(fds[1]);
    ASSERT_EQ(pthread_join(tid, NULL), 0);
}

static void test_crlf_splits(void) {
    const char *variants[] = {
        "GET / HTTP/1.1\r\nHost: x\r\n\r\n",
        NULL,
    };
    /* Force splits around the terminator using explicit chunk schedules. */
    /* Message ends with \r\n\r\n at offsets 23..26 (length 27). */
    size_t schedules[][8] = {
        {23, 1, 1, 1, 1}, /* \r | \n | \r | \n */
        {24, 1, 1, 1},    /* \r\n | \r | \n | (pad) */
        {25, 1, 1},       /* \r\n\r | \n */
        {22, 2, 2, 1},    /* ... | \r\n | \r\n | pad */
    };
    size_t schedule_lens[] = {5, 4, 3, 4};

    for (size_t s = 0; s < sizeof(schedule_lens) / sizeof(schedule_lens[0]); s++) {
        int fds[2];
        ASSERT_EQ(open_pair(fds), 0);
        const char *msg = variants[0];
        writer_args_t wargs = {.fd = fds[0],
                               .data = (const unsigned char *)msg,
                               .length = strlen(msg),
                               .chunks = schedules[s],
                               .chunk_count = schedule_lens[s],
                               .close_after = 1};
        pthread_t tid;
        ASSERT_EQ(pthread_create(&tid, NULL, fragmented_writer, &wargs), 0);

        unsigned char *buf = NULL;
        size_t len = 0;
        http_read_result_t rc = http_read_request(fds[1], &buf, &len);
        ASSERT_EQ(rc, HTTP_READ_OK);
        if (rc == HTTP_READ_OK && buf != NULL) {
            ASSERT_EQ(len, strlen(msg));
            ASSERT_TRUE(memcmp(buf, msg, len) == 0);
            free(buf);
        }
        close(fds[1]);
        ASSERT_EQ(pthread_join(tid, NULL), 0);
    }
}

static void test_fragmented_post_body(void) {
    int fds[2];
    ASSERT_EQ(open_pair(fds), 0);

    const char *msg = "POST /api/echo HTTP/1.1\r\nContent-Length: 11\r\n\r\nhello world";
    size_t chunks[] = {5, 10, 15, 6, 5};
    writer_args_t wargs = {.fd = fds[0],
                           .data = (const unsigned char *)msg,
                           .length = strlen(msg),
                           .chunks = chunks,
                           .chunk_count = 5,
                           .close_after = 1};

    pthread_t tid;
    ASSERT_EQ(pthread_create(&tid, NULL, fragmented_writer, &wargs), 0);

    unsigned char *buf = NULL;
    size_t len = 0;
    http_read_result_t rc = http_read_request(fds[1], &buf, &len);
    ASSERT_EQ(rc, HTTP_READ_OK);
    if (rc == HTTP_READ_OK && buf != NULL) {
        ASSERT_EQ(len, strlen(msg));
        ASSERT_TRUE(memcmp(buf + len - 11, "hello world", 11) == 0);
        free(buf);
    }

    close(fds[1]);
    ASSERT_EQ(pthread_join(tid, NULL), 0);
}

static void test_immediate_disconnect(void) {
    int fds[2];
    ASSERT_EQ(open_pair(fds), 0);
    close(fds[0]); /* peer gone before any bytes */

    unsigned char *buf = NULL;
    size_t len = 0;
    ASSERT_EQ(http_read_request(fds[1], &buf, &len), HTTP_READ_CLIENT_CLOSED);
    ASSERT_TRUE(buf == NULL);
    ASSERT_EQ(len, 0);
    close(fds[1]);
}

static void test_disconnect_during_headers(void) {
    int fds[2];
    ASSERT_EQ(open_pair(fds), 0);

    const char *partial = "GET / HTTP/1.1\r\nHost:";
    ASSERT_EQ(send_all(fds[0], partial, strlen(partial)), (ssize_t)strlen(partial));
    shutdown(fds[0], SHUT_WR);
    close(fds[0]);

    unsigned char *buf = NULL;
    size_t len = 0;
    ASSERT_EQ(http_read_request(fds[1], &buf, &len), HTTP_READ_CLIENT_CLOSED);
    ASSERT_TRUE(buf == NULL);
    close(fds[1]);
}

static void test_disconnect_during_body(void) {
    int fds[2];
    ASSERT_EQ(open_pair(fds), 0);

    const char *partial = "POST /api/echo HTTP/1.1\r\nContent-Length: 100\r\n\r\nshort";
    ASSERT_EQ(send_all(fds[0], partial, strlen(partial)), (ssize_t)strlen(partial));
    shutdown(fds[0], SHUT_WR);
    close(fds[0]);

    unsigned char *buf = NULL;
    size_t len = 0;
    ASSERT_EQ(http_read_request(fds[1], &buf, &len), HTTP_READ_CLIENT_CLOSED);
    ASSERT_TRUE(buf == NULL);
    close(fds[1]);
}

static void test_body_size_boundary(void) {
    /* Framing inspect rejects body > HTTP_MAX_BODY_SIZE before reading it. */
    int fds[2];
    ASSERT_EQ(open_pair(fds), 0);

    char header[128];
    int n = snprintf(header, sizeof(header),
                     "POST / HTTP/1.1\r\nContent-Length: %d\r\n\r\n", HTTP_MAX_BODY_SIZE + 1);
    ASSERT_TRUE(n > 0);
    ASSERT_EQ(send_all(fds[0], header, (size_t)n), (ssize_t)n);
    shutdown(fds[0], SHUT_WR);
    close(fds[0]);

    unsigned char *buf = NULL;
    size_t len = 0;
    ASSERT_EQ(http_read_request(fds[1], &buf, &len), HTTP_READ_TOO_LARGE);
    ASSERT_TRUE(buf == NULL);
    close(fds[1]);
}

static void test_chunked_rejected_by_reader(void) {
    int fds[2];
    ASSERT_EQ(open_pair(fds), 0);
    const char *msg = "POST / HTTP/1.1\r\nTransfer-Encoding: chunked\r\n\r\n";
    ASSERT_EQ(send_all(fds[0], msg, strlen(msg)), (ssize_t)strlen(msg));
    close(fds[0]);

    unsigned char *buf = NULL;
    size_t len = 0;
    ASSERT_EQ(http_read_request(fds[1], &buf, &len), HTTP_READ_UNSUPPORTED_TRANSFER_ENCODING);
    ASSERT_TRUE(buf == NULL);
    close(fds[1]);
}

static void test_send_all_socketpair(void) {
    int fds[2];
    ASSERT_EQ(open_pair(fds), 0);

    unsigned char payload[] = {0x00, 0x01, 0x7F, 0x80, 0xFF, 'A', 'B'};
    ASSERT_EQ(send_all(fds[0], payload, sizeof(payload)), (ssize_t)sizeof(payload));
    shutdown(fds[0], SHUT_WR);

    unsigned char got[16];
    size_t total = 0;
    while (total < sizeof(payload)) {
        ssize_t n = recv(fds[1], got + total, sizeof(payload) - total, 0);
        ASSERT_TRUE(n > 0);
        total += (size_t)n;
    }
    ASSERT_TRUE(memcmp(got, payload, sizeof(payload)) == 0);

    close(fds[0]);
    close(fds[1]);

    /* Peer closed before send: must fail, not raise SIGPIPE. */
    ASSERT_EQ(open_pair(fds), 0);
    close(fds[1]);
    ASSERT_EQ(send_all(fds[0], "x", 1), -1);
    close(fds[0]);
}

int main(void) {
    test_fragmented_get();
    test_crlf_splits();
    test_fragmented_post_body();
    test_immediate_disconnect();
    test_disconnect_during_headers();
    test_disconnect_during_body();
    test_body_size_boundary();
    test_chunked_rejected_by_reader();
    test_send_all_socketpair();

    printf("test_http_reader: %d assertions passed, %d failed\n", g_passed, g_failures);
    return g_failures == 0 ? 0 : 1;
}
