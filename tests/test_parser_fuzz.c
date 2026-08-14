/*
 * test_parser_fuzz.c - deterministic fuzz-style stress for http_parse_request.
 *
 * Uses a fixed PRNG seed so CI is reproducible. Default iteration count is
 * modest for `make test`; override with CINDERHTTP_FUZZ_ITERS for deeper runs.
 */
#include "http_parser.h"
#include "http_request.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define DEFAULT_FUZZ_ITERS 2000
#define MAX_FUZZ_ITERS 1000000
#define MAX_INPUT 2048

static unsigned int g_rng;

static unsigned int xorshift32(void) {
    unsigned int x = g_rng;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    g_rng = x ? x : 0xC1D3u;
    return g_rng;
}

static unsigned int fuzz_iters_from_env(void) {
    const char *env = getenv("CINDERHTTP_FUZZ_ITERS");
    if (env == NULL || env[0] == '\0') {
        return DEFAULT_FUZZ_ITERS;
    }
    char *end = NULL;
    unsigned long v = strtoul(env, &end, 10);
    if (end == env || *end != '\0' || v == 0 || v > MAX_FUZZ_ITERS) {
        fprintf(stderr, "test_parser_fuzz: invalid CINDERHTTP_FUZZ_ITERS='%s', using %d\n", env,
                DEFAULT_FUZZ_ITERS);
        return DEFAULT_FUZZ_ITERS;
    }
    return (unsigned int)v;
}

static void fill_random(unsigned char *buf, size_t len) {
    for (size_t i = 0; i < len; i++) {
        unsigned int r = xorshift32();
        /* Bias toward protocol-interesting bytes. */
        switch (r % 8) {
            case 0:
                buf[i] = 0x00;
                break;
            case 1:
                buf[i] = '\r';
                break;
            case 2:
                buf[i] = '\n';
                break;
            case 3:
                buf[i] = ' ';
                break;
            case 4:
                buf[i] = 0x7F;
                break;
            case 5:
                buf[i] = 0xFF;
                break;
            default:
                buf[i] = (unsigned char)(r & 0xFF);
                break;
        }
    }
}

static void mutate_seed(unsigned char *buf, size_t *len, size_t cap) {
    if (*len == 0) {
        return;
    }
    unsigned int op = xorshift32() % 10;
    size_t idx = xorshift32() % *len;
    switch (op) {
        case 0: /* replace */
            buf[idx] = (unsigned char)(xorshift32() & 0xFF);
            break;
        case 1: /* delete */
            memmove(buf + idx, buf + idx + 1, *len - idx - 1);
            (*len)--;
            break;
        case 2: /* duplicate */
            if (*len + 1 <= cap) {
                memmove(buf + idx + 1, buf + idx, *len - idx);
                (*len)++;
            }
            break;
        case 3:
            buf[idx] = '\r';
            break;
        case 4:
            buf[idx] = '\n';
            break;
        case 5:
            buf[idx] = 0x00;
            break;
        case 6:
            buf[idx] = ' ';
            break;
        case 7: /* truncate */
            *len = idx + 1;
            break;
        case 8: /* append random */
            if (*len + 1 <= cap) {
                buf[(*len)++] = (unsigned char)(xorshift32() & 0xFF);
            }
            break;
        default: /* corrupt a digit if present */
            for (size_t i = 0; i < *len; i++) {
                if (isdigit((unsigned char)buf[i])) {
                    buf[i] = (unsigned char)('0' + (xorshift32() % 10));
                    break;
                }
            }
            break;
    }
}

static void parse_once(const unsigned char *data, size_t length) {
    http_request_t req;
    http_request_init(&req);
    (void)http_parse_request(data, length, &req);
    http_request_destroy(&req);
}

int main(void) {
    unsigned int iters = fuzz_iters_from_env();
    g_rng = 0xC1D3u;

    static const size_t lengths[] = {0, 1, 2, 3, 7, 15, 31, 63, 127, 255, 512, 1024};
    unsigned char buf[MAX_INPUT];

    unsigned int done = 0;

    /* Pure random lengths / bytes. */
    while (done < iters / 2) {
        size_t len = lengths[xorshift32() % (sizeof(lengths) / sizeof(lengths[0]))];
        if (len > MAX_INPUT) {
            len = MAX_INPUT;
        }
        fill_random(buf, len);
        parse_once(buf, len);
        done++;
    }

    /* Structured mutations of valid seeds. */
    static const char *seeds[] = {
        "GET / HTTP/1.1\r\nHost: localhost\r\n\r\n",
        "HEAD /index.html HTTP/1.1\r\nHost: x\r\n\r\n",
        "POST /api/echo HTTP/1.1\r\nContent-Length: 5\r\n\r\nhello",
        "GET /api/health HTTP/1.0\r\n\r\n",
        "POST / HTTP/1.1\r\nContent-Length: 0\r\n\r\n",
    };

    while (done < iters) {
        const char *seed = seeds[xorshift32() % (sizeof(seeds) / sizeof(seeds[0]))];
        size_t len = strlen(seed);
        if (len > MAX_INPUT) {
            len = MAX_INPUT;
        }
        memcpy(buf, seed, len);
        mutate_seed(buf, &len, MAX_INPUT);
        parse_once(buf, len);
        done++;
    }

    printf("test_parser_fuzz: %u iterations completed (seed=0xC1D3)\n", iters);
    return 0;
}
