#include "test_io.h"
#include "test_wait.h"
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <sqlite3.h>

static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t cv = PTHREAD_COND_INITIALIZER;
static char held_path[4096];
static pthread_t owner;
static bool held, entered, on_owner;

void PicoTestIo_Hold(const char *path)
{
    pthread_mutex_lock(&mu);
    snprintf(held_path, sizeof(held_path), "%s", path);
    owner = pthread_self();
    held = true;
    entered = on_owner = false;
    pthread_mutex_unlock(&mu);
}

bool PicoTestIo_OnOwner(void)
{
    pthread_mutex_lock(&mu);
    bool result = on_owner;
    pthread_mutex_unlock(&mu);
    return result;
}

void PicoTestIo_WaitEntered(void)
{
    PicoTest_Wait(__func__, "worker reached the held I/O operation");
    pthread_mutex_lock(&mu);
    while (!entered) pthread_cond_wait(&cv, &mu);
    pthread_mutex_unlock(&mu);
}

void PicoTestIo_Release(void)
{
    pthread_mutex_lock(&mu);
    held = false;
    pthread_cond_broadcast(&cv);
    pthread_mutex_unlock(&mu);
}

static void BeforeOpen(const char *path)
{
    pthread_mutex_lock(&mu);
    if (held && path && strcmp(held_path, path) == 0)
    {
        if (pthread_equal(owner, pthread_self())) on_owner = true;
        else
        {
            entered = true;
            pthread_cond_broadcast(&cv);
            while (held) pthread_cond_wait(&cv, &mu);
        }
    }
    pthread_mutex_unlock(&mu);
}

FILE *__real_fopen(const char *, const char *);
FILE *__wrap_fopen(const char *path, const char *mode)
{
    BeforeOpen(path);
    return __real_fopen(path, mode);
}
int __real_sqlite3_open_v2(const char *, sqlite3 **, int, const char *);
int __wrap_sqlite3_open_v2(const char *path, sqlite3 **db, int flags, const char *vfs)
{
    BeforeOpen(path);
    return __real_sqlite3_open_v2(path, db, flags, vfs);
}
