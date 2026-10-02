#define _POSIX_C_SOURCE 200809L

#include "test_wait.h"
#include "builtins/background_model.h"

#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

static int Fail(const char *test, const char *message)
{
    fprintf(stderr, "%s: %s\n", test, message);
    return 1;
}

static bool PumpUntilExited(PicoBgTable *table, PicoAgentId agent, const char *id)
{
    PICO_TEST_WAIT_LOOP("background job progress")
    {
        PicoBgJobInfo info[PICO_BG_MAX_RECORDS];
        int n;
        int j;
        PicoBgTable_Pump(table);
        n = PicoBgTable_CopyJobs(table, agent, info, PICO_BG_MAX_RECORDS);
        for (j = 0; j < n; j++)
        {
            if (strcmp(info[j].id, id) == 0 && info[j].status != PICO_BG_RUNNING)
            {
                return true;
            }
        }
    }
    return false;
}

static bool WaitForLog(PicoBgTable *table, PicoAgentId agent, const char *id, const char *needle)
{
    PICO_TEST_WAIT_LOOP("background job progress")
    {
        char *log;
        char *error = NULL;
        PicoBgTable_Pump(table);
        log = PicoBgTable_Log(table, agent, id, &error);
        free(error);
        if (log && strstr(log, needle))
        {
            free(log);
            return true;
        }
        free(log);
    }
    return false;
}

static int TestSpawnAndList(PicoBgTable *table, const char *cwd)
{
    const char *test = "spawn_and_list";
    char *error = NULL;
    char *json = PicoBgTable_Spawn(table, 1, cwd, "sleep", "while :; do sleep 3600; done", &error);
    char *list;
    int running;
    if (!json || error)
    {
        free(json);
        free(error);
        return Fail(test, "spawn failed");
    }
    if (!strstr(json, "\"id\":\"bg_1\"") || !strstr(json, "\"status\":\"running\""))
    {
        free(json);
        return Fail(test, "spawn result missing running id");
    }
    free(json);
    list = PicoBgTable_ListJson(table, 1);
    running = PicoBgTable_RunningCount(table, 1);
    if (!list || !strstr(list, "bg_1") || !strstr(list, "running") || running < 1)
    {
        free(list);
        return Fail(test, "list did not show a running job");
    }
    free(list);
    return 0;
}

static int TestIsolation(PicoBgTable *table)
{
    const char *test = "isolation";
    char *list = PicoBgTable_ListJson(table, 2);
    int running = PicoBgTable_RunningCount(table, 2);
    if (!list || strcmp(list, "[]") != 0 || running != 0)
    {
        free(list);
        return Fail(test, "other agent saw jobs");
    }
    free(list);
    return 0;
}

static int TestCap(PicoBgTable *table, const char *cwd)
{
    const char *test = "running_cap";
    int spawned = 0;
    char *error = NULL;
    char *json;
    for (;;)
    {
        error = NULL;
        json = PicoBgTable_Spawn(table, 3, cwd, "cap", "while :; do sleep 3600; done", &error);
        if (!json)
        {
            free(error);
            break;
        }
        free(json);
        spawned++;
        if (spawned > 64)
        {
            return Fail(test, "spawn never hit a cap");
        }
    }
    if (spawned < 1)
    {
        return Fail(test, "could not spawn a job");
    }
    error = NULL;
    json = PicoBgTable_Spawn(table, 3, cwd, "cap", "while :; do sleep 3600; done", &error);
    if (json)
    {
        free(json);
        free(error);
        return Fail(test, "spawn succeeded past the cap");
    }
    free(error);
    if (PicoBgTable_RunningCount(table, 3) != spawned)
    {
        return Fail(test, "running count does not match successful spawns");
    }
    PicoBgTable_ResetAgent(table, 3);
    if (PicoBgTable_RunningCount(table, 3) != 0)
    {
        return Fail(test, "reset left running jobs");
    }
    return 0;
}

static int TestSmallLog(PicoBgTable *table, const char *cwd)
{
    const char *test = "small_log";
    char *error = NULL;
    char *json = PicoBgTable_Spawn(table, 4, cwd, "echo", "printf 'hello\\nworld\\n'", &error);
    char *log;
    if (!json || !strstr(json, "bg_1"))
    {
        free(json);
        free(error);
        return Fail(test, "spawn failed");
    }
    free(json);
    if (!WaitForLog(table, 4, "bg_1", "hello") || !PumpUntilExited(table, 4, "bg_1"))
    {
        return Fail(test, "output did not appear");
    }
    error = NULL;
    log = PicoBgTable_Log(table, 4, "bg_1", &error);
    if (!log || !strstr(log, "hello") || !strstr(log, "world") || strstr(log, "…") ||
        strstr(log, "/tmp") || error)
    {
        free(log);
        free(error);
        return Fail(test, "small log was not returned complete");
    }
    free(log);
    return 0;
}

static int TestRollingLog(PicoBgTable *table, const char *cwd)
{
    const char *test = "rolling_log";
    char path[4096];
    FILE *file;
    char *error = NULL;
    char *json;
    char *log;
    char command[4200];

    snprintf(path, sizeof(path), "%s/rolling.txt", cwd);
    file = fopen(path, "w");
    if (!file)
    {
        return Fail(test, "could not write rolling input");
    }
    fprintf(file, "FIRST_LINE\n");
    for (int i = 0; i < PICO_BG_LOG_MAX; i++)
    {
        fprintf(file, "PAD-%05d\n", i);
    }
    fprintf(file, "LAST_LINE\n");
    fclose(file);

    snprintf(command, sizeof(command), "cat '%s'", path);
    json = PicoBgTable_Spawn(table, 5, cwd, "roll", command, &error);
    if (!json)
    {
        free(error);
        return Fail(test, "spawn failed");
    }
    free(json);
    if (!WaitForLog(table, 5, "bg_1", "LAST_LINE"))
    {
        error = NULL;
        log = PicoBgTable_Log(table, 5, "bg_1", &error);
        fprintf(stderr, "rolling log so far (%zu): %s\n", log ? strlen(log) : 0, log ? log : "(null)");
        free(log);
        free(error);
        return Fail(test, "rolling output missing LAST_LINE");
    }
    if (!PumpUntilExited(table, 5, "bg_1"))
    {
        return Fail(test, "rolling job did not exit");
    }
    error = NULL;
    log = PicoBgTable_Log(table, 5, "bg_1", &error);
    if (!log || error)
    {
        free(log);
        free(error);
        return Fail(test, "missing log");
    }
    if (!strstr(log, "LAST_LINE") || strstr(log, "FIRST_LINE") || strstr(log, "/tmp") ||
        strlen(log) > (size_t)PICO_BG_LOG_MAX)
    {
        free(log);
        return Fail(test, "oldest lines were kept or log exceeded the window");
    }
    free(log);
    return 0;
}

static int TestKillAndReap(PicoBgTable *table, const char *cwd)
{
    const char *test = "kill_and_reap";
    char *error = NULL;
    char *json;
    char *killed;
    PicoBgJobInfo info[PICO_BG_MAX_RECORDS];
    int n;
    pid_t child = 0;
    char pid_path[4096];
    FILE *pid_file;

    snprintf(pid_path, sizeof(pid_path), "%s/child.pid", cwd);
    json = PicoBgTable_Spawn(table, 6, cwd, "loop",
                             "echo $$ > child.pid; while true; do sleep 1; done", &error);
    if (!json)
    {
        free(error);
        return Fail(test, "spawn failed");
    }
    free(json);

    PICO_TEST_WAIT_LOOP("background job progress")
    {
        PicoBgTable_Pump(table);
        pid_file = fopen(pid_path, "r");
        if (pid_file)
        {
            int raw = 0;
            if (fscanf(pid_file, "%d", &raw) == 1 && raw > 0)
            {
                child = (pid_t)raw;
            }
            fclose(pid_file);
            if (child > 0)
            {
                break;
            }
        }
    }
    if (child <= 0)
    {
        return Fail(test, "child pid file missing");
    }

    error = NULL;
    killed = PicoBgTable_Kill(table, 6, "bg_1", &error);
    if (!killed || error || !strstr(killed, "\"status\":\"killed\""))
    {
        free(killed);
        free(error);
        return Fail(test, "kill did not report killed");
    }
    /* A killed job died by signal; the response must not claim a clean exit code. */
    if (!strstr(killed, "\"signal\":") || strstr(killed, "\"exit_code\":0"))
    {
        free(killed);
        return Fail(test, "kill response did not report the signal");
    }
    free(killed);

    {
        bool gone = false;
        PICO_TEST_WAIT_LOOP("background job progress")
        {
            PicoBgTable_Pump(table);
            if (kill(child, 0) != 0 && errno == ESRCH)
            {
                gone = true;
                break;
            }
        }
        if (!gone)
        {
            return Fail(test, "child still exists after reap");
        }
    }
    n = PicoBgTable_CopyJobs(table, 6, info, PICO_BG_MAX_RECORDS);
    if (n != 1 || info[0].status != PICO_BG_KILLED)
    {
        return Fail(test, "reap overwrote killed status");
    }
    return 0;
}

static int TestNaturalExit(PicoBgTable *table, const char *cwd)
{
    const char *test = "natural_exit";
    char *error = NULL;
    char *json = PicoBgTable_Spawn(table, 7, cwd, "fail", "exit 3", &error);
    PicoBgJobInfo info[PICO_BG_MAX_RECORDS];
    int n;
    if (!json)
    {
        free(error);
        return Fail(test, "spawn failed");
    }
    free(json);
    if (!PumpUntilExited(table, 7, "bg_1"))
    {
        return Fail(test, "job did not exit");
    }
    n = PicoBgTable_CopyJobs(table, 7, info, PICO_BG_MAX_RECORDS);
    if (n != 1 || info[0].status != PICO_BG_EXITED || info[0].exit_code != 3)
    {
        return Fail(test, "list did not report exited with code 3");
    }
    return 0;
}

static int TestEmptyPump(void)
{
    const char *test = "empty_pump";
    PicoBgTable *table = PicoBgTable_Create();
    if (!table)
    {
        return Fail(test, "create failed");
    }
    PicoBgTable_Pump(table);
    PicoBgTable_Pump(NULL);
    PicoBgTable_Destroy(table);
    return 0;
}

static int TestResetClears(PicoBgTable *table, const char *cwd)
{
    const char *test = "reset_clears";
    char *error = NULL;
    char *json = PicoBgTable_Spawn(table, 8, cwd, "sleep", "while :; do sleep 3600; done", &error);
    char *list;
    if (!json)
    {
        free(error);
        return Fail(test, "spawn failed");
    }
    free(json);
    PicoBgTable_ResetAgent(table, 8);
    list = PicoBgTable_ListJson(table, 8);
    if (!list || strcmp(list, "[]") != 0 || PicoBgTable_RunningCount(table, 8) != 0)
    {
        free(list);
        return Fail(test, "reset did not clear the list");
    }
    free(list);
    return 0;
}

static int TestResetReaps(PicoBgTable *table, const char *cwd)
{
    const char *test = "reset_reaps";
    char pid_path[4096];
    char *error = NULL;
    char *json;
    pid_t child = 0;

    snprintf(pid_path, sizeof(pid_path), "%s/reset.pid", cwd);
    json = PicoBgTable_Spawn(table, 9, cwd, "loop",
                             "echo $$ > reset.pid; while true; do sleep 1; done", &error);
    if (!json)
    {
        free(error);
        return Fail(test, "spawn failed");
    }
    free(json);
    PICO_TEST_WAIT(child <= 0)
    {
        FILE *pid_file;
        PicoBgTable_Pump(table);
        pid_file = fopen(pid_path, "r");
        if (pid_file)
        {
            int raw = 0;
            if (fscanf(pid_file, "%d", &raw) == 1 && raw > 0)
            {
                child = (pid_t)raw;
            }
            fclose(pid_file);
        }
    }
    if (child <= 0)
    {
        return Fail(test, "child pid file missing");
    }
    PicoBgTable_ResetAgent(table, 9);
    /* A reaped child is gone entirely; a leaked zombie still answers kill(pid, 0). */
    PICO_TEST_WAIT_LOOP("background job progress")
    {
        if (kill(child, 0) != 0 && errno == ESRCH)
        {
            return 0;
        }
    }
    return Fail(test, "reset left a zombie child");
}

static int TestAgentChurn(PicoBgTable *table, const char *cwd)
{
    const char *test = "agent_churn";
    /* More distinct agents than can ever be live at once must not exhaust spawns:
     * resetting an agent recycles its id serial. */
    for (int i = 0; i < PICO_MAX_TOTAL_AGENTS * 2; i++)
    {
        PicoAgentId id = 100 + (PicoAgentId)i;
        char *error = NULL;
        char *json = PicoBgTable_Spawn(table, id, cwd, "churn", "true", &error);
        if (!json)
        {
            Fail(test, error ? error : "spawn failed during agent churn");
            free(error);
            return 1;
        }
        free(json);
        PicoBgTable_ResetAgent(table, id);
    }
    return 0;
}

int main(void)
{
    char cwd[] = "/tmp/pico-bg-test-XXXXXX";
    PicoBgTable *table;
    int rc = 0;
    if (!mkdtemp(cwd))
    {
        return Fail("setup", "mkdtemp");
    }
    table = PicoBgTable_Create();
    if (!table)
    {
        rmdir(cwd);
        return Fail("setup", "create table");
    }

    rc |= PICO_TEST_RUN(TestEmptyPump());
    rc |= PICO_TEST_RUN(TestSpawnAndList(table, cwd));
    rc |= PICO_TEST_RUN(TestIsolation(table));
    rc |= PICO_TEST_RUN(TestCap(table, cwd));
    rc |= PICO_TEST_RUN(TestSmallLog(table, cwd));
    rc |= PICO_TEST_RUN(TestRollingLog(table, cwd));
    rc |= PICO_TEST_RUN(TestKillAndReap(table, cwd));
    rc |= PICO_TEST_RUN(TestNaturalExit(table, cwd));
    rc |= PICO_TEST_RUN(TestResetClears(table, cwd));
    rc |= PICO_TEST_RUN(TestResetReaps(table, cwd));
    rc |= PICO_TEST_RUN(TestAgentChurn(table, cwd));

    PicoBgTable_Destroy(table);
    unlink("rolling.txt");
    {
        char path[4096];
        snprintf(path, sizeof(path), "%s/rolling.txt", cwd);
        unlink(path);
        snprintf(path, sizeof(path), "%s/child.pid", cwd);
        unlink(path);
        snprintf(path, sizeof(path), "%s/reset.pid", cwd);
        unlink(path);
    }
    rmdir(cwd);
    return rc;
}
