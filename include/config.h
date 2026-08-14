/*
 * config.h - server configuration structure and command-line parsing.
 *
 * server_config_t is deliberately a plain struct with no hidden state: it is
 * filled in once at startup (defaults, then command-line overrides) and
 * treated as read-only for the rest of the process's life. Every subsystem
 * that needs configuration (sockets, thread pool, static file server, ...)
 * receives a `const server_config_t *` rather than reaching for globals.
 */
#ifndef CINDERHTTP_CONFIG_H
#define CINDERHTTP_CONFIG_H

#include <limits.h>
#include <stdio.h>

/* POSIX guarantees PATH_MAX in <limits.h> on most systems, but it is not
 * required by ISO C, so a conservative fallback keeps this header portable. */
#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

/*
 * Default values applied by config_set_defaults() before any command-line
 * arguments are parsed. Named here (rather than left as literals) so the
 * defaults, the --help text, and config_set_defaults() cannot drift apart.
 */
#define CONFIG_DEFAULT_PORT 8080
#define CONFIG_DEFAULT_WORKER_COUNT 4
#define CONFIG_DEFAULT_QUEUE_CAPACITY 64
#define CONFIG_DEFAULT_DOCUMENT_ROOT "./public"

/*
 * Validation bounds for user-supplied values. These exist so a typo (or a
 * hostile caller) cannot push the server into a degenerate configuration,
 * e.g. binding to port 0, spawning hundreds of worker threads, or sizing
 * the connection queue absurdly large.
 */
#define CONFIG_MIN_PORT 1
#define CONFIG_MAX_PORT 65535
#define CONFIG_MIN_WORKER_COUNT 1
#define CONFIG_MAX_WORKER_COUNT 256
#define CONFIG_MIN_QUEUE_CAPACITY 1
#define CONFIG_MAX_QUEUE_CAPACITY 4096

/*
 * Immutable-after-startup server configuration.
 * `worker_count` sizes the fixed pthread worker pool; `queue_capacity` sizes
 * the bounded connection queue used for accept→worker handoff.
 * `document_root` is the static-file document root.
 */
typedef struct {
    int port;
    int worker_count;
    int queue_capacity;
    char document_root[PATH_MAX];
    int verbose;
} server_config_t;

/* Outcome of config_parse_args(), distinguishing "keep going" from the two
 * ways command-line handling can end the process early. */
typedef enum {
    CONFIG_PARSE_OK = 0,
    CONFIG_PARSE_HELP,
    CONFIG_PARSE_ERROR
} config_parse_result_t;

/* Populates `config` with the documented defaults. Always succeeds. */
void config_set_defaults(server_config_t *config);

/*
 * Parses `argv[1..argc-1]` as CinderHTTP command-line options, overriding
 * fields in `config` as recognized options are found.
 *
 * Returns CONFIG_PARSE_OK if parsing succeeded and the caller should
 * continue starting the server; CONFIG_PARSE_HELP if --help was requested
 * (usage text has already been printed to stdout); or CONFIG_PARSE_ERROR if
 * an option was missing, malformed, or unrecognized (a diagnostic has
 * already been printed to stderr).
 */
config_parse_result_t config_parse_args(server_config_t *config, int argc, char *argv[]);

/* Prints usage/help text documenting every supported option to `stream`. */
void config_print_usage(FILE *stream);

#endif /* CINDERHTTP_CONFIG_H */
