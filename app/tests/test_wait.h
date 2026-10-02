#ifndef PICO_TEST_WAIT_H
#define PICO_TEST_WAIT_H

#include <errno.h>
#include <stdio.h>
#include <time.h>

/* Readiness has no machine-speed budget. CTest owns the overall hang watchdog;
 * stderr progress remains visible in its timeout report, even with buffered
 * stdout. Polling only yields CPU where the production API requires pumping. */
static inline void PicoTest_Wait(const char *test, const char *condition)
{
    fprintf(stderr, "WAIT: %s: %s (guarded by CTest overall timeout)\n", test, condition);
    fflush(stderr);
}

static inline void PicoTest_Poll(void)
{
    struct timespec pause = {.tv_nsec = 1000000};
    while (nanosleep(&pause, &pause) != 0 && errno == EINTR) {}
}

static inline void PicoTest_Case(const char *test)
{
    fprintf(stderr, "RUN: %s\n", test);
    fflush(stderr);
}

#define PICO_TEST_WAIT(condition) \
    for (PicoTest_Wait(__func__, #condition); (condition); PicoTest_Poll())
#define PICO_TEST_WAIT_LOOP(event) \
    for (PicoTest_Wait(__func__, event); ; PicoTest_Poll())
#define PICO_TEST_RUN(call) (PicoTest_Case(#call), (call))

#endif
