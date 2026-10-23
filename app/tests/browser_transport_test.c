#define _GNU_SOURCE
#include "browser/transport.h"
#include <stdlib.h>
#include <dirent.h>
#include <fcntl.h>
#include <stdio.h>
#include <sys/mman.h>

#define CHECK(condition) do { if (!(condition)) { fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #condition); exit(1); } } while (0)

static int OpenDescriptors(void) {
    DIR *dir = opendir("/proc/self/fd"); CHECK(dir);
    int count = 0; struct dirent *entry;
    while ((entry = readdir(dir))) if (entry->d_name[0] != '.') ++count;
    closedir(dir); return count;
}
int main(void) {
    int stream[2]; CHECK(!socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, stream));
    int file = memfd_create("browser-transport-test", MFD_CLOEXEC); CHECK(file >= 0);
    CHECK(write(file, "owned", 5) == 5);
    PbFds outgoing = {.fd={file}, .count=1}, received = {0};
    CHECK(PbSend(stream[0], "head", 4, &outgoing, 0) == 4);
    CHECK(PbSend(stream[0], "er", 2, NULL, 0) == 2);
    PbFdsClose(&outgoing); // sender closure must not invalidate the transferred handle
    char data[8] = {0}; size_t used = 0;
    while (used < 6) {
        ssize_t n = PbReceive(stream[1], data + used, 6 - used, &received, 0); CHECK(n > 0); used += n;
    }
    CHECK(!strcmp(data, "header") && received.count == 1);
    CHECK(fcntl(received.fd[0], F_GETFD) & FD_CLOEXEC);
    CHECK(pread(received.fd[0], data, 5, 0) == 5 && !memcmp(data, "owned", 5));
    PbFdsClose(&received); close(stream[0]); close(stream[1]);

    // Kernel truncation must fail closed, including closing every delivered FD.
    // Send more ancillary entries than the receiver can represent, via raw IPC.
    int pair[2]; CHECK(!socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0, pair));
    file = memfd_create("browser-truncated-rights", MFD_CLOEXEC); CHECK(file >= 0);
    int before = OpenDescriptors();
    int descriptors[PB_MAX_FDS + 4];
    for (size_t i = 0; i < sizeof(descriptors)/sizeof(*descriptors); ++i) descriptors[i] = file;
    union { struct cmsghdr align; char bytes[CMSG_SPACE(sizeof(descriptors))]; } control = {0};
    struct iovec io = {.iov_base=data, .iov_len=1};
    struct msghdr msg = {.msg_iov=&io, .msg_iovlen=1, .msg_control=control.bytes, .msg_controllen=sizeof(control.bytes)};
    struct cmsghdr *c = CMSG_FIRSTHDR(&msg); c->cmsg_level = SOL_SOCKET; c->cmsg_type = SCM_RIGHTS; c->cmsg_len = CMSG_LEN(sizeof(descriptors));
    memcpy(CMSG_DATA(c), descriptors, sizeof(descriptors));
    CHECK(sendmsg(pair[0], &msg, 0) == 1);
    CHECK(PbReceive(pair[1], data, sizeof(data), &received, MSG_TRUNC) == -1 && errno == EPROTO);
    CHECK(!received.count && OpenDescriptors() == before);
    close(file);

    // A short receive buffer must expose the original seqpacket length so
    // malformed commands cannot be accepted as valid prefixes.
    CHECK(PbSend(pair[0], "oversize", 8, NULL, 0) == 8);
    CHECK(PbReceive(pair[1], data, 1, &received, MSG_TRUNC) == 8);
    close(pair[0]); close(pair[1]);
    puts("PASS: descriptor lifetime/CLOEXEC, fragmented stream, truncated ancillary cleanup, atomic command length");
    return 0;
}
