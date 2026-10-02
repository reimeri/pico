#ifndef PICO_TEST_LOCK_H
#define PICO_TEST_LOCK_H
/* Notify the fixture only after the OS reports a conflicting record lock. */
void PicoTestLock_NotifyContention(int ready_fd);
#endif
