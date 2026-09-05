#define _POSIX_C_SOURCE 200809L
#include "pico/plugin.h"
#include "agent.h"
#include "docs_path.h"
#include "workspace_internal.h"
#include "path.h"
#include "session.h"
#include "settings.h"
#include "host_internal.h"

#include <assert.h>
#include <signal.h>
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
#define PICO_VERSION "0.1.10"
#endif

#define PICO_MAX_USER_PLUGINS 32
#define PICO_EXT_WALK_DEPTH 8

typedef PicoModuleGeneration LoadedPlugin;

static int ModuleCapacity(const PicoHost *host)
{
    return host ? host->module_capacity : 0;
}

static bool ValidateUserSources(PicoHost *app, bool retry_compile_failures);
static int CollectGlobalSources(char paths[][4096], time_t *mtimes,
                                uint64_t *hashes, int cap);
static int CollectWorkspaceSources(const PicoWorkspace *workspace, char paths[][4096],
                                   time_t *mtimes, uint64_t *hashes, int cap);
static bool ConfigExtDir(char *out, size_t cap);
static bool WorkspaceExtDir(const PicoWorkspace *workspace, char *out, size_t cap);
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
    pico_ext_background,
    pico_ext_subagent,
    pico_ext_commands,
    pico_ext_files,
    pico_ext_skills,
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

/* Hash contents only when the file's stat generation changes. Dependency
 * polling otherwise rereads the entire SDK on every frame during a build. */
typedef struct FileHashEntry {
    char path[4096];
    struct stat stat;
    uint64_t hash;
} FileHashEntry;

static bool SameFileGeneration(const struct stat *a, const struct stat *b)
{
#ifdef __APPLE__
    struct timespec am = a->st_mtimespec, bm = b->st_mtimespec;
    struct timespec ac = a->st_ctimespec, bc = b->st_ctimespec;
#else
    struct timespec am = a->st_mtim, bm = b->st_mtim;
    struct timespec ac = a->st_ctim, bc = b->st_ctim;
#endif
    return a->st_dev == b->st_dev && a->st_ino == b->st_ino && a->st_size == b->st_size &&
           am.tv_sec == bm.tv_sec && am.tv_nsec == bm.tv_nsec &&
           ac.tv_sec == bc.tv_sec && ac.tv_nsec == bc.tv_nsec;
}

static uint64_t FileHash(const char *src)
{
    static FileHashEntry cache[128]; /* main-thread loader only */
    static unsigned next;
    struct stat st;
    if (stat(src, &st) != 0 || !S_ISREG(st.st_mode)) return 0;
    FileHashEntry *entry = NULL;
    for (size_t i = 0; i < sizeof(cache) / sizeof(cache[0]); i++)
    {
        if (strcmp(cache[i].path, src) == 0)
        {
            if (SameFileGeneration(&cache[i].stat, &st)) return cache[i].hash;
            entry = &cache[i];
            break;
        }
    }
    int fd = open(src, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0) return 0;
    if (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode))
    {
        close(fd);
        return 0;
    }
    unsigned char buffer[4096];
    uint64_t hash = 1469598103934665603ULL;
    ssize_t n;
    while ((n = read(fd, buffer, sizeof(buffer))) != 0)
    {
        if (n < 0)
        {
            if (errno == EINTR) continue;
            close(fd);
            return 0;
        }
        for (ssize_t i = 0; i < n; i++) hash = (hash ^ buffer[i]) * 1099511628211ULL;
    }
    struct stat after;
    bool stable = fstat(fd, &after) == 0 && SameFileGeneration(&st, &after);
    close(fd);
    if (stable)
    {
        if (!entry) entry = &cache[next++ % (sizeof(cache) / sizeof(cache[0]))];
        snprintf(entry->path, sizeof(entry->path), "%s", src);
        entry->stat = st;
        entry->hash = hash;
    }
    return hash;
}

static bool DependencyPath(const char *src, char *out, size_t cap)
{
    char cache[4096];
    return CacheDir(cache, sizeof(cache)) &&
           PicoPath_Format(out, cap, "%s/%08x.deps", cache, PathHash(src));
}

static uint64_t MixDependency(uint64_t hash, const char *path,
                              const struct timespec *started, bool *changed)
{
    if (started)
    {
        struct stat st;
        if (stat(path, &st) != 0) *changed = true;
        else
        {
#ifdef __APPLE__
            struct timespec ctime = st.st_ctimespec;
#else
            struct timespec ctime = st.st_ctim;
#endif
            if (ctime.tv_sec > started->tv_sec ||
                (ctime.tv_sec == started->tv_sec && ctime.tv_nsec > started->tv_nsec)) *changed = true;
        }
    }
    return (hash * 1099511628211ULL) ^ FileHash(path);
}

static uint64_t HashDependencies(const char *manifest, uint64_t hash,
                                 const struct timespec *started, bool *changed)
{
    char path[4096];
    FILE *file = fopen(manifest, "rb");
    if (!file)
    {
        if (changed) *changed = true;
        return hash;
    }
    /* GCC/Clang make dependencies: fixed target, escaped spaces/backslashes,
     * continued lines, and doubled dollars. */
    int ch;
    while ((ch = fgetc(file)) != EOF && ch != ':') {}
    size_t n = 0;
    bool overflow = false;
    while ((ch = fgetc(file)) != EOF)
    {
        if (ch == '\\')
        {
            ch = fgetc(file);
            if (ch == '\n') continue;
            if (ch == EOF) break;
        }
        else if (ch == ' ' || ch == '\t' || ch == '\r' || ch == '\n')
        {
            if (n && !overflow)
            {
                path[n] = '\0';
                hash = MixDependency(hash, path, started, changed);
            }
            n = 0;
            overflow = false;
            continue;
        }
        else if (ch == '$')
        {
            int next = fgetc(file);
            if (next != '$' && next != EOF) ungetc(next, file);
        }
        if (n + 1 < sizeof(path)) path[n++] = (char)ch;
        else overflow = true;
    }
    if (n && !overflow)
    {
        path[n] = '\0';
        hash = MixDependency(hash, path, started, changed);
    }
    fclose(file);
    return hash;
}

static uint64_t SourceHash(const char *src)
{
    char manifest[4096];
    uint64_t hash = FileHash(src);
    return DependencyPath(src, manifest, sizeof(manifest))
               ? HashDependencies(manifest, hash, NULL, NULL) : hash;
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
    return PicoPath_Format(out, cap, "%s/%08x-%s-%ld-%016llx-" PICO_VERSION "-%d-deps.so", cache,
                           PathHash(src), base, (long)mtime,
                           (unsigned long long)content_hash, PICO_EXT_ABI);
}

typedef struct PicoCompileJob {
    struct PicoCompileJob *next;
    bool done;
    pid_t pid;
    int fd;
    double deadline;
    bool timed_out;
    char source[4096];
    struct timespec started;
    char tmp[4096];
    char deps[4100];
    uint64_t source_hash;
    size_t error_len;
    char error[8192];
} PicoCompileJob;

static double CompileTime(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec / 1e9;
}

static void CompileJobFree(PicoHost *host, PicoCompileJob *job)
{
    PicoCompileJob **link = &host->plugin_compile;
    while (*link && *link != job) link = &(*link)->next;
    if (!*link) return;
    *link = job->next;
    close(job->fd);
    unlink(job->tmp);
    unlink(job->deps);
    free(job);
}

/* Drain even after diagnostics fill up: a verbose compiler must never block
 * waiting for a reader. Limit each pump so output cannot monopolize a frame. */
static bool CompileJobPoll(PicoCompileJob *job, int *status)
{
    char buf[4096];
    for (int i = 0; i < 16; i++)
    {
        ssize_t n = read(job->fd, buf, sizeof(buf));
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) break;
        size_t keep = sizeof(job->error) - 1 - job->error_len;
        if (keep > (size_t)n) keep = (size_t)n;
        memcpy(job->error + job->error_len, buf, keep);
        job->error_len += keep;
        job->error[job->error_len] = '\0';
    }
    if (!job->timed_out && CompileTime() >= job->deadline)
    {
        job->timed_out = true;
        kill(-job->pid, SIGKILL);
    }
    pid_t result = waitpid(job->pid, status, WNOHANG);
    if (result == 0 || (result < 0 && errno == EINTR)) return false;
    if (result < 0) *status = -1;
    return true;
}

/* Publish completed build artifacts independently of their requesting scope.
 * This lets a host reload and a workspace reload make progress concurrently.
 * Failed results stay until their requesting loader can report/quarantine them. */
static void AdvanceCompiles(PicoHost *host)
{
    for (PicoCompileJob *job = host->plugin_compile, *next; job; job = next)
    {
        next = job->next;
        struct stat st;
        char global[4096];
        bool global_source = ConfigExtDir(global, sizeof(global)) &&
                             strncmp(job->source, global, strlen(global)) == 0 &&
                             job->source[strlen(global)] == '/';
        bool wanted = (global_source || SourceWorkspace(host, job->source)) &&
                      stat(job->source, &st) == 0 && job->source_hash == SourceHash(job->source);
        if (!wanted && !job->done) kill(-job->pid, SIGKILL);
        int status = 0;
        if (!job->done && !CompileJobPoll(job, &status)) continue;
        if (!wanted)
        {
            CompileJobFree(host, job);
            continue;
        }
        if (job->done) continue;
        job->done = true;
        bool ok = !job->timed_out && status >= 0 && WIFEXITED(status) && WEXITSTATUS(status) == 0;
        if (ok)
        {
            bool changed = false;
            (void)HashDependencies(job->deps, 0, &job->started, &changed);
            if (changed)
            {
                CompileJobFree(host, job);
                continue;
            }
            char deps[4096], so[4096];
            ok = DependencyPath(job->source, deps, sizeof(deps)) && rename(job->deps, deps) == 0 &&
                 SoPathFor(job->source, st.st_mtime, SourceHash(job->source), so, sizeof(so)) &&
                 rename(job->tmp, so) == 0;
            if (ok)
            {
                CompileJobFree(host, job);
                continue;
            }
            snprintf(job->error, sizeof(job->error), "could not publish compiled extension: %s", strerror(errno));
            job->source_hash = SourceHash(job->source);
        }
        else
        {
            /* Syntax failures can still emit a complete dependency list. Keep
             * it so fixing an included header retries the failed generation. */
            char deps[4096];
            if (DependencyPath(job->source, deps, sizeof(deps)) && rename(job->deps, deps) == 0)
                job->source_hash = SourceHash(job->source);
        }
    }
}

/* 0 ready, -1 failed, -2 compiling. No compiler work blocks the UI thread. */
static int CompileExt(PicoHost *host, const char *src, char *so, char *err, size_t err_cap)
{
    host->plugin_compile_pending = false;
    AdvanceCompiles(host);
    struct stat st;
    if (stat(src, &st) != 0 || !SoPathFor(src, st.st_mtime, SourceHash(src), so, 4096)) return -1;
    if (access(so, R_OK) == 0) return 0;
    bool running = false;
    for (PicoCompileJob *existing = host->plugin_compile; existing; existing = existing->next)
    {
        if (strcmp(existing->source, src) == 0 && existing->done)
        {
            snprintf(err, err_cap, "%.3000s: %s%.4000s", src,
                     existing->timed_out ? "compiler timed out" : "compile failed: ",
                     existing->timed_out ? "" : existing->error);
            CompileJobFree(host, existing);
            return -1;
        }
        running |= !existing->done;
    }
    if (running)
    {
        host->plugin_compile_pending = true;
        return -2;
    }
    PicoCompileJob *job;
    char cache[4096], sdk_include[4096], sdk_flag[4112], srcdir_flag[4112];
    const char *compiler = getenv("PICO_CC");
    if (!compiler || !compiler[0]) compiler = PICO_CC;
    if (!Pico_SdkIncludeDir(sdk_include, sizeof(sdk_include)) ||
        access(sdk_include, R_OK) != 0 || !CacheDir(cache, sizeof(cache)) || MkdirP(cache) != 0)
    {
        snprintf(err, err_cap, "%s: extension SDK or cache is unavailable", src);
        return -1;
    }
    job = calloc(1, sizeof(*job));
    if (!job) return -1;
    snprintf(job->source, sizeof(job->source), "%s", src);
    clock_gettime(CLOCK_REALTIME, &job->started);
    job->source_hash = SourceHash(src);
    if (!PicoPath_Format(job->tmp, sizeof(job->tmp), "%s.tmp-XXXXXX", so))
    {
        free(job);
        return -1;
    }
    int tmp_fd = mkstemp(job->tmp);
    if (tmp_fd < 0) { free(job); return -1; }
    close(tmp_fd);
    snprintf(job->deps, sizeof(job->deps), "%s.d", job->tmp);
    snprintf(sdk_flag, sizeof(sdk_flag), "-I%s", sdk_include);
    char dir[4096];
    snprintf(dir, sizeof(dir), "%s", src);
    char *slash = strrchr(dir, '/');
    if (slash) *slash = '\0';
    else snprintf(dir, sizeof(dir), ".");
    snprintf(srcdir_flag, sizeof(srcdir_flag), "-I%s", dir);
    int fds[2];
    if (pipe(fds) != 0)
    {
        unlink(job->tmp);
        free(job);
        return -1;
    }
    fcntl(fds[0], F_SETFD, FD_CLOEXEC);
    fcntl(fds[1], F_SETFD, FD_CLOEXEC);
    fcntl(fds[0], F_SETFL, O_NONBLOCK);
    pid_t pid = fork();
    if (pid == 0)
    {
        setpgid(0, 0);
        dup2(fds[1], STDOUT_FILENO);
        dup2(fds[1], STDERR_FILENO);
        close(fds[0]);
        close(fds[1]);
        char *args[] = {(char *)compiler, "-shared", "-fPIC", "-std=c99",
                        sdk_flag, srcdir_flag, "-MMD", "-MF", job->deps, "-MT", "pico",
                        "-o", job->tmp, (char *)src, NULL};
        execvp(compiler, args);
        _exit(127);
    }
    close(fds[1]);
    if (pid < 0)
    {
        close(fds[0]);
        unlink(job->tmp);
        free(job);
        return -1;
    }
    setpgid(pid, pid);
    job->pid = pid;
    job->fd = fds[0];
    job->deadline = CompileTime() + 30.0;
    job->next = host->plugin_compile;
    host->plugin_compile = job;
    host->plugin_compile_pending = true;
    return -2;
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
        if (module->generation > generation_floor && module->desired &&
            !module->compile_failed)
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
    LoadedPlugin *stub = NULL;
    if (!app || !src)
    {
        return NULL;
    }
    for (int i = 0; i < app->module_count; i++)
    {
        LoadedPlugin *p = &app->modules[i];
        if (!p->builtin && p->desired && strcmp(p->source, src) == 0)
        {
            if (p->handle)
            {
                return p;
            }
            if (!stub)
            {
                stub = p;
            }
        }
    }
    return stub;
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

static bool CompileFailureMatches(const LoadedPlugin *module, const char *src,
                                  time_t mtime, uint64_t content_hash)
{
    return module && module->compile_failed && src &&
           strcmp(module->source, src) == 0 && module->mtime == mtime &&
           module->content_hash == content_hash;
}

static bool PreserveCompileFailure(const PicoHost *app, const LoadedPlugin *module,
                                   uint64_t generation_floor,
                                   bool retry_compile_failures)
{
    struct stat st;
    if (!app || !module || retry_compile_failures || !module->compile_failed ||
        !module->source[0] || stat(module->source, &st) != 0)
    {
        return false;
    }
    for (int i = 0; i < app->module_count; i++)
    {
        const LoadedPlugin *candidate = &app->modules[i];
        if (candidate->generation > generation_floor && candidate->desired &&
            strcmp(candidate->source, module->source) == 0)
        {
            return false;
        }
    }
    return true;
}

static void RememberCompileFailure(PicoHost *app, const char *src, time_t mtime,
                                   uint64_t content_hash)
{
    LoadedPlugin *current;
    if (!app || !src)
    {
        return;
    }
    current = FindDesiredSource(app, src);
    if (!current)
    {
        RecordStub(app, src, mtime, NULL);
        current = FindDesiredSource(app, src);
    }
    if (!current)
    {
        return;
    }
    current->mtime = mtime;
    (void)content_hash;
    current->content_hash = SourceHash(src);
    current->compile_failed = true;
    RetireSourceGeneration(app, src, current);
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
        char err[8192] = {0};
        if (CompileExt(app, src, so, err, sizeof(err)) != 0)
        {
            if (app->plugin_compile_pending) return;
            char line[8700];
            snprintf(line, sizeof(line), "compile %s:\n%s", src, err);
            pico_status_warn(app, line);
            RememberCompileFailure(app, src, st.st_mtime, content_hash);
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

static int LoadUserCandidate(PicoHost *app, const char *src,
                             bool retry_compile_failures)
{
    struct stat st;
    char so[4096];
    uint64_t content_hash;
    LoadedPlugin *current;
    if (stat(src, &st) != 0)
    {
        return -1;
    }
    content_hash = SourceHash(src);
    current = FindDesiredSource(app, src);
    if (!retry_compile_failures &&
        CompileFailureMatches(current, src, st.st_mtime, content_hash))
    {
        return 0;
    }
    if (!SoPathFor(src, st.st_mtime, content_hash, so, sizeof(so)))
    {
        return -1;
    }
    if (access(so, R_OK) != 0)
    {
        char err[8192] = {0};
        if (CompileExt(app, src, so, err, sizeof(err)) != 0)
        {
            if (app->plugin_compile_pending) return -2;
            pico_status_warn(app, err);
            RememberCompileFailure(app, src, st.st_mtime, content_hash);
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

static bool LoadCandidateUsers(PicoHost *app, bool retry_compile_failures)
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
        if (LoadUserCandidate(app, paths[i], retry_compile_failures) != 0)
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
    app->plugin_compile_pending = false;
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

bool PicoPlugins_LoadWorkspaceSources(PicoHost *app, PicoWorkspace *workspace)
{
    char paths[PICO_MAX_USER_PLUGINS][4096];
    time_t mtimes[PICO_MAX_USER_PLUGINS];
    uint64_t hashes[PICO_MAX_USER_PLUGINS];
    char ws_ext_dir[4096];
    uint64_t old_module_generation;
    int n;
    int i;
    bool retry_compile_failures;
    bool ok = true;

    if (app) app->plugin_compile_pending = false;
    if (app && workspace && app->safe_mode) return true;
    if (!app || !workspace || app->safe_mode || app->terminal_shutdown || PicoHost_ProcessRetired())
    {
        return false;
    }

    n = CollectWorkspaceSources(workspace, paths, mtimes, hashes, PICO_MAX_USER_PLUGINS);
    (void)mtimes;
    (void)hashes;
    retry_compile_failures = workspace->reload_retry_compile_failures;
    app->plugin_compile_pending = false;
    for (i = 0; i < n; i++)
    {
        LoadedPlugin *current = FindDesiredSource(app, paths[i]);
        if (!retry_compile_failures && CompileFailureMatches(current, paths[i], mtimes[i], hashes[i])) continue;
        char so[4096], err[8192] = {0};
        if (!SoPathFor(paths[i], mtimes[i], hashes[i], so, sizeof(so))) return false;
        if (access(so, R_OK) != 0 && CompileExt(app, paths[i], so, err, sizeof(err)) != 0)
        {
            if (!app->plugin_compile_pending)
            {
                pico_status_warn(app, err);
                RememberCompileFailure(app, paths[i], mtimes[i], hashes[i]);
            }
            return false;
        }
    }
    old_module_generation = app->next_module_generation;
    for (i = 0; i < n && ok; i++)
    {
        if (LoadUserCandidate(app, paths[i], retry_compile_failures) != 0)
        {
            ok = false;
        }
    }
    if (!ok)
    {
        RollbackModuleCandidates(app, old_module_generation);
        return false;
    }
    if (!WorkspaceExtDir(workspace, ws_ext_dir, sizeof(ws_ext_dir)))
    {
        return false;
    }
    {
        for (i = 0; i < app->module_count; i++)
        {
            LoadedPlugin *p = &app->modules[i];
            if (p->generation <= old_module_generation && p->desired && !p->builtin &&
                SourceWorkspace(app, p->source) == workspace &&
                !PreserveCompileFailure(app, p, old_module_generation,
                                        retry_compile_failures))
            {
                p->desired = false;
                PicoModule_Release(p);
            }
        }
    }
    return true;
}

static bool ReloadHostWithPolicy(PicoHost *app, bool retry_compile_failures)
{
    if (!app || app->terminal_shutdown || PicoHost_ProcessRetired())
    {
        return false;
    }

    app->plugin_compile_pending = false;
    app->plugin_reload_retry = retry_compile_failures;
    if (!ValidateUserSources(app, retry_compile_failures))
    {
        app->plugin_reload_pending = app->plugin_compile_pending;
        return false;
    }
    app->plugin_reload_pending = false;

    uint64_t old_module_generation = app->next_module_generation;
    bool candidate_ok = true;

    for (size_t b = 0; candidate_ok && b < sizeof(kBuiltins) / sizeof(kBuiltins[0]); b++)
    {
        candidate_ok = AddBuiltinCandidate(app, kBuiltins[b]());
    }
    if (candidate_ok)
    {
        candidate_ok = LoadCandidateUsers(app, retry_compile_failures);
    }

    if (!candidate_ok)
    {
        RollbackModuleCandidates(app, old_module_generation);
        return false;
    }

    for (int i = 0; i < app->module_count; i++)
    {
        LoadedPlugin *module = &app->modules[i];
        if (module->generation <= old_module_generation && module->desired &&
            !IsWorkspaceLocalSource(app, module->source) &&
            !PreserveCompileFailure(app, module, old_module_generation,
                                    retry_compile_failures))
        {
            module->desired = false;
            PicoModule_Release(module);
        }
    }

    PicoHostExtensions_Reload(app);
    return true;
}

bool PicoPlugins_ReloadHost(PicoHost *app)
{
    return ReloadHostWithPolicy(app, true);
}

static void RequestWorkspaceRollout(PicoHost *host, PicoWorkspace *workspace,
                                    bool retry_compile_failures)
{
    if (PicoWorkspace_RequestReload(host, workspace, retry_compile_failures) != PICO_OK)
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
        app->plugin_rollout_pending = app->plugin_reload_pending;
        return;
    }
    for (int i = 0; i < app->workspace_count; i++)
    {
        RequestWorkspaceRollout(app, app->workspaces[i], true);
    }
}

static void *ReapCompiler(void *arg)
{
    pid_t pid = (pid_t)(intptr_t)arg;
    while (waitpid(pid, NULL, 0) < 0 && errno == EINTR) {}
    return NULL;
}

void PicoPlugins_CancelCompiles(PicoHost *app)
{
    while (app && app->plugin_compile)
    {
        PicoCompileJob *job = app->plugin_compile;
        if (!job->done)
        {
            kill(-job->pid, SIGKILL);
            pthread_t reaper;
            if (pthread_create(&reaper, NULL, ReapCompiler, (void *)(intptr_t)job->pid) == 0)
                pthread_detach(reaper);
        }
        CompileJobFree(app, job);
    }
    if (app)
    {
        app->plugin_compile_pending = false;
        app->plugin_reload_pending = false;
        app->plugin_rollout_pending = false;
    }
}

void PicoPlugins_Shutdown(PicoHost *app)
{
    PicoPlugins_CancelCompiles(app);
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
    CollectCtx ctx = {.paths = paths, .mtimes = mtimes, .hashes = hashes, .n = 0};
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
        .collect = {.paths = paths, .mtimes = mtimes, .hashes = hashes, .n = 0},
        .workspace = workspace,
    };
    char dir[4096];
    if (workspace && WorkspaceExtDir(workspace, dir, sizeof(dir)))
    {
        WalkExtTree(dir, 0, &ctx.collect.n, cap, CollectWorkspaceWalk, &ctx);
    }
    return ctx.collect.n;
}

static bool ValidateUserSources(PicoHost *app, bool retry_compile_failures)
{
    if (app->safe_mode) return true;
    char paths[PICO_MAX_USER_PLUGINS][4096];
    time_t mtimes[PICO_MAX_USER_PLUGINS];
    uint64_t hashes[PICO_MAX_USER_PLUGINS];
    int n = CollectGlobalSources(paths, mtimes, hashes, PICO_MAX_USER_PLUGINS);
    for (int i = 0; i < n; i++)
    {
        char so[4096];
        LoadedPlugin *current = FindDesiredSource(app, paths[i]);
        if (!retry_compile_failures &&
            CompileFailureMatches(current, paths[i], mtimes[i], hashes[i]))
        {
            continue;
        }
        if (!SoPathFor(paths[i], mtimes[i], hashes[i], so, sizeof(so)))
        {
            pico_status_warn(app, "Extension cache path is too long.");
            return false;
        }
        if (access(so, R_OK) != 0)
        {
            char err[8192] = {0};
            if (CompileExt(app, paths[i], so, err, sizeof(err)) != 0)
            {
                if (app->plugin_compile_pending) return false;
                char line[8700];
                snprintf(line, sizeof(line), "compile %.4000s:\n%.4000s", paths[i], err);
                pico_status_warn(app, line);
                RememberCompileFailure(app, paths[i], mtimes[i], hashes[i]);
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
    bool had_compile = app->plugin_compile != NULL;
    AdvanceCompiles(app);
    double now = GetTime();
    if (!had_compile && !app->plugin_compile && !app->plugin_reload_pending && now - app->plugin_last_poll < 0.5)
    {
        return;
    }
    app->plugin_last_poll = now;

    bool workspace_changed[PICO_MAX_WORKSPACES] = {0};
    bool explicit_reload = app->plugin_reload_pending;
    bool global_changed = explicit_reload || SourceSetChanged(app, NULL);
    for (int w = 0; w < app->workspace_count; w++)
    {
        workspace_changed[w] = app->workspaces[w] &&
                               SourceSetChanged(app, app->workspaces[w]);
    }

    bool global_ready = !global_changed;
    if (global_changed)
    {
        global_ready = ReloadHostWithPolicy(app, explicit_reload && app->plugin_reload_retry);
        if (global_ready && (!explicit_reload || !app->plugin_reload_retry || app->plugin_rollout_pending))
        {
            for (int w = 0; w < app->workspace_count; w++)
            {
                RequestWorkspaceRollout(app, app->workspaces[w], app->plugin_rollout_pending);

            }
        }
    }
    if (global_ready) app->plugin_rollout_pending = false;
    for (int w = 0; w < app->workspace_count; w++)
    {
        PicoWorkspace *workspace = app->workspaces[w];
        if (workspace && workspace_changed[w] && !(global_changed && global_ready))
        {
            RequestWorkspaceRollout(app, workspace, false);
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
