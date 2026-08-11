#include "utils.h"

#include <errno.h>
#include <sys/socket.h>

ssize_t send_all(int socket_fd, const void *buffer, size_t length) {
    const unsigned char *cursor = buffer;
    size_t total_sent = 0;

    while (total_sent < length) {
        ssize_t sent = send(socket_fd, cursor + total_sent, length - total_sent, 0);

        if (sent < 0) {
            if (errno == EINTR) {
                continue;
            }
            return -1;
        }

        total_sent += (size_t)sent;
    }

    return (ssize_t)total_sent;
}
