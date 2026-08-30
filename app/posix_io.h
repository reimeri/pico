#ifndef PICO_POSIX_IO_H
#define PICO_POSIX_IO_H

#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <unistd.h>

static inline bool PicoIO_WriteAll(int fd, const void *data, size_t len)
{
    const char *bytes = (const char *)data;
    size_t offset = 0;
    while (offset < len)
    {
        ssize_t wrote = write(fd, bytes + offset, len - offset);
        if (wrote > 0)
        {
            offset += (size_t)wrote;
        }
        else if (wrote < 0 && errno == EINTR)
        {
            continue;
        }
        else
        {
            return false;
        }
    }
    return true;
}

#endif
