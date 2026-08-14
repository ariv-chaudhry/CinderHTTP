#include "logger.h"

#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

static pthread_mutex_t g_log_mutex = PTHREAD_MUTEX_INITIALIZER;
static int g_logger_ready = 1;

int logger_init(void) {
    /* Mutex is statically initialized; kept for explicit lifecycle symmetry. */
    g_logger_ready = 1;
    return 0;
}

void logger_destroy(void) {
    g_logger_ready = 0;
}

void logger_log(const char *format, ...) {
    if (format == NULL) {
        return;
    }

    va_list args;
    va_start(args, format);

    pthread_mutex_lock(&g_log_mutex);
    if (g_logger_ready) {
        vfprintf(stderr, format, args);
        size_t len = strlen(format);
        if (len == 0 || format[len - 1] != '\n') {
            fputc('\n', stderr);
        }
        fflush(stderr);
    }
    pthread_mutex_unlock(&g_log_mutex);

    va_end(args);
}

void logger_verbose(int verbose, const char *format, ...) {
    if (!verbose || format == NULL) {
        return;
    }

    va_list args;
    va_start(args, format);

    pthread_mutex_lock(&g_log_mutex);
    if (g_logger_ready) {
        fputs("[verbose] ", stderr);
        vfprintf(stderr, format, args);
        size_t len = strlen(format);
        if (len == 0 || format[len - 1] != '\n') {
            fputc('\n', stderr);
        }
        fflush(stderr);
    }
    pthread_mutex_unlock(&g_log_mutex);

    va_end(args);
}
