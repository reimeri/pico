#define _POSIX_C_SOURCE 200809L

#include "pico/plugin.h"

#include <errno.h>
#include <inttypes.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <unistd.h>

#define EXPECTED_HEAD ((size_t)12800)
#define EXPECTED_TAIL ((size_t)27200)
#define EXPECTED_LIMIT (EXPECTED_HEAD + EXPECTED_TAIL)
#define OVER_LIMIT (EXPECTED_LIMIT + (size_t)1)
#define LARGE_HEAD_SIZE UINT64_C(33554432)
#define MAX_CAPTURE_RSS_KB (8L * 1024L)

static PicoToolFn g_shell_run;
static void *g_shell_state;
static volatile sig_atomic_t g_alarm_count;

bool pico_add_tool(PicoWorkspace *workspace, const char *name, const char *description,
                   const char *params_json, PicoToolFn run, PicoToolApplyFn apply)
{
    (void)workspace;
    (void)description;
    (void)params_json;
    (void)apply;
    if (!name || strcmp(name, "sh") != 0 || !run)
    {
        return false;
    }
    g_shell_run = run;
    g_shell_state = NULL;
    return true;
}

const char *pico_agent_context_workspace(const PicoAgentContext *ctx)
{
    (void)ctx;
    return "";
}

void pico_tool_set_child(PicoAgentContext *ctx, pid_t pid)
{
    (void)ctx;
    (void)pid;
}

static int Fail(const char *message)
{
    fprintf(stderr, "shell capture: %s\n", message);
    return 1;
}

static PicoToolResult Run(const char *args)
{
    PicoToolResult result;
    memset(&result, 0, sizeof(result));
    g_shell_run(NULL, args, &result, g_shell_state);
    return result;
}

static PicoToolResult RunCommand(const char *description, const char *json_command)
{
    char args[1024];
    int length = snprintf(args, sizeof(args),
                          "{\"description\":\"%s\",\"command\":\"%s\"}",
                          description, json_command);
    if (length < 0 || (size_t)length >= sizeof(args))
    {
        PicoToolResult result;
        memset(&result, 0, sizeof(result));
        return result;
    }
    return Run(args);
}

static void AlarmHandler(int signal_number)
{
    (void)signal_number;
    g_alarm_count++;
}

static PicoToolResult RunInterrupted(const char *description, const char *json_command)
{
    struct sigaction action;
    struct sigaction previous;
    memset(&action, 0, sizeof(action));
    action.sa_handler = AlarmHandler;
    sigemptyset(&action.sa_mask);
    sigaction(SIGALRM, &action, &previous);
    struct itimerval timer;
    memset(&timer, 0, sizeof(timer));
    timer.it_value.tv_usec = 50000;
    setitimer(ITIMER_REAL, &timer, NULL);
    PicoToolResult result = RunCommand(description, json_command);
    memset(&timer, 0, sizeof(timer));
    setitimer(ITIMER_REAL, &timer, NULL);
    sigaction(SIGALRM, &previous, NULL);
    return result;
}

static bool Repeated(const char *text, size_t len, char expected)
{
    for (size_t i = 0; i < len; i++)
    {
        if (text[i] != expected)
        {
            return false;
        }
    }
    return true;
}

static bool ParseLargeOutput(const char *message, uint64_t expected_size,
                             char *path, size_t path_cap)
{
    char prefix[96];
    snprintf(prefix, sizeof(prefix), "Shell output exceeded %zu bytes.\nFull output path: ",
             EXPECTED_LIMIT);
    const char *size_prefix = "\nFull output size: ";
    if (!message || strncmp(message, prefix, strlen(prefix)) != 0)
    {
        return false;
    }
    const char *start = message + strlen(prefix);
    const char *size = strstr(start, size_prefix);
    if (!size || size == start || (size_t)(size - start) >= path_cap)
    {
        return false;
    }
    memcpy(path, start, (size_t)(size - start));
    path[size - start] = '\0';
    char expected[96];
    snprintf(expected, sizeof(expected), "%s%" PRIu64 " bytes", size_prefix, expected_size);
    return strcmp(size, expected) == 0;
}

static bool FileHasMode(const char *path, mode_t mode)
{
    struct stat st;
    return stat(path, &st) == 0 && (st.st_mode & 0777) == mode;
}

static bool ReadAll(const char *path, char *data, size_t len)
{
    FILE *file = fopen(path, "rb");
    if (!file)
    {
        return false;
    }
    size_t offset = 0;
    while (offset < len)
    {
        size_t n = fread(data + offset, 1, len - offset, file);
        if (n == 0)
        {
            fclose(file);
            return false;
        }
        offset += n;
    }
    bool at_end = fgetc(file) == EOF;
    fclose(file);
    return at_end;
}

static bool VerifyLargeFile(const char *path, uint64_t expected_size)
{
    struct stat st;
    if (stat(path, &st) != 0 || (uint64_t)st.st_size != expected_size ||
        (st.st_mode & 0777) != 0600)
    {
        return false;
    }
    FILE *file = fopen(path, "rb");
    if (!file)
    {
        return false;
    }
    char buf[4096];
    uint64_t offset = 0;
    while (offset < expected_size)
    {
        size_t wanted = expected_size - offset < sizeof(buf)
                            ? (size_t)(expected_size - offset)
                            : sizeof(buf);
        size_t n = fread(buf, 1, wanted, file);
        if (n != wanted)
        {
            fclose(file);
            return false;
        }
        for (size_t i = 0; i < n; i++)
        {
            char expected = offset + i < LARGE_HEAD_SIZE ? 'H' : 'T';
            if (buf[i] != expected)
            {
                fclose(file);
                return false;
            }
        }
        offset += n;
    }
    bool at_end = fgetc(file) == EOF;
    fclose(file);
    return at_end;
}

static bool TestExactLimit(void)
{
    char command[256];
    snprintf(command, sizeof(command),
             "head -c %zu /dev/zero | tr '\\\\000' H; "
             "head -c %zu /dev/zero | tr '\\\\000' T",
             EXPECTED_HEAD, EXPECTED_TAIL);
    PicoToolResult result = RunCommand("exercise exact capture limit", command);
    bool ok = result.output && !result.is_error && strlen(result.output) == EXPECTED_LIMIT &&
              Repeated(result.output, EXPECTED_HEAD, 'H') &&
              Repeated(result.output + EXPECTED_HEAD, EXPECTED_TAIL, 'T');
    free(result.output);
    return ok;
}

static bool TestFirstSpooledByte(void)
{
    char command[320];
    snprintf(command, sizeof(command),
             "head -c %zu /dev/zero | tr '\\\\000' H; printf '\\\\000'; "
             "head -c %zu /dev/zero | tr '\\\\000' T",
             EXPECTED_HEAD, EXPECTED_TAIL);
    mode_t previous_umask = umask(0777);
    PicoToolResult result = RunCommand("exercise first spooled byte", command);
    umask(previous_umask);

    char path[4096] = {0};
    char *data = (char *)malloc(OVER_LIMIT);
    bool ok = result.output && !result.is_error && data &&
              ParseLargeOutput(result.output, OVER_LIMIT, path, sizeof(path)) &&
              FileHasMode(path, 0600) && ReadAll(path, data, OVER_LIMIT) &&
              Repeated(data, EXPECTED_HEAD, 'H') && data[EXPECTED_HEAD] == '\0' &&
              Repeated(data + EXPECTED_HEAD + 1, EXPECTED_TAIL, 'T');
    free(result.output);
    free(data);
    if (path[0])
    {
        unlink(path);
    }
    return ok;
}

static bool TestTempFallback(const char *temp)
{
    char invalid_temp[4096];
    snprintf(invalid_temp, sizeof(invalid_temp), "%s/not-a-directory", temp);
    FILE *invalid = fopen(invalid_temp, "wb");
    bool prepared = invalid && fclose(invalid) == 0 && setenv("TMPDIR", invalid_temp, 1) == 0;

    PicoToolResult result;
    memset(&result, 0, sizeof(result));
    if (prepared)
    {
        char command[160];
        snprintf(command, sizeof(command), "head -c %zu /dev/zero | tr '\\\\000' F", OVER_LIMIT);
        result = RunCommand("fallback from invalid temp", command);
    }
    char path[4096] = {0};
    const char *fallback_prefix = "/tmp/pico-sh-output-";
    bool ok = prepared && result.output && !result.is_error &&
              ParseLargeOutput(result.output, OVER_LIMIT, path, sizeof(path)) &&
              strncmp(path, fallback_prefix, strlen(fallback_prefix)) == 0;

    free(result.output);
    if (path[0])
    {
        unlink(path);
    }
    unlink(invalid_temp);
    if (setenv("TMPDIR", temp, 1) != 0)
    {
        ok = false;
    }
    return ok;
}

static bool TestLargeOutput(void)
{
    char command[256];
    snprintf(command, sizeof(command),
             "head -c %" PRIu64 " /dev/zero | tr '\\\\000' H; "
             "head -c %zu /dev/zero | tr '\\\\000' T",
             LARGE_HEAD_SIZE, EXPECTED_TAIL);
    const uint64_t expected_size = LARGE_HEAD_SIZE + (uint64_t)EXPECTED_TAIL;
    struct rusage before;
    struct rusage after;
    if (getrusage(RUSAGE_SELF, &before) != 0)
    {
        return false;
    }
    PicoToolResult result = RunCommand("exercise bounded spooling", command);
    bool usage_read = getrusage(RUSAGE_SELF, &after) == 0;
    char path[4096] = {0};
    bool ok = usage_read && result.output && !result.is_error &&
              ParseLargeOutput(result.output, expected_size, path, sizeof(path)) &&
              after.ru_maxrss - before.ru_maxrss <= MAX_CAPTURE_RSS_KB &&
              VerifyLargeFile(path, expected_size);
    free(result.output);
    if (path[0])
    {
        unlink(path);
    }
    return ok;
}

static bool TestInterruptedRead(void)
{
    g_alarm_count = 0;
    PicoToolResult result = RunInterrupted("interrupt shell read", "sleep 1; printf complete");
    bool ok = result.output && !result.is_error && g_alarm_count > 0 &&
              strcmp(result.output, "complete") == 0;
    free(result.output);
    return ok;
}

static bool TestInterruptedWait(void)
{
    g_alarm_count = 0;
    PicoToolResult result = RunInterrupted("interrupt shell wait", "exec 1>&- 2>&-; sleep 1");
    errno = 0;
    pid_t stray_child = waitpid(-1, NULL, WNOHANG);
    bool ok = result.output && !result.is_error && g_alarm_count > 0 &&
              strcmp(result.output, "(no output)") == 0 && stray_child == -1 && errno == ECHILD;
    free(result.output);
    if (stray_child == 0)
    {
        waitpid(-1, NULL, 0);
    }
    return ok;
}

static bool TestCommandFailure(void)
{
    PicoToolResult result =
        RunCommand("preserve command failure", "printf failed; exit 7");
    bool ok = result.output && result.is_error && strcmp(result.output, "failed\n(exit 7)") == 0;
    free(result.output);
    return ok;
}

int main(void)
{
    char temp[] = "/tmp/pico-shell-output-test-XXXXXX";
    const char *failure = NULL;
    if (!mkdtemp(temp))
    {
        return Fail("could not create the test temp directory");
    }
    if (setenv("TMPDIR", temp, 1) != 0)
    {
        failure = "could not configure the test temp directory";
    }

    PicoExt shell = pico_ext_shell();
    if (!failure &&
        (!shell.workspace_init || shell.workspace_init(NULL, &g_shell_state) != 0 || !g_shell_run))
    {
        failure = "builtin did not register";
    }
    else if (!failure && !TestExactLimit())
    {
        failure = "output at the capture limit was not preserved exactly";
    }
    else if (!TestFirstSpooledByte())
    {
        failure = "the first byte beyond the capture limit was not saved completely";
    }
    else if (!TestTempFallback(temp))
    {
        failure = "invalid TMPDIR did not fall back to /tmp";
    }
    else if (!TestLargeOutput())
    {
        failure = "large output was not saved completely with bounded memory";
    }
    else if (!TestInterruptedRead())
    {
        failure = "interrupted pipe read lost command output";
    }
    else if (!TestInterruptedWait())
    {
        failure = "interrupted wait did not reap the command";
    }
    else if (!TestCommandFailure())
    {
        failure = "command failure annotation changed";
    }

    unsetenv("TMPDIR");
    if (rmdir(temp) != 0 && !failure)
    {
        failure = "test output files were not cleaned up";
    }
    return failure ? Fail(failure) : 0;
}
