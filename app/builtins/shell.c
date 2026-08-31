#define _POSIX_C_SOURCE 200809L

#include "pico/plugin.h"
#include "json.h"
#include "posix_io.h"

#include <errno.h>
#include <inttypes.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define SH_HEAD 12800
#define SH_TAIL 27200
#define SH_CAPTURE_LIMIT ((size_t)SH_HEAD + (size_t)SH_TAIL)

static const char *kShTruncated = "\n…output truncated…\n";

typedef struct ShellCapture {
    char head[SH_HEAD];
    size_t head_len;
    char tail[SH_TAIL];
    size_t tail_start;
    size_t tail_len;
    bool truncated;
} ShellCapture;

typedef struct ShellSpool {
    int fd;
    char path[4096];
    bool failed;
} ShellSpool;

static void ShellCaptureAppend(ShellCapture *capture, const char *data, size_t len)
{
    if (!capture || !data || len == 0)
    {
        return;
    }

    size_t head_room = (size_t)SH_HEAD - capture->head_len;
    size_t head_copy = len < head_room ? len : head_room;
    if (head_copy > 0)
    {
        memcpy(capture->head + capture->head_len, data, head_copy);
        capture->head_len += head_copy;
        data += head_copy;
        len -= head_copy;
    }
    if (len == 0)
    {
        return;
    }

    if (len >= (size_t)SH_TAIL)
    {
        capture->truncated = capture->truncated || len > (size_t)SH_TAIL || capture->tail_len > 0;
        memcpy(capture->tail, data + len - (size_t)SH_TAIL, (size_t)SH_TAIL);
        capture->tail_start = 0;
        capture->tail_len = (size_t)SH_TAIL;
        return;
    }

    size_t tail_room = (size_t)SH_TAIL - capture->tail_len;
    if (len > tail_room)
    {
        size_t overwritten = len - tail_room;
        capture->tail_start = (capture->tail_start + overwritten) % (size_t)SH_TAIL;
        capture->tail_len -= overwritten;
        capture->truncated = true;
    }
    size_t tail_end = (capture->tail_start + capture->tail_len) % (size_t)SH_TAIL;
    size_t first = len < (size_t)SH_TAIL - tail_end ? len : (size_t)SH_TAIL - tail_end;
    memcpy(capture->tail + tail_end, data, first);
    memcpy(capture->tail, data + first, len - first);
    capture->tail_len += len;
}

static size_t ShellCaptureFirstTailSpan(const ShellCapture *capture)
{
    return capture->tail_len < (size_t)SH_TAIL - capture->tail_start
               ? capture->tail_len
               : (size_t)SH_TAIL - capture->tail_start;
}

static void ShellCaptureFinish(const ShellCapture *capture, JsonBuf *output)
{
    JsonBuf_Init(output);
    JsonBuf_Append(output, capture->head, capture->head_len);
    if (capture->truncated)
    {
        JsonBuf_Puts(output, kShTruncated);
    }
    size_t first = ShellCaptureFirstTailSpan(capture);
    JsonBuf_Append(output, capture->tail + capture->tail_start, first);
    JsonBuf_Append(output, capture->tail, capture->tail_len - first);
}

static bool ShellCaptureWrite(int fd, const ShellCapture *capture)
{
    size_t first = ShellCaptureFirstTailSpan(capture);
    return PicoIO_WriteAll(fd, capture->head, capture->head_len) &&
           PicoIO_WriteAll(fd, capture->tail + capture->tail_start, first) &&
           PicoIO_WriteAll(fd, capture->tail, capture->tail_len - first);
}

static int OpenOutputFileAt(const char *temp, char *path, size_t path_cap)
{
    int n = snprintf(path, path_cap, "%s%s%s", temp,
                     temp[strlen(temp) - 1] == '/' ? "" : "/",
                     "pico-sh-output-XXXXXX");
    if (n < 0 || (size_t)n >= path_cap)
    {
        path[0] = '\0';
        return -1;
    }
    int fd = mkstemp(path);
    if (fd < 0)
    {
        path[0] = '\0';
        return -1;
    }
    if (fchmod(fd, 0600) != 0)
    {
        close(fd);
        unlink(path);
        path[0] = '\0';
        return -1;
    }
    return fd;
}

static int OpenOutputFile(char *path, size_t path_cap)
{
    char resolved[4096];
    const char *configured = getenv("TMPDIR");
    if (configured && configured[0] && realpath(configured, resolved))
    {
        int fd = OpenOutputFileAt(resolved, path, path_cap);
        if (fd >= 0)
        {
            return fd;
        }
    }
    return OpenOutputFileAt("/tmp", path, path_cap);
}

static void ShellSpoolDiscard(ShellSpool *spool)
{
    if (spool->fd >= 0)
    {
        close(spool->fd);
        spool->fd = -1;
    }
    if (spool->path[0])
    {
        unlink(spool->path);
        spool->path[0] = '\0';
    }
}

static void ShellSpoolFail(ShellSpool *spool)
{
    ShellSpoolDiscard(spool);
    spool->failed = true;
}

static void ShellSpoolAppend(ShellSpool *spool, const ShellCapture *capture,
                             uint64_t captured_size, const char *data, size_t len)
{
    if (spool->failed)
    {
        return;
    }
    if (spool->fd < 0)
    {
        if (captured_size > (uint64_t)SH_CAPTURE_LIMIT ||
            (uint64_t)len <= (uint64_t)SH_CAPTURE_LIMIT - captured_size)
        {
            return;
        }
        spool->fd = OpenOutputFile(spool->path, sizeof(spool->path));
        if (spool->fd < 0 || !ShellCaptureWrite(spool->fd, capture))
        {
            ShellSpoolFail(spool);
            return;
        }
    }
    if (!PicoIO_WriteAll(spool->fd, data, len))
    {
        ShellSpoolFail(spool);
    }
}

static void ShellSpoolFinish(ShellSpool *spool)
{
    if (spool->fd < 0)
    {
        return;
    }
    int fd = spool->fd;
    spool->fd = -1;
    if (close(fd) != 0)
    {
        ShellSpoolFail(spool);
    }
}

static void LargeOutputMessage(JsonBuf *output, const char *path, uint64_t size)
{
    JsonBuf_Init(output);
    char line[96];
    snprintf(line, sizeof(line), "Shell output exceeded %zu bytes.\nFull output path: ",
             SH_CAPTURE_LIMIT);
    JsonBuf_Puts(output, line);
    JsonBuf_Puts(output, path);
    JsonBuf_Puts(output, "\nFull output size: ");
    snprintf(line, sizeof(line), "%" PRIu64 " bytes", size);
    JsonBuf_Puts(output, line);
}

#define SH_DEFAULT_TIMEOUT 180

static const char *kShParams =
    "{\"type\":\"object\",\"properties\":{"
    "\"description\":{\"type\":\"string\",\"description\":\"Succinct 2-10 word summary of what the "
    "command achieves\"},"
    "\"command\":{\"type\":\"string\",\"description\":\"Shell command to run in the workspace\"},"
    "\"timeout\":{\"type\":\"integer\",\"description\":\"Optional command timeout in seconds (default 180)\"}},"
    "\"required\":[\"description\",\"command\"]}";

static double MonotonicSeconds(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1000000000.0;
}

static void KillProcessGroup(pid_t pid)
{
    if (pid <= 0)
    {
        return;
    }
    if (kill(-pid, SIGKILL) != 0)
    {
        kill(pid, SIGKILL);
    }
}

static char *ExtractArgs(const char *args_json, int *timeout_seconds)
{
    if (timeout_seconds)
    {
        *timeout_seconds = SH_DEFAULT_TIMEOUT;
    }
    if (!args_json || !args_json[0])
    {
        return NULL;
    }
    JsonDoc doc;
    if (JsonParse(&doc, args_json, strlen(args_json)) != 0)
    {
        return NULL;
    }
    char *cmd = JsonObjStr(&doc, 0, "command");
    if (timeout_seconds)
    {
        int t = JsonObjInt(&doc, 0, "timeout", SH_DEFAULT_TIMEOUT);
        *timeout_seconds = t > 0 ? t : SH_DEFAULT_TIMEOUT;
    }
    JsonFree(&doc);
    return cmd;
}

static void ShRun(PicoAgentContext *ctx, const char *args_json, PicoToolResult *out, void *state)
{
    (void)state;
    if (out)
    {
        memset(out, 0, sizeof(*out));
    }
    int timeout_seconds = SH_DEFAULT_TIMEOUT;
    char *command = ExtractArgs(args_json, &timeout_seconds);
    if (!command || !command[0])
    {
        if (out)
        {
            out->output = JsonDup("sh: missing command");
            out->is_error = true;
        }
        free(command);
        return;
    }

    const char *workspace = pico_agent_context_workspace(ctx);

    int pipefd[2];
    if (pipe(pipefd) != 0)
    {
        if (out)
        {
            out->output = JsonDup("sh: pipe failed");
            out->is_error = true;
        }
        free(command);
        return;
    }

    pid_t pid = fork();
    if (pid < 0)
    {
        close(pipefd[0]);
        close(pipefd[1]);
        if (out)
        {
            out->output = JsonDup("sh: fork failed");
            out->is_error = true;
        }
        free(command);
        return;
    }
    if (pid == 0)
    {
        setpgid(0, 0);
        close(pipefd[0]);
        dup2(pipefd[1], STDOUT_FILENO);
        dup2(pipefd[1], STDERR_FILENO);
        close(pipefd[1]);
        if (workspace[0])
        {
            if (chdir(workspace) != 0)
            {
                _exit(127);
            }
        }
        execl("/bin/sh", "sh", "-c", command, (char *)NULL);
        _exit(127);
    }

    setpgid(pid, pid);
    pico_tool_set_child(ctx, pid);
    close(pipefd[1]);
    ShellCapture capture;
    memset(&capture, 0, sizeof(capture));
    ShellSpool spool;
    memset(&spool, 0, sizeof(spool));
    spool.fd = -1;
    uint64_t output_size = 0;
    char buf[4096];
    int read_error = 0;
    bool timed_out = false;
    double deadline = MonotonicSeconds() + (double)timeout_seconds;
    for (;;)
    {
        double now = MonotonicSeconds();
        if (now >= deadline)
        {
            timed_out = true;
            break;
        }
        int timeout_ms = (int)((deadline - now) * 1000.0 + 0.999);
        if (timeout_ms <= 0)
        {
            timeout_ms = 0;
        }
        struct pollfd pfd;
        pfd.fd = pipefd[0];
        pfd.events = POLLIN;
        pfd.revents = 0;
        int pr = poll(&pfd, 1, timeout_ms);
        if (pr < 0)
        {
            if (errno == EINTR)
            {
                continue;
            }
            read_error = errno;
            break;
        }
        if (pr == 0)
        {
            timed_out = true;
            break;
        }
        ssize_t n = read(pipefd[0], buf, sizeof(buf));
        if (n < 0)
        {
            if (errno == EINTR)
            {
                continue;
            }
            read_error = errno;
            break;
        }
        if (n <= 0)
        {
            read_error = n < 0 ? errno : 0;
            break;
        }
        size_t chunk = (size_t)n;
        ShellSpoolAppend(&spool, &capture, output_size, buf, chunk);
        ShellCaptureAppend(&capture, buf, chunk);
        output_size = UINT64_MAX - output_size < (uint64_t)chunk
                          ? UINT64_MAX
                          : output_size + (uint64_t)chunk;
    }
    close(pipefd[0]);
    ShellSpoolFinish(&spool);
    if (read_error)
    {
        ShellSpoolDiscard(&spool);
    }
    int status = 0;
    int wait_error = 0;
    if (timed_out)
    {
        KillProcessGroup(pid);
        pid_t waited;
        do
        {
            waited = waitpid(pid, &status, 0);
        } while (waited < 0 && errno == EINTR);
        wait_error = waited < 0 ? errno : 0;
    }
    else
    {
        for (;;)
        {
            pid_t waited = waitpid(pid, &status, WNOHANG);
            if (waited == pid)
            {
                break;
            }
            if (waited < 0)
            {
                if (errno == EINTR)
                {
                    continue;
                }
                wait_error = errno;
                break;
            }
            if (MonotonicSeconds() >= deadline)
            {
                timed_out = true;
                KillProcessGroup(pid);
                do
                {
                    waited = waitpid(pid, &status, 0);
                } while (waited < 0 && errno == EINTR);
                wait_error = waited < 0 ? errno : 0;
                break;
            }
            struct timespec req = {.tv_sec = 0, .tv_nsec = 5000000L};
            nanosleep(&req, NULL);
        }
    }
    pico_tool_set_child(ctx, 0);
    free(command);

    JsonBuf b;
    if (spool.path[0])
    {
        LargeOutputMessage(&b, spool.path, output_size);
    }
    else
    {
        ShellCaptureFinish(&capture, &b);
        if (spool.failed && capture.truncated)
        {
            JsonBuf_Puts(&b, "\n(sh: could not save complete output to a temporary file)");
        }
    }
    bool failed = false;
    if (timed_out)
    {
        char line[64];
        snprintf(line, sizeof(line), "\n(timed out after %d second%s)", timeout_seconds,
                 timeout_seconds == 1 ? "" : "s");
        JsonBuf_Puts(&b, line);
        failed = true;
    }
    else if (read_error)
    {
        char line[256];
        snprintf(line, sizeof(line), "\n(sh: could not read complete command output: %s)",
                 strerror(read_error));
        JsonBuf_Puts(&b, line);
        failed = true;
    }
    else if (wait_error)
    {
        char line[256];
        snprintf(line, sizeof(line), "\n(sh: could not wait for command: %s)", strerror(wait_error));
        JsonBuf_Puts(&b, line);
        failed = true;
    }
    else if (WIFEXITED(status) && WEXITSTATUS(status) != 0)
    {
        char line[64];
        snprintf(line, sizeof(line), "\n(exit %d)", WEXITSTATUS(status));
        JsonBuf_Puts(&b, line);
        failed = true;
    }
    else if (WIFSIGNALED(status))
    {
        char line[64];
        snprintf(line, sizeof(line), "\n(signal %d)", WTERMSIG(status));
        JsonBuf_Puts(&b, line);
        failed = true;
    }
    if (!b.len)
    {
        JsonBuf_Puts(&b, "(no output)");
    }
    if (out)
    {
        out->output = JsonBuf_Steal(&b);
        out->is_error = failed;
    }
    else
    {
        JsonBuf_Free(&b);
        ShellSpoolDiscard(&spool);
    }
}

static int ShellInit(PicoWorkspace *workspace, void **state_out)
{
    (void)state_out;
    pico_add_tool(workspace, "sh", "Run a shell command in the workspace", kShParams, ShRun, NULL);
    return 0;
}

PicoExt pico_ext_shell(void)
{
    return (PicoExt){
        .abi = PICO_EXT_ABI,
        .name = "sh",
        .description = "Shell tool",
        .workspace_init = ShellInit,
    };
}
