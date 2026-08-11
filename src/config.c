#include "config.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>

/*
 * Parses the value following a flag like "--port 9000" as a base-10 long.
 * Centralizing this keeps every numeric option honest about rejecting
 * trailing garbage ("9000x"), empty strings, and out-of-range input instead
 * of silently truncating it the way atoi() would.
 */
static int parse_long_arg(int argc, char *argv[], int *index, const char *option_name,
                           long *out_value) {
    if (*index + 1 >= argc) {
        fprintf(stderr, "cinderhttp: option '%s' requires a value\n", option_name);
        return 0;
    }

    (*index)++;
    const char *value_str = argv[*index];

    errno = 0;
    char *end_ptr = NULL;
    long value = strtol(value_str, &end_ptr, 10);

    if (end_ptr == value_str || *end_ptr != '\0' || errno == ERANGE) {
        fprintf(stderr, "cinderhttp: option '%s' expects an integer, got '%s'\n", option_name,
                value_str);
        return 0;
    }

    *out_value = value;
    return 1;
}

void config_set_defaults(server_config_t *config) {
    config->port = CONFIG_DEFAULT_PORT;
    config->worker_count = CONFIG_DEFAULT_WORKER_COUNT;
    config->queue_capacity = CONFIG_DEFAULT_QUEUE_CAPACITY;
    config->verbose = 0;

    /* The default root is a short, known-good literal; truncation here would
     * mean CONFIG_DEFAULT_DOCUMENT_ROOT itself was misconfigured at build
     * time, not a runtime condition worth checking for. */
    (void)snprintf(config->document_root, sizeof(config->document_root), "%s",
                    CONFIG_DEFAULT_DOCUMENT_ROOT);
}

config_parse_result_t config_parse_args(server_config_t *config, int argc, char *argv[]) {
    for (int i = 1; i < argc; i++) {
        const char *arg = argv[i];
        long value = 0;

        if (strcmp(arg, "--help") == 0) {
            config_print_usage(stdout);
            return CONFIG_PARSE_HELP;
        } else if (strcmp(arg, "--verbose") == 0) {
            config->verbose = 1;
        } else if (strcmp(arg, "--port") == 0) {
            if (!parse_long_arg(argc, argv, &i, "--port", &value)) {
                return CONFIG_PARSE_ERROR;
            }
            if (value < CONFIG_MIN_PORT || value > CONFIG_MAX_PORT) {
                fprintf(stderr, "cinderhttp: --port must be between %d and %d (got %ld)\n",
                        CONFIG_MIN_PORT, CONFIG_MAX_PORT, value);
                return CONFIG_PARSE_ERROR;
            }
            config->port = (int)value;
        } else if (strcmp(arg, "--workers") == 0) {
            if (!parse_long_arg(argc, argv, &i, "--workers", &value)) {
                return CONFIG_PARSE_ERROR;
            }
            if (value < CONFIG_MIN_WORKER_COUNT || value > CONFIG_MAX_WORKER_COUNT) {
                fprintf(stderr, "cinderhttp: --workers must be between %d and %d (got %ld)\n",
                        CONFIG_MIN_WORKER_COUNT, CONFIG_MAX_WORKER_COUNT, value);
                return CONFIG_PARSE_ERROR;
            }
            config->worker_count = (int)value;
        } else if (strcmp(arg, "--queue-size") == 0) {
            if (!parse_long_arg(argc, argv, &i, "--queue-size", &value)) {
                return CONFIG_PARSE_ERROR;
            }
            if (value < CONFIG_MIN_QUEUE_CAPACITY || value > CONFIG_MAX_QUEUE_CAPACITY) {
                fprintf(stderr, "cinderhttp: --queue-size must be between %d and %d (got %ld)\n",
                        CONFIG_MIN_QUEUE_CAPACITY, CONFIG_MAX_QUEUE_CAPACITY, value);
                return CONFIG_PARSE_ERROR;
            }
            config->queue_capacity = (int)value;
        } else if (strcmp(arg, "--root") == 0) {
            if (i + 1 >= argc) {
                fprintf(stderr, "cinderhttp: option '--root' requires a value\n");
                return CONFIG_PARSE_ERROR;
            }
            i++;
            int written =
                snprintf(config->document_root, sizeof(config->document_root), "%s", argv[i]);
            if (written < 0 || (size_t)written >= sizeof(config->document_root)) {
                fprintf(stderr, "cinderhttp: --root path is too long (max %zu characters)\n",
                        sizeof(config->document_root) - 1);
                return CONFIG_PARSE_ERROR;
            }
        } else {
            fprintf(stderr, "cinderhttp: unrecognized option '%s'\n", arg);
            fprintf(stderr, "Try 'cinderhttp --help' for usage information.\n");
            return CONFIG_PARSE_ERROR;
        }
    }

    return CONFIG_PARSE_OK;
}

void config_print_usage(FILE *stream) {
    fprintf(stream,
            "Usage: cinderhttp [OPTIONS]\n"
            "\n"
            "CinderHTTP is a multithreaded HTTP/1.1 server written from scratch in C.\n"
            "\n"
            "Options:\n"
            "  --port <port>          TCP port to listen on (default: %d)\n"
            "  --workers <count>      Worker thread pool size (default: %d)\n"
            "  --queue-size <count>   Bounded connection queue capacity (default: %d)\n"
            "  --root <path>          Document root for static files (default: %s)\n"
            "  --verbose              Enable verbose diagnostic logging\n"
            "  --help                 Show this help message and exit\n"
            "\n"
            "Examples:\n"
            "  cinderhttp\n"
            "  cinderhttp --port 9000\n"
            "  cinderhttp --workers 8 --root ./public\n"
            "  cinderhttp --queue-size 128 --verbose\n"
            "\n"
            "Note: --workers, --queue-size, and --root are parsed and validated now but do\n"
            "not yet change server behavior; the thread pool and static file server are\n"
            "introduced in later development stages (see docs/roadmap.md).\n",
            CONFIG_DEFAULT_PORT, CONFIG_DEFAULT_WORKER_COUNT, CONFIG_DEFAULT_QUEUE_CAPACITY,
            CONFIG_DEFAULT_DOCUMENT_ROOT);
}
