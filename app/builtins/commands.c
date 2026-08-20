#define _DEFAULT_SOURCE
#define _POSIX_C_SOURCE 200809L

#include "pico/plugin.h"
#include "agent.h"
#include "session.h"
#include "settings.h"
#include "overlay.h"
#include "pico/auth.h"
#include "json.h"

#include <ctype.h>
#include <dirent.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#ifndef PICO_DOCS
#define PICO_DOCS ""
#endif

#include "raylib.h"

static void Note(PicoApp *app, const char *text)
{
    PicoApp_AddMessage(app, PICO_ROLE_ASSISTANT, text);
}

static void ClearComposer(PicoApp *app)
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

static PicoModel *FindCatalog(PicoApp *app, const char *q)
{
    if (!q || !q[0])
    {
        return NULL;
    }
    for (int i = 0; i < app->model_count; i++)
    {
        if (FoldEq(app->models[i].id, q) || FoldEq(app->models[i].name, q))
        {
            return &app->models[i];
        }
    }
    return NULL;
}

static void LogSelection(PicoApp *app)
{
    PicoSession_LogModelChange(app, app->settings.model, PicoSettings_ActiveEffort(app));
}

static void CmdModel(PicoApp *app, const char *args)
{
    while (args && *args && isspace((unsigned char)*args))
    {
        args++;
    }
    if (!args || !args[0])
    {
        PicoComposer_SetText(app, "/model ");
        app->submit_cancel = true;
        PicoComplete_Refresh(app);
        return;
    }
    PicoModel *m = FindCatalog(app, args);
    if (!m)
    {
        char line[256];
        snprintf(line, sizeof(line), "Unknown model `%s`. Try `/model` for the catalog.", args);
        PicoOverlay_Notify(app, line);
        ClearComposer(app);
        app->submit_cancel = true;
        return;
    }
    snprintf(app->settings.model, sizeof(app->settings.model), "%s", m->id);
    PicoSettings_SyncActive(app);
    PicoSettings_SaveSelection(app, true, false);
    LogSelection(app);
    char line[256];
    snprintf(line, sizeof(line), "Model `%s` · effort `%s`", m->name[0] ? m->name : m->id,
             PicoSettings_ActiveEffort(app));
    PicoOverlay_Notify(app, line);
    ClearComposer(app);
    app->submit_cancel = true;
}

static void CmdEffort(PicoApp *app, const char *args)
{
    while (args && *args && isspace((unsigned char)*args))
    {
        args++;
    }
    PicoModel *m = PicoSettings_ActiveModel(app);
    if (!args || !args[0])
    {
        PicoComposer_SetText(app, "/effort ");
        app->submit_cancel = true;
        PicoComplete_Refresh(app);
        return;
    }
    if (!m)
    {
        PicoOverlay_Notify(app, "No model in the catalog. Add one in settings.json.");
        ClearComposer(app);
        app->submit_cancel = true;
        return;
    }
    const char *level = args;
    if (m->effort_count > 0 && !PicoSettings_EffortAllowed(m, level))
    {
        char line[256];
        snprintf(line, sizeof(line), "`%s` is not in this model's effort list.", level);
        PicoOverlay_Notify(app, line);
        ClearComposer(app);
        app->submit_cancel = true;
        return;
    }
    snprintf(m->selected_effort, sizeof(m->selected_effort), "%s", level);
    PicoSettings_SaveSelection(app, false, true);
    LogSelection(app);
    char line[256];
    snprintf(line, sizeof(line), "Effort `%s` for `%s`", m->selected_effort, m->name[0] ? m->name : m->id);
    PicoOverlay_Notify(app, line);
    ClearComposer(app);
    app->submit_cancel = true;
}

static void CmdCompact(PicoApp *app, const char *args)
{
    (void)args;
    PicoAgent_Compact(app);
    ClearComposer(app);
    app->submit_cancel = true;
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

static void CmdResume(PicoApp *app, const char *args)
{
    while (args && *args && isspace((unsigned char)*args))
    {
        args++;
    }
    if (!args || !args[0])
    {
        PicoComposer_SetText(app, "/resume ");
        app->submit_cancel = true;
        PicoComplete_Refresh(app);
        return;
    }
    if (PicoAgent_IsBusy(app))
    {
        Note(app, "Wait until the agent is idle before resuming a session.");
        ClearComposer(app);
        app->submit_cancel = true;
        return;
    }
    if (FoldEq(app->session_id, args))
    {
        ClearComposer(app);
        app->submit_cancel = true;
        return;
    }
    if (PicoSession_Open(app, args) != 0)
    {
        char line[256];
        snprintf(line, sizeof(line), "Unknown session `%s`. Try `/resume`.", args);
        Note(app, line);
        ClearComposer(app);
        app->submit_cancel = true;
        return;
    }
    ClearComposer(app);
    app->submit_cancel = true;
}

static void CmdQuit(PicoApp *app, const char *args)
{
    (void)args;
    ClearComposer(app);
    app->submit_cancel = true;
    CloseWindow();
}

static const char *const kDocTopics[] = {
    "README", "anatomy", "views", "hooks", "tools", "commands", "completers", "providers", "auth", "contracts",
};

static void DocsTopicName(char *out, size_t cap, const char *args)
{
    while (args && *args && isspace((unsigned char)*args))
    {
        args++;
    }
    if (!args || !args[0] || FoldEq(args, "readme") || FoldEq(args, "index"))
    {
        snprintf(out, cap, "README");
        return;
    }
    size_t n = 0;
    for (; args[n] && n + 1 < cap; n++)
    {
        unsigned char c = (unsigned char)args[n];
        if (!(isalnum(c) || c == '_' || c == '-'))
        {
            break;
        }
        out[n] = (char)Fold(c);
    }
    out[n] = '\0';
    if (FoldEq(out, "readme") || FoldEq(out, "index"))
    {
        snprintf(out, cap, "README");
    }
}

static size_t Append(char *buf, size_t cap, size_t n, const char *fmt, ...);

static void CmdDocs(PicoApp *app, const char *args)
{
    ClearComposer(app);
    app->submit_cancel = true;
    if (!PICO_DOCS[0])
    {
        Note(app, "Extension docs path is not configured.");
        return;
    }
    char topic[64];
    DocsTopicName(topic, sizeof(topic), args);
    if (!topic[0])
    {
        Note(app, "Unknown docs topic. Try `/docs`.");
        return;
    }
    char path[4096];
    snprintf(path, sizeof(path), "%s/%s.md", PICO_DOCS, topic);
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
        Note(app, buf);
        return;
    }
    Note(app, src);
    free(src);
}

static void CmdHelp(PicoApp *app, const char *args)
{
    (void)args;
    char buf[1024];
    size_t n = 0;
    n += (size_t)snprintf(buf + n, sizeof(buf) - n, "Commands:\n");
    for (int i = 0; i < app->command_count && n + 8 < sizeof(buf); i++)
    {
        n += (size_t)snprintf(buf + n, sizeof(buf) - n, "`/%s` — %s\n", app->commands[i].name,
                              app->commands[i].help ? app->commands[i].help : "");
    }
    Note(app, buf);
    ClearComposer(app);
    app->submit_cancel = true;
}

static const PicoAuth *OnlyAuth(PicoApp *app)
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

static void ListAuthProviders(PicoApp *app, const char *prefix)
{
    char buf[1024];
    size_t n = Append(buf, sizeof(buf), 0, "%s", prefix);
    if (app->auth_count == 0)
    {
        Append(buf, sizeof(buf), n, " No providers registered.");
        Note(app, buf);
        return;
    }
    for (int i = 0; i < app->auth_count; i++)
    {
        n = Append(buf, sizeof(buf), n, "\n- `%s` — %s", app->auths[i].provider,
                   app->auths[i].help ? app->auths[i].help : "");
    }
    Note(app, buf);
}

static void RunLogin(PicoApp *app, const PicoAuth *a, const char *args)
{
    if (!a || !a->login)
    {
        Note(app, "That provider has no login.");
        return;
    }
    a->login(app, args ? args : "");
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
static void CmdLogin(PicoApp *app, const char *args)
{
    const char *all = Trim(args);
    char first[64];
    const char *rest = "";
    SplitPrefix(all, first, sizeof(first), &rest);
    const PicoAuth *a = first[0] ? pico_find_auth(app, first) : NULL;
    if (a)
    {
        RunLogin(app, a, rest);
    }
    else if ((a = OnlyAuth(app)) != NULL)
    {
        RunLogin(app, a, all);
    }
    else
    {
        ListAuthProviders(app, first[0] ? "Unknown provider. Try:" : "Usage: `/login [provider]`.");
    }
    ClearComposer(app);
    app->submit_cancel = true;
}

static void CmdLogout(PicoApp *app, const char *args)
{
    char first[64];
    const char *rest = "";
    SplitPrefix(Trim(args), first, sizeof(first), &rest);
    const PicoAuth *a = first[0] ? pico_find_auth(app, first) : OnlyAuth(app);
    if (!a)
    {
        ListAuthProviders(app, first[0] ? "Unknown provider. Try:" : "Usage: `/logout [provider]`.");
    }
    else if (a->logout)
    {
        a->logout(app);
    }
    else
    {
        Note(app, pico_auth_clear_oauth(app, a->provider)
                      ? "Logged out."
                      : "Logged out, but `~/.config/pico/auth.json` could not be written, so the "
                        "stored credentials may still be on disk.");
    }
    ClearComposer(app);
    app->submit_cancel = true;
}

static void CmdReload(PicoApp *app, const char *args)
{
    (void)args;
    PicoApp_RequestReload(app);
    Note(app, "Reloading extensions…");
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

static void FormatHomePath(const char *path, char *out, size_t cap)
{
    const char *home = getenv("HOME");
    if (home && home[0] && path)
    {
        size_t n = strlen(home);
        while (n > 1 && home[n - 1] == '/')
        {
            n--;
        }
        if (strncmp(path, home, n) == 0 && (path[n] == '\0' || path[n] == '/'))
        {
            snprintf(out, cap, "~%s", path + n);
            return;
        }
    }
    snprintf(out, cap, "%s", path ? path : "");
}

static void CmdCd(PicoApp *app, const char *args)
{
    while (args && *args && isspace((unsigned char)*args))
    {
        args++;
    }
    if (!args || !args[0])
    {
        PicoComposer_SetText(app, "/cd ");
        app->submit_cancel = true;
        PicoComplete_Refresh(app);
        return;
    }
    if (PicoAgent_IsBusy(app))
    {
        PicoOverlay_Notify(app, "Wait until the agent is idle before changing directory.");
        ClearComposer(app);
        app->submit_cancel = true;
        return;
    }

    char trimmed[4096];
    snprintf(trimmed, sizeof(trimmed), "%s", args);
    size_t tlen = strlen(trimmed);
    while (tlen > 0 && isspace((unsigned char)trimmed[tlen - 1]))
    {
        trimmed[--tlen] = '\0';
    }

    char resolved[4096];
    if (ResolveWorkspaceDir(app->workspace, trimmed, resolved, sizeof(resolved)) != 0)
    {
        char shown[400];
        snprintf(shown, sizeof(shown), "%s", trimmed);
        char line[512];
        snprintf(line, sizeof(line), "Not a directory `%s`.", shown);
        PicoOverlay_Notify(app, line);
        ClearComposer(app);
        app->submit_cancel = true;
        return;
    }

    char current[4096];
    const char *ws = app->workspace[0] ? app->workspace : ".";
    if (realpath(ws, current) && strcmp(current, resolved) == 0)
    {
        char pretty[400];
        FormatHomePath(resolved, pretty, sizeof(pretty));
        char line[512];
        snprintf(line, sizeof(line), "Already in `%s`.", pretty);
        PicoOverlay_Notify(app, line);
        ClearComposer(app);
        app->submit_cancel = true;
        return;
    }

    snprintf(app->workspace, sizeof(app->workspace), "%s", resolved);
    PicoSession_Reset(app);
    PicoSettings_Load(app);
    PicoApp_RequestReload(app);

    char pretty[400];
    FormatHomePath(resolved, pretty, sizeof(pretty));
    char line[512];
    snprintf(line, sizeof(line), "Workspace `%s`.", pretty);
    PicoOverlay_Notify(app, line);
    ClearComposer(app);
    app->submit_cancel = true;
}

static int CdQuery(PicoApp *app, const char *rest, PicoCompleteItem *out, int max)
{
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
    if (!parent_typed[0])
    {
        const char *ws = app->workspace[0] ? app->workspace : ".";
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
        if (ResolveWorkspaceDir(app->workspace, parent_arg, list_dir, sizeof(list_dir)) != 0)
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

static const PicoCommand *FindCommand(PicoApp *app, const char *name)
{
    for (int i = 0; i < app->command_count; i++)
    {
        if (FoldEq(app->commands[i].name, name))
        {
            return &app->commands[i];
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

static int AuthQuery(PicoApp *app, bool is_login, const char *rest, PicoCompleteItem *out, int max)
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

static int CommandQuery(PicoApp *app, const char *prefix, PicoCompleteItem *out, int max)
{
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
        return n;
    }
    if (FoldEq(cmd, "model"))
    {
        for (int i = 0; i < app->model_count && n < max; i++)
        {
            PicoModel *m = &app->models[i];
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
        PicoModel *m = PicoSettings_ActiveModel(app);
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
        int nlist = PicoSession_List(app, &list);
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
            if (s->id[0] && FoldEq(s->id, app->session_id))
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
        return CdQuery(app, rest, out, max);
    }
    return 0;
}

static void CommandsBeforeSubmit(PicoApp *app)
{
    if (app->submit_cancel || !app->composer.text)
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
    const PicoCommand *found = FindCommand(app, cmd);
    if (!found)
    {
        return;
    }
    found->run(app, rest);
}

static void CommandsInit(PicoApp *app)
{
    pico_add_command(app, "model", "Switch model", CmdModel);
    pico_add_command(app, "effort", "Set reasoning effort for this model", CmdEffort);
    pico_add_command(app, "login", "Sign in a provider", CmdLogin);
    pico_add_command(app, "logout", "Sign out a provider", CmdLogout);
    pico_add_command(app, "resume", "Resume a previous session", CmdResume);
    pico_add_command(app, "cd", "Change workspace directory", CmdCd);
    pico_add_command(app, "compact", "Compact the current session", CmdCompact);
    pico_add_command(app, "quit", "Quit Pico", CmdQuit);
    pico_add_command(app, "help", "List commands", CmdHelp);
    pico_add_command(app, "docs", "Show extension docs", CmdDocs);
    pico_add_command(app, "reload", "Reload extensions", CmdReload);
    pico_add_completer(app, '/', true, CommandQuery, NULL);
    pico_add_hook(app, PICO_HOOK_BEFORE_SUBMIT, CommandsBeforeSubmit);
}

PicoExt pico_ext_commands(void)
{
    return (PicoExt){
        .abi = PICO_EXT_ABI,
        .name = "commands",
        .description = "Slash commands",
        .init = CommandsInit,
    };
}
