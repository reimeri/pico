#ifndef PICO_BROWSER_TRANSPORT_H
#define PICO_BROWSER_TRANSPORT_H
/* Same-build Unix socket transport. Ancillary descriptors belong to the
 * receiver, are CLOEXEC, and are closed even on malformed/truncated messages. */
#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>
enum { PB_MAX_FDS = 5 };
typedef struct PbFds { int fd[PB_MAX_FDS]; unsigned count; } PbFds;
static inline void PbFdsClose(PbFds *fds) {
    for (unsigned i = 0; i < fds->count; ++i) close(fds->fd[i]);
    fds->count = 0;
}
static inline ssize_t PbSend(int socket, const void *data, size_t size, const PbFds *fds, int flags) {
    struct iovec io = {(void *)data, size};
    union { struct cmsghdr align; char bytes[CMSG_SPACE(sizeof(int) * PB_MAX_FDS)]; } control;
    struct msghdr msg; memset(&msg, 0, sizeof(msg)); msg.msg_iov = &io; msg.msg_iovlen = 1;
    if (fds && fds->count) {
        if (fds->count > PB_MAX_FDS) { errno = EINVAL; return -1; }
        memset(&control, 0, sizeof(control));
        msg.msg_control = control.bytes; msg.msg_controllen = CMSG_SPACE(sizeof(int) * fds->count);
        struct cmsghdr *c = CMSG_FIRSTHDR(&msg);
        c->cmsg_level = SOL_SOCKET; c->cmsg_type = SCM_RIGHTS; c->cmsg_len = CMSG_LEN(sizeof(int) * fds->count);
        memcpy(CMSG_DATA(c), fds->fd, sizeof(int) * fds->count);
    }
    ssize_t n;
    do { n = sendmsg(socket, &msg, flags | MSG_NOSIGNAL); } while (n < 0 && errno == EINTR);
    return n;
}
/* Stream callers repeat this until their header is complete. Seqpacket callers
 * use MSG_TRUNC and verify the returned record size. Never use recv for headers
 * that can carry descriptors (even if a record will subsequently be rejected). */
static inline ssize_t PbReceive(int socket, void *data, size_t size, PbFds *fds, int flags) {
    struct iovec io = {data, size};
    union { struct cmsghdr align; char bytes[CMSG_SPACE(sizeof(int) * PB_MAX_FDS)]; } control;
    struct msghdr msg; memset(&msg, 0, sizeof(msg)); msg.msg_iov = &io; msg.msg_iovlen = 1;
    msg.msg_control = control.bytes; msg.msg_controllen = sizeof(control.bytes);
    ssize_t n;
    do { n = recvmsg(socket, &msg, flags | MSG_CMSG_CLOEXEC); } while (n < 0 && errno == EINTR);
    if (n < 0) return n;
    bool valid = !(msg.msg_flags & MSG_CTRUNC);
    for (struct cmsghdr *c = CMSG_FIRSTHDR(&msg); c; c = CMSG_NXTHDR(&msg, c)) {
        if (c->cmsg_level != SOL_SOCKET || c->cmsg_type != SCM_RIGHTS || c->cmsg_len < CMSG_LEN(0)) { valid = false; continue; }
        size_t bytes = c->cmsg_len - CMSG_LEN(0);
        if (bytes % sizeof(int)) valid = false;
        for (size_t i = 0; i < bytes / sizeof(int); ++i) {
            int fd; memcpy(&fd, (char *)CMSG_DATA(c) + i * sizeof(fd), sizeof(fd));
            if (fds->count < PB_MAX_FDS) fds->fd[fds->count++] = fd;
            else { close(fd); valid = false; }
        }
    }
    if (!valid) { PbFdsClose(fds); errno = EPROTO; return -1; }
    return n;
}
#endif
