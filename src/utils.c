#include "utils.h"

#include <errno.h>
#include <stddef.h>
#include <sys/socket.h>

#ifndef MSG_NOSIGNAL
#define MSG_NOSIGNAL 0
#endif

ssize_t send_all(int socket_fd, const void *buffer, size_t length) {
    if (buffer == NULL && length > 0) {
        errno = EINVAL;
        return -1;
    }
    if (length == 0) {
        return 0;
    }

    const unsigned char *cursor = buffer;
    size_t total_sent = 0;

    /*
     * MSG_NOSIGNAL (Linux/BSD) avoids generating SIGPIPE on this write when
     * the peer has closed. The process also ignores SIGPIPE in
     * server_install_signal_handlers() for portability where MSG_NOSIGNAL is
     * unavailable. Either way, a disconnected client is an I/O error here —
     * never a process-terminating signal.
     */
    while (total_sent < length) {
        ssize_t sent =
            send(socket_fd, cursor + total_sent, length - total_sent, MSG_NOSIGNAL);

        if (sent < 0) {
            if (errno == EINTR) {
                continue;
            }
            return -1;
        }
        if (sent == 0) {
            /* Should not happen for a connected stream socket; treat as error. */
            errno = ECONNRESET;
            return -1;
        }

        total_sent += (size_t)sent;
    }

    return (ssize_t)total_sent;
}
