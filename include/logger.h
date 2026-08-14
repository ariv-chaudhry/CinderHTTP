/*
 * logger.h - small mutex-protected logger for concurrent workers.
 *
 * One logger_log() call emits one line atomically relative to other logger
 * users. Do not call from signal handlers (not async-signal-safe).
 */
#ifndef CINDERHTTP_LOGGER_H
#define CINDERHTTP_LOGGER_H

#include <stdarg.h>

/* Returns 0 on success, -1 on failure. */
int logger_init(void);

void logger_destroy(void);

/* Thread-safe fprintf(stderr, ...)-style logging with a trailing newline if
 * the format does not already end with one. */
void logger_log(const char *format, ...);

/* Same as logger_log but only when verbose is non-zero. */
void logger_verbose(int verbose, const char *format, ...);

#endif /* CINDERHTTP_LOGGER_H */
