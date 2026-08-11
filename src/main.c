/*
 * main.c - process entry point.
 *
 * By design, main() only wires together three things it does not itself
 * implement: configuration, the listening socket, and the accept loop. Any
 * behavior more interesting than that belongs in config.c or server.c, not
 * here.
 */
#include <stdio.h>
#include <stdlib.h>

#include "config.h"
#include "server.h"

int main(int argc, char *argv[]) {
    server_config_t config;
    config_set_defaults(&config);

    config_parse_result_t parse_result = config_parse_args(&config, argc, argv);
    if (parse_result == CONFIG_PARSE_HELP) {
        return EXIT_SUCCESS;
    }
    if (parse_result == CONFIG_PARSE_ERROR) {
        return EXIT_FAILURE;
    }

    if (config.verbose) {
        fprintf(stderr, "[verbose] port=%d workers=%d queue_capacity=%d root=%s\n", config.port,
                config.worker_count, config.queue_capacity, config.document_root);
    }

    server_install_signal_handlers();

    int listen_fd = server_create_listening_socket(&config);
    if (listen_fd < 0) {
        fprintf(stderr, "cinderhttp: failed to start server, exiting\n");
        return EXIT_FAILURE;
    }

    int run_result = server_run(&config, listen_fd);

    server_close_listening_socket(listen_fd);
    printf("CinderHTTP shut down cleanly.\n");

    return (run_result == 0) ? EXIT_SUCCESS : EXIT_FAILURE;
}
