#ifndef PICO_TEST_CLOCK_H
#define PICO_TEST_CLOCK_H
#include <stdbool.h>
#include <time.h>

/* Link-time clock substitution in test executables only. Realtime deadlines
 * remain real outside explicit controlled-expiry tests; streaming tests control monotonic time independently of CPU load. */
/* Override time() only; worker wait deadlines remain real. Zero restores it. */
void PicoTestClock_SetWallTime(time_t seconds);
void PicoTestClock_Set(double seconds);
void PicoTestClock_Advance(double seconds);
void PicoTestClock_Reset(void);
void PicoTestClock_ExpireWaits(void);
bool PicoTestClock_SharedFutureDeadline(unsigned minimum_waits);
void PicoTestClock_ResumeWaits(void);

#endif
