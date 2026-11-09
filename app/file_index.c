#define _GNU_SOURCE
#include "file_index.h"
#include "file_mention.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <spawn.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#ifdef __linux__
#include <sys/inotify.h>
#endif

/* A memory safety budget, not a truncated searchable corpus. Exhaustion is an
 * explicit error and leaves the last usable generation intact. */
#define INDEX_BYTES (512u * 1024u * 1024u)
#define RECONCILE_SECONDS 30.0
#define REFRESH_DEBOUNCE 0.25

typedef struct Snapshot {
    atomic_uint refs;
    char *arena;
    size_t used, capacity, count, pointer_capacity;
    char **paths, **names;
    uint64_t generation;
    bool cached, ignore_aware;
    char status[256];
} Snapshot;

struct PicoFileIndex {
    pthread_mutex_t mu;
    pthread_cond_t changed;
    atomic_bool stop;
    atomic_uint refs;
    atomic_uint_fast64_t request;
    char root[4096], cache[4096], cache_relative[4096];
    uint64_t token;
    char query[512];
    bool requested, refresh, failed;
    Snapshot *latest;
    PicoFileIndexResult result;
    PicoFileIndexInfo info;
};

static double Now(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec + t.tv_nsec / 1e9;
}
static void Release(Snapshot *s)
{
    if (s && atomic_fetch_sub(&s->refs, 1) == 1)
    { free(s->paths); free(s->names); free(s->arena); free(s); }
}
static Snapshot *NewSnapshot(void)
{
    Snapshot *s = calloc(1, sizeof(*s));
    if (s) atomic_init(&s->refs, 1);
    return s;
}
static void Retain(Snapshot *s) { if (s) atomic_fetch_add(&s->refs, 1); }
static void ServiceRelease(PicoFileIndex *i)
{
    if (atomic_fetch_sub(&i->refs, 1) != 1) return;
    Release(i->latest);
    pthread_cond_destroy(&i->changed);
    pthread_mutex_destroy(&i->mu);
    free(i);
}
static bool Append(Snapshot *s, const char *p, size_t n)
{
    if (n > INDEX_BYTES - s->used) return false;
    if (s->used + n > s->capacity)
    {
        size_t cap = s->capacity ? s->capacity * 2 : 65536;
        if (cap < s->used + n) cap = s->used + n;
        if (cap > INDEX_BYTES) cap = INDEX_BYTES;
        char *next = realloc(s->arena, cap);
        if (!next) return false;
        s->arena = next; s->capacity = cap;
    }
    memcpy(s->arena + s->used, p, n); s->used += n;
    return true;
}
static const char *Base(const char *p)
{ const char *s = strrchr(p, '/'); return s ? s + 1 : p; }
static int Fold(int c) { return c >= 'A' && c <= 'Z' ? c + 'a' - 'A' : c; }
static int CompareFold(const char *a, const char *b)
{
    while (*a && Fold((unsigned char)*a) == Fold((unsigned char)*b)) { a++; b++; }
    return Fold((unsigned char)*a) - Fold((unsigned char)*b);
}
static int PathCompare(const void *a, const void *b)
{
    const char *x = *(char *const *)a, *y = *(char *const *)b;
    int r = CompareFold(x, y); return r ? r : strcmp(x, y);
}
static int NameCompare(const void *a, const void *b)
{
    const char *x = *(char *const *)a, *y = *(char *const *)b;
    int r = CompareFold(Base(x), Base(y)); return r ? r : PathCompare(a, b);
}
static bool SafeRelative(const char *p)
{
    if (!*p || *p == '/') return false;
    for (const char *s = p; *s; )
    {
        const char *end = strchr(s, '/'); size_t n = end ? (size_t)(end-s) : strlen(s);
        if (!n || (n == 1 && s[0] == '.') || (n == 2 && s[0] == '.' && s[1] == '.')) return false;
        if (!end) break;
        s = end+1;
    }
    return true;
}
static bool InternalPath(PicoFileIndex *i, const char *path)
{
    size_t cachelen = strlen(i->cache_relative);
    if (cachelen && !strncmp(path,i->cache_relative,cachelen) && (!path[cachelen] || path[cachelen] == '/')) return true;
    for (const char *part = path; part; )
    {
        const char *end = strchr(part,'/'); size_t len = end ? (size_t)(end-part) : strlen(part);
        if ((len == 4 && !memcmp(part,".git",4)) || (len == 5 && !memcmp(part,".pico",5))) return true;
        part = end ? end+1 : NULL;
    }
    return false;
}

/* Native traversal never follows directory symlinks, including the explicitly
 * addressed scope. External enumerators do not recurse directory symlinks. */
static int ScopeDirectory(PicoFileIndex *i, const char *rel)
{
    if (!SafeRelative(rel)) return false;
    if (InternalPath(i,rel)) return 0;
    char path[4096]; int n = snprintf(path, sizeof(path), "%s/%s", i->root, rel);
    if (n < 0 || (size_t)n >= sizeof(path)) return false;
    for (char *p = path + strlen(i->root) + 1; ; p++)
    {
        if (*p && *p != '/') continue;
        char save = *p; *p = 0; struct stat st;
        int ok = 1;
        if (lstat(path, &st)) ok = errno == ENOENT || errno == ENOTDIR ? 0 : -1;
        else if (!S_ISDIR(st.st_mode)) ok = 0;
        char marker[4102];
        if (ok == 1)
        {
            int n = snprintf(marker,sizeof(marker),"%s/.git",path);
            if (n < 0 || n >= (int)sizeof(marker)) ok = -1;
            else if (!lstat(marker,&st)) ok = 0;
            else if (errno != ENOENT && errno != ENOTDIR) ok = -1;
        }
        *p = save;
        if (ok != 1) return ok;
        if (!save) break;
    }
    return true;
}
static bool Cancelled(PicoFileIndex *i, uint64_t serial)
{ return atomic_load(&i->stop) || (serial && atomic_load(&i->request) != serial); }

/* Iterative traversal: deep trees cannot exhaust a worker stack. */
static bool Walk(PicoFileIndex *i, Snapshot *s, const char *rel, uint64_t serial)
{
    char **dirs = malloc(sizeof(char *));
    if (!dirs) return false;
    size_t count = 1, cap = 1, queued = strlen(rel)+1;
    dirs[0] = strdup(rel);
    bool ok = dirs[0] != NULL;
    while (ok && count && !Cancelled(i, serial))
    {
        char *dir = dirs[--count]; queued -= strlen(dir)+1;
        char full[8192];
        int n = snprintf(full, sizeof(full), "%s%s%s", i->root, *dir ? "/" : "", dir);
        DIR *d = n >= 0 && n < (int)sizeof(full) ? opendir(full) : NULL;
        if (!d) { free(dir); ok = false; break; }
        struct dirent *e;
        while (ok && !Cancelled(i, serial) && (e = readdir(d)))
        {
            if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..") ||
                !strcmp(e->d_name, ".git") || !strcmp(e->d_name, ".pico")) continue;
            char child[4096];
            n = snprintf(child, sizeof(child), "%s%s%s", dir, *dir ? "/" : "", e->d_name);
            if (n < 0 || n >= (int)sizeof(child) ||
                snprintf(full, sizeof(full), "%s/%s", i->root, child) >= (int)sizeof(full)) { ok = false; break; }
            if (InternalPath(i,child)) continue;
            struct stat st;
            if (lstat(full, &st)) { if (errno != ENOENT) ok = false; continue; }
            if (S_ISDIR(st.st_mode))
            {
                char marker[8200]; snprintf(marker, sizeof(marker), "%s/.git", full);
                if (lstat(marker, &st) == 0) continue;
                if (count == cap)
                {
                    size_t nextcap = cap*2;
                    char **next = nextcap <= INDEX_BYTES/sizeof(char *) ? realloc(dirs, nextcap*sizeof(char *)) : NULL;
                    if (!next) { ok = false; break; }
                    dirs = next; cap = nextcap;
                }
                if (s->capacity + queued + cap*sizeof(char *) + (size_t)n+1 > INDEX_BYTES) { ok = false; break; }
                dirs[count] = strdup(child);
                if (!dirs[count]) { ok = false; break; }
                count++; queued += (size_t)n+1;
            }
            else if (S_ISREG(st.st_mode))
            {
                ok = Append(s, child, (size_t)n+1);
                if (s->capacity + queued + cap*sizeof(char *) > INDEX_BYTES) ok = false;
            }
        }
        closedir(d); free(dir);
    }
    for (size_t n = 0; n < count; n++) free(dirs[n]);
    free(dirs);
    return ok && !Cancelled(i, serial);
}

/* Stream NUL output; never invoke a shell, change process cwd, or buffer an
 * unbounded subprocess result. Cancellation kills/reaps the process group. */
static int Enumerate(PicoFileIndex *i, Snapshot *s, char *const argv[], uint64_t serial)
{
    int fds[2];
#ifdef __linux__
    if (pipe2(fds, O_CLOEXEC)) return -1;
#else
    if (pipe(fds)) return -1;
    fcntl(fds[0], F_SETFD, FD_CLOEXEC); fcntl(fds[1], F_SETFD, FD_CLOEXEC);
#endif
    posix_spawn_file_actions_t actions; posix_spawnattr_t attr;
    if (posix_spawn_file_actions_init(&actions)) { close(fds[0]); close(fds[1]); return -1; }
    if (posix_spawnattr_init(&attr)) { posix_spawn_file_actions_destroy(&actions); close(fds[0]); close(fds[1]); return -1; }
    posix_spawn_file_actions_adddup2(&actions, fds[1], STDOUT_FILENO);
    posix_spawn_file_actions_addopen(&actions, STDERR_FILENO, "/dev/null", O_WRONLY, 0);
    posix_spawn_file_actions_addclose(&actions, fds[0]);
    posix_spawn_file_actions_addclose(&actions, fds[1]);
    posix_spawnattr_setflags(&attr, POSIX_SPAWN_SETPGROUP);
    posix_spawnattr_setpgroup(&attr, 0);
    extern char **environ; pid_t pid;
    int rc = posix_spawnp(&pid, argv[0], &actions, &attr, argv, environ);
    posix_spawn_file_actions_destroy(&actions); posix_spawnattr_destroy(&attr);
    close(fds[1]);
    if (rc) { close(fds[0]); return -1; }
    fcntl(fds[0], F_SETFL, O_NONBLOCK);
    bool ok = true, eof = false, reaped = false, killed = false;
    int status = 0;
    while (!eof || !reaped)
    {
        if ((!ok || Cancelled(i, serial)) && !killed) { kill(-pid, SIGKILL); killed = true; }
        char buf[65536]; ssize_t n;
        unsigned drained = 0;
        while ((n = read(fds[0], buf, sizeof(buf))) > 0)
        {
            if (ok && !Append(s, buf, (size_t)n)) ok = false;
            if (!ok || Cancelled(i, serial) || ++drained >= 16) break;
        }
        if (!n) eof = true;
        else if (n < 0 && errno != EAGAIN && errno != EINTR) { ok = false; eof = true; }
        if (!reaped) { pid_t w = waitpid(pid, &status, WNOHANG); reaped = w == pid || (w < 0 && errno != EINTR); }
        if (!eof || !reaped) { struct pollfd fd = {eof ? -1 : fds[0], POLLIN, 0}; poll(&fd, 1, eof ? 1 : 20); }
    }
    close(fds[0]);
    if (!ok || killed) return -2;
    return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

static bool Sort(PicoFileIndex *i, char **paths, char **scratch, size_t count,
                 int (*compare)(const void *, const void *), uint64_t serial)
{
    char **src = paths, **dst = scratch;
    for (size_t width = 1; width < count; width *= 2)
    {
        for (size_t from = 0; from < count; from += 2*width)
        {
            size_t mid = from+width < count ? from+width : count;
            size_t end = mid+width < count ? mid+width : count;
            size_t a = from, b = mid;
            for (size_t p = from; p < end; p++)
            {
                if (!(p & 1023) && Cancelled(i, serial)) return false;
                dst[p] = b == end || (a < mid && compare(&src[a], &src[b]) <= 0) ? src[a++] : src[b++];
            }
        }
        char **swap = src; src = dst; dst = swap;
    }
    if (src != paths) memcpy(paths, src, count*sizeof(char *));
    return !Cancelled(i, serial);
}

static bool Finish(PicoFileIndex *i, Snapshot *s, bool absolute, bool validate, uint64_t serial)
{
    if (s->used && s->arena[s->used-1]) return false;
    size_t count = 0;
    for (size_t p = 0; p < s->used; p += strlen(s->arena+p)+1)
    { if (!(count & 1023) && Cancelled(i, serial)) return false; count++; }
    /* Include actual arena capacity, both retained pointer arrays and sorting
     * scratch in the per-snapshot construction budget. */
    size_t alloc_count = count ? count : 1;
    if (alloc_count > (INDEX_BYTES - s->capacity) / (3 * sizeof(char *))) return false;
    s->pointer_capacity = alloc_count;
    s->paths = malloc(alloc_count * sizeof(char *));
    s->names = malloc(alloc_count * sizeof(char *));
    if (!s->paths || !s->names) return false;
    size_t rootlen = strlen(i->root);
    char last_parent[4096] = ""; int parent_ok = 1;
    for (size_t p = 0; p < s->used; )
    {
        char *path = s->arena+p; p += strlen(path)+1;
        if (Cancelled(i, serial)) return false;
        if (absolute)
        { if (strncmp(path, i->root, rootlen) || path[rootlen] != '/') return false; path += rootlen+1; }
        if (!SafeRelative(path)) return false;
        if (InternalPath(i,path)) continue;
        if (validate)
        {
            const char *slash = strrchr(path, '/');
            if (slash)
            {
                size_t len = (size_t)(slash-path);
                if (len >= sizeof(last_parent)) return false;
                if (strlen(last_parent) != len || memcmp(last_parent, path, len))
                { memcpy(last_parent, path, len); last_parent[len] = 0; parent_ok = ScopeDirectory(i, last_parent); }
                if (parent_ok < 0) return false;
                if (!parent_ok) continue;
            }
            char full[8192]; struct stat st;
            if (snprintf(full, sizeof(full), "%s/%s", i->root, path) >= (int)sizeof(full)) return false;
            if (lstat(full, &st)) { if (errno == ENOENT || errno == ENOTDIR) continue; return false; }
            if (!S_ISREG(st.st_mode)) continue;
        }
        s->paths[s->count++] = path;
    }
    /* External tools may return absolute paths. Compact validated relative
     * paths into one exact-sized arena, improving cache locality and avoiding
     * retaining one copy of the workspace prefix per file. */
    size_t compact_bytes = 0;
    for (size_t n = 0; n < s->count; n++) compact_bytes += strlen(s->paths[n])+1;
    size_t compact_alloc = compact_bytes ? compact_bytes : 1;
    if (compact_alloc > INDEX_BYTES - s->capacity - 2*alloc_count*sizeof(char *)) return false;
    char *compact = malloc(compact_alloc);
    if (!compact) return false;
    size_t at = 0;
    for (size_t n = 0; n < s->count; n++)
    {
        if (!(n & 1023) && Cancelled(i, serial)) { free(compact); return false; }
        size_t len = strlen(s->paths[n])+1;
        memcpy(compact+at, s->paths[n], len); s->paths[n] = compact+at; at += len;
    }
    free(s->arena); s->arena = compact; s->used = compact_bytes; s->capacity = compact_alloc;
    char **scratch = malloc(alloc_count*sizeof(char *));
    if (!scratch) return false;
    if (!Sort(i, s->paths, scratch, s->count, PathCompare, serial)) { free(scratch); return false; }
    count = 0;
    for (size_t n = 0; n < s->count; n++)
        if (!count || strcmp(s->paths[n], s->paths[count-1])) s->paths[count++] = s->paths[n];
    s->count = count;
    memcpy(s->names, s->paths, count * sizeof(char *));
    bool sorted = Sort(i, s->names, scratch, count, NameCompare, serial);
    free(scratch);
    return sorted;
}
static Snapshot *Discover(PicoFileIndex *i)
{
    Snapshot *s = NewSnapshot(); if (!s) return NULL;
    char *git[] = {"git", "-C", i->root, "ls-files", "--cached", "--others", "--exclude-standard", "--deduplicate", "-z", "--", ".", NULL};
    int rc = Enumerate(i, s, git, 0); bool absolute = false;
    if (rc == -2) { Release(s); return NULL; }
    if (rc != 0)
    {
        s->used = 0;
        char *rg[] = {"rg", "--files", "--hidden", "--null", "--glob", "!.git", "--glob", "!.pico", i->root, NULL};
        rc = Enumerate(i, s, rg, 0); absolute = true;
        if (rc == -2) { Release(s); return NULL; }
        if (rc != 0 && !(rc == 1 && !s->used))
        {
            s->used = 0; absolute = false;
            snprintf(s->status, sizeof(s->status), "Ignore-aware discovery unavailable (git/rg failed); showing native fallback without ignore rules.");
            if (!Walk(i, s, "", 0)) { Release(s); return NULL; }
        }
    }
    s->ignore_aware = !s->status[0];
    if (!Finish(i, s, absolute, true, 0)) { Release(s); return NULL; }
    return s;
}

static uint64_t Hash(const char *p)
{
    uint64_t h = UINT64_C(14695981039346656037);
    while (*p) { h ^= (unsigned char)*p++; h *= UINT64_C(1099511628211); }
    return h;
}
static bool MkdirParents(char *p)
{
    for (char *q = p+1; *q; q++) if (*q == '/')
    { *q = 0; bool ok = !mkdir(p, 0700) || errno == EEXIST; *q = '/'; if (!ok) return false; }
    return true;
}
/* Cache is disposable, versioned, root-identified and atomically replaced. It
 * stores only eligible paths; no file contents or extension-owned storage. */
static Snapshot *LoadCache(PicoFileIndex *i)
{
    FILE *f = fopen(i->cache, "rb"); if (!f) return NULL;
    char magic[8], root[4096]; uint64_t bytes = 0, dev = 0, ino = 0; uint32_t len = 0;
    struct stat st;
    bool ok = fread(magic, 1, 8, f) == 8 && !memcmp(magic, "PICOFI03", 8) &&
        fread(&len, sizeof(len), 1, f) == 1 && len < sizeof(root) && len > 0 &&
        fread(root, 1, len, f) == len && fread(&dev, sizeof(dev), 1, f) == 1 &&
        fread(&ino, sizeof(ino), 1, f) == 1 && fread(&bytes, sizeof(bytes), 1, f) == 1 && bytes <= INDEX_BYTES;
    root[len < sizeof(root) ? len : 0] = 0;
    ok = ok && !strcmp(root, i->root) && !stat(i->root, &st) && dev == (uint64_t)st.st_dev && ino == (uint64_t)st.st_ino;
    Snapshot *s = ok ? NewSnapshot() : NULL;
    if (s)
    {
        s->arena = malloc(bytes ? (size_t)bytes : 1); s->capacity = s->used = (size_t)bytes;
        ok = s->arena && fread(s->arena, 1, s->used, f) == s->used && fgetc(f) == EOF;
        if (!ok || !Finish(i, s, false, false, 0)) { Release(s); s = NULL; }
        else { s->ignore_aware = true; s->cached = true; }
    }
    fclose(f); return s;
}
static bool SaveCache(PicoFileIndex *i, Snapshot *s)
{
    if (!s->ignore_aware) return true; /* never cache an unfiltered fallback corpus */
    if (!i->cache[0]) return false;
    char tmp[8192]; snprintf(tmp, sizeof(tmp), "%s.XXXXXX", i->cache);
    if (!MkdirParents(tmp)) return false;
    int fd = mkstemp(tmp); if (fd < 0) return false;
    FILE *f = fdopen(fd, "wb"); if (!f) { close(fd); unlink(tmp); return false; }
    uint32_t len = (uint32_t)strlen(i->root); uint64_t bytes = 0; struct stat st;
    for (size_t n = 0; n < s->count; n++) bytes += strlen(s->paths[n])+1;
    bool ok = !stat(i->root, &st); uint64_t dev = ok ? st.st_dev : 0, ino = ok ? st.st_ino : 0;
    ok = ok && fwrite("PICOFI03", 1, 8, f) == 8 && fwrite(&len, sizeof(len), 1, f) == 1 &&
        fwrite(i->root, 1, len, f) == len && fwrite(&dev, sizeof(dev), 1, f) == 1 &&
        fwrite(&ino, sizeof(ino), 1, f) == 1 && fwrite(&bytes, sizeof(bytes), 1, f) == 1;
    for (size_t n = 0; ok && n < s->count; n++)
    { size_t size = strlen(s->paths[n])+1; ok = !atomic_load(&i->stop) && fwrite(s->paths[n], 1, size, f) == size; }
    if (fclose(f)) ok = false;
    if (ok) ok = rename(tmp, i->cache) == 0;
    if (!ok) unlink(tmp);
    return ok;
}

static void Publish(PicoFileIndex *i, Snapshot *s, double ms)
{
    pthread_mutex_lock(&i->mu);
    Snapshot *old = i->latest;
    s->generation = i->info.generation + 1;
    i->latest = s;
    i->failed = false;
    i->info.generation = s->generation; i->info.files = s->count;
    i->info.bytes = s->capacity + 2*s->pointer_capacity*sizeof(char *);
    i->info.cached = s->cached; i->info.discovery_ms = ms;
    snprintf(i->info.status, sizeof(i->info.status), "%s", s->status);
    pthread_cond_broadcast(&i->changed);
    pthread_mutex_unlock(&i->mu);
    Release(old);
}

#ifdef __linux__
static bool WatchDirectory(int fd, const char *path)
{
    return inotify_add_watch(fd, path, IN_CREATE | IN_DELETE | IN_MOVED_FROM | IN_MOVED_TO |
        IN_CLOSE_WRITE | IN_ATTRIB | IN_DELETE_SELF | IN_MOVE_SELF | IN_ONLYDIR | IN_DONT_FOLLOW) >= 0;
}
static bool WatchSnapshot(PicoFileIndex *i, int fd, Snapshot *s)
{
    if (fd < 0) return false;
    bool ok = WatchDirectory(fd, i->root);
    char last[4096] = "", path[8192];
    for (size_t n = 0; n < s->count && !atomic_load(&i->stop); n++)
    {
        const char *p = s->paths[n]; const char *slash = strrchr(p, '/'); if (!slash) continue;
        size_t len = (size_t)(slash-p); if (len >= sizeof(last)) { ok = false; continue; }
        if (strlen(last) == len && !memcmp(last, p, len)) continue;
        memcpy(last, p, len); last[len] = 0;
        snprintf(path, sizeof(path), "%s/%s", i->root, last);
        /* Add ancestors too, including directories containing only children. */
        for (char *q = path+strlen(i->root)+1; ; q++)
        {
            if (*q && *q != '/') continue;
            char save = *q; *q = 0; if (!WatchDirectory(fd, path)) ok = false; *q = save;
            if (!save) break;
        }
    }
    snprintf(path, sizeof(path), "%s/.git", i->root);
    struct stat st;
    if (!lstat(path, &st) && S_ISDIR(st.st_mode)) { if (!WatchDirectory(fd, path)) ok = false; }
    return ok;
}
static bool WatchEvents(int fd)
{
    if (fd < 0) return false;
    char buf[65536] __attribute__((aligned(8))); bool dirty = false; ssize_t n;
    while ((n = read(fd, buf, sizeof(buf))) > 0)
    {
        for (size_t at = 0; at < (size_t)n; )
        {
            struct inotify_event *e = (struct inotify_event *)(buf+at);
            /* Ordinary source-content writes don't change a path index. */
            bool ignorefile = e->len && (!strcmp(e->name, ".gitignore") || !strcmp(e->name, ".ignore") ||
                !strcmp(e->name, ".rgignore") || !strcmp(e->name, "index") || !strcmp(e->name, "exclude"));
            if ((e->mask & ~(IN_CLOSE_WRITE | IN_ATTRIB | IN_ISDIR)) || ignorefile) dirty = true;
            at += sizeof(*e)+e->len;
        }
    }
    return dirty;
}
#else
static bool WatchSnapshot(PicoFileIndex *i, int fd, Snapshot *s) { (void)i; (void)fd; (void)s; return false; }
static bool WatchEvents(int fd) { (void)fd; return false; }
#endif

static void *DiscoveryMain(void *arg)
{
    PicoFileIndex *i = arg;
#ifdef __linux__
    int watch = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
#else
    int watch = -1;
#endif
    Snapshot *cached = LoadCache(i);
    if (cached) Publish(i, cached, 0);
    double next = Now(), dirty = 0;
    while (!atomic_load(&i->stop))
    {
        double now = Now();
        if (WatchEvents(watch)) { if (!dirty) dirty = now + REFRESH_DEBOUNCE; }
        pthread_mutex_lock(&i->mu);
        bool refresh = i->refresh; i->refresh = false;
        pthread_mutex_unlock(&i->mu);
        if (refresh || now >= next || (dirty && now >= dirty))
        {
            pthread_mutex_lock(&i->mu); i->info.refreshing = true; pthread_mutex_unlock(&i->mu);
            double start = Now(); Snapshot *s = Discover(i);
            if (s && !atomic_load(&i->stop))
            {
#ifdef __linux__
                int replacement = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
#else
                int replacement = -1;
#endif
                bool changed = WatchEvents(watch);
                bool watching = WatchSnapshot(i, replacement, s);
                changed = WatchEvents(watch) || changed;
                if (watch >= 0) close(watch);
                watch = replacement;
                if (changed) { pthread_mutex_lock(&i->mu); i->refresh = true; pthread_mutex_unlock(&i->mu); }
                if (!watching && !s->status[0])
                    snprintf(s->status, sizeof(s->status), "Some filesystem watches unavailable; periodic reconciliation remains active.");
                /* Watch first, then drain events after publication; changes during
                 * enumeration are also caught by the periodic reconciliation. */
                bool saved = SaveCache(i, s);
                if (!saved && !s->status[0] && !atomic_load(&i->stop))
                    snprintf(s->status, sizeof(s->status), "File index ready; disk cache unavailable.");
                Publish(i, s, (Now()-start)*1000);
            }
            else
            {
                Release(s);
                pthread_mutex_lock(&i->mu); i->failed = true;
                snprintf(i->info.status, sizeof(i->info.status), "File discovery failed (I/O or memory budget); %s.", i->latest ? "using previous file list" : "file list unavailable");
                pthread_cond_broadcast(&i->changed); pthread_mutex_unlock(&i->mu);
            }
            pthread_mutex_lock(&i->mu); i->info.refreshing = false; pthread_mutex_unlock(&i->mu);
            next = Now()+RECONCILE_SECONDS; dirty = 0;
        }
        pthread_mutex_lock(&i->mu);
        struct timespec until; clock_gettime(CLOCK_REALTIME, &until); until.tv_nsec += 100000000;
        if (until.tv_nsec >= 1000000000) { until.tv_sec++; until.tv_nsec -= 1000000000; }
        if (!atomic_load(&i->stop)) pthread_cond_timedwait(&i->changed, &i->mu, &until);
        pthread_mutex_unlock(&i->mu);
    }
    if (watch >= 0) close(watch);
    ServiceRelease(i); return NULL;
}

static bool Starts(const char *s, const char *q)
{
    while (*q) { if (Fold((unsigned char)*s++) != Fold((unsigned char)*q++)) return false; }
    return true;
}
static bool Contains(const char *s, const char *q)
{ for (; *s; s++) if (Starts(s, q)) return true; return !*q; }
static int Rank(const char *path, const char *q)
{
    const char *base = Base(path);
    if (!*q) return 3;
    if (!CompareFold(base, q)) return 0;
    if (Starts(base, q)) return 1;
    if (Starts(path, q)) return 2;
    return Contains(path, q) ? 3 : 4;
}
static void Offer(PicoFileIndexResult *out, const char *p)
{
    if (strlen(p) >= PICO_FILE_INDEX_PATH) return;
    char encoded[512]; if (!PicoFileMention_Encode(p, encoded, sizeof(encoded))) return;
    for (int n = 0; n < out->count; n++) if (!strcmp(out->paths[n], p)) return;
    snprintf(out->paths[out->count++], PICO_FILE_INDEX_PATH, "%s", p);
}
static size_t Lower(char **paths, size_t count, const char *q, bool basename)
{
    size_t lo = 0, hi = count;
    while (lo < hi)
    { size_t m = lo+(hi-lo)/2; if (CompareFold(basename ? Base(paths[m]) : paths[m], q) < 0) lo = m+1; else hi = m; }
    return lo;
}
static bool Search(PicoFileIndex *i, Snapshot *s, const char *q, uint64_t serial, PicoFileIndexResult *out)
{
    if (!s) return false;
    out->generation = s->generation;
    if (!*q)
    {
        for (size_t n = 0; n < s->count && out->count < PICO_FILE_INDEX_RESULTS; n++) Offer(out, s->paths[n]);
        return !Cancelled(i, serial);
    }
    for (int rank = 0; rank < 4 && out->count < PICO_FILE_INDEX_RESULTS; rank++)
    {
        bool names = rank < 2;
        char **paths = names ? s->names : s->paths;
        size_t start = rank < 3 ? Lower(paths, s->count, q, names) : 0;
        for (size_t n = start; n < s->count && out->count < PICO_FILE_INDEX_RESULTS; n++)
        {
            if (!(n & 255) && (atomic_load(&i->stop) || atomic_load(&i->request) != serial)) return false;
            const char *path = paths[n];
            if (rank < 3 && !Starts(names ? Base(path) : path, q)) break;
            if (rank == 0 && CompareFold(Base(path), q)) break;
            if (rank == 3 && !Contains(path, q)) continue;
            if (Rank(path, q) == rank) Offer(out, path);
        }
    }
    return true;
}
static bool IgnoredScope(PicoFileIndex *i, const char *dir, uint64_t serial)
{
    Snapshot *output = NewSnapshot(); if (!output) return false;
    char name[513]; snprintf(name, sizeof(name), "%s/", dir);
    char *args[] = {"git", "-C", i->root, "check-ignore", "--no-index", "--quiet", "--", name, NULL};
    bool ignored = Enumerate(i, output, args, serial) == 0;
    Release(output); return ignored;
}

static void *SearchMain(void *arg)
{
    PicoFileIndex *i = arg; Snapshot *pinned = NULL, *scoped = NULL;
    uint64_t token = UINT64_MAX, done = 0; char scope[512] = "", scope_status[256] = "";
    while (!atomic_load(&i->stop))
    {
        pthread_mutex_lock(&i->mu);
        while (!atomic_load(&i->stop) && (!i->requested || (atomic_load(&i->request) == done && (pinned || !i->latest)) || (!i->latest && !i->failed)))
            pthread_cond_wait(&i->changed, &i->mu);
        if (atomic_load(&i->stop)) { pthread_mutex_unlock(&i->mu); break; }
        uint64_t serial = atomic_load(&i->request), current = i->token;
        char query[512]; snprintf(query, sizeof(query), "%s", i->query);
        if (token != current || !pinned)
        {
            Snapshot *old = pinned; pinned = i->latest; Retain(pinned);
            pthread_mutex_unlock(&i->mu); Release(old); Release(scoped); scoped = NULL; scope[0] = 0; scope_status[0] = 0;
            token = current;
        }
        else pthread_mutex_unlock(&i->mu);
        PicoFileIndexResult result = {0}; double start = Now();
        char dir[512] = ""; const char *slash = strrchr(query, '/');
        if (slash && (size_t)(slash-query) < sizeof(dir)) { memcpy(dir, query, (size_t)(slash-query)); dir[slash-query] = 0; }
        if (strcmp(dir, scope))
        {
            Release(scoped); scoped = NULL; scope[0] = 0; scope_status[0] = 0;
            char prefix[513]; snprintf(prefix, sizeof(prefix), "%s/", dir);
            size_t at = pinned ? Lower(pinned->paths, pinned->count, prefix, false) : 0;
            bool indexed = pinned && at < pinned->count && Starts(pinned->paths[at], prefix);
            int eligible = *dir ? ScopeDirectory(i,dir) : 0;
            if (eligible < 0) snprintf(scope_status,sizeof(scope_status),"Explicit directory validation failed.");
            if (eligible == 1 && (!indexed || IgnoredScope(i, dir, serial)))
            {
                scoped = NewSnapshot();
                if (scoped && (!Walk(i, scoped, dir, serial) || !Finish(i, scoped, false, false, serial)))
                { Release(scoped); scoped = NULL; snprintf(scope_status, sizeof(scope_status), "Explicit directory discovery failed."); }
                if (scoped) scoped->generation = pinned ? pinned->generation : 0;
            }
            if (!Cancelled(i, serial)) snprintf(scope, sizeof(scope), "%s", dir);
            else { Release(scoped); scoped = NULL; scope[0] = 0; }
        }
        if (scope_status[0]) snprintf(result.status,sizeof(result.status),"%s",scope_status);
        if (!result.status[0] && pinned) snprintf(result.status, sizeof(result.status), "%s", pinned->status);
        bool ok = Search(i, scoped ? scoped : pinned, query, serial, &result);
        if (!pinned) ok = true; /* publish discovery failure instead of endless Loading */
        pthread_mutex_lock(&i->mu);
        if (ok && serial == atomic_load(&i->request) && !atomic_load(&i->stop))
        {
            i->result = result; done = serial; i->info.search_ms = (Now()-start)*1000;
        }
        pthread_mutex_unlock(&i->mu);
    }
    Release(pinned); Release(scoped); ServiceRelease(i); return NULL;
}

PicoFileIndex *PicoFileIndex_Open(const char *root)
{
    PicoFileIndex *i = calloc(1, sizeof(*i)); if (!i) return NULL;
    if (!root || strlen(root) >= sizeof(i->root)) { free(i); return NULL; }
    snprintf(i->root, sizeof(i->root), "%s", root);
    const char *cache = getenv("XDG_CACHE_HOME"), *home = getenv("HOME");
    if (cache && *cache == '/') snprintf(i->cache, sizeof(i->cache), "%s/pico/files/%016llx.idx", cache, (unsigned long long)Hash(root));
    else if (home) snprintf(i->cache, sizeof(i->cache), "%s/.cache/pico/files/%016llx.idx", home, (unsigned long long)Hash(root));
    if (strlen(i->cache) >= sizeof(i->cache)-1) i->cache[0] = 0;
    size_t rootlen = strlen(root);
    if (i->cache[0] && !strncmp(i->cache,root,rootlen) && i->cache[rootlen] == '/')
    {
        snprintf(i->cache_relative,sizeof(i->cache_relative),"%s",i->cache+rootlen+1);
        char *slash = strrchr(i->cache_relative,'/'); if (slash) *slash = 0;
    }
    if (pthread_mutex_init(&i->mu, NULL)) { free(i); return NULL; }
    if (pthread_cond_init(&i->changed, NULL)) { pthread_mutex_destroy(&i->mu); free(i); return NULL; }
    atomic_init(&i->stop, false); atomic_init(&i->refs, 1); atomic_init(&i->request, 0);
    pthread_attr_t attr; pthread_attr_init(&attr); pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    pthread_t thread;
    atomic_fetch_add(&i->refs, 1);
    if (pthread_create(&thread, &attr, DiscoveryMain, i))
    { atomic_fetch_sub(&i->refs, 1); pthread_attr_destroy(&attr); ServiceRelease(i); return NULL; }
    atomic_fetch_add(&i->refs, 1);
    if (pthread_create(&thread, &attr, SearchMain, i))
    {
        atomic_fetch_sub(&i->refs, 1); pthread_attr_destroy(&attr); PicoFileIndex_Close(i); return NULL;
    }
    pthread_attr_destroy(&attr); return i;
}
void PicoFileIndex_Query(PicoFileIndex *i, uint64_t token, const char *query, PicoFileIndexResult *out)
{
    memset(out, 0, sizeof(*out));
    if (!i || !query || strlen(query) >= sizeof(i->query)) { snprintf(out->status, sizeof(out->status), "File index unavailable."); return; }
    pthread_mutex_lock(&i->mu);
    if (!i->requested || token != i->token || strcmp(query, i->query))
    {
        i->requested = true; i->token = token; snprintf(i->query, sizeof(i->query), "%s", query);
        atomic_fetch_add(&i->request, 1); memset(&i->result, 0, sizeof(i->result)); i->result.pending = true;
        pthread_cond_broadcast(&i->changed);
    }
    *out = i->result;
    if (i->failed || out->pending) snprintf(out->status, sizeof(out->status), "%s", i->info.status);
    pthread_mutex_unlock(&i->mu);
}
void PicoFileIndex_Info(PicoFileIndex *i, PicoFileIndexInfo *out)
{ memset(out, 0, sizeof(*out)); if (i) { pthread_mutex_lock(&i->mu); *out = i->info; pthread_mutex_unlock(&i->mu); } }
void PicoFileIndex_Refresh(PicoFileIndex *i)
{ if (i) { pthread_mutex_lock(&i->mu); i->refresh = true; pthread_cond_broadcast(&i->changed); pthread_mutex_unlock(&i->mu); } }
void PicoFileIndex_Close(PicoFileIndex *i)
{
    if (!i) return;
    pthread_mutex_lock(&i->mu); atomic_store(&i->stop, true); pthread_cond_broadcast(&i->changed); pthread_mutex_unlock(&i->mu);
    ServiceRelease(i);
}
