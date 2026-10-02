#include "test_lock.h"
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <unistd.h>

static int notify_fd = -1;
void PicoTestLock_NotifyContention(int ready_fd) { notify_fd = ready_fd; }

int __real_fcntl(int fd, int command, ...);
int __wrap_fcntl(int fd, int command, ...)
{
    if (command == F_GETFD || command == F_GETFL || command == F_GETOWN)
        return __real_fcntl(fd, command);
    va_list args;
    va_start(args, command);
    if (command == F_SETLK || command == F_SETLKW || command == F_GETLK)
    {
        struct flock *lock = va_arg(args, struct flock *);
        va_end(args);
        if (command == F_SETLKW && notify_fd >= 0)
        {
            int result = __real_fcntl(fd, F_SETLK, lock);
            if (result == 0 || (errno != EAGAIN && errno != EACCES)) return result;
            char ready = 'x';
            ssize_t sent;
            do { sent = write(notify_fd, &ready, 1); } while (sent < 0 && errno == EINTR);
            notify_fd = -1;
            if (sent != 1) return -1;
        }
        return __real_fcntl(fd, command, lock);
    }
    int value = va_arg(args, int);
    va_end(args);
    return __real_fcntl(fd, command, value);
}
