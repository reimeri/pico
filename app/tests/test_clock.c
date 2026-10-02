#include "test_clock.h"
#include <stdatomic.h>
#include <stdint.h>
#include <time.h>
#include <pthread.h>
#include <errno.h>

static atomic_bool enabled;
static atomic_bool expire_waits;
static struct timespec expiry_now, expiry_origin;
static pthread_mutex_t expiry_mu = PTHREAD_MUTEX_INITIALIZER;
static atomic_uint_fast64_t nanoseconds;

void PicoTestClock_Set(double seconds)
{
    atomic_store(&nanoseconds, (uint64_t)(seconds * 1000000000.0));
    atomic_store(&enabled, 1);
}

void PicoTestClock_Advance(double seconds)
{
    /* A shared fake provider must not enable time control for later turns. */
    if (atomic_load(&enabled))
        atomic_store(&nanoseconds, (uint64_t)(seconds * 1000000000.0));
}

void PicoTestClock_Reset(void)
{
    atomic_store(&enabled, 0);
}

int __real_clock_gettime(clockid_t clock, struct timespec *time);
int __wrap_clock_gettime(clockid_t clock, struct timespec *time)
{
    if (clock == CLOCK_REALTIME && atomic_load(&expire_waits))
    {
        pthread_mutex_lock(&expiry_mu);
        *time = expiry_now;
        pthread_mutex_unlock(&expiry_mu);
        return 0;
    }
    if (clock == CLOCK_MONOTONIC && atomic_load(&enabled))
    {
        uint64_t value = atomic_load(&nanoseconds);
        time->tv_sec = (time_t)(value / 1000000000);
        time->tv_nsec = (long)(value % 1000000000);
        return 0;
    }
    return __real_clock_gettime(clock, time);
}

/* Deadline tests observe the platform wait boundary instead of comparing
 * elapsed wall time. Expiry is controlled; every blocked owner must receive
 * the same future deadline, regardless of how many owners exist. */
static struct timespec observed_deadline;
static unsigned wait_count;
static bool shared_future;

void PicoTestClock_ExpireWaits(void)
{
    pthread_mutex_lock(&expiry_mu);
    __real_clock_gettime(CLOCK_REALTIME, &expiry_now);
    expiry_origin = expiry_now;
    wait_count = 0;
    shared_future = true;
    atomic_store(&expire_waits, true);
    pthread_mutex_unlock(&expiry_mu);
}

bool PicoTestClock_SharedFutureDeadline(unsigned minimum_waits)
{
    pthread_mutex_lock(&expiry_mu);
    bool result = wait_count >= minimum_waits && shared_future;
    pthread_mutex_unlock(&expiry_mu);
    return result;
}

void PicoTestClock_ResumeWaits(void) { atomic_store(&expire_waits, false); }

int __real_pthread_cond_timedwait(pthread_cond_t *, pthread_mutex_t *, const struct timespec *);
int __wrap_pthread_cond_timedwait(pthread_cond_t *cv, pthread_mutex_t *mu,
                                 const struct timespec *deadline)
{
    if (!atomic_load(&expire_waits)) return __real_pthread_cond_timedwait(cv, mu, deadline);
    pthread_mutex_lock(&expiry_mu);
    struct timespec now = expiry_origin;
    if (deadline->tv_sec < now.tv_sec ||
        (deadline->tv_sec == now.tv_sec && deadline->tv_nsec <= now.tv_nsec))
        shared_future = false;
    if (wait_count && (deadline->tv_sec != observed_deadline.tv_sec ||
                      deadline->tv_nsec != observed_deadline.tv_nsec))
        shared_future = false;
    observed_deadline = *deadline;
    wait_count++;
    /* Expiration advances the clock. A regressed per-workspace budget would
     * compute a new later deadline instead of reusing the expired shared one. */
    expiry_now = *deadline;
    pthread_mutex_unlock(&expiry_mu);
    return ETIMEDOUT;
}
