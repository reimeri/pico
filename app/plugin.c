#include "pico/plugin.h"
#include "agent.h"
#include "agent_manager.h"
#include "session.h"

#include <dirent.h>
#include <dlfcn.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#ifndef PICO_CC
#define PICO_CC "gcc"
#endif
#ifndef PICO_VERSION
#define PICO_VERSION "0.1.0"
#endif
#ifndef PICO_INCLUDE_APP
#define PICO_INCLUDE_APP "."
#endif
#ifndef PICO_INCLUDE_HEADERS
#define PICO_INCLUDE_HEADERS "./include"
#endif
#ifndef PICO_INCLUDE_CLAY
#define PICO_INCLUDE_CLAY ".."
#endif
#ifndef PICO_INCLUDE_RAYLIB
#define PICO_INCLUDE_RAYLIB "."
#endif

#define PICO_MAX_USER_PLUGINS 32
#define PICO_EXT_WALK_DEPTH 8

typedef struct LoadedPlugin {
    char source[4096];
    char so_path[4096];
    time_t mtime;
    void *handle;
    PicoExt ext;
    bool builtin;
} LoadedPlugin;

static LoadedPlugin g_plugins[PICO_MAX_USER_PLUGINS + 16];
static int g_plugin_count = 0;
static double g_last_poll = 0;

static PicoExt (*kBuiltins[])(void) = {
    pico_ext_chat,
    pico_ext_composer,
    pico_ext_footer,
    pico_ext_overlay,
    pico_ext_ask_user,
    pico_ext_todo,
    pico_ext_shell,
    pico_ext_subagent,
    pico_ext_commands,
    pico_ext_files,
    pico_ext_openai,
    pico_ext_extensions,
    pico_ext_prompt,
};

static void WarnClear(PicoApp *app)
{
    free(app->status_warn);
    app->status_warn = NULL;
}


static int MkdirP(const char *path)
{
    char buf[4096];
    snprintf(buf, sizeof(buf), "%s", path);
    for (char *p = buf + 1; *p; p++)
    {
        if (*p == '/')
        {
            *p = 0;
            mkdir(buf, 0755);
            *p = '/';
        }
    }
    if (mkdir(buf, 0755) != 0 && errno != EEXIST)
    {
        return -1;
    }
    return 0;
}

static const char *HomeDir(void)
{
    const char *home = getenv("HOME");
    return home && home[0] ? home : ".";
}

static void ConfigExtDir(char *out, size_t cap)
{
    const char *xdg = getenv("XDG_CONFIG_HOME");
    if (xdg && xdg[0])
    {
        snprintf(out, cap, "%s/pico/extensions", xdg);
    }
    else
    {
        snprintf(out, cap, "%s/.config/pico/extensions", HomeDir());
    }
}

static void CacheDir(char *out, size_t cap)
{
    const char *xdg = getenv("XDG_CACHE_HOME");
    if (xdg && xdg[0])
    {
        snprintf(out, cap, "%s/pico/ext", xdg);
    }
    else
    {
        snprintf(out, cap, "%s/.cache/pico/ext", HomeDir());
    }
}

static void WorkspaceExtDir(const PicoApp *app, char *out, size_t cap)
{
    snprintf(out, cap, "%s/.pico/extensions", app->workspace[0] ? app->workspace : ".");
}

static unsigned PathHash(const char *s)
{
    unsigned h = 2166136261u;
    for (; *s; s++)
    {
        h ^= (unsigned char)*s;
        h *= 16777619u;
    }
    return h;
}

static void SoPathFor(const char *src, time_t mtime, char *out, size_t cap)
{
    char cache[4096];
    CacheDir(cache, sizeof(cache));
    const char *base = strrchr(src, '/');
    base = base ? base + 1 : src;
    snprintf(out, cap, "%s/%08x-%s-%ld-" PICO_VERSION "-%d.so", cache, PathHash(src), base, (long)mtime,
             PICO_EXT_ABI);
}

static int CompileExt(const char *src, const char *so, char *err, size_t err_cap)
{
    char cache[4096];
    CacheDir(cache, sizeof(cache));
    MkdirP(cache);

    int fds[2];
    if (pipe(fds) != 0)
    {
        snprintf(err, err_cap, "%s: pipe failed: %s", src, strerror(errno));
        return -1;
    }
    pid_t pid = fork();
    if (pid < 0)
    {
        close(fds[0]);
        close(fds[1]);
        snprintf(err, err_cap, "%s: fork failed: %s", src, strerror(errno));
        return -1;
    }
    if (pid == 0)
    {
        dup2(fds[1], STDOUT_FILENO);
        dup2(fds[1], STDERR_FILENO);
        close(fds[0]);
        close(fds[1]);
        char srcdir_flag[4112];
        {
            char dir[4096];
            snprintf(dir, sizeof(dir), "%s", src);
            char *slash = strrchr(dir, '/');
            if (slash)
            {
                *slash = '\0';
            }
            else
            {
                memcpy(dir, ".", 2);
            }
            snprintf(srcdir_flag, sizeof(srcdir_flag), "-I%s", dir);
        }
        char *args[] = {(char *)PICO_CC,
                        "-shared",
                        "-fPIC",
                        "-std=c99",
                        "-I" PICO_INCLUDE_HEADERS,
                        "-I" PICO_INCLUDE_APP,
                        "-I" PICO_INCLUDE_CLAY,
                        "-I" PICO_INCLUDE_RAYLIB,
                        srcdir_flag,
                        "-o",
                        (char *)so,
                        (char *)src,
                        NULL};
        if (strchr(PICO_CC, '/'))
        {
            execv(PICO_CC, args);
        }
        else
        {
            execvp(PICO_CC, args);
        }
        _exit(127);
    }
    close(fds[1]);
    size_t n = 0;
    if (err_cap > 0)
    {
        err[0] = '\0';
    }
    while (n + 1 < err_cap)
    {
        ssize_t r = read(fds[0], err + n, err_cap - 1 - n);
        if (r <= 0)
        {
            break;
        }
        n += (size_t)r;
        err[n] = '\0';
    }
    close(fds[0]);
    int status = 0;
    waitpid(pid, &status, 0);
    if (WIFEXITED(status) && WEXITSTATUS(status) == 0)
    {
        return 0;
    }
    if (WIFEXITED(status) && WEXITSTATUS(status) == 127 && n == 0)
    {
        snprintf(err, err_cap, "%s: compiler not found (%s)", src, PICO_CC);
    }
    else if (n == 0)
    {
        snprintf(err, err_cap, "%s: compile failed (status %d)", src, WIFEXITED(status) ? WEXITSTATUS(status) : -1);
    }
    return -1;
}

static void RecordStub(const char *src, time_t mtime)
{
    if (g_plugin_count >= (int)(sizeof(g_plugins) / sizeof(g_plugins[0])))
    {
        return;
    }
    LoadedPlugin *p = &g_plugins[g_plugin_count++];
    memset(p, 0, sizeof(*p));
    snprintf(p->source, sizeof(p->source), "%s", src);
    p->mtime = mtime;
}

static int LoadSo(PicoApp *app, const char *src, const char *so, time_t mtime)
{
    void *handle = dlopen(so, RTLD_NOW | RTLD_GLOBAL);
    if (!handle)
    {
        char line[2048];
        snprintf(line, sizeof(line), "%s: dlopen: %s", src, dlerror());
        pico_status_warn(app, line);
        RecordStub(src, mtime);
        return -1;
    }
    PicoExt (*entry)(void) = (PicoExt(*)(void))dlsym(handle, "pico_ext");
    if (!entry)
    {
        char line[2048];
        snprintf(line, sizeof(line), "%s: missing pico_ext() (%s)", src, dlerror());
        pico_status_warn(app, line);
        dlclose(handle);
        RecordStub(src, mtime);
        return -1;
    }
    PicoExt ext = entry();
    if (ext.abi != PICO_EXT_ABI)
    {
        char line[2048];
        snprintf(line, sizeof(line), "%s: abi %d != %d", src, ext.abi, PICO_EXT_ABI);
        pico_status_warn(app, line);
        dlclose(handle);
        RecordStub(src, mtime);
        return -1;
    }
    if (g_plugin_count >= (int)(sizeof(g_plugins) / sizeof(g_plugins[0])))
    {
        pico_status_warn(app, "too many extensions");
        dlclose(handle);
        return -1;
    }
    LoadedPlugin *p = &g_plugins[g_plugin_count++];
    memset(p, 0, sizeof(*p));
    snprintf(p->source, sizeof(p->source), "%s", src);
    snprintf(p->so_path, sizeof(p->so_path), "%s", so);
    p->mtime = mtime;
    p->handle = handle;
    p->ext = ext;
    p->builtin = false;
    if (ext.init)
    {
        ext.init(app);
    }
    return 0;
}

static void LoadUserFile(PicoApp *app, const char *src)
{
    struct stat st;
    if (stat(src, &st) != 0)
    {
        return;
    }
    char so[4096];
    SoPathFor(src, st.st_mtime, so, sizeof(so));
    if (access(so, R_OK) != 0)
    {
        char err[8192];
        if (CompileExt(src, so, err, sizeof(err)) != 0)
        {
            char line[8700];
            snprintf(line, sizeof(line), "compile %s:\n%s", src, err);
            pico_status_warn(app, line);
            RecordStub(src, st.st_mtime);
            return;
        }
    }
    LoadSo(app, src, so, st.st_mtime);
}

static bool IsCSourceName(const char *name)
{
    size_t n = strlen(name);
    return n >= 3 && strcmp(name + n - 2, ".c") == 0;
}

static void WalkExtTree(const char *dir, int depth, int *seen, int seen_max,
                        void (*fn)(void *ctx, const char *path, time_t mtime), void *ctx)
{
    if (depth > PICO_EXT_WALK_DEPTH || *seen >= seen_max)
    {
        return;
    }
    DIR *d = opendir(dir);
    if (!d)
    {
        return;
    }
    struct dirent *ent;
    while ((ent = readdir(d)) != NULL && *seen < seen_max)
    {
        if (ent->d_name[0] == '.')
        {
            continue;
        }
        char path[4096];
        if ((size_t)snprintf(path, sizeof(path), "%s/%s", dir, ent->d_name) >= sizeof(path))
        {
            continue;
        }
        struct stat st;
        if (lstat(path, &st) != 0)
        {
            continue;
        }
        if (S_ISDIR(st.st_mode))
        {
            WalkExtTree(path, depth + 1, seen, seen_max, fn, ctx);
        }
        else if (S_ISREG(st.st_mode) && IsCSourceName(ent->d_name))
        {
            fn(ctx, path, st.st_mtime);
            (*seen)++;
        }
    }
    closedir(d);
}

static void LoadWalk(void *ctx, const char *path, time_t mtime)
{
    (void)mtime;
    LoadUserFile((PicoApp *)ctx, path);
}

static void LoadBuiltins(PicoApp *app)
{
    for (size_t i = 0; i < sizeof(kBuiltins) / sizeof(kBuiltins[0]); i++)
    {
        if (g_plugin_count >= (int)(sizeof(g_plugins) / sizeof(g_plugins[0])))
        {
            break;
        }
        LoadedPlugin *p = &g_plugins[g_plugin_count++];
        memset(p, 0, sizeof(*p));
        p->builtin = true;
        p->ext = kBuiltins[i]();
        if (p->ext.init)
        {
            p->ext.init(app);
        }
    }
}

static void ShutdownRange(PicoApp *app, bool users_only)
{
    for (int i = g_plugin_count - 1; i >= 0; i--)
    {
        if (users_only && g_plugins[i].builtin)
        {
            continue;
        }
        if (g_plugins[i].ext.shutdown)
        {
            g_plugins[i].ext.shutdown(app);
        }
        if (g_plugins[i].handle)
        {
            dlclose(g_plugins[i].handle);
            g_plugins[i].handle = NULL;
        }
    }
}

void PicoPlugins_UnloadUser(PicoApp *app)
{
    ShutdownRange(app, true);
    int w = 0;
    for (int i = 0; i < g_plugin_count; i++)
    {
        if (g_plugins[i].builtin)
        {
            g_plugins[w++] = g_plugins[i];
        }
    }
    g_plugin_count = w;
}

static void LoadUsers(PicoApp *app)
{
    if (app->safe_mode)
    {
        return;
    }
    char dir[4096];
    int seen = 0;
    ConfigExtDir(dir, sizeof(dir));
    MkdirP(dir);
    WalkExtTree(dir, 0, &seen, PICO_MAX_USER_PLUGINS, LoadWalk, app);
    WorkspaceExtDir(app, dir, sizeof(dir));
    WalkExtTree(dir, 0, &seen, PICO_MAX_USER_PLUGINS, LoadWalk, app);
}

void PicoPlugins_Load(PicoApp *app)
{
    g_plugin_count = 0;
    pico_clear_registrations(app);
    WarnClear(app);
    LoadBuiltins(app);
    LoadUsers(app);
}

void PicoPlugins_Reload(PicoApp *app)
{
    if (PicoAgentManager_BlocksReload(app->agents))
    {
        app->reload_queued = true;
        return;
    }
    app->reload_queued = false;
    PicoPlugins_UnloadUser(app);
    pico_clear_registrations(app);
    WarnClear(app);
    for (int i = 0; i < g_plugin_count; i++)
    {
        if (g_plugins[i].builtin && g_plugins[i].ext.init)
        {
            g_plugins[i].ext.init(app);
        }
    }
    LoadUsers(app);
    PicoAgentManager_LoadProfiles(app->agents);
    PicoAgentManager_ReplayToolDetails(app->agents);
}

void PicoPlugins_Shutdown(PicoApp *app)
{
    ShutdownRange(app, false);
    g_plugin_count = 0;
    pico_clear_registrations(app);
}

typedef struct CollectCtx {
    char (*paths)[4096];
    time_t *mtimes;
    int n;
    int cap;
} CollectCtx;

static void CollectWalk(void *ctx, const char *path, time_t mtime)
{
    CollectCtx *c = (CollectCtx *)ctx;
    snprintf(c->paths[c->n], 4096, "%s", path);
    c->mtimes[c->n] = mtime;
}

static int CollectSources(const PicoApp *app, char paths[][4096], time_t *mtimes, int cap)
{
    CollectCtx ctx = {.paths = paths, .mtimes = mtimes, .n = 0, .cap = cap};
    char dirs[2][4096];
    ConfigExtDir(dirs[0], sizeof(dirs[0]));
    WorkspaceExtDir(app, dirs[1], sizeof(dirs[1]));
    for (int d = 0; d < 2; d++)
    {
        WalkExtTree(dirs[d], 0, &ctx.n, cap, CollectWalk, &ctx);
    }
    return ctx.n;
}

void PicoPlugins_Poll(PicoApp *app)
{
    if (app->safe_mode)
    {
        return;
    }
    double now = GetTime();
    if (now - g_last_poll < 0.5)
    {
        return;
    }
    g_last_poll = now;

    char paths[PICO_MAX_USER_PLUGINS][4096];
    time_t mtimes[PICO_MAX_USER_PLUGINS];
    int n = CollectSources(app, paths, mtimes, PICO_MAX_USER_PLUGINS);

    int user_count = 0;
    for (int i = 0; i < g_plugin_count; i++)
    {
        if (!g_plugins[i].builtin)
        {
            user_count++;
        }
    }
    bool changed = n != user_count;
    if (!changed)
    {
        for (int i = 0; i < n; i++)
        {
            bool found = false;
            for (int p = 0; p < g_plugin_count; p++)
            {
                if (!g_plugins[p].builtin && strcmp(g_plugins[p].source, paths[i]) == 0)
                {
                    found = true;
                    if (g_plugins[p].mtime != mtimes[i])
                    {
                        changed = true;
                    }
                    break;
                }
            }
            if (!found)
            {
                changed = true;
            }
            if (changed)
            {
                break;
            }
        }
    }
    if (changed)
    {
        PicoPlugins_Reload(app);
    }
}

void PicoPlugins_OnFrame(PicoApp *app, float dt)
{
    for (int i = 0; i < g_plugin_count; i++)
    {
        if (g_plugins[i].ext.on_frame)
        {
            g_plugins[i].ext.on_frame(app, dt);
        }
    }
}

int PicoPlugins_Count(void)
{
    return g_plugin_count;
}

bool PicoPlugins_Get(int index, PicoExtInfo *out)
{
    if (!out || index < 0 || index >= g_plugin_count)
    {
        return false;
    }
    LoadedPlugin *p = &g_plugins[index];
    out->name = p->ext.name;
    out->description = p->ext.description;
    out->source = (p->builtin || p->source[0] == '\0') ? NULL : p->source;
    out->builtin = p->builtin;
    out->loaded = p->builtin || p->handle != NULL;
    out->enabled = out->loaded;
    return true;
}
