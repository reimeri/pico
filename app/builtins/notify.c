#define _POSIX_C_SOURCE 200809L

/* Host builtin: desktop notifications when an agent needs attention. */

#include "pico/plugin.h"
#include "session.h"
#include "host_internal.h"

#include <GLFW/glfw3.h>

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

typedef struct NotifyState {
    bool have_notify_send;
} NotifyState;

static bool CommandOnPath(const char *name)
{
    const char *path;
    const char *p;
    size_t name_len;
    if (!name || !name[0] || strchr(name, '/'))
    {
        return false;
    }
    path = getenv("PATH");
    if (!path)
    {
        return false;
    }
    name_len = strlen(name);
    for (p = path; *p;)
    {
        const char *colon = strchr(p, ':');
        size_t n = colon ? (size_t)(colon - p) : strlen(p);
        if (n > 0 && n + name_len + 2 <= 4096)
        {
            char buf[4096];
            memcpy(buf, p, n);
            buf[n] = '/';
            memcpy(buf + n + 1, name, name_len + 1);
            if (access(buf, X_OK) == 0)
            {
                return true;
            }
        }
        if (!colon)
        {
            break;
        }
        p = colon + 1;
    }
    return false;
}

static void SendNotifySend(const char *title, const char *body)
{
    pid_t pid;
    if (!title || !title[0] || !body || !body[0])
    {
        return;
    }
    pid = fork();
    if (pid == 0)
    {
        pid_t child = fork();
        if (child == 0)
        {
            int fd = open("/dev/null", O_RDWR);
            const char *args[7];
            if (fd >= 0)
            {
                (void)dup2(fd, STDIN_FILENO);
                (void)dup2(fd, STDOUT_FILENO);
                (void)dup2(fd, STDERR_FILENO);
                if (fd > STDERR_FILENO)
                {
                    close(fd);
                }
            }
            args[0] = "notify-send";
            args[1] = "-a";
            args[2] = "Pico";
            args[3] = "--";
            args[4] = title;
            args[5] = body;
            args[6] = NULL;
            execvp(args[0], (char *const *)args);
            _exit(127);
        }
        _exit(child < 0 ? 127 : 0);
    }
    if (pid > 0)
    {
        int status = 0;
        (void)waitpid(pid, &status, 0);
    }
}

static bool AskVisibleForSelection(const PicoHost *host, PicoAgentId owner_id)
{
    PicoAgentId id = owner_id;
    while (id)
    {
        PicoAgentInfo info;
        if (id == pico_agent_active(host))
        {
            return true;
        }
        if (!pico_agent_find(host, id, &info))
        {
            return false;
        }
        id = info.parent_id;
    }
    return false;
}

static const PicoAgent *NotifyTitleAgent(const PicoHost *host, PicoAgentId id)
{
    const PicoAgent *agent = PicoHost_FindAgentConst(host, id);
    while (agent && agent->kind == PICO_AGENT_SUBAGENT && agent->parent_id)
    {
        const PicoAgent *parent = PicoHost_FindAgentConst(host, agent->parent_id);
        if (!parent)
        {
            break;
        }
        agent = parent;
    }
    return agent;
}

static void SessionTitle(const PicoHost *host, PicoAgentId id, char *out, size_t cap)
{
    PicoSession_CopyDisplayTitle(NotifyTitleAgent(host, id), out, cap);
}

static void RequestAttention(void)
{
    GLFWwindow *win;
    if (!IsWindowReady())
    {
        return;
    }
    win = (GLFWwindow *)GetWindowHandle();
    if (win)
    {
        glfwRequestWindowAttention(win);
    }
}

static void ShowDesktopNotify(NotifyState *state, PicoHost *host, PicoAgentId agent_id,
                              const char *body)
{
    char title[PICO_SESSION_TITLE_MAX_BYTES + 1];
    if (!state || !host || !body || !body[0] || !IsWindowReady())
    {
        return;
    }
    if (IsWindowFocused() && AskVisibleForSelection(host, agent_id))
    {
        return;
    }
    RequestAttention();
    if (!state->have_notify_send)
    {
        return;
    }
    SessionTitle(host, agent_id, title, sizeof(title));
    SendNotifySend(title, body);
}

static NotifyState *HostNotifyState(PicoWorkspace *workspace)
{
    PicoHost *host = pico_workspace_host(workspace);
    return host ? (NotifyState *)PicoPlugins_HostState(host, "notify") : NULL;
}

static void OnAsk(PicoWorkspace *workspace, const PicoHookEvent *event, void *state)
{
    PicoHost *host = pico_workspace_host(workspace);
    (void)state;
    if (!event)
    {
        return;
    }
    ShowDesktopNotify(HostNotifyState(workspace), host, event->agent_id, "Waiting for input");
}

static bool IsMainAgent(PicoHost *host, PicoAgentId id)
{
    PicoAgentInfo info;
    return host && id && pico_agent_find(host, id, &info) && info.kind == PICO_AGENT_MAIN;
}

static void OnTurnEnd(PicoWorkspace *workspace, const PicoHookEvent *event, void *state)
{
    PicoHost *host = pico_workspace_host(workspace);
    (void)state;
    if (!event || !IsMainAgent(host, event->agent_id))
    {
        return;
    }
    ShowDesktopNotify(HostNotifyState(workspace), host, event->agent_id, "Session finished");
}

static void OnError(PicoWorkspace *workspace, const PicoHookEvent *event, void *state)
{
    PicoHost *host = pico_workspace_host(workspace);
    (void)state;
    if (!event || !IsMainAgent(host, event->agent_id))
    {
        return;
    }
    ShowDesktopNotify(HostNotifyState(workspace), host, event->agent_id, "Agent error");
}

static int NotifyHostInit(PicoHost *host, void **state_out)
{
    NotifyState *s = (NotifyState *)calloc(1, sizeof(NotifyState));
    (void)host;
    if (!s)
    {
        return 1;
    }
    s->have_notify_send = CommandOnPath("notify-send");
    if (state_out)
    {
        *state_out = s;
    }
    return 0;
}

static void NotifyHostShutdown(PicoHost *host, void *state)
{
    (void)host;
    free(state);
}

static int NotifyWorkspaceInit(PicoWorkspace *workspace, void **state_out)
{
    (void)state_out;
    pico_workspace_add_hook(workspace, PICO_HOOK_ON_ASK, OnAsk);
    pico_workspace_add_hook(workspace, PICO_HOOK_ON_TURN_END, OnTurnEnd);
    pico_workspace_add_hook(workspace, PICO_HOOK_ON_ERROR, OnError);
    return 0;
}

PicoExt pico_ext_notify(void)
{
    return (PicoExt){
        .abi = PICO_EXT_ABI,
        .name = "notify",
        .description = "Desktop notifications when an agent needs attention",
        .host_init = NotifyHostInit,
        .host_shutdown = NotifyHostShutdown,
        .workspace_init = NotifyWorkspaceInit,
    };
}
