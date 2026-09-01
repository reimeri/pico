#include "pico/plugin.h"
#include "agent.h"
#include "docs_path.h"
#include "workspace_internal.h"
#include "path.h"
#include "session.h"
#include "settings.h"
#include "host_internal.h"

#include <assert.h>
#include <dirent.h>
#include <dlfcn.h>
#include <fcntl.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#ifndef PICO_CC
#define PICO_CC "cc"
#endif
#ifndef PICO_VERSION
#define PICO_VERSION "0.1.5"
#endif

#define PICO_MAX_USER_PLUGINS 32
#define PICO_EXT_WALK_DEPTH 8

typedef PicoModuleGeneration LoadedPlugin;

static int ModuleCapacity(const PicoHost *host)
{
    return host ? host->module_capacity : 0;
}

static bool ValidateUserSources(PicoHost *app);
static int CollectGlobalSources(char paths[][4096], time_t *mtimes,
                                uint64_t *hashes, int cap);
static int CollectWorkspaceSources(const PicoWorkspace *workspace, char paths[][4096],
                                   time_t *mtimes, uint64_t *hashes, int cap);
static bool ConfigExtDir(char *out, size_t cap);
static bool WorkspaceExtDir(const PicoWorkspace *workspace, char *out, size_t cap);
static bool PathWithinDirectory(const char *path, const char *directory);
static PicoWorkspace *SourceWorkspace(const PicoHost *host, const char *src);
static bool IsWorkspaceLocalSource(const PicoHost *host, const char *src);
static bool ModuleAppliesToWorkspace(const PicoHost *host,
                                     const PicoWorkspace *workspace,
                                     const PicoModuleGeneration *module);

static PicoExt (*kBuiltins[])(void) = {
    pico_ext_chat,
    pico_ext_composer,
    pico_ext_footer,
    pico_ext_sidebar,
    pico_ext_overlay,
    pico_ext_notify,
    pico_ext_ask_user,
    pico_ext_todo,
    pico_ext_shell,
    pico_ext_subagent,
    pico_ext_commands,
    pico_ext_files,
    pico_ext_openai,
    pico_ext_hyper,
    pico_ext_xai,
    pico_ext_extensions,
    pico_ext_settings,
    pico_ext_prompt,
    pico_ext_diff,
};

static void ShutdownPlugin(PicoHost *host, LoadedPlugin *p)
{
    if (!host || !p)
    {
        return;
    }
    for (int w = 0; w < host->workspace_count; w++)
    {
        if (host->workspaces[w])
        {
            PicoWorkspaceExtensions_ShutdownModule(host->workspaces[w], p);
        }
    }
    PicoHostExtensions_ShutdownModule(host, p);
}

static bool ActivatePlugin(PicoHost *host, LoadedPlugin *p)
{
    if (!host || !p)
    {
        return false;
    }
    bool loaded = p->builtin || p->handle != NULL;
    if (!loaded)
    {
        return false;
    }

    bool success = true;
    if (!PicoHostExtensions_Activate(host, p))
    {
        success = false;
    }

    for (int w = 0; w < host->workspace_count; w++)
    {
        PicoWorkspace *ws = host->workspaces[w];
        if (!ws)
        {
            continue;
        }
        if (!ModuleAppliesToWorkspace(host, ws, p))
        {
            continue;
        }
        if (!PicoWorkspaceExtensions_Activate(ws, p))
        {
            success = false;
        }
    }
    return success;
}

static void WarnClear(PicoHost *app)
{
    free(app->status_warn);
    app->status_warn = NULL;
}


static int MkdirP(const char *path)
{
    char buf[4096];
    if (!PicoPath_Format(buf, sizeof(buf), "%s", path))
    {
        return -1;
    }
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

static bool ConfigExtDir(char *out, size_t cap)
{
    const char *xdg = getenv("XDG_CONFIG_HOME");
    if (xdg && xdg[0])
    {
        return PicoPath_Format(out, cap, "%s/pico/extensions", xdg);
    }
    return PicoPath_Format(out, cap, "%s/.config/pico/extensions", HomeDir());
}

static bool CacheDir(char *out, size_t cap)
{
    const char *xdg = getenv("XDG_CACHE_HOME");
    if (xdg && xdg[0])
    {
        return PicoPath_Format(out, cap, "%s/pico/ext", xdg);
    }
    return PicoPath_Format(out, cap, "%s/.cache/pico/ext", HomeDir());
}

static bool WorkspaceExtDir(const PicoWorkspace *workspace, char *out, size_t cap)
{
    const char *root = PicoWorkspace_Path(workspace);
    if (!root[0])
    {
        return false;
    }
    return PicoPath_Format(out, cap, "%s/.pico/extensions", root);
}

static bool PathWithinDirectory(const char *path, const char *directory)
{
    if (!path || !directory || !directory[0])
    {
        return false;
    }
    size_t len = strlen(directory);
    return strncmp(path, directory, len) == 0 &&
           (path[len] == '/' || path[len] == '\0');
}

static PicoWorkspace *SourceWorkspace(const PicoHost *host, const char *src)
{
    return PicoHost_SourceWorkspace(host, src);
}

static bool IsWorkspaceLocalSource(const PicoHost *host, const char *src)
{
    return SourceWorkspace(host, src) != NULL;
}

static bool ModuleAppliesToWorkspace(const PicoHost *host,
                                     const PicoWorkspace *workspace,
                                     const PicoModuleGeneration *module)
{
    if (!host || !workspace || !module || module->builtin)
    {
        return host && workspace && module;
    }
    PicoWorkspace *owner = SourceWorkspace(host, module->source);
    return !owner || owner == workspace;
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

static uint64_t SourceHash(const char *src)
{
    FILE *file = fopen(src, "rb");
    unsigned char buffer[4096];
    uint64_t hash = 1469598103934665603ULL;
    size_t n;
    if (!file)
    {
        return 0;
    }
    while ((n = fread(buffer, 1, sizeof(buffer), file)) > 0)
    {
        for (size_t i = 0; i < n; i++)
        {
            hash ^= buffer[i];
            hash *= 1099511628211ULL;
        }
    }
    fclose(file);
    return hash;
}

static bool SoPathFor(const char *src, time_t mtime, uint64_t content_hash, char *out, size_t cap)
{
    char cache[4096];
    if (!CacheDir(cache, sizeof(cache)))
    {
        return false;
    }
    const char *base = strrchr(src, '/');
    base = base ? base + 1 : src;
    return PicoPath_Format(out, cap, "%s/%08x-%s-%ld-%016llx-" PICO_VERSION "-%d.so", cache,
                           PathHash(src), base, (long)mtime,
                           (unsigned long long)content_hash, PICO_EXT_ABI);
}

static int CompileExt(const char *src, const char *so, char *err, size_t err_cap)
{
    char cache[4096];
    char tmp[4096];
    char sdk_include[4096];
    const char *compiler = getenv("PICO_CC");
    if (!compiler || !compiler[0])
    {
        compiler = PICO_CC;
    }
    if (!Pico_SdkIncludeDir(sdk_include, sizeof(sdk_include)) ||
        access(sdk_include, R_OK) != 0)
    {
        snprintf(err, err_cap, "%s: Pico extension SDK is unavailable", src);
        return -1;
    }
    if (!CacheDir(cache, sizeof(cache)) || MkdirP(cache) != 0)
    {
        snprintf(err, err_cap, "%s: extension cache path is unavailable", src);
        return -1;
    }

    if (snprintf(tmp, sizeof(tmp), "%s.tmp-%ld", so, (long)getpid()) >= (int)sizeof(tmp))
    {
        snprintf(err, err_cap, "%s: extension cache path is unavailable", src);
        return -1;
    }
    unlink(tmp);

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
        char sdk_flag[4112];
        snprintf(sdk_flag, sizeof(sdk_flag), "-I%s", sdk_include);
        char *args[] = {(char *)compiler,
                        "-shared",
                        "-fPIC",
                        "-std=c99",
                        sdk_flag,
                        srcdir_flag,
                        "-o",
                        tmp,
                        (char *)src,
                        NULL};
        if (strchr(compiler, '/'))
        {
            execv(compiler, args);
        }
        else
        {
            execvp(compiler, args);
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
        int fd = open(tmp, O_RDONLY);
        if (fd >= 0)
        {
            (void)fsync(fd);
            close(fd);
        }
        if (rename(tmp, so) != 0)
        {
            snprintf(err, err_cap, "%s: could not publish extension cache: %s", src, strerror(errno));
            unlink(tmp);
            return -1;
        }
        return 0;
    }
    unlink(tmp);
    if (WIFEXITED(status) && WEXITSTATUS(status) == 127 && n == 0)
    {
        snprintf(err, err_cap, "%s: compiler not found (%s)", src, compiler);
    }
    else if (n == 0)
    {
        snprintf(err, err_cap, "%s: compile failed (status %d)", src, WIFEXITED(status) ? WEXITSTATUS(status) : -1);
    }
    return -1;
}

static bool ModuleSlotFree(const LoadedPlugin *module)
{
    return module && module->generation == 0 && module->ref_count == 0 &&
           !module->desired && module->handle == NULL;
}

static LoadedPlugin *NewModuleSlot(PicoHost *app)
{
    if (!app || !app->modules)
    {
        return NULL;
    }
    for (int i = 0; i < app->module_count; i++)
    {
        if (ModuleSlotFree(&app->modules[i]))
        {
            return &app->modules[i];
        }
    }
    if (app->module_count >= ModuleCapacity(app))
    {
        return NULL;
    }
    return &app->modules[app->module_count++];
}

static void TrimModuleSlots(PicoHost *app)
{
    while (app && app->module_count > 0 &&
           ModuleSlotFree(&app->modules[app->module_count - 1]))
    {
        app->module_count--;
    }
}

static void RollbackModuleCandidates(PicoHost *app, uint64_t generation_floor)
{
    for (int i = app ? app->module_count - 1 : -1; i >= 0; i--)
    {
        LoadedPlugin *module = &app->modules[i];
        if (module->generation > generation_floor && module->desired)
        {
            module->desired = false;
            PicoModule_Release(module);
        }
    }
    TrimModuleSlots(app);
}

static void RecordStub(PicoHost *app, const char *src, time_t mtime, const char *err)
{
    LoadedPlugin *p = NewModuleSlot(app);
    if (!p)
    {
        return;
    }
    memset(p, 0, sizeof(*p));
    snprintf(p->source, sizeof(p->source), "%s", src);
    p->mtime = mtime;
    p->content_hash = SourceHash(src);
    p->generation = ++app->next_module_generation;
    p->desired = true;
    p->ref_count = 1; /* host module-store reference, even for a failed stub */
    const char *base = strrchr(p->source, '/');
    p->ext.name = base ? base + 1 : p->source;
    if (err)
    {
        pico_status_warn(app, err);
    }
}

static LoadedPlugin *FindDesiredSource(PicoHost *app, const char *src)
{
    if (!app || !src)
    {
        return NULL;
    }
    for (int i = 0; i < app->module_count; i++)
    {
        LoadedPlugin *p = &app->modules[i];
        if (!p->builtin && p->desired && p->handle && strcmp(p->source, src) == 0)
        {
            return p;
        }
    }
    return NULL;
}

static void RecordStubIfMissing(PicoHost *app, const char *src, time_t mtime, const char *err)
{
    if (!FindDesiredSource(app, src))
    {
        RecordStub(app, src, mtime, err);
    }
}

static void RetireSourceGeneration(PicoHost *app, const char *src, LoadedPlugin *keep)
{
    if (!app || !src)
    {
        return;
    }
    for (int i = 0; i < app->module_count; i++)
    {
        LoadedPlugin *p = &app->modules[i];
        if (p != keep && !p->builtin && p->desired && strcmp(p->source, src) == 0)
        {
            p->desired = false;
            PicoModule_Release(p); /* release the module-store reference */
        }
    }
}

static int LoadSo(PicoHost *app, const char *src, const char *so, time_t mtime, bool activate)
{
    void *handle = dlopen(so, RTLD_NOW | RTLD_LOCAL);
    if (!handle)
    {
        char line[2048];
        snprintf(line, sizeof(line), "%s: dlopen: %s", src, dlerror());
        pico_status_warn(app, line);
        RecordStubIfMissing(app, src, mtime, line);
        return -1;
    }
    PicoExt (*entry)(void) = (PicoExt(*)(void))dlsym(handle, "pico_ext");
    if (!entry)
    {
        char line[2048];
        snprintf(line, sizeof(line), "%s: missing pico_ext() (%s)", src, dlerror());
        pico_status_warn(app, line);
        dlclose(handle);
        RecordStubIfMissing(app, src, mtime, line);
        return -1;
    }
    PicoExt ext = entry();
    if (ext.abi != PICO_EXT_ABI)
    {
        char line[2048];
        snprintf(line, sizeof(line), "%s: abi %d != %d", src, ext.abi, PICO_EXT_ABI);
        pico_status_warn(app, line);
        dlclose(handle);
        RecordStubIfMissing(app, src, mtime, line);
        return -1;
    }
    if (SourceWorkspace(app, src) &&
        (ext.host_init || ext.host_shutdown || ext.host_on_frame))
    {
        char line[2048];
        snprintf(line, sizeof(line), "%s: workspace-local extension cannot have host callbacks", src);
        pico_status_warn(app, line);
        dlclose(handle);
        RecordStubIfMissing(app, src, mtime, line);
        return -1;
    }
    LoadedPlugin *p = NewModuleSlot(app);
    if (!p)
    {
        pico_status_warn(app, "too many extension generations");
        dlclose(handle);
        return -1;
    }
    memset(p, 0, sizeof(*p));
    snprintf(p->source, sizeof(p->source), "%s", src);
    snprintf(p->so_path, sizeof(p->so_path), "%s", so);
    p->mtime = mtime;
    p->content_hash = SourceHash(src);
    p->handle = handle;
    p->ext = ext;
    p->generation = ++app->next_module_generation;
    p->builtin = false;
    p->desired = true;
    p->ref_count = 1; /* host module-store reference */
    if (activate)
    {
        ActivatePlugin(app, p);
        RetireSourceGeneration(app, src, p);
    }
    return 0;
}

static void LoadUserFile(PicoHost *app, const char *src)
{
    struct stat st;
    if (stat(src, &st) != 0)
    {
        return;
    }
    char so[4096];
    uint64_t content_hash = SourceHash(src);
    LoadedPlugin *current = FindDesiredSource(app, src);
    if (current && current->mtime == st.st_mtime && current->content_hash == content_hash)
    {
        return;
    }
    if (!SoPathFor(src, st.st_mtime, content_hash, so, sizeof(so)))
    {
        pico_status_warn(app, "Extension cache path is too long.");
        if (!current)
        {
            RecordStub(app, src, st.st_mtime, "Extension cache path is too long.");
        }
        return;
    }
    if (access(so, R_OK) != 0)
    {
        char err[8192];
        if (CompileExt(src, so, err, sizeof(err)) != 0)
        {
            char line[8700];
            snprintf(line, sizeof(line), "compile %s:\n%s", src, err);
            pico_status_warn(app, line);
            if (!current)
            {
                RecordStub(app, src, st.st_mtime, line);
            }
            return;
        }
    }
    if (LoadSo(app, src, so, st.st_mtime, true) != 0 && current)
    {
        /* A bad replacement never displaces the last complete generation. */
        current->desired = true;
        (void)ActivatePlugin(app, current);
    }
}

static int LoadUserCandidate(PicoHost *app, const char *src)
{
    struct stat st;
    char so[4096];
    uint64_t content_hash;
    if (stat(src, &st) != 0)
    {
        return -1;
    }
    content_hash = SourceHash(src);
    if (!SoPathFor(src, st.st_mtime, content_hash, so, sizeof(so)))
    {
        return -1;
    }
    if (access(so, R_OK) != 0)
    {
        char err[8192];
        if (CompileExt(src, so, err, sizeof(err)) != 0)
        {
            pico_status_warn(app, err);
            return -1;
        }
    }
    return LoadSo(app, src, so, st.st_mtime, false);
}

static bool IsCSourceName(const char *name)
{
    size_t n = strlen(name);
    return n >= 3 && strcmp(name + n - 2, ".c") == 0;
}

static void WalkExtTree(const char *dir, int depth, int *seen, int seen_max,
                        bool (*fn)(void *ctx, const char *path, time_t mtime), void *ctx)
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
        else if (S_ISREG(st.st_mode) && IsCSourceName(ent->d_name) &&
                 fn(ctx, path, st.st_mtime))
        {
            (*seen)++;
        }
    }
    closedir(d);
}

static bool LoadWalk(void *ctx, const char *path, time_t mtime)
{
    (void)mtime;
    LoadUserFile((PicoHost *)ctx, path);
    return true;
}

typedef struct WorkspaceLoadCtx {
    PicoHost *host;
    PicoWorkspace *workspace;
} WorkspaceLoadCtx;

static bool LoadWorkspaceWalk(void *ctx, const char *path, time_t mtime)
{
    WorkspaceLoadCtx *load = (WorkspaceLoadCtx *)ctx;
    if (!load || PicoHost_SourceWorkspace(load->host, path) != load->workspace)
    {
        return false;
    }
    return LoadWalk(load->host, path, mtime);
}

static void LoadBuiltins(PicoHost *app)
{
    for (size_t i = 0; i < sizeof(kBuiltins) / sizeof(kBuiltins[0]); i++)
    {
        LoadedPlugin *p = NewModuleSlot(app);
        if (!p)
        {
            break;
        }
        memset(p, 0, sizeof(*p));
        p->builtin = true;
        p->ext = kBuiltins[i]();
        p->generation = ++app->next_module_generation;
        p->desired = true;
        p->ref_count = 1; /* host module-store reference */
        ActivatePlugin(app, p);
    }
}

static void ShutdownRange(PicoHost *app, bool users_only)
{
    for (int i = app ? app->module_count - 1 : -1; i >= 0; i--)
    {
        LoadedPlugin *p = &app->modules[i];
        if (users_only && p->builtin)
        {
            continue;
        }
        ShutdownPlugin(app, p);
        if (!users_only && p->desired)
        {
            p->desired = false;
            PicoModule_Release(p);
        }
    }
}

void PicoPlugins_UnloadUser(PicoHost *app)
{
    if (PicoHost_ProcessRetired())
    {
        return;
    }
    ShutdownRange(app, true);
}

static void LoadUsers(PicoHost *app)
{
    if (app->safe_mode)
    {
        return;
    }
    char dir[4096];
    int seen = 0;
    if (ConfigExtDir(dir, sizeof(dir)))
    {
        MkdirP(dir);
        WalkExtTree(dir, 0, &seen, PICO_MAX_USER_PLUGINS, LoadWalk, app);
    }
    for (int w = 0; w < app->workspace_count; w++)
    {
        PicoWorkspace *workspace = app->workspaces[w];
        if (workspace && WorkspaceExtDir(workspace, dir, sizeof(dir)))
        {
            WorkspaceLoadCtx load = {.host = app, .workspace = workspace};
            WalkExtTree(dir, 0, &seen, PICO_MAX_USER_PLUGINS,
                        LoadWorkspaceWalk, &load);
        }
    }
}

static bool AddBuiltinCandidate(PicoHost *app, PicoExt ext)
{
    LoadedPlugin *p = NewModuleSlot(app);
    if (!p)
    {
        return false;
    }
    memset(p, 0, sizeof(*p));
    p->builtin = true;
    p->ext = ext;
    p->generation = ++app->next_module_generation;
    p->desired = true;
    p->ref_count = 1;
    return true;
}

static bool LoadCandidateUsers(PicoHost *app)
{
    if (app->safe_mode)
    {
        return true;
    }
    char paths[PICO_MAX_USER_PLUGINS][4096];
    time_t mtimes[PICO_MAX_USER_PLUGINS];
    uint64_t hashes[PICO_MAX_USER_PLUGINS];
    int n = CollectGlobalSources(paths, mtimes, hashes, PICO_MAX_USER_PLUGINS);
    (void)mtimes;
    (void)hashes;
    for (int i = 0; i < n; i++)
    {
        if (LoadUserCandidate(app, paths[i]) != 0)
        {
            return false;
        }
    }
    return true;
}

void PicoPlugins_Load(PicoHost *app)
{
    if (!app || app->terminal_shutdown || PicoHost_ProcessRetired())
    {
        return;
    }
    if (app->module_count == 0)
    {
        pico_clear_registrations(app);
        pico_ui_modal_reset(app);
        WarnClear(app);
        LoadBuiltins(app);
    }
    LoadUsers(app);
}

void PicoPlugins_InitWorkspace(PicoHost *app, PicoWorkspace *workspace)
{
    if (!app || !workspace || app->terminal_shutdown || PicoHost_ProcessRetired())
    {
        return;
    }
    if (app->module_count == 0)
    {
        PicoPlugins_Load(app);
        return;
    }
    for (int i = 0; i < app->module_count; i++)
    {
        LoadedPlugin *p = &app->modules[i];
        if (!p->desired || !p->ext.workspace_init)
        {
            continue;
        }
        if (!ModuleAppliesToWorkspace(app, workspace, p))
        {
            continue;
        }
        PicoWorkspaceExtensions_Activate(workspace, p);
    }
    if (!app->safe_mode)
    {
        char dir[4096];
        int seen = 0;
        if (WorkspaceExtDir(workspace, dir, sizeof(dir)))
        {
            WorkspaceLoadCtx load = {.host = app, .workspace = workspace};
            WalkExtTree(dir, 0, &seen, PICO_MAX_USER_PLUGINS,
                        LoadWorkspaceWalk, &load);
        }
    }
    if (!workspace->active_registration)
    {
        PicoWorkspace_PublishRegistrationGeneration(workspace);
    }
    PicoWorkspace_LoadProfiles(workspace);
}

void PicoPlugins_LoadWorkspaceSources(PicoHost *app, PicoWorkspace *workspace)
{
    char paths[PICO_MAX_USER_PLUGINS][4096];
    time_t mtimes[PICO_MAX_USER_PLUGINS];
    uint64_t hashes[PICO_MAX_USER_PLUGINS];
    char ws_ext_dir[4096];
    uint64_t old_module_generation;
    int n;
    int i;
    bool ok = true;

    if (!app || !workspace || app->safe_mode || app->terminal_shutdown || PicoHost_ProcessRetired())
    {
        return;
    }

    n = CollectWorkspaceSources(workspace, paths, mtimes, hashes, PICO_MAX_USER_PLUGINS);
    (void)mtimes;
    (void)hashes;
    old_module_generation = app->next_module_generation;
    for (i = 0; i < n && ok; i++)
    {
        if (LoadUserCandidate(app, paths[i]) != 0)
        {
            ok = false;
        }
    }
    if (!ok)
    {
        RollbackModuleCandidates(app, old_module_generation);
        return;
    }
    if (!WorkspaceExtDir(workspace, ws_ext_dir, sizeof(ws_ext_dir)))
    {
        return;
    }
    {
        for (i = 0; i < app->module_count; i++)
        {
            LoadedPlugin *p = &app->modules[i];
            if (p->generation <= old_module_generation && p->desired && !p->builtin &&
                SourceWorkspace(app, p->source) == workspace)
            {
                p->desired = false;
                PicoModule_Release(p);
            }
        }
    }
}

bool PicoPlugins_ReloadHost(PicoHost *app)
{
    if (!app || app->terminal_shutdown || PicoHost_ProcessRetired())
    {
        return false;
    }

    if (!ValidateUserSources(app))
    {
        return false;
    }

    uint64_t old_module_generation = app->next_module_generation;
    bool candidate_ok = true;

    for (size_t b = 0; candidate_ok && b < sizeof(kBuiltins) / sizeof(kBuiltins[0]); b++)
    {
        candidate_ok = AddBuiltinCandidate(app, kBuiltins[b]());
    }
    if (candidate_ok)
    {
        candidate_ok = LoadCandidateUsers(app);
    }

    if (!candidate_ok)
    {
        RollbackModuleCandidates(app, old_module_generation);
        return false;
    }

    for (int i = 0; i < app->module_count; i++)
    {
        if (app->modules[i].generation <= old_module_generation &&
            app->modules[i].desired &&
            !IsWorkspaceLocalSource(app, app->modules[i].source))
        {
            app->modules[i].desired = false;
            PicoModule_Release(&app->modules[i]);
        }
    }

    PicoHostExtensions_Reload(app);
    return true;
}

static void RequestWorkspaceRollout(PicoHost *host, PicoWorkspace *workspace)
{
    if (!host || !workspace ||
        pico_workspace_request_reload(host, workspace->id) != PICO_OK)
    {
        return;
    }
    if (!PicoWorkspace_BlocksReload(workspace))
    {
        (void)PicoWorkspace_Reload(workspace);
    }
}

void PicoPlugins_Reload(PicoHost *app)
{
    if (!PicoPlugins_ReloadHost(app))
    {
        return;
    }
    for (int i = 0; i < app->workspace_count; i++)
    {
        RequestWorkspaceRollout(app, app->workspaces[i]);
    }
}

void PicoPlugins_Shutdown(PicoHost *app)
{
    if (!app || app->terminal_shutdown || PicoHost_ProcessRetired())
    {
        return;
    }
    for (int w = 0; w < app->workspace_count; w++)
    {
        if (app->workspaces[w])
        {
            PicoWorkspaceExtensions_Shutdown(app->workspaces[w]);
        }
    }
    PicoHostExtensions_Shutdown(app);
    for (int i = 0; i < app->module_count; i++)
    {
        if (app->modules[i].desired)
        {
            app->modules[i].desired = false;
            PicoModule_Release(&app->modules[i]);
        }
    }
    pico_clear_registrations(app);
    for (int i = 0; i < app->module_count; i++)
    {
        assert(app->modules[i].ref_count == 0);
        assert(app->modules[i].handle == NULL);
    }
    if (app->modules && app->module_capacity > 0)
    {
        memset(app->modules, 0, (size_t)app->module_capacity * sizeof(*app->modules));
    }
    app->module_count = 0;
    pico_ui_modal_reset(app);
}

typedef struct CollectCtx {
    char (*paths)[4096];
    time_t *mtimes;
    uint64_t *hashes;
    int n;
    int cap;
} CollectCtx;

static bool CollectWalk(void *ctx, const char *path, time_t mtime)
{
    CollectCtx *c = (CollectCtx *)ctx;
    snprintf(c->paths[c->n], 4096, "%s", path);
    c->mtimes[c->n] = mtime;
    c->hashes[c->n] = SourceHash(path);
    return true;
}

typedef struct WorkspaceCollectCtx {
    CollectCtx collect;
    const PicoWorkspace *workspace;
} WorkspaceCollectCtx;

static bool CollectWorkspaceWalk(void *ctx, const char *path, time_t mtime)
{
    WorkspaceCollectCtx *collection = (WorkspaceCollectCtx *)ctx;
    if (!collection ||
        PicoHost_SourceWorkspace(collection->workspace->host, path) != collection->workspace)
    {
        return false;
    }
    return CollectWalk(&collection->collect, path, mtime);
}

static int CollectGlobalSources(char paths[][4096], time_t *mtimes,
                                uint64_t *hashes, int cap)
{
    CollectCtx ctx = {.paths = paths, .mtimes = mtimes, .hashes = hashes, .n = 0, .cap = cap};
    char dir[4096];
    if (ConfigExtDir(dir, sizeof(dir)))
    {
        WalkExtTree(dir, 0, &ctx.n, cap, CollectWalk, &ctx);
    }
    return ctx.n;
}

static int CollectWorkspaceSources(const PicoWorkspace *workspace, char paths[][4096],
                                   time_t *mtimes, uint64_t *hashes, int cap)
{
    WorkspaceCollectCtx ctx = {
        .collect = {.paths = paths, .mtimes = mtimes, .hashes = hashes, .n = 0, .cap = cap},
        .workspace = workspace,
    };
    char dir[4096];
    if (workspace && WorkspaceExtDir(workspace, dir, sizeof(dir)))
    {
        WalkExtTree(dir, 0, &ctx.collect.n, cap, CollectWorkspaceWalk, &ctx);
    }
    return ctx.collect.n;
}

static bool ValidateUserSources(PicoHost *app)
{
    char paths[PICO_MAX_USER_PLUGINS][4096];
    time_t mtimes[PICO_MAX_USER_PLUGINS];
    uint64_t hashes[PICO_MAX_USER_PLUGINS];
    int n = CollectGlobalSources(paths, mtimes, hashes, PICO_MAX_USER_PLUGINS);
    for (int i = 0; i < n; i++)
    {
        char so[4096];
        if (!SoPathFor(paths[i], mtimes[i], hashes[i], so, sizeof(so)))
        {
            pico_status_warn(app, "Extension cache path is too long.");
            return false;
        }
        if (access(so, R_OK) != 0)
        {
            char err[8192];
            if (CompileExt(paths[i], so, err, sizeof(err)) != 0)
            {
                char line[8700];
                snprintf(line, sizeof(line), "compile %.4000s:\n%.4000s", paths[i], err);
                pico_status_warn(app, line);
                return false;
            }
        }
        void *handle = dlopen(so, RTLD_NOW | RTLD_LOCAL);
        if (!handle)
        {
            char line[2048];
            snprintf(line, sizeof(line), "%.1000s: dlopen: %.1000s", paths[i], dlerror());
            pico_status_warn(app, line);
            return false;
        }
        PicoExt (*entry)(void) = (PicoExt(*)(void))dlsym(handle, "pico_ext");
        PicoExt ext;
        if (!entry)
        {
            pico_status_warn(app, "extension is missing pico_ext() during reload validation");
            dlclose(handle);
            return false;
        }
        ext = entry();
        if (ext.abi != PICO_EXT_ABI)
        {
            pico_status_warn(app, "extension ABI validation failed during reload");
            dlclose(handle);
            return false;
        }
        if (SourceWorkspace(app, paths[i]) &&
            (ext.host_init || ext.host_shutdown || ext.host_on_frame))
        {
            pico_status_warn(app, "workspace-local extension cannot have host callbacks");
            dlclose(handle);
            return false;
        }
        dlclose(handle);
    }
    return true;
}

static bool SourceSetChanged(const PicoHost *host, const PicoWorkspace *workspace)
{
    char paths[PICO_MAX_USER_PLUGINS][4096];
    time_t mtimes[PICO_MAX_USER_PLUGINS];
    uint64_t hashes[PICO_MAX_USER_PLUGINS];
    int count = workspace
                    ? CollectWorkspaceSources(workspace, paths, mtimes, hashes,
                                              PICO_MAX_USER_PLUGINS)
                    : CollectGlobalSources(paths, mtimes, hashes,
                                           PICO_MAX_USER_PLUGINS);
    int desired_count = 0;
    for (int i = 0; i < host->module_count; i++)
    {
        const LoadedPlugin *module = &host->modules[i];
        if (module->builtin || !module->desired)
        {
            continue;
        }
        PicoWorkspace *owner = SourceWorkspace(host, module->source);
        if ((workspace && owner == workspace) || (!workspace && !owner))
        {
            desired_count++;
        }
    }
    if (count != desired_count)
    {
        return true;
    }
    for (int i = 0; i < count; i++)
    {
        bool found = false;
        for (int m = 0; m < host->module_count; m++)
        {
            const LoadedPlugin *module = &host->modules[m];
            if (!module->builtin && module->desired &&
                strcmp(module->source, paths[i]) == 0)
            {
                found = true;
                if (module->mtime != mtimes[i] || module->content_hash != hashes[i])
                {
                    return true;
                }
                break;
            }
        }
        if (!found)
        {
            return true;
        }
    }
    return false;
}

void PicoPlugins_Poll(PicoHost *app)
{
    if (!app || app->terminal_shutdown || PicoHost_ProcessRetired() || app->reload_queued)
    {
        return;
    }
    if (app->safe_mode)
    {
        return;
    }
    double now = GetTime();
    if (now - app->plugin_last_poll < 0.5)
    {
        return;
    }
    app->plugin_last_poll = now;

    bool workspace_changed[PICO_MAX_WORKSPACES] = {0};
    bool global_changed = SourceSetChanged(app, NULL);
    for (int w = 0; w < app->workspace_count; w++)
    {
        workspace_changed[w] = app->workspaces[w] &&
                               SourceSetChanged(app, app->workspaces[w]);
    }

    bool global_ready = !global_changed;
    if (global_changed)
    {
        global_ready = PicoPlugins_ReloadHost(app);
        if (global_ready)
        {
            for (int w = 0; w < app->workspace_count; w++)
            {
                RequestWorkspaceRollout(app, app->workspaces[w]);
            }
        }
    }
    for (int w = 0; w < app->workspace_count; w++)
    {
        PicoWorkspace *workspace = app->workspaces[w];
        if (workspace && workspace_changed[w] && !(global_changed && global_ready))
        {
            RequestWorkspaceRollout(app, workspace);
        }
        else if (workspace && workspace->reload_queued &&
                 !PicoWorkspace_BlocksReload(workspace))
        {
            (void)PicoWorkspace_Reload(workspace);
        }
    }
}

void PicoPlugins_OnFrame(PicoHost *app, float dt)
{
    if (!app || app->terminal_shutdown || PicoHost_ProcessRetired())
    {
        return;
    }
    PicoHostExtensions_OnFrame(app, dt);
    for (int w = 0; w < app->workspace_count; w++)
    {
        if (app->workspaces[w])
        {
            PicoWorkspaceExtensions_OnFrame(app->workspaces[w], dt);
        }
    }
}

typedef struct PluginSlotRecord {
    const LoadedPlugin *module;
    PicoExtensionScope scope;
    PicoWorkspaceId workspace_id;
} PluginSlotRecord;

static bool PluginSlotMatchesModule(const PicoPluginSlot *slot,
                                    const LoadedPlugin *module)
{
    if (!slot || !module)
    {
        return false;
    }
    if (module->source[0])
    {
        return slot->source && strcmp(slot->source, module->source) == 0;
    }
    return !slot->source && slot->name[0] && module->ext.name &&
           strcmp(slot->name, module->ext.name) == 0;
}

static int EnumerateSlots(const PicoHost *host, PluginSlotRecord *records, int max_records)
{
    int count = 0;
    if (!host)
    {
        return 0;
    }
    for (int i = 0; i < host->module_count; i++)
    {
        const LoadedPlugin *p = &host->modules[i];
        if (!p->desired)
        {
            continue;
        }
        PicoWorkspace *local_owner = p->builtin ? NULL : SourceWorkspace(host, p->source);
        bool is_ws_local = local_owner != NULL;
        PicoWorkspaceId ws_local_id = local_owner ? local_owner->id : 0;

        bool has_host = !is_ws_local && (p->ext.host_init != NULL || p->ext.host_shutdown != NULL ||
                                         p->ext.host_on_frame != NULL || (p->builtin && !p->ext.workspace_init) ||
                                         (!p->builtin && p->handle == NULL));
        bool has_ws = p->ext.workspace_init != NULL || p->ext.workspace_shutdown != NULL ||
                      p->ext.workspace_on_frame != NULL || (p->builtin && p->ext.workspace_init) ||
                      (!p->builtin && p->handle == NULL);

        if (has_host)
        {
            if (records && count < max_records)
            {
                records[count].module = p;
                records[count].scope = PICO_EXTENSION_HOST;
                records[count].workspace_id = 0;
            }
            count++;
        }
        if (has_ws)
        {
            if (is_ws_local)
            {
                if (records && count < max_records)
                {
                    records[count].module = p;
                    records[count].scope = PICO_EXTENSION_WORKSPACE;
                    records[count].workspace_id = ws_local_id;
                }
                count++;
            }
            else
            {
                for (int w = 0; w < host->workspace_count; w++)
                {
                    PicoWorkspace *ws = host->workspaces[w];
                    if (!ws) continue;
                    if (records && count < max_records)
                    {
                        records[count].module = p;
                        records[count].scope = PICO_EXTENSION_WORKSPACE;
                        records[count].workspace_id = ws->id;
                    }
                    count++;
                }
                if (host->workspace_count == 0)
                {
                    if (records && count < max_records)
                    {
                        records[count].module = p;
                        records[count].scope = PICO_EXTENSION_WORKSPACE;
                        records[count].workspace_id = 0;
                    }
                    count++;
                }
            }
        }
    }
    return count;
}

int PicoPlugins_Count(const PicoHost *host)
{
    return EnumerateSlots(host, NULL, 0);
}

bool PicoPlugins_Get(const PicoHost *host, int index, PicoExtInfo *out)
{
    PluginSlotRecord records[PICO_MAX_MODULE_GENERATIONS * (PICO_MAX_WORKSPACES + 1)];
    int total = EnumerateSlots(host, records, (int)(sizeof(records) / sizeof(records[0])));
    if (!host || !out || index < 0 || index >= total)
    {
        return false;
    }
    const PluginSlotRecord *rec = &records[index];
    const LoadedPlugin *p = rec->module;
    out->name = p->ext.name;
    out->description = p->ext.description;
    out->source = (p->builtin || p->source[0] == '\0') ? NULL : p->source;
    out->builtin = p->builtin;
    out->loaded = p->builtin || p->handle != NULL;
    out->scope = rec->scope;
    out->workspace_id = rec->workspace_id;
    out->desired_generation = p->generation;
    out->last_error = NULL;
    if (rec->scope == PICO_EXTENSION_HOST)
    {
        const PicoPluginSlot *slot = NULL;
        for (int s = 0; s < host->host_plugin_count; s++)
        {
            if (PluginSlotMatchesModule(&host->host_plugins[s], p))
            {
                slot = &host->host_plugins[s];
                break;
            }
        }
        out->enabled = out->loaded && !PicoHost_ExtensionDisabled(host, p->ext.name);
        out->active_generation = (slot && slot->initialized) ? slot->active_generation : 0;
        out->last_error = (slot && slot->last_error[0]) ? slot->last_error : NULL;
    }
    else
    {
        PicoWorkspace *ws = PicoHost_FindWorkspace((PicoHost *)host, rec->workspace_id);
        if (!ws && host->workspace_count > 0)
        {
            ws = host->workspaces[0];
        }
        const PicoPluginSlot *slot = NULL;
        if (ws)
        {
            for (int s = 0; s < ws->workspace_plugin_count; s++)
            {
                if (PluginSlotMatchesModule(&ws->workspace_plugins[s], p))
                {
                    slot = &ws->workspace_plugins[s];
                    break;
                }
            }
        }
        out->enabled = out->loaded && (ws ? !PicoWorkspace_ExtensionDisabled(ws, p->ext.name) : false);
        out->active_generation = (slot && slot->initialized) ? slot->active_generation : 0;
        out->last_error = (slot && slot->last_error[0]) ? slot->last_error : NULL;
    }
    return true;
}

bool PicoPlugins_SetEnabled(PicoHost *app, int index, bool enabled)
{
    PicoExtInfo info;
    if (!app || !PicoPlugins_Get(app, index, &info))
    {
        return false;
    }
    if (!info.name || !info.name[0] || strcmp(info.name, "extensions") == 0 ||
        strcmp(info.name, "settings") == 0)
    {
        return false;
    }
    if (info.enabled == enabled)
    {
        return true;
    }
    bool ok = true;
    if (info.scope == PICO_EXTENSION_HOST)
    {
        ok = PicoHost_SetExtensionDisabled(app, info.name, !enabled);
        if (ok)
        {
            PicoHost_RequestHostReload(app);
        }
    }
    else
    {
        PicoWorkspace *ws = PicoHost_FindWorkspace(app, info.workspace_id);
        if (!ws && app->workspace_count > 0)
        {
            ws = app->workspaces[0];
        }
        if (ws)
        {
            ok = PicoWorkspace_SetExtensionDisabled(ws, info.name, !enabled);
            if (ok)
            {
                (void)PicoWorkspace_Reload(ws);
            }
        }
    }
    return ok;
}

void *PicoPlugins_HostState(const PicoHost *host, const char *name)
{
    return PicoHostExtensions_State(host, name);
}

void *PicoPlugins_WorkspaceState(const PicoWorkspace *workspace, const char *name)
{
    return PicoWorkspaceExtensions_State(workspace, name);
}
