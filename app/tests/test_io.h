#ifndef PICO_TEST_IO_H
#define PICO_TEST_IO_H
#include <stdbool.h>
/* Block a specific file/database open on a worker, and observe whether its
 * caller tried to do that I/O on the owning UI thread instead. */
void PicoTestIo_Hold(const char *path);
bool PicoTestIo_OnOwner(void);
void PicoTestIo_WaitEntered(void);
void PicoTestIo_Release(void);
#endif
