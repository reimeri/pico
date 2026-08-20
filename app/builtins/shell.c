#define _POSIX_C_SOURCE 200809L

#include "pico/plugin.h"
#include "json.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#define SH_HEAD 12800
#define SH_TAIL 27200

static const char *kShParams =
    "{\"type\":\"object\",\"properties\":{\"command\":{\"type\":\"string\",\"description\":\"Shell "
    "command to run in the workspace\"}},\"required\":[\"command\"]}";

static char *ExtractCommand(const char *args_json)
{
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
    JsonFree(&doc);
    return cmd;
}

static void ShRun(PicoApp *app, const char *args_json, char **out)
{
    if (out)
    {
        *out = NULL;
    }
    char *command = ExtractCommand(args_json);
    if (!command || !command[0])
    {
        if (out)
        {
            *out = JsonDup("sh: missing command");
        }
        free(command);
        return;
    }

    int pipefd[2];
    if (pipe(pipefd) != 0)
    {
        if (out)
        {
            *out = JsonDup("sh: pipe failed");
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
            *out = JsonDup("sh: fork failed");
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
        if (app->workspace[0])
        {
            if (chdir(app->workspace) != 0)
            {
                _exit(127);
            }
        }
        execl("/bin/sh", "sh", "-c", command, (char *)NULL);
        _exit(127);
    }

    setpgid(pid, pid);
    pico_tool_set_child(app, pid);
    close(pipefd[1]);
    JsonBuf b;
    JsonBuf_Init(&b);
    char buf[4096];
    ssize_t n;
    while ((n = read(pipefd[0], buf, sizeof(buf))) > 0)
    {
        JsonBuf_Append(&b, buf, (size_t)n);
    }
    close(pipefd[0]);
    int status = 0;
    waitpid(pid, &status, 0);
    pico_tool_set_child(app, 0);
    free(command);

    if (b.len > (size_t)(SH_HEAD + SH_TAIL))
    {
        JsonBuf t;
        JsonBuf_Init(&t);
        JsonBuf_Append(&t, b.data, (size_t)SH_HEAD);
        JsonBuf_Puts(&t, "\n…output truncated…\n");
        JsonBuf_Append(&t, b.data + (b.len - (size_t)SH_TAIL), (size_t)SH_TAIL);
        JsonBuf_Free(&b);
        b = t;
    }
    if (WIFEXITED(status) && WEXITSTATUS(status) != 0)
    {
        char line[64];
        snprintf(line, sizeof(line), "\n(exit %d)", WEXITSTATUS(status));
        JsonBuf_Puts(&b, line);
    }
    else if (WIFSIGNALED(status))
    {
        char line[64];
        snprintf(line, sizeof(line), "\n(signal %d)", WTERMSIG(status));
        JsonBuf_Puts(&b, line);
    }
    if (!b.len)
    {
        JsonBuf_Puts(&b, "(no output)");
    }
    if (out)
    {
        *out = JsonBuf_Steal(&b);
    }
    else
    {
        JsonBuf_Free(&b);
    }
}

static void ShellInit(PicoApp *app)
{
    pico_add_tool(app, "sh", "Run a shell command in the workspace", kShParams, ShRun);
}

PicoExt pico_ext_shell(void)
{
    return (PicoExt){
        .abi = PICO_EXT_ABI,
        .name = "sh",
        .init = ShellInit,
    };
}
