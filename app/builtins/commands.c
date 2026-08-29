#define _DEFAULT_SOURCE

#include "../agent_internal.h"
#define _POSIX_C_SOURCE 200809L
#include "host_internal.h"

#include "pico/plugin.h"
#include "agent.h"
#include "workspace_internal.h"
#include "session.h"
#include "settings.h"
#include "overlay.h"
#include "pico/auth.h"
#include "json.h"
#include "docs_path.h"

#include <ctype.h>
#include <dirent.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "raylib.h"

static void Note(PicoHost *app, PicoAgentId agent_id, const char *text)
{
    PicoHost_AddMessage(app, agent_id, PICO_ROLE_ASSISTANT, text);
}

static void ClearComposer(PicoHost *app)
{
    PicoComposer_SetText(app, "");
}

static void SplitPrefix(const char *prefix, char *cmd, size_t cmd_cap, const char **rest);

static int Fold(int c)
{
    return (c >= 'A' && c <= 'Z') ? c - 'A' + 'a' : c;
}

static bool FoldEq(const char *a, const char *b)
{
    if (!a || !b)
    {
        return false;
    }
    while (*a && *b)
    {
        if (Fold((unsigned char)*a) != Fold((unsigned char)*b))
        {
            return false;
        }
        a++;
        b++;
    }
    return *a == *b;
}

static bool FoldPrefix(const char *s, const char *prefix)
{
    if (!prefix || !prefix[0])
    {
        return true;
    }
    while (*prefix)
    {
        if (Fold((unsigned char)*s) != Fold((unsigned char)*prefix))
        {
            return false;
        }
        s++;
        prefix++;
    }
    return true;
}

static bool FoldContains(const char *s, const char *needle)
{
    if (!needle || !needle[0])
    {
        return true;
    }
    for (; *s; s++)
    {
        if (FoldPrefix(s, needle))
        {
            return true;
        }
    }
    return false;
}

static void CmdModel(PicoWorkspace *workspace, PicoAgentId agent_id, const char *args, void *state)
{
    PicoHost *app = workspace ? workspace->host : NULL;
    (void)state;
    while (args && *args && isspace((unsigned char)*args))
    {
        args++;
    }
    if (!args || !args[0])
    {
        if (app)
        {
            PicoComposer_SetText(app, "/model ");
            app->submit_cancel = true;
            PicoComplete_Refresh(app);
        }
        return;
    }
    PicoSettings_SetModel(PicoWorkspace_FindAgent(workspace, agent_id), args);
    ClearComposer(app);
    if (app)
    {
        app->submit_cancel = true;
    }
}

static void CmdEffort(PicoWorkspace *workspace, PicoAgentId agent_id, const char *args, void *state)
{
    PicoHost *app = workspace ? workspace->host : NULL;
    (void)state;
    while (args && *args && isspace((unsigned char)*args))
    {
        args++;
    }
    if (!args || !args[0])
    {
        if (app)
        {
            PicoComposer_SetText(app, "/effort ");
            app->submit_cancel = true;
            PicoComplete_Refresh(app);
        }
        return;
    }
    PicoSettings_SetEffort(PicoWorkspace_FindAgent(workspace, agent_id), args);
    ClearComposer(app);
    if (app)
    {
        app->submit_cancel = true;
    }
}

static void CmdCompact(PicoWorkspace *workspace, PicoAgentId agent_id, const char *args, void *state)
{
    PicoHost *app = workspace ? workspace->host : NULL;
    (void)state;
    (void)args;
    PicoAgent_Compact(app, PicoWorkspace_FindAgent(workspace, agent_id));
    ClearComposer(app);
    if (app)
    {
        app->submit_cancel = true;
    }
}

static void RelAge(char *out, size_t cap, time_t mtime)
{
    time_t now = time(NULL);
    long sec = (long)(now - mtime);
    if (sec < 0)
    {
        sec = 0;
    }
    if (sec < 60)
    {
        snprintf(out, cap, "<1m");
    }
    else if (sec < 3600)
    {
        snprintf(out, cap, "%ldm", sec / 60);
    }
    else if (sec < 86400)
    {
        snprintf(out, cap, "%ldh", sec / 3600);
    }
    else
    {
        snprintf(out, cap, "%ldd", sec / 86400);
    }
}

static void CmdNew(PicoWorkspace *workspace, PicoAgentId agent_id, const char *args, void *state)
{
    PicoHost *app = workspace ? workspace->host : NULL;
    PicoAgent *agent = PicoWorkspace_FindAgent(workspace, agent_id);
    (void)state;
    (void)args;
    if (PicoAgent_IsBusy(agent))
    {
        PicoOverlay_Notify(app, "Wait until the agent is idle before starting a new session.");
        ClearComposer(app);
        if (app)
        {
            app->submit_cancel = true;
        }
        return;
    }
    PicoSession_Reset(app, agent);
    PicoOverlay_Notify(app, "New session.");
    ClearComposer(app);
    if (app)
    {
        app->submit_cancel = true;
    }
}

static void CmdResume(PicoWorkspace *workspace, PicoAgentId agent_id, const char *args, void *state)
{
    PicoHost *app = workspace ? workspace->host : NULL;
    PicoAgent *agent = PicoWorkspace_FindAgent(workspace, agent_id);
    (void)state;
    while (args && *args && isspace((unsigned char)*args))
    {
        args++;
    }
    if (!args || !args[0])
    {
        if (app)
        {
            PicoComposer_SetText(app, "/resume ");
            app->submit_cancel = true;
            PicoComplete_Refresh(app);
        }
        return;
    }
    if (PicoAgent_IsBusy(agent))
    {
        Note(app, agent_id, "Wait until the agent is idle before resuming a session.");
        ClearComposer(app);
        if (app)
        {
            app->submit_cancel = true;
        }
        return;
    }
    PicoResult result = PicoWorkspace_Resume(app, agent_id, args, true);
    if (result != PICO_OK)
    {
        char line[256];
        snprintf(line, sizeof(line),
                 result == PICO_SESSION_IN_USE
                     ? "Session `%s` is already open by another agent."
                     : "Unknown session `%s`. Try `/resume`.", args);
        Note(app, agent_id, line);
        ClearComposer(app);
        if (app)
        {
            app->submit_cancel = true;
        }
        return;
    }
    ClearComposer(app);
    if (app)
    {
        app->submit_cancel = true;
    }
}

static void CmdQuit(PicoHost *app, PicoAgentId agent_id, const char *args, void *state)
{
    (void)state;
    (void)args;
    (void)agent_id;
    ClearComposer(app);
    app->submit_cancel = true;
    CloseWindow();
}

static const char *const kDocTopics[] = {
    "README", "subagents", "anatomy", "host", "workspace", "agents", "views", "hooks", "context",
    "tools", "commands", "completers", "providers", "auth", "contracts",
};

static size_t Append(char *buf, size_t cap, size_t n, const char *fmt, ...);

static void CmdDocs(PicoHost *app, PicoAgentId agent_id, const char *args, void *state)
{
    (void)state;
    ClearComposer(app);
    app->submit_cancel = true;
    char rel[256];
    if (!Pico_DocsRelPath(args, rel, sizeof(rel)))
    {
        Note(app, agent_id, "Unknown docs topic. Try `/docs`.");
        return;
    }
    const char *dir = Pico_DocsAppDir();
    if (!dir || !dir[0])
    {
        dir = GetApplicationDirectory();
    }
    char path[4096];
    if (!Pico_DocsJoin(dir, rel, path, sizeof(path)))
    {
        Note(app, agent_id, "Extension docs path is not configured.");
        return;
    }
    size_t len = 0;
    char *src = Pico_ReadFile(path, &len);
    if (!src)
    {
        char buf[512];
        size_t n = Append(buf, sizeof(buf), 0, "Unknown topic. Try:");
        for (size_t i = 0; i < sizeof(kDocTopics) / sizeof(kDocTopics[0]); i++)
        {
            n = Append(buf, sizeof(buf), n, i == 0 ? " `%s`" : ", `%s`", kDocTopics[i]);
        }
        Note(app, agent_id, buf);
        return;
    }
    Note(app, agent_id, src);
    free(src);
}

static void CmdHelp(PicoHost *app, PicoAgentId agent_id, const char *args, void *state)
{
    (void)state;
    (void)args;
    char buf[1024];
    size_t n = 0;
    n += (size_t)snprintf(buf + n, sizeof(buf) - n, "Commands:\n");
    for (int i = 0; i < app->command_count && n + 8 < sizeof(buf); i++)
    {
        n += (size_t)snprintf(buf + n, sizeof(buf) - n, "`/%s` — %s\n", app->commands[i].name,
                              app->commands[i].help ? app->commands[i].help : "");
    }
    PicoWorkspace *ws = PicoHost_SelectedWorkspace(app);
    for (int i = 0; ws && i < ws->command_count && n + 8 < sizeof(buf); i++)
    {
        n += (size_t)snprintf(buf + n, sizeof(buf) - n, "`/%s` — %s\n", ws->commands[i].name,
                              ws->commands[i].help ? ws->commands[i].help : "");
    }
    Note(app, agent_id, buf);
    ClearComposer(app);
    app->submit_cancel = true;
}

static const PicoAuth *OnlyAuth(PicoHost *app)
{
    return app->auth_count == 1 ? &app->auths[0] : NULL;
}

/* snprintf reports the length it wanted to write, so a truncated result must be
 * clamped before it is reused as an offset. Returns the new write position. */
static size_t Append(char *buf, size_t cap, size_t n, const char *fmt, ...)
{
    if (cap == 0 || n + 1 >= cap)
    {
        return n;
    }
    va_list ap;
    va_start(ap, fmt);
    int wrote = vsnprintf(buf + n, cap - n, fmt, ap);
    va_end(ap);
    if (wrote < 0)
    {
        return n;
    }
    n += (size_t)wrote;
    return n < cap ? n : cap - 1;
}

static void ListAuthProviders(PicoHost *app, PicoAgentId agent_id, const char *prefix)
{
    char buf[1024];
    size_t n = Append(buf, sizeof(buf), 0, "%s", prefix);
    if (app->auth_count == 0)
    {
        Append(buf, sizeof(buf), n, " No providers registered.");
        Note(app, agent_id, buf);
        return;
    }
    for (int i = 0; i < app->auth_count; i++)
    {
        n = Append(buf, sizeof(buf), n, "\n- `%s` — %s", app->auths[i].provider,
                   app->auths[i].help ? app->auths[i].help : "");
    }
    Note(app, agent_id, buf);
}

static void RunLogin(PicoHost *app, PicoAgentId agent_id, const PicoAuth *a, const char *args)
{
    if (!a || !a->login)
    {
        Note(app, agent_id, "That provider has no login.");
        return;
    }
    a->login(app, agent_id, args ? args : "", a->state);
}

static const char *Trim(const char *s)
{
    while (s && *s && isspace((unsigned char)*s))
    {
        s++;
    }
    return s ? s : "";
}

/* Sub-verbs like `key` or `cancel` belong to the provider, so an argument that is
 * not a provider name is forwarded verbatim for the provider to interpret. */
static void CmdLogin(PicoHost *app, PicoAgentId agent_id, const char *args, void *state)
{
    (void)state;
    const char *all = Trim(args);
    char first[64];
    const char *rest = "";
    SplitPrefix(all, first, sizeof(first), &rest);
    const PicoAuth *a = first[0] ? pico_find_auth(app, first) : NULL;
    if (a)
    {
        RunLogin(app, agent_id, a, rest);
    }
    else if ((a = OnlyAuth(app)) != NULL)
    {
        RunLogin(app, agent_id, a, all);
    }
    else
    {
        ListAuthProviders(app, agent_id,
                          first[0] ? "Unknown provider. Try:" : "Usage: `/login [provider]`.");
    }
    ClearComposer(app);
    app->submit_cancel = true;
}

static void CmdLogout(PicoHost *app, PicoAgentId agent_id, const char *args, void *state)
{
    (void)state;
    char first[64];
    const char *rest = "";
    SplitPrefix(Trim(args), first, sizeof(first), &rest);
    const PicoAuth *a = first[0] ? pico_find_auth(app, first) : OnlyAuth(app);
    if (!a)
    {
        ListAuthProviders(app, agent_id,
                          first[0] ? "Unknown provider. Try:" : "Usage: `/logout [provider]`.");
    }
    else if (a->logout)
    {
        a->logout(app, agent_id, a->state);
    }
    else
    {
        Note(app, agent_id, pico_auth_clear_oauth(app, a->provider)
                      ? "Logged out."
                      : "Logged out, but `~/.config/pico/auth.json` could not be written, so the "
                        "stored credentials may still be on disk.");
    }
    ClearComposer(app);
    app->submit_cancel = true;
}

static void CmdReload(PicoHost *app, PicoAgentId agent_id, const char *args, void *state)
{
    PicoAgent *agent = PicoHost_FindAgent(app, agent_id);
    (void)state;
    (void)args;
    PicoHost_RequestHostReload(app);
    if (agent && agent->workspace)
    {
        pico_workspace_request_reload(app, agent->workspace->id);
    }
    Note(app, agent_id, "Reloading extensions…");
    ClearComposer(app);
    app->submit_cancel = true;
}

static void StripTrailingSlashes(char *s)
{
    size_t n = strlen(s);
    while (n > 1 && s[n - 1] == '/')
    {
        s[--n] = '\0';
    }
}

static int ExpandUserPath(const char *workspace, const char *arg, char *out, size_t cap)
{
    if (!arg || !arg[0] || !out || cap < 2)
    {
        return -1;
    }
    if (arg[0] == '~' && (arg[1] == '\0' || arg[1] == '/'))
    {
        const char *home = getenv("HOME");
        if (!home || !home[0])
        {
            return -1;
        }
        if (arg[1] == '\0')
        {
            snprintf(out, cap, "%s", home);
            return 0;
        }
        int n = snprintf(out, cap, "%s%s", home, arg + 1);
        if (n < 0 || (size_t)n >= cap)
        {
            return -1;
        }
        return 0;
    }
    if (arg[0] == '/')
    {
        if (strlen(arg) >= cap)
        {
            return -1;
        }
        snprintf(out, cap, "%s", arg);
        return 0;
    }
    const char *ws = (workspace && workspace[0]) ? workspace : ".";
    int n = snprintf(out, cap, "%s/%s", ws, arg);
    if (n < 0 || (size_t)n >= cap)
    {
        return -1;
    }
    return 0;
}

static int ResolveWorkspaceDir(const char *workspace, const char *arg, char *out, size_t cap)
{
    char expanded[4096];
    if (ExpandUserPath(workspace, arg, expanded, sizeof(expanded)) != 0)
    {
        return -1;
    }
    char real[4096];
    if (!realpath(expanded, real))
    {
        return -1;
    }
    struct stat st;
    if (stat(real, &st) != 0 || !S_ISDIR(st.st_mode))
    {
        return -1;
    }
    if (strlen(real) >= cap)
    {
        return -1;
    }
    snprintf(out, cap, "%s", real);
    return 0;
}

static void CmdCd(PicoWorkspace *workspace, PicoAgentId agent_id, const char *args, void *state)
{
    PicoHost *app = workspace ? workspace->host : NULL;
    (void)state;
    (void)agent_id;
    while (args && *args && isspace((unsigned char)*args))
    {
        args++;
    }
    if (!args || !args[0])
    {
        if (app)
        {
            PicoComposer_SetText(app, "/cd ");
            app->submit_cancel = true;
            PicoComplete_Refresh(app);
        }
        return;
    }

    if (app)
    {
        PicoHost_ChangeWorkspace(app, workspace, args);
        ClearComposer(app);
        app->submit_cancel = true;
    }
}

static int CdQuery(PicoHost *app, const char *rest, PicoCompleteItem *out, int max, void *state)
{
    (void)state;
    if (!rest)
    {
        rest = "";
    }
    char parent_typed[4096];
    const char *name_prefix;
    if (strcmp(rest, "~") == 0)
    {
        snprintf(parent_typed, sizeof(parent_typed), "~/");
        name_prefix = "";
    }
    else
    {
        const char *slash = strrchr(rest, '/');
        if (slash)
        {
            size_t plen = (size_t)(slash - rest + 1);
            if (plen >= sizeof(parent_typed))
            {
                return 0;
            }
            memcpy(parent_typed, rest, plen);
            parent_typed[plen] = '\0';
            name_prefix = slash + 1;
        }
        else
        {
            parent_typed[0] = '\0';
            name_prefix = rest;
        }
    }

    char list_dir[4096];
    const PicoAgent *selected = PicoHost_SelectedAgentConst(app);
    const char *ws = PicoAgent_WorkspacePath(selected);
    if (!ws[0])
    {
        ws = PicoWorkspace_Path(PicoHost_PrimaryWorkspaceConst(app));
    }
    if (!ws[0])
    {
        ws = ".";
    }
    if (!parent_typed[0])
    {
        if (!realpath(ws, list_dir))
        {
            return 0;
        }
    }
    else
    {
        char parent_arg[4096];
        snprintf(parent_arg, sizeof(parent_arg), "%s", parent_typed);
        StripTrailingSlashes(parent_arg);
        if (ResolveWorkspaceDir(ws, parent_arg, list_dir, sizeof(list_dir)) != 0)
        {
            return 0;
        }
    }

    DIR *d = opendir(list_dir);
    if (!d)
    {
        return 0;
    }
    int n = 0;
    struct dirent *ent;
    while ((ent = readdir(d)) != NULL && n < max)
    {
        const char *name = ent->d_name;
        if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0)
        {
            continue;
        }
        if (!FoldPrefix(name, name_prefix) && !FoldContains(name, name_prefix))
        {
            continue;
        }
        char full[4096];
        int wn = snprintf(full, sizeof(full), "%s/%s", list_dir, name);
        if (wn < 0 || (size_t)wn >= sizeof(full))
        {
            continue;
        }
        struct stat st;
        if (stat(full, &st) != 0 || !S_ISDIR(st.st_mode))
        {
            continue;
        }
        snprintf(out[n].label, sizeof(out[n].label), "%s", name);
        out[n].detail[0] = '\0';
        int ins = snprintf(out[n].insert, sizeof(out[n].insert), "/cd %s%s", parent_typed, name);
        if (ins < 0 || (size_t)ins >= sizeof(out[n].insert))
        {
            continue;
        }
        n++;
    }
    closedir(d);
    return n;
}

static const PicoCommand *FindCommand(PicoHost *app, PicoWorkspace *workspace, const char *name)
{
    for (int i = 0; i < app->command_count; i++)
    {
        if (FoldEq(app->commands[i].name, name))
        {
            return &app->commands[i];
        }
    }
    if (workspace)
    {
        for (int i = 0; i < workspace->command_count; i++)
        {
            if (FoldEq(workspace->commands[i].name, name))
            {
                return &workspace->commands[i];
            }
        }
    }
    return NULL;
}

static void SplitPrefix(const char *prefix, char *cmd, size_t cmd_cap, const char **rest)
{
    cmd[0] = '\0';
    *rest = "";
    if (!prefix)
    {
        return;
    }
    while (*prefix && isspace((unsigned char)*prefix))
    {
        prefix++;
    }
    size_t n = 0;
    while (prefix[n] && !isspace((unsigned char)prefix[n]) && n + 1 < cmd_cap)
    {
        cmd[n] = prefix[n];
        n++;
    }
    cmd[n] = '\0';
    const char *r = prefix + n;
    while (*r && isspace((unsigned char)*r))
    {
        r++;
    }
    *rest = r;
}

static bool NeedsArgs(const char *name)
{
    return FoldEq(name, "model") || FoldEq(name, "effort") || FoldEq(name, "login") ||
           FoldEq(name, "logout") || FoldEq(name, "docs") || FoldEq(name, "resume") ||
           FoldEq(name, "cd");
}

static bool HasSpace(const char *s)
{
    for (; s && *s; s++)
    {
        if (isspace((unsigned char)*s))
        {
            return true;
        }
    }
    return false;
}

/* Offers the provider's own `verbs` list; `insert_prefix` is the command text the
 * verb is appended to, e.g. "/login" or "/login openai". */
static int AuthVerbs(const PicoAuth *a, const char *insert_prefix, const char *partial,
                     PicoCompleteItem *out, int max, int n)
{
    const char *p = a->verbs;
    while (p && *p && n < max)
    {
        while (*p == ' ')
        {
            p++;
        }
        size_t len = 0;
        while (p[len] && p[len] != ' ')
        {
            len++;
        }
        if (len == 0)
        {
            break;
        }
        char verb[32];
        if (len < sizeof(verb))
        {
            memcpy(verb, p, len);
            verb[len] = '\0';
            if (FoldPrefix(verb, partial))
            {
                snprintf(out[n].label, sizeof(out[n].label), "%s", verb);
                snprintf(out[n].detail, sizeof(out[n].detail), "%s", a->provider);
                snprintf(out[n].insert, sizeof(out[n].insert), "%s %s", insert_prefix, verb);
                n++;
            }
        }
        p += len;
    }
    return n;
}

static int AuthQuery(PicoHost *app, bool is_login, const char *rest, PicoCompleteItem *out, int max)
{
    char first[64];
    const char *tail = "";
    SplitPrefix(rest, first, sizeof(first), &tail);
    const char *verb_cmd = is_login ? "/login" : "/logout";
    const PicoAuth *typed = first[0] ? pico_find_auth(app, first) : NULL;
    if (typed && HasSpace(rest))
    {
        if (!is_login)
        {
            return 0;
        }
        char prefix[96];
        snprintf(prefix, sizeof(prefix), "%s %s", verb_cmd, typed->provider);
        return AuthVerbs(typed, prefix, tail, out, max, 0);
    }
    int n = 0;
    for (int i = 0; i < app->auth_count && n < max; i++)
    {
        const char *name = app->auths[i].provider;
        if (!name || (!FoldPrefix(name, rest) && !FoldContains(name, rest)))
        {
            continue;
        }
        snprintf(out[n].label, sizeof(out[n].label), "%s", name);
        snprintf(out[n].detail, sizeof(out[n].detail), "%s",
                 app->auths[i].help ? app->auths[i].help : "");
        snprintf(out[n].insert, sizeof(out[n].insert), "%s %s", verb_cmd, name);
        n++;
    }
    /* With one provider, `/login <verb>` is unambiguous, so offer its verbs bare. */
    if (is_login && app->auth_count == 1)
    {
        n = AuthVerbs(&app->auths[0], verb_cmd, rest, out, max, n);
    }
    return n;
}

static int CommandQuery(PicoHost *app, const char *prefix, PicoCompleteItem *out, int max, void *state)
{
    (void)state;
    char cmd[64];
    const char *rest = "";
    SplitPrefix(prefix, cmd, sizeof(cmd), &rest);
    bool has_space = false;
    if (prefix)
    {
        for (const char *p = prefix; *p; p++)
        {
            if (isspace((unsigned char)*p))
            {
                has_space = true;
                break;
            }
        }
    }
    int n = 0;
    if (!has_space)
    {
        for (int i = 0; i < app->command_count && n < max; i++)
        {
            if (!FoldPrefix(app->commands[i].name, cmd) && !FoldContains(app->commands[i].name, cmd))
            {
                continue;
            }
            snprintf(out[n].label, sizeof(out[n].label), "/%s", app->commands[i].name);
            snprintf(out[n].detail, sizeof(out[n].detail), "%s",
                     app->commands[i].help ? app->commands[i].help : "");
            if (NeedsArgs(app->commands[i].name))
            {
                snprintf(out[n].insert, sizeof(out[n].insert), "/%s ", app->commands[i].name);
            }
            else
            {
                snprintf(out[n].insert, sizeof(out[n].insert), "/%s", app->commands[i].name);
            }
            n++;
        }
        PicoWorkspace *ws = PicoHost_SelectedWorkspace(app);
        for (int i = 0; ws && i < ws->command_count && n < max; i++)
        {
            if (!FoldPrefix(ws->commands[i].name, cmd) && !FoldContains(ws->commands[i].name, cmd))
            {
                continue;
            }
            snprintf(out[n].label, sizeof(out[n].label), "/%s", ws->commands[i].name);
            snprintf(out[n].detail, sizeof(out[n].detail), "%s",
                     ws->commands[i].help ? ws->commands[i].help : "");
            if (NeedsArgs(ws->commands[i].name))
            {
                snprintf(out[n].insert, sizeof(out[n].insert), "/%s ", ws->commands[i].name);
            }
            else
            {
                snprintf(out[n].insert, sizeof(out[n].insert), "/%s", ws->commands[i].name);
            }
            n++;
        }
        return n;
    }
    if (FoldEq(cmd, "model"))
    {
        PicoWorkspace *ws = PicoHost_SelectedWorkspace(app);
        for (int i = 0; ws && i < ws->model_count && n < max; i++)
        {
            PicoModel *m = &ws->models[i];
            if (!FoldPrefix(m->id, rest) && !FoldContains(m->id, rest) && !FoldContains(m->name, rest))
            {
                continue;
            }
            snprintf(out[n].label, sizeof(out[n].label), "%s", m->id);
            snprintf(out[n].detail, sizeof(out[n].detail), "%s", m->name);
            snprintf(out[n].insert, sizeof(out[n].insert), "/model %s", m->id);
            n++;
        }
        return n;
    }
    if (FoldEq(cmd, "effort"))
    {
        PicoModel *m = PicoSettings_ActiveModel(PicoHost_SelectedAgent(app));
        if (!m)
        {
            return 0;
        }
        for (int i = 0; i < m->effort_count && n < max; i++)
        {
            if (!FoldPrefix(m->effort[i], rest) && !FoldContains(m->effort[i], rest))
            {
                continue;
            }
            snprintf(out[n].label, sizeof(out[n].label), "%s", m->effort[i]);
            out[n].detail[0] = '\0';
            snprintf(out[n].insert, sizeof(out[n].insert), "/effort %s", m->effort[i]);
            n++;
        }
        if (n == 0 && m->effort_count == 0 && FoldPrefix("none", rest))
        {
            snprintf(out[n].label, sizeof(out[n].label), "none");
            snprintf(out[n].insert, sizeof(out[n].insert), "/effort none");
            n++;
        }
        return n;
    }
    if (FoldEq(cmd, "login") || FoldEq(cmd, "logout"))
    {
        return AuthQuery(app, FoldEq(cmd, "login"), rest, out, max);
    }
    if (FoldEq(cmd, "docs"))
    {
        for (size_t i = 0; i < sizeof(kDocTopics) / sizeof(kDocTopics[0]) && n < max; i++)
        {
            if (!FoldPrefix(kDocTopics[i], rest) && !FoldContains(kDocTopics[i], rest))
            {
                continue;
            }
            snprintf(out[n].label, sizeof(out[n].label), "%s", kDocTopics[i]);
            out[n].detail[0] = '\0';
            snprintf(out[n].insert, sizeof(out[n].insert), "/docs %s", kDocTopics[i]);
            n++;
        }
        return n;
    }
    if (FoldEq(cmd, "resume"))
    {
        PicoSessionInfo *list = NULL;
        const PicoAgent *selected = PicoHost_SelectedAgentConst(app);
        int nlist = PicoSession_List(PicoAgent_Workspace(selected), &list, true);
        for (int i = 0; i < nlist && n < max; i++)
        {
            const PicoSessionInfo *s = &list[i];
            if (rest[0] && !FoldContains(s->title, rest) && !FoldContains(s->id, rest))
            {
                continue;
            }
            snprintf(out[n].label, sizeof(out[n].label), "%s", s->title);
            char age[32];
            RelAge(age, sizeof(age), s->mtime);
            if (s->id[0] && selected && FoldEq(s->id, selected->session_id))
            {
                snprintf(out[n].detail, sizeof(out[n].detail), "current · %s", age);
            }
            else
            {
                snprintf(out[n].detail, sizeof(out[n].detail), "%s", age);
            }
            snprintf(out[n].insert, sizeof(out[n].insert), "/resume %s", s->id);
            n++;
        }
        free(list);
        return n;
    }
    if (FoldEq(cmd, "cd"))
    {
        return CdQuery(app, rest, out, max, state);
    }
    return 0;
}

static void CommandsBeforeSubmit(PicoWorkspace *workspace, const PicoHookEvent *event, void *state)
{
    PicoHost *app = workspace ? workspace->host : NULL;
    PicoAgentId agent_id = event ? event->agent_id : 0;
    (void)state;
    if (!app || app->submit_cancel || !app->composer.text)
    {
        return;
    }
    const char *s = app->composer.text;
    if (s[0] != '/')
    {
        return;
    }
    char cmd[64];
    const char *rest = "";
    SplitPrefix(s + 1, cmd, sizeof(cmd), &rest);
    const PicoCommand *found = FindCommand(app, workspace, cmd);
    if (!found)
    {
        return;
    }
    if (found->host_run)
    {
        found->host_run(app, agent_id, rest, found->state);
    }
    else if (found->workspace_run && found->workspace)
    {
        found->workspace_run(found->workspace, agent_id, rest, found->state);
    }
}

static int CommandsHostInit(PicoHost *app, void **state_out)
{
    (void)state_out;
    pico_host_add_command(app, "login", "Sign in a provider", CmdLogin);
    pico_host_add_command(app, "logout", "Sign out a provider", CmdLogout);
    pico_host_add_command(app, "quit", "Quit Pico", CmdQuit);
    pico_host_add_command(app, "help", "List commands", CmdHelp);
    pico_host_add_command(app, "docs", "Show extension docs", CmdDocs);
    pico_host_add_command(app, "reload", "Reload host extensions and the selected workspace", CmdReload);
    pico_host_add_completer(app, '/', true, CommandQuery, NULL);
    return 0;
}

static int CommandsWorkspaceInit(PicoWorkspace *workspace, void **state_out)
{
    (void)state_out;
    pico_workspace_add_command(workspace, "model", "Switch model", CmdModel);
    pico_workspace_add_command(workspace, "effort", "Set reasoning effort for this model", CmdEffort);
    pico_workspace_add_command(workspace, "new", "Start a new session", CmdNew);
    pico_workspace_add_command(workspace, "resume", "Resume a previous session", CmdResume);
    pico_workspace_add_command(workspace, "cd", "Open or select a workspace", CmdCd);
    pico_workspace_add_command(workspace, "compact", "Compact the current session", CmdCompact);
    pico_workspace_add_hook(workspace, PICO_HOOK_BEFORE_SUBMIT, CommandsBeforeSubmit);
    return 0;
}

PicoExt pico_ext_commands(void)
{
    return (PicoExt){
        .abi = PICO_EXT_ABI,
        .name = "commands",
        .description = "Slash commands",
        .host_init = CommandsHostInit,
        .workspace_init = CommandsWorkspaceInit,
    };
}
