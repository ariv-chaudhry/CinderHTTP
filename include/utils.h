/*
 * utils.h - small, dependency-free helpers shared across modules.
 *
 * This file intentionally stays small. It is not a dumping ground: only
 * genuinely cross-cutting helpers belong here (e.g. reliable socket I/O).
 * Anything specific to HTTP, routing, or file serving belongs in its own
 * module instead.
 */
#ifndef CINDERHTTP_UTILS_H
#define CINDERHTTP_UTILS_H

#include <sys/types.h>

/*
 * send() is not guaranteed to transmit every byte of `buffer` in one call;
 * it may write only part of the data if the socket's send buffer is nearly
 * full, and it can be interrupted by a signal. send_all() retries until
 * either all `length` bytes have been written or an unrecoverable error
 * occurs.
 *
 * On platforms that define MSG_NOSIGNAL, that flag is used so a write to a
 * closed peer does not raise SIGPIPE. The server also installs SIG_IGN for
 * SIGPIPE as a portable baseline. A disconnected client therefore surfaces
 * as a normal I/O failure (-1), not process death.
 *
 * Returns `length` (cast to ssize_t) on success, or -1 if send() failed
 * with an error other than EINTR (errno is left as set by send()).
 */
ssize_t send_all(int socket_fd, const void *buffer, size_t length);

#endif /* CINDERHTTP_UTILS_H */
