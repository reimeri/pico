#define _DEFAULT_SOURCE
#define _POSIX_C_SOURCE 200809L

#include "session.h"
#include "agent.h"
#include "workspace_internal.h"
#include "json.h"
#include "path.h"
#include "posix_io.h"
#include "settings.h"
#include "usage.h"
#include "host_internal.h"

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

static bool CatalogMetaPath(const char *dir, char *out, size_t cap);
static bool CatalogLoadMeta(const char *path, PicoCatalogWorkspace *out);
static int CmpCatalogOrder(const void *a, const void *b);
static void CatalogClearSessions(PicoCatalogWorkspace *ws);
static const PicoCatalogSession *CatalogFindSession(const PicoCatalogWorkspace *ws,
                                                    const char *id);
static void CatalogMarkChanged(void);
static void CatalogWriteThrough(PicoHost *app, const PicoAgent *agent,
                                const char *title_override, const char *event_json,
                                const struct stat *previous_stat);
static void CatalogWriteThroughFields(PicoAgentKind kind, PicoSessionPersistence persistence,
                                      const char *session_id, const char *session_path,
                                      const char *ws_path, const char *title_override,
                                      const char *event_json, const struct stat *previous_stat);
static PicoSessionWriteResult QueueSessionLine(PicoHost *app, PicoAgent *agent,
                                                 const char *json);
static bool DrainPersistUiBound(PicoHost *app, PicoAgent *agent);
#ifdef PICO_SESSION_TEST_HOOKS
extern bool PicoSession_TestHook(const char *stage);
#endif

static int EncodeCwd(const char *cwd, char *out, size_t cap)
{
    char real[4096];
    const char *src = cwd && cwd[0] ? cwd : ".";
    if (!realpath(src, real) && !PicoPath_Format(real, sizeof(real), "%s", src))
    {
        return -1;
    }
    const char *p = real;
    if (*p == '/')
    {
        p++;
    }
    JsonBuf b;
    JsonBuf_Init(&b);
    JsonBuf_Puts(&b, "--");
    for (; *p; p++)
    {
        JsonBuf_Putc(&b, *p == '/' ? '-' : *p);
    }
    JsonBuf_Puts(&b, "--");
    if (b.len + 1 > cap)
    {
        JsonBuf_Free(&b);
        return -1;
    }
    snprintf(out, cap, "%s", b.data ? b.data : "--.--");
    JsonBuf_Free(&b);
    return 0;
}

static const PicoWorkspace *SessionWorkspace(const PicoHost *host, const PicoAgent *agent)
{
    PicoWorkspace *from_agent = PicoAgent_Workspace(agent);
    if (from_agent)
    {
        return from_agent;
    }
    return PicoHost_PrimaryWorkspaceConst(host);
}

static bool SessionDir(const PicoWorkspace *workspace, char *out, size_t cap)
{
    char cfg[4096];
    char enc[4096];
    const char *root = PicoWorkspace_Path(workspace);
    return Pico_ConfigDir(cfg, sizeof(cfg)) &&
           EncodeCwd(root[0] ? root : ".", enc, sizeof(enc)) == 0 &&
           PicoPath_Format(out, cap, "%s/sessions/%s", cfg, enc);
}

static bool IsSessionJsonl(const char *name)
{
    size_t len = name ? strlen(name) : 0;
    return len >= 7 && name[0] != '.' && strcmp(name + len - 6, ".jsonl") == 0;
}

static int FindLatest(const char *dir, char *out, size_t cap)
{
    DIR *d = opendir(dir);
    if (!d)
    {
        return -1;
    }
    char best[256];
    best[0] = '\0';
    time_t best_mtime = 0;
    struct dirent *ent;
    while ((ent = readdir(d)))
    {
        const char *n = ent->d_name;
        if (!IsSessionJsonl(n))
        {
            continue;
        }
        char path[4096];
        if (!PicoPath_Format(path, sizeof(path), "%s/%s", dir, n))
        {
            continue;
        }
        struct stat st;
        if (stat(path, &st) != 0 || !S_ISREG(st.st_mode))
        {
            continue;
        }
        if (!best[0] || st.st_mtime > best_mtime)
        {
            snprintf(best, sizeof(best), "%s", n);
            best_mtime = st.st_mtime;
        }
    }
    closedir(d);
    if (!best[0])
    {
        return -1;
    }
    return PicoPath_Format(out, cap, "%s/%s", dir, best) ? 0 : -1;
}

static void IdFromName(const char *name, char *out, size_t cap)
{
    out[0] = '\0';
    if (!name || cap < 2)
    {
        return;
    }
    const char *us = strchr(name, '_');
    if (!us || !us[1])
    {
        return;
    }
    us++;
    size_t len = strlen(us);
    if (len > 6 && strcmp(us + len - 6, ".jsonl") == 0)
    {
        len -= 6;
    }
    if (len >= cap)
    {
        len = cap - 1;
    }
    memcpy(out, us, len);
    out[len] = '\0';
}

static void MakeTitle(char *out, size_t cap, const char *src)
{
    if (!out || cap == 0)
    {
        return;
    }
    out[0] = '\0';
    if (!src)
    {
        snprintf(out, cap, "Untitled");
        return;
    }
    while (*src && isspace((unsigned char)*src))
    {
        src++;
    }
    size_t max_keep = 72;
    if (max_keep + 1 > cap)
    {
        max_keep = cap - 1;
    }
    size_t n = 0;
    bool space = false;
    for (const char *p = src; *p && n < max_keep; p++)
    {
        unsigned char c = (unsigned char)*p;
        if (c == '\n' || c == '\r' || c == '\t' || c == ' ')
        {
            if (n == 0)
            {
                continue;
            }
            space = true;
            continue;
        }
        if (space)
        {
            if (n + 1 >= max_keep)
            {
                break;
            }
            out[n++] = ' ';
            space = false;
        }
        out[n++] = (char)c;
    }
    out[n] = '\0';
    if (n == 0)
    {
        snprintf(out, cap, "Untitled");
    }
}

static void ScanSessionFile(const char *path, PicoSessionInfo *info, bool header_only)
{
#ifdef PICO_SESSION_TEST_HOOKS
    (void)PicoSession_TestHook("scan_session_file");
#endif
    if (!info)
    {
        return;
    }
    info->title[0] = '\0';
    info->cwd[0] = '\0';
    info->model[0] = '\0';
    info->effort[0] = '\0';
    info->kind = PICO_AGENT_MAIN;
    info->unseen_complete = false;
    FILE *f = fopen(path, "rb");
    if (!f)
    {
        snprintf(info->title, sizeof(info->title), "Untitled");
        return;
    }
    char *buf = NULL;
    size_t buf_cap = 0;
    bool got_title = false;
    while (getline(&buf, &buf_cap, f) != -1)
    {
        size_t len = strlen(buf);
        while (len > 0 && (buf[len - 1] == '\n' || buf[len - 1] == '\r'))
        {
            buf[--len] = '\0';
        }
        if (len == 0)
        {
            continue;
        }
        JsonDoc doc;
        if (JsonParse(&doc, buf, len) != 0)
        {
            continue;
        }
        char *type = JsonObjStr(&doc, 0, "type");
        if (type && strcmp(type, "session") == 0)
        {
            char *sid = JsonObjStr(&doc, 0, "id");
            if (sid && sid[0])
            {
                snprintf(info->id, sizeof(info->id), "%s", sid);
            }
            free(sid);
            char *kind = JsonObjStr(&doc, 0, "kind");
            if (kind && strcmp(kind, "subagent") == 0)
            {
                info->kind = PICO_AGENT_SUBAGENT;
            }
            free(kind);
            char *title = JsonObjStr(&doc, 0, "title");
            if (title && title[0])
            {
                snprintf(info->title, sizeof(info->title), "%s", title);
                got_title = true;
            }
            free(title);
            char *cwd = JsonObjStr(&doc, 0, "cwd");
            if (cwd && cwd[0])
            {
                snprintf(info->cwd, sizeof(info->cwd), "%s", cwd);
            }
            free(cwd);
            char *model = JsonObjStr(&doc, 0, "model");
            if (model && model[0])
            {
                snprintf(info->model, sizeof(info->model), "%s", model);
            }
            free(model);
            if (header_only)
            {
                free(type);
                JsonFree(&doc);
                break;
            }
        }
        else if (!header_only && type && strcmp(type, "message") == 0 && !got_title)
        {
            char *role = JsonObjStr(&doc, 0, "role");
            if (role && strcmp(role, "user") == 0)
            {
                char *display = JsonObjStr(&doc, 0, "display");
                char *content = JsonObjStr(&doc, 0, "content");
                const char *src = (display && display[0]) ? display : content;
                MakeTitle(info->title, sizeof(info->title), src);
                got_title = true;
                free(display);
                free(content);
            }
            free(role);
        }
        else if (!header_only && type && strcmp(type, "model_change") == 0)
        {
            char *model = JsonObjStr(&doc, 0, "model");
            char *effort = JsonObjStr(&doc, 0, "effort");
            if (model && model[0])
            {
                snprintf(info->model, sizeof(info->model), "%s", model);
            }
            if (effort && effort[0])
            {
                snprintf(info->effort, sizeof(info->effort), "%s", effort);
            }
            free(model);
            free(effort);
        }
        else if (!header_only && type && strcmp(type, "unseen_complete") == 0)
        {
            int tok = JsonObjGet(&doc, 0, "complete");
            info->unseen_complete = JsonEq(&doc, tok, "true") || JsonEq(&doc, tok, "1");
        }
        free(type);
        JsonFree(&doc);
    }
    free(buf);
    fclose(f);
    if (!info->title[0])
    {
        snprintf(info->title, sizeof(info->title), "Untitled");
    }
}

typedef struct SessionListingRow {
    PicoSessionInfo info;
    bool cache_hit;
} SessionListingRow;

static long StatMtimeNsec(const struct stat *st)
{
#if defined(__APPLE__)
    return st ? st->st_mtimespec.tv_nsec : 0;
#else
    return st ? st->st_mtim.tv_nsec : 0;
#endif
}

static long StatCtimeNsec(const struct stat *st)
{
#if defined(__APPLE__)
    return st ? st->st_ctimespec.tv_nsec : 0;
#else
    return st ? st->st_ctim.tv_nsec : 0;
#endif
}

static void CopyStatToInfo(PicoSessionInfo *info, const struct stat *st)
{
    if (!info || !st)
    {
        return;
    }
    info->mtime = st->st_mtime;
    info->mtime_nsec = StatMtimeNsec(st);
    info->ctime = st->st_ctime;
    info->ctime_nsec = StatCtimeNsec(st);
    info->inode = (uint64_t)st->st_ino;
    info->size = (uint64_t)st->st_size;
}

static void CopyStatToCatalog(PicoCatalogSession *session, const struct stat *st)
{
    if (!session || !st)
    {
        return;
    }
    session->mtime = st->st_mtime;
    session->mtime_nsec = StatMtimeNsec(st);
    session->ctime = st->st_ctime;
    session->ctime_nsec = StatCtimeNsec(st);
    session->inode = (uint64_t)st->st_ino;
    session->size = (uint64_t)st->st_size;
}

static bool CatalogGenerationMatches(const PicoCatalogSession *cached, const struct stat *st)
{
    return cached && st && cached->mtime == st->st_mtime &&
           cached->mtime_nsec == StatMtimeNsec(st) && cached->ctime == st->st_ctime &&
           cached->ctime_nsec == StatCtimeNsec(st) && cached->inode == (uint64_t)st->st_ino &&
           cached->size == (uint64_t)st->st_size;
}

static int CmpListingMtimeDesc(const void *a, const void *b)
{
    const SessionListingRow *x = (const SessionListingRow *)a;
    const SessionListingRow *y = (const SessionListingRow *)b;
    if (x->info.mtime > y->info.mtime)
    {
        return -1;
    }
    if (x->info.mtime < y->info.mtime)
    {
        return 1;
    }
    if (x->info.mtime_nsec > y->info.mtime_nsec)
    {
        return -1;
    }
    if (x->info.mtime_nsec < y->info.mtime_nsec)
    {
        return 1;
    }
    return strcmp(y->info.path, x->info.path);
}

static void CopyCacheToInfo(PicoSessionInfo *info, const PicoCatalogSession *cached,
                            const struct stat *st)
{
    snprintf(info->id, sizeof(info->id), "%s", cached->id);
    snprintf(info->title, sizeof(info->title), "%s", cached->title);
    snprintf(info->model, sizeof(info->model), "%s", cached->model);
    snprintf(info->effort, sizeof(info->effort), "%s", cached->effort);
    info->kind = cached->kind;
    CopyStatToInfo(info, st);
    info->unseen_complete = cached->unseen_complete;
}

static int ListSessionsInDir(const char *dir, PicoSessionInfo **out, bool parents_only,
                             const PicoCatalogWorkspace *cache, int max_results)
{
    SessionListingRow *rows = NULL;
    PicoSessionInfo *list = NULL;
    int n = 0;
    int cap = 0;
    int result_n;
    struct dirent *ent;
    DIR *d;
    if (out)
    {
        *out = NULL;
    }
    if (!dir || !dir[0] || !out)
    {
        return 0;
    }
    d = opendir(dir);
    if (!d)
    {
        return 0;
    }
    while ((ent = readdir(d)))
    {
        SessionListingRow *row;
        const PicoCatalogSession *cached;
        struct stat st;
        if (!IsSessionJsonl(ent->d_name))
        {
            continue;
        }
        if (n >= cap)
        {
            int next_cap = cap == 0 ? 8 : cap * 2;
            SessionListingRow *next =
                (SessionListingRow *)realloc(rows, (size_t)next_cap * sizeof(*next));
            if (!next)
            {
                break;
            }
            rows = next;
            cap = next_cap;
        }
        row = &rows[n];
        memset(row, 0, sizeof(*row));
        if (!PicoPath_Format(row->info.path, sizeof(row->info.path), "%s/%s", dir, ent->d_name) ||
            stat(row->info.path, &st) != 0 || !S_ISREG(st.st_mode))
        {
            continue;
        }
        CopyStatToInfo(&row->info, &st);
        IdFromName(ent->d_name, row->info.id, sizeof(row->info.id));
        cached = CatalogFindSession(cache, row->info.id);
        if (CatalogGenerationMatches(cached, &st))
        {
            CopyCacheToInfo(&row->info, cached, &st);
            row->cache_hit = true;
        }
        else
        {
            ScanSessionFile(row->info.path, &row->info, true);
            CopyStatToInfo(&row->info, &st);
        }
        if (!row->info.id[0] ||
            (parents_only && row->info.kind == PICO_AGENT_SUBAGENT))
        {
            continue;
        }
        n++;
    }
    closedir(d);
    if (n > 1)
    {
        qsort(rows, (size_t)n, sizeof(*rows), CmpListingMtimeDesc);
    }
    result_n = max_results > 0 && n > max_results ? max_results : n;
    if (result_n > 0)
    {
        list = (PicoSessionInfo *)malloc((size_t)result_n * sizeof(*list));
        if (!list)
        {
            free(rows);
            return 0;
        }
    }
    for (int i = 0; i < result_n; i++)
    {
        list[i] = rows[i].info;
        if (!rows[i].cache_hit)
        {
            PicoSessionInfo parsed;
            memset(&parsed, 0, sizeof(parsed));
            snprintf(parsed.path, sizeof(parsed.path), "%s", rows[i].info.path);
            snprintf(parsed.id, sizeof(parsed.id), "%s", rows[i].info.id);
            ScanSessionFile(rows[i].info.path, &parsed, false);
            parsed.mtime = rows[i].info.mtime;
            parsed.mtime_nsec = rows[i].info.mtime_nsec;
            parsed.ctime = rows[i].info.ctime;
            parsed.ctime_nsec = rows[i].info.ctime_nsec;
            parsed.inode = rows[i].info.inode;
            parsed.size = rows[i].info.size;
            if (!parsed.id[0])
            {
                snprintf(parsed.id, sizeof(parsed.id), "%s", rows[i].info.id);
            }
            list[i] = parsed;
        }
    }
    free(rows);
    *out = list;
    return result_n;
}

int PicoSession_List(const PicoWorkspace *workspace, PicoSessionInfo **out, bool parents_only)
{
    PicoCatalogWorkspace cache;
    char dir[4096];
    char meta[4096];
    int n;
    if (out)
    {
        *out = NULL;
    }
    if (!workspace || !out)
    {
        return 0;
    }
    if (!SessionDir(workspace, dir, sizeof(dir)))
    {
        return 0;
    }
    memset(&cache, 0, sizeof(cache));
    if (CatalogMetaPath(dir, meta, sizeof(meta)))
    {
        (void)CatalogLoadMeta(meta, &cache);
    }
    n = ListSessionsInDir(dir, out, parents_only, &cache, 0);
    CatalogClearSessions(&cache);
    return n;
}

#ifdef PICO_SESSION_TEST_HOOKS
static bool SessionTestFail(const char *stage)
{
    if (PicoSession_TestHook(stage))
    {
        errno = EIO;
        return true;
    }
    return false;
}
#else
static bool SessionTestFail(const char *stage)
{
    (void)stage;
    return false;
}
#endif

static int SessionLockAcquire(const char *session_path, char *error, size_t error_cap)
{
    char lock_path[4102];
    if (!session_path ||
        (size_t)snprintf(lock_path, sizeof(lock_path), "%s.lock", session_path) >= sizeof(lock_path))
    {
        if (error && error_cap > 0)
        {
            snprintf(error, error_cap, "session lock path is too long");
        }
        return -1;
    }
    int fd = open(lock_path, O_RDWR | O_CREAT | O_CLOEXEC, 0600);
    if (fd < 0)
    {
        if (error && error_cap > 0)
        {
            snprintf(error, error_cap, "%s", strerror(errno ? errno : EIO));
        }
        return -1;
    }
    struct flock lock;
    memset(&lock, 0, sizeof(lock));
    lock.l_type = F_WRLCK;
    lock.l_whence = SEEK_SET;
    if (SessionTestFail("lock_before_wait"))
    {
        int failure = errno ? errno : EIO;
        close(fd);
        if (error && error_cap > 0)
        {
            snprintf(error, error_cap, "%s", strerror(failure));
        }
        return -1;
    }
    while (fcntl(fd, F_SETLKW, &lock) != 0)
    {
        if (errno == EINTR)
        {
            continue;
        }
        int failure = errno ? errno : EIO;
        close(fd);
        if (error && error_cap > 0)
        {
            snprintf(error, error_cap, "%s", strerror(failure));
        }
        return -1;
    }
    return fd;
}

static void SessionLockRelease(int fd)
{
    if (fd >= 0)
    {
        close(fd);
    }
}

static bool SyncParentDir(const char *path)
{
    char dir[4096];
    if (!path || (size_t)snprintf(dir, sizeof(dir), "%s", path) >= sizeof(dir))
    {
        errno = ENAMETOOLONG;
        return false;
    }
    char *slash = strrchr(dir, '/');
    if (!slash)
    {
        snprintf(dir, sizeof(dir), ".");
    }
    else if (slash == dir)
    {
        slash[1] = '\0';
    }
    else
    {
        *slash = '\0';
    }
    int fd = open(dir, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (fd < 0)
    {
        return false;
    }
    bool ok = fsync(fd) == 0;
    int failure = ok ? 0 : (errno ? errno : EIO);
    if (close(fd) != 0 && ok)
    {
        ok = false;
        failure = errno ? errno : EIO;
    }
    if (!ok)
    {
        errno = failure;
    }
    return ok;
}

static bool WriteLineAtPath(const char *session_path, const char *json, bool write_catalog,
                            PicoAgentKind kind, PicoSessionPersistence persistence,
                            const char *session_id, const char *workspace_path,
                            char *error, size_t error_cap)
{
    int failure = 0;
    struct stat previous_stat;
    bool have_previous_stat = false;
    int lock_fd;
    if (!session_path || !session_path[0] || !json)
    {
        if (error && error_cap > 0)
        {
            snprintf(error, error_cap, "session path is missing");
        }
        return false;
    }
    lock_fd = SessionLockAcquire(session_path, error, error_cap);
    if (lock_fd < 0)
    {
        return false;
    }
    int fd = open(session_path, O_WRONLY | O_APPEND | O_CREAT, 0600);
    off_t original_size = -1;
    if (fd < 0)
    {
        failure = errno ? errno : EIO;
    }
    else
    {
        original_size = lseek(fd, 0, SEEK_END);
        have_previous_stat = fstat(fd, &previous_stat) == 0;
        size_t len = strlen(json);
        if (SessionTestFail("append_write") || !PicoIO_WriteAll(fd, json, len) ||
            !PicoIO_WriteAll(fd, "\n", 1) || fsync(fd) != 0)
        {
            failure = errno ? errno : EIO;
            if (original_size >= 0)
            {
                int rollback_result = ftruncate(fd, original_size);
                (void)rollback_result;
            }
        }
        if (close(fd) != 0 && failure == 0)
        {
            failure = errno ? errno : EIO;
        }
    }
    if (failure == 0 && write_catalog)
    {
        CatalogWriteThroughFields(kind, persistence, session_id, session_path, workspace_path, NULL, json,
                                  have_previous_stat ? &previous_stat : NULL);
    }
    if (failure == 0 && kind == PICO_AGENT_MAIN && persistence == PICO_SESSION_DURABLE)
    {
        CatalogMarkChanged();
    }
    SessionLockRelease(lock_fd);
    if (failure != 0 && error && error_cap > 0)
    {
        snprintf(error, error_cap, "%s", strerror(failure));
    }
    return failure == 0;
}

static bool WriteLine(PicoHost *app, PicoAgent *agent, const char *json, bool write_catalog,
                      char *error, size_t error_cap)
{
    if (!agent)
    {
        return false;
    }
    return WriteLineAtPath(agent->session_path, json, write_catalog, agent->kind, agent->persistence,
                           agent->session_id, PicoWorkspace_Path(SessionWorkspace(app, agent)), error,
                           error_cap);
}

static void PersistenceFailed(PicoHost *app, PicoAgent *agent, const char *reason)
{
    if (!agent || agent->persistence == PICO_SESSION_FAILED)
    {
        return;
    }
    agent->persistence = PICO_SESSION_FAILED;
    if (app)
    {
        char line[4608];
        snprintf(line, sizeof(line), "Session persistence failed%s%s. This conversation is no longer resumable.",
                 reason && reason[0] ? ": " : "", reason && reason[0] ? reason : "");
        pico_status_warn(app, line);
    }
}

static char *EventPrefix(const char *type)
{
    char id[9];
    char ts[40];
    Pico_RandomHex(id, sizeof(id));
    Pico_IsoTime(ts, sizeof(ts), false);
    JsonBuf b;
    JsonBuf_Init(&b);
    JsonBuf_Puts(&b, "{\"type\":");
    JsonBuf_String(&b, type);
    JsonBuf_Puts(&b, ",\"id\":");
    JsonBuf_String(&b, id);
    JsonBuf_Puts(&b, ",\"timestamp\":");
    JsonBuf_String(&b, ts);
    return JsonBuf_Steal(&b);
}

static char *BuildSessionHeaderJson(PicoHost *app, PicoAgent *agent)
{
    char ts[40];
    JsonBuf b;
    if (!agent)
    {
        return NULL;
    }
    Pico_IsoTime(ts, sizeof(ts), false);
    JsonBuf_Init(&b);
    JsonBuf_Puts(&b, "{\"type\":\"session\",\"version\":4,\"id\":");
    JsonBuf_String(&b, agent->session_id);
    JsonBuf_Puts(&b, ",\"timestamp\":");
    JsonBuf_String(&b, ts);
    JsonBuf_Puts(&b, ",\"cwd\":");
    JsonBuf_String(&b, PicoWorkspace_Path(SessionWorkspace(app, agent)));
    JsonBuf_Puts(&b, ",\"model\":");
    JsonBuf_String(&b, agent->model);
    JsonBuf_Puts(&b, ",\"kind\":");
    JsonBuf_String(&b, agent->kind == PICO_AGENT_SUBAGENT ? "subagent" : "normal");
    if (agent->kind == PICO_AGENT_SUBAGENT)
    {
        JsonBuf_Puts(&b, ",\"profile\":");
        JsonBuf_String(&b, agent->profile);
        JsonBuf_Puts(&b, ",\"initial_purpose\":");
        JsonBuf_String(&b, agent->purpose);
        if (agent->parent_session_id[0])
        {
            JsonBuf_Puts(&b, ",\"parent_session_id\":");
            JsonBuf_String(&b, agent->parent_session_id);
        }
    }
    const char *cache_key = PicoAgent_CacheKey(agent);
    if (cache_key && cache_key[0])
    {
        JsonBuf_Puts(&b, ",\"prompt_cache_key\":");
        JsonBuf_String(&b, cache_key);
    }
    JsonBuf_Putc(&b, '}');
    return JsonBuf_Steal(&b);
}

static char *BuildModelChangeJson(const char *model, const char *effort)
{
    char *pre = EventPrefix("model_change");
    JsonBuf b;
    JsonBuf_Init(&b);
    JsonBuf_Puts(&b, pre);
    JsonBuf_Puts(&b, ",\"model\":");
    JsonBuf_String(&b, model ? model : "");
    if (effort && effort[0])
    {
        JsonBuf_Puts(&b, ",\"effort\":");
        JsonBuf_String(&b, effort);
    }
    JsonBuf_Putc(&b, '}');
    char *line = JsonBuf_Steal(&b);
    free(pre);
    return line;
}

static int AssignSessionIdentity(PicoHost *app, PicoAgent *agent)
{
    char dir[4096];
    char stamp[40];
    if (!agent)
    {
        return -1;
    }
    if (agent->session_path[0])
    {
        return 0;
    }
    if (!SessionDir(SessionWorkspace(app, agent), dir, sizeof(dir)))
    {
        PersistenceFailed(app, agent, "session directory path is too long");
        return -1;
    }
    Pico_RandomHex(agent->session_id, sizeof(agent->session_id));
    Pico_IsoTime(stamp, sizeof(stamp), true);
    if ((size_t)snprintf(agent->session_path, sizeof(agent->session_path), "%s/%s_%s.jsonl",
                         dir, stamp, agent->session_id) >= sizeof(agent->session_path))
    {
        PersistenceFailed(app, agent, "session path is too long");
        return -1;
    }
    PicoWorkspace *ws = agent->workspace;
    if (ws && !PicoWorkspace_ReserveSession(ws, agent->id, agent->session_path))
    {
        PersistenceFailed(app, agent, "session path is already reserved");
        return -1;
    }
    return 0;
}

static bool EnsureSessionParent(const char *session_path, char *error, size_t error_cap)
{
    char dir[4096];
    struct stat st;
    char *slash;
    if (!session_path ||
        (size_t)snprintf(dir, sizeof(dir), "%s", session_path) >= sizeof(dir) ||
        !(slash = strrchr(dir, '/')))
    {
        if (error && error_cap > 0)
        {
            snprintf(error, error_cap, "session directory path is invalid");
        }
        return false;
    }
    *slash = '\0';
    Pico_MkdirP(dir);
    if (stat(dir, &st) != 0)
    {
        if (error && error_cap > 0)
        {
            snprintf(error, error_cap, "%s", strerror(errno ? errno : EIO));
        }
        return false;
    }
    if (!S_ISDIR(st.st_mode))
    {
        if (error && error_cap > 0)
        {
            snprintf(error, error_cap, "%s", strerror(ENOTDIR));
        }
        return false;
    }
    return true;
}

static int CreateNew(PicoHost *app, PicoAgent *agent)
{
    char error[256] = {0};
    if (AssignSessionIdentity(app, agent) != 0)
    {
        return -1;
    }
    if (!EnsureSessionParent(agent->session_path, error, sizeof(error)))
    {
        PersistenceFailed(app, agent, error);
        return -1;
    }
    char *line = BuildSessionHeaderJson(app, agent);
    if (!line)
    {
        PersistenceFailed(app, agent, "out of memory while creating the session header");
        return -1;
    }
    bool wrote = WriteLine(app, agent, line, false, error, sizeof(error));
    free(line);
    if (!wrote)
    {
        PersistenceFailed(app, agent, error);
        return -1;
    }
    return 0;
}

static PicoSessionWriteResult AppendLine(PicoHost *app, PicoAgent *agent, const char *json)
{
    if (!app || !agent || !json || !json[0])
    {
        return PICO_SESSION_WRITE_FAILED;
    }
    if (agent->persistence == PICO_SESSION_EPHEMERAL)
    {
        return PICO_SESSION_WRITE_SKIPPED;
    }
    /* With a persist thread, session appends are queued and written off the
     * calling thread so the UI never blocks on the session lock or fsync. */
    if (app->persist_ready)
    {
        return QueueSessionLine(app, agent, json);
    }
    PicoSession_DrainPersist(app, agent);
    if (agent->persistence == PICO_SESSION_FAILED)
    {
        return PICO_SESSION_WRITE_FAILED;
    }
    if (!agent->session_path[0] && CreateNew(app, agent) != 0)
    {
        return PICO_SESSION_WRITE_FAILED;
    }
    if (!agent->session_path[0])
    {
        PersistenceFailed(app, agent, "session path was not created");
        return PICO_SESSION_WRITE_FAILED;
    }
    char error[256] = {0};
    if (!WriteLine(app, agent, json, true, error, sizeof(error)))
    {
        PersistenceFailed(app, agent, error);
        return PICO_SESSION_WRITE_FAILED;
    }
    return PICO_SESSION_WRITE_OK;
}

static void ApplyHeader(PicoHost *app, PicoAgent *agent, const JsonDoc *doc, int obj)
{
    char *profile = JsonObjStr(doc, obj, "profile");
    if (profile)
    {
        snprintf(agent->profile, sizeof(agent->profile), "%s", profile);
    }
    free(profile);
    char *purpose = JsonObjStr(doc, obj, "initial_purpose");
    if (purpose)
    {
        snprintf(agent->purpose, sizeof(agent->purpose), "%s", purpose);
    }
    free(purpose);
    char *parent_session = JsonObjStr(doc, obj, "parent_session_id");
    if (parent_session)
    {
        snprintf(agent->parent_session_id, sizeof(agent->parent_session_id), "%s", parent_session);
    }
    free(parent_session);
    char *id = JsonObjStr(doc, obj, "id");
    if (id && id[0])
    {
        snprintf(agent->session_id, sizeof(agent->session_id), "%s", id);
    }
    free(id);
    char *model = JsonObjStr(doc, obj, "model");
    if (model && model[0])
    {
        snprintf(agent->model, sizeof(agent->model), "%s", model);
        agent->effort[0] = '\0';
        PicoSettings_SyncAgent(agent);
    }
    free(model);
    char *key = JsonObjStr(doc, obj, "prompt_cache_key");
    if (key && key[0] && agent->runtime)
    {
        PicoAgent_SetCacheKey(agent, key);
    }
    free(key);
}

static bool StartsAssistantGroup(const PicoMessage *messages, int count,
                                 int message_group, int *active_group)
{
    bool starts = !active_group || *active_group != message_group || count <= 0 ||
                  messages[count - 1].role != PICO_ROLE_ASSISTANT;
    if (active_group)
    {
        *active_group = message_group;
    }
    return starts;
}

static bool HasNonWhitespace(const char *text)
{
    if (!text)
    {
        return false;
    }
    while (*text == ' ' || *text == '\n' || *text == '\t' || *text == '\r')
    {
        text++;
    }
    return *text != '\0';
}

static bool JsonObjNonNegativeInt(const JsonDoc *doc, int obj, const char *key, int *out)
{
    int tok = JsonObjGet(doc, obj, key);
    int start = JsonTokStart(doc, tok);
    int end = JsonTokEnd(doc, tok);
    if (tok < 0 || start < 0 || end <= start || (size_t)end > doc->len ||
        (start > 0 && (size_t)end < doc->len &&
         doc->src[start - 1] == '"' && doc->src[end] == '"'))
    {
        return false;
    }
    int value = 0;
    for (int i = start; i < end; i++)
    {
        unsigned char c = (unsigned char)doc->src[i];
        if (c < '0' || c > '9' || value > (INT_MAX - (int)(c - '0')) / 10)
        {
            return false;
        }
        value = value * 10 + (int)(c - '0');
    }
    if (out)
    {
        *out = value;
    }
    return true;
}

static void ApplyToolDetails(PicoHost *app, PicoAgent *agent, const char *name,
                             const char *details, bool is_error)
{
    size_t details_len = details ? strlen(details) : 0;
    if (!app || is_error || !name || !details || details_len > PICO_TOOL_DETAILS_MAX ||
        !JsonValidUtf8(details, details_len))
    {
        return;
    }
    PicoWorkspace *ws = agent ? agent->workspace : PicoHost_SelectedWorkspace(app);
    if (!ws)
    {
        return;
    }
    const PicoRegistrationGeneration *registration = ws->active_registration;
    const PicoTool *tools = registration ? registration->tools : ws->tools;
    int tool_count = registration ? registration->tool_count : ws->tool_count;
    for (int i = 0; i < tool_count; i++)
    {
        const PicoTool *tool = &tools[i];
        if (tool->name && strcmp(tool->name, name) == 0)
        {
            if (tool->apply)
            {
                (void)tool->apply(ws, agent->id, details, true, tool->state);
            }
            return;
        }
    }
}

static bool ReplayThinkParts(PicoHost *app, PicoAgent *agent, const JsonDoc *doc, int obj,
                             int thinking_ms)
{
    int parts = JsonObjGet(doc, obj, "thinking_parts");
    if (!JsonIsArray(doc, parts))
    {
        return false;
    }
    int count = JsonArrayLen(doc, parts);
    bool restored = false;
    for (int i = 0; i < count; i++)
    {
        char *text = JsonStrDup(doc, JsonArrayAt(doc, parts, i));
        if (text && text[0])
        {
            PicoAgent_AppendThinkSummary(app, agent, text, i + 1, thinking_ms);
            restored = true;
        }
        free(text);
    }
    return restored;
}

static void ReplayLine(PicoHost *app, PicoAgent *agent, const JsonDoc *doc, int obj,
                       bool into_input, int *active_group)
{
    char *type = JsonObjStr(doc, obj, "type");
    if (!type)
    {
        return;
    }
    if (strcmp(type, "session") == 0)
    {
        ApplyHeader(app, agent, doc, obj);
    }
    else if (strcmp(type, "usage") == 0)
    {
        int input_tokens = JsonObjInt(doc, obj, "input_tokens", 0);
        int cached_tokens = JsonObjInt(doc, obj, "cached_tokens", 0);
        PicoUsage_Apply(agent, input_tokens, cached_tokens, NULL);
    }
    else if (strcmp(type, "message") == 0)
    {
        char *role = JsonObjStr(doc, obj, "role");
        char *content = JsonObjStr(doc, obj, "content");
        if (role && strcmp(role, "user") == 0)
        {
            if (active_group)
            {
                *active_group = -1;
            }
            char *display = JsonObjStr(doc, obj, "display");
            char *parts = JsonObjRaw(doc, obj, "parts");
            PicoAgent_AddMessage(app, agent, PICO_ROLE_USER,
                               display && display[0] ? display : (content ? content : ""));
            if (into_input)
            {
                if (parts && parts[0] == '[')
                {
                    PicoAgent_PushHistoryUserParts(agent, content ? content : "", parts);
                }
                else
                {
                    PicoAgent_PushHistoryUser(agent, content ? content : "");
                }
            }
            free(display);
            free(parts);
        }
        else if (role && strcmp(role, "assistant") == 0)
        {
            const char *text = content ? content : "";
            int message_group = -1;
            (void)JsonObjNonNegativeInt(doc, obj, "message_group", &message_group);
            if (StartsAssistantGroup(agent->messages, agent->message_count,
                                     message_group, active_group))
            {
                PicoAgent_AddMessage(app, agent, PICO_ROLE_ASSISTANT, text);
            }
            else
            {
                PicoAgent_AppendAssistant(app, agent, text);
            }
            char *thinking = JsonObjStr(doc, obj, "thinking");
            char *signature = JsonObjStr(doc, obj, "thinking_signature");
            char *parts = JsonObjRaw(doc, obj, "parts");
            int thinking_ms = 0;
            (void)JsonObjNonNegativeInt(doc, obj, "thinking_ms", &thinking_ms);
            bool restored_summary = ReplayThinkParts(app, agent, doc, obj, thinking_ms);
            if (!restored_summary && thinking && thinking[0])
            {
                PicoAgent_AppendThink(app, agent, thinking, thinking_ms);
            }
            if (into_input &&
                ((content && content[0]) || (thinking && thinking[0]) || (signature && signature[0]) ||
                 (parts && parts[0] == '[')))
            {
                PicoAgent_PushHistoryAssistantParts(agent, content, thinking, signature, parts);
            }
            free(thinking);
            free(signature);
            free(parts);
        }
        free(role);
        free(content);
    }
    else if (strcmp(type, "tool_call") == 0)
    {
        char *call_id = JsonObjStr(doc, obj, "call_id");
        char *name = JsonObjStr(doc, obj, "name");
        char *args = JsonObjStr(doc, obj, "arguments");
        char *item_id = JsonObjStr(doc, obj, "item_id");
        int message_group = -1;
        (void)JsonObjNonNegativeInt(doc, obj, "message_group", &message_group);
        if (StartsAssistantGroup(agent->messages, agent->message_count,
                                 message_group, active_group))
        {
            PicoAgent_AddMessage(app, agent, PICO_ROLE_ASSISTANT, "");
        }
        PicoAgent_AddToolCallWithId(app, agent, call_id, name, args);
        if (into_input)
        {
            PicoAgent_PushHistoryFunctionCall(agent, call_id, name, args, item_id);
        }
        free(call_id);
        free(name);
        free(args);
        free(item_id);
    }
    else if (strcmp(type, "tool_result") == 0)
    {
        char *call_id = JsonObjStr(doc, obj, "call_id");
        char *name = JsonObjStr(doc, obj, "name");
        char *output = JsonObjStr(doc, obj, "output");
        bool is_error = JsonEq(doc, JsonObjGet(doc, obj, "is_error"), "true");
        char *details = NULL;
        int details_tok = JsonObjGet(doc, obj, "details");
        if (JsonIsObject(doc, details_tok))
        {
            details = JsonRawDup(doc, details_tok);
        }
        ApplyToolDetails(app, agent, name, details, is_error);
        PicoAgent_SetToolOutputByCallId(agent, call_id, output, is_error);
        if (into_input)
        {
            PicoAgent_PushHistoryFunctionOutput(agent, call_id, name, output, is_error);
        }
        free(call_id);
        free(name);
        free(output);
        free(details);
    }
    else if (strcmp(type, "compaction") == 0)
    {
        agent->tokens_used = 0;
        agent->tokens_cached = 0;
        char *summary = JsonObjStr(doc, obj, "summary");
        if (into_input)
        {
            PicoAgent_ClearInput(agent);
            JsonBuf b;
            JsonBuf_Init(&b);
            JsonBuf_Puts(&b, "Briefing:\n");
            JsonBuf_Puts(&b, summary ? summary : "");
            PicoAgent_PushHistoryUser(agent, b.data ? b.data : "Briefing:\n");
            JsonBuf_Free(&b);
        }
        free(summary);
    }
    else if (strcmp(type, "model_change") == 0)
    {
        char *model = JsonObjStr(doc, obj, "model");
        char *effort = JsonObjStr(doc, obj, "effort");
        if (model && model[0])
        {
            snprintf(agent->model, sizeof(agent->model), "%s", model);
        }
        if (effort && effort[0])
        {
            snprintf(agent->effort, sizeof(agent->effort), "%s", effort);
        }
        PicoSettings_SyncAgent(agent);
        free(model);
        free(effort);
    }
    else if (strcmp(type, "unseen_complete") == 0)
    {
        int tok = JsonObjGet(doc, obj, "complete");
        agent->unseen_complete = JsonEq(doc, tok, "true") || JsonEq(doc, tok, "1");
    }
    free(type);
}

int PicoSession_Replay(PicoHost *app, PicoAgent *agent, const char *path,
                       bool append_interrupted)
{
    FILE *f = fopen(path, "rb");
    if (!f)
    {
        return -1;
    }
    snprintf(agent->session_path, sizeof(agent->session_path), "%s", path);

    char **lines = NULL;
    int n = 0;
    int cap = 0;
    char *buf = NULL;
    size_t buf_cap = 0;
    bool read_failed = false;
    while (getline(&buf, &buf_cap, f) != -1)
    {
        size_t len = strlen(buf);
        while (len > 0 && (buf[len - 1] == '\n' || buf[len - 1] == '\r'))
        {
            buf[--len] = '\0';
        }
        if (len == 0)
        {
            continue;
        }
        if (n >= cap)
        {
            cap = cap == 0 ? 32 : cap * 2;
            char **next = (char **)realloc(lines, (size_t)cap * sizeof(char *));
            if (!next)
            {
                read_failed = true;
                break;
            }
            lines = next;
        }
        char *copy = JsonDup(buf);
        if (!copy)
        {
            read_failed = true;
            break;
        }
        lines[n++] = copy;
    }
    if (ferror(f))
    {
        read_failed = true;
    }
    free(buf);
    fclose(f);
    if (read_failed || n == 0)
    {
        goto invalid;
    }

    int last_compact = -1;
    int last_tool_call = -1;
    int last_tool_result = -1;
    bool valid_header = false;
    for (int i = 0; i < n; i++)
    {
        JsonDoc doc;
        memset(&doc, 0, sizeof(doc));
        if (JsonParse(&doc, lines[i], strlen(lines[i])) != 0 || !JsonIsObject(&doc, 0))
        {
            if (doc.toks)
            {
                JsonFree(&doc);
            }
            goto invalid;
        }
        char *type = JsonObjStr(&doc, 0, "type");
        if (i == 0)
        {
            char *header_id = JsonObjStr(&doc, 0, "id");
            char *kind = JsonObjStr(&doc, 0, "kind");
            char *profile = JsonObjStr(&doc, 0, "profile");
            char *purpose = JsonObjStr(&doc, 0, "initial_purpose");
            int version = JsonObjInt(&doc, 0, "version", 0);
            bool normal = kind && strcmp(kind, "normal") == 0;
            bool subagent = kind && strcmp(kind, "subagent") == 0 &&
                            profile && profile[0] && purpose && purpose[0];
            bool compatible_kind = (normal && agent->kind == PICO_AGENT_MAIN) ||
                                   (subagent && agent->kind == PICO_AGENT_SUBAGENT);
            valid_header = type && strcmp(type, "session") == 0 &&
                           header_id && header_id[0] && version == 4 &&
                           compatible_kind;
            free(header_id);
            free(kind);
            free(profile);
            free(purpose);
            if (!valid_header)
            {
                free(type);
                JsonFree(&doc);
                goto invalid;
            }
        }
        bool requires_group = type && strcmp(type, "tool_call") == 0;
        if (type && strcmp(type, "message") == 0)
        {
            char *role = JsonObjStr(&doc, 0, "role");
            requires_group = role && strcmp(role, "assistant") == 0;
            free(role);
        }
        if (requires_group && !JsonObjNonNegativeInt(&doc, 0, "message_group", NULL))
        {
            free(type);
            JsonFree(&doc);
            goto invalid;
        }
        if (type && strcmp(type, "compaction") == 0)
        {
            last_compact = i;
        }
        else if (type && strcmp(type, "tool_call") == 0)
        {
            last_tool_call = i;
        }
        else if (type && strcmp(type, "tool_result") == 0)
        {
            last_tool_result = i;
        }
        free(type);
        JsonFree(&doc);
    }
    if (!valid_header)
    {
        goto invalid;
    }

    PicoAgent_ClearInput(agent);
    int active_group = -1;
    for (int i = 0; i < n; i++)
    {
        JsonDoc doc;
        if (JsonParse(&doc, lines[i], strlen(lines[i])) != 0)
        {
            continue;
        }
        bool into_input = (last_compact < 0) || (i >= last_compact);
        ReplayLine(app, agent, &doc, 0, into_input, &active_group);
        JsonFree(&doc);
    }

    for (int i = 0; i < n; i++)
    {
        free(lines[i]);
    }
    free(lines);
    app->chat_follow_bottom = true;
    if (append_interrupted && last_tool_call > last_tool_result)
    {
        PicoSession_AppendInterrupted(app, agent);
    }
    return 0;

invalid:
    for (int i = 0; i < n; i++)
    {
        free(lines[i]);
    }
    free(lines);
    agent->session_path[0] = '\0';
    return -1;
}

void PicoSession_AppendInterrupted(PicoHost *app, PicoAgent *agent)
{
    if (!app || !agent || !agent->session_path[0])
    {
        return;
    }
    FILE *f = fopen(agent->session_path, "rb");
    if (!f)
    {
        return;
    }
    char *line = NULL;
    size_t cap = 0;
    char *last_call = NULL;
    int calls = 0;
    int results = 0;
    while (getline(&line, &cap, f) != -1)
    {
        JsonDoc doc;
        if (JsonParse(&doc, line, strlen(line)) != 0)
        {
            continue;
        }
        if (JsonEq(&doc, JsonObjGet(&doc, 0, "type"), "tool_call"))
        {
            calls++;
            free(last_call);
            last_call = JsonDup(line);
        }
        else if (JsonEq(&doc, JsonObjGet(&doc, 0, "type"), "tool_result"))
        {
            results++;
        }
        JsonFree(&doc);
    }
    free(line);
    fclose(f);
    if (calls > results && last_call)
    {
        JsonDoc doc;
        if (JsonParse(&doc, last_call, strlen(last_call)) == 0)
        {
            char *call_id = JsonObjStr(&doc, 0, "call_id");
            char *name = JsonObjStr(&doc, 0, "name");
            PicoSession_LogToolResult(app, agent, call_id, name, "(interrupted)", true, NULL);
            PicoAgent_SetLastToolOutput(agent, "(interrupted)", true);
            PicoAgent_PushHistoryFunctionOutput(agent, call_id, name, "(interrupted)", true);
            free(call_id);
            free(name);
            JsonFree(&doc);
        }
    }
    free(last_call);
}

void PicoSession_ReplayToolDetails(PicoHost *app, PicoAgent *agent)
{
    if (!app || !agent->session_path[0])
    {
        return;
    }
    if (!DrainPersistUiBound(app, agent))
    {
        pico_status_warn(app, "Session writes are still pending; replayed tool state may be incomplete.");
    }
    FILE *f = fopen(agent->session_path, "rb");
    if (!f)
    {
        return;
    }
    char *line = NULL;
    size_t cap = 0;
    while (getline(&line, &cap, f) != -1)
    {
        size_t len = strlen(line);
        while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r'))
        {
            line[--len] = '\0';
        }
        JsonDoc doc;
        if (len == 0 || JsonParse(&doc, line, len) != 0)
        {
            continue;
        }
        if (JsonEq(&doc, JsonObjGet(&doc, 0, "type"), "tool_result"))
        {
            char *name = JsonObjStr(&doc, 0, "name");
            bool is_error = JsonEq(&doc, JsonObjGet(&doc, 0, "is_error"), "true");
            char *details = NULL;
            int details_tok = JsonObjGet(&doc, 0, "details");
            if (JsonIsObject(&doc, details_tok))
            {
                details = JsonRawDup(&doc, details_tok);
            }
            ApplyToolDetails(app, agent, name, details, is_error);
            free(name);
            free(details);
        }
        JsonFree(&doc);
    }
    free(line);
    fclose(f);
}

void PicoSession_Start(PicoHost *app, PicoAgent *agent, PicoSessionStart start, const char *session_file)
{
    if (!app || !agent)
    {
        return;
    }
    if (start == PICO_SESSION_NONE)
    {
        agent->persistence = PICO_SESSION_EPHEMERAL;
        agent->session_path[0] = '\0';
        return;
    }
    agent->persistence = PICO_SESSION_DURABLE;
    PicoWorkspace *ws = agent->workspace;
    if (session_file && session_file[0])
    {
        char canonical[4096];
        if (!realpath(session_file, canonical) ||
            (ws && !PicoWorkspace_ReserveSession(ws, agent->id, canonical)) ||
            PicoSession_Replay(app, agent, canonical, true) != 0)
        {
            agent->persistence = PICO_SESSION_FAILED;
            pico_status_warn(app, "Could not open the requested session file.");
        }
        return;
    }
    if (start == PICO_SESSION_RESUME || (ws && ws->settings.resume_last))
    {
        char dir[4096];
        char latest[4096];
        if (SessionDir(SessionWorkspace(app, agent), dir, sizeof(dir)) &&
            FindLatest(dir, latest, sizeof(latest)) == 0)
        {
            char canonical[4096];
            if (realpath(latest, canonical) &&
                (!ws || PicoWorkspace_ReserveSession(ws, agent->id, canonical)))
            {
                (void)PicoSession_Replay(app, agent, canonical, true);
            }
        }
    }
}

int PicoSession_ReadHeader(const char *path, PicoSessionHeader *out)
{
    if (!path || !out)
    {
        return -1;
    }
    memset(out, 0, sizeof(*out));
    FILE *f = fopen(path, "rb");
    if (!f)
    {
        return -1;
    }
    char *line = NULL;
    size_t cap = 0;
    ssize_t got = getline(&line, &cap, f);
    fclose(f);
    if (got <= 0)
    {
        free(line);
        return -1;
    }
    JsonDoc doc;
    if (JsonParse(&doc, line, (size_t)got) != 0)
    {
        free(line);
        return -1;
    }
    if (!JsonEq(&doc, JsonObjGet(&doc, 0, "type"), "session"))
    {
        JsonFree(&doc);
        free(line);
        return -1;
    }
    out->version = JsonObjInt(&doc, 0, "version", 0);
    char *id = JsonObjStr(&doc, 0, "id");
    char *kind = JsonObjStr(&doc, 0, "kind");
    char *profile = JsonObjStr(&doc, 0, "profile");
    char *purpose = JsonObjStr(&doc, 0, "initial_purpose");
    char *parent = JsonObjStr(&doc, 0, "parent_session_id");
    char *model = JsonObjStr(&doc, 0, "model");
    char *title = JsonObjStr(&doc, 0, "title");
    if (id) snprintf(out->id, sizeof(out->id), "%s", id);
    bool kind_valid = kind && (strcmp(kind, "normal") == 0 || strcmp(kind, "subagent") == 0);
    out->kind = kind && strcmp(kind, "subagent") == 0 ? PICO_AGENT_SUBAGENT : PICO_AGENT_MAIN;
    if (profile) snprintf(out->profile, sizeof(out->profile), "%s", profile);
    if (purpose) snprintf(out->initial_purpose, sizeof(out->initial_purpose), "%s", purpose);
    if (parent) snprintf(out->parent_session_id, sizeof(out->parent_session_id), "%s", parent);
    if (model) snprintf(out->model, sizeof(out->model), "%s", model);
    if (title) snprintf(out->title, sizeof(out->title), "%s", title);
    bool valid = out->version == 4 && out->id[0] && kind_valid &&
                 (out->kind == PICO_AGENT_MAIN || (out->profile[0] && out->initial_purpose[0]));
    free(id);
    free(kind);
    free(profile);
    free(purpose);
    free(parent);
    free(model);
    free(title);
    JsonFree(&doc);
    free(line);
    return valid ? 0 : -1;
}

void PicoSession_CopyDisplayTitle(const PicoAgent *agent, char *out, size_t cap)
{
    PicoSessionHeader header;
    PicoSessionInfo info;
    int i;
    if (!out || cap == 0)
    {
        return;
    }
    out[0] = '\0';
    if (agent && agent->session_path[0] && PicoSession_ReadHeader(agent->session_path, &header) == 0 &&
        header.title[0])
    {
        snprintf(out, cap, "%s", header.title);
        return;
    }
    if (agent)
    {
        for (i = 0; i < agent->message_count; i++)
        {
            if (agent->messages[i].role == PICO_ROLE_USER)
            {
                MakeTitle(out, cap, agent->messages[i].source);
                return;
            }
        }
    }
    if (agent && agent->session_path[0])
    {
        memset(&info, 0, sizeof(info));
        ScanSessionFile(agent->session_path, &info, false);
        if (info.title[0])
        {
            snprintf(out, cap, "%s", info.title);
            return;
        }
    }
    snprintf(out, cap, "Untitled");
}

static void LoadedTranscriptFree(PicoMessage *messages, int count)
{
    if (!messages)
    {
        return;
    }
    for (int i = 0; i < count; i++)
    {
        free(messages[i].source);
        for (int t = 0; t < messages[i].trace_count; t++)
        {
            PicoTraceLine_Release(&messages[i].trace[t]);
        }
        free(messages[i].trace);
    }
    free(messages);
}

static bool LoadedAddMessage(PicoMessage **messages, int *count, int *capacity,
                             PicoRole role, const char *text)
{
    if (*count >= *capacity)
    {
        int next = *capacity == 0 ? 8 : *capacity * 2;
        PicoMessage *grown = (PicoMessage *)realloc(*messages, (size_t)next * sizeof(PicoMessage));
        if (!grown)
        {
            return false;
        }
        *messages = grown;
        *capacity = next;
    }
    PicoMessage *msg = &(*messages)[(*count)++];
    memset(msg, 0, sizeof(*msg));
    msg->role = role;
    msg->source = JsonDup(text ? text : "");
    return msg->source != NULL;
}

static bool LoadedAppendAssistant(PicoMessage **messages, int *count, int *capacity,
                                  int message_group, int *active_group, const char *text)
{
    if (StartsAssistantGroup(*messages, *count, message_group, active_group))
    {
        return LoadedAddMessage(messages, count, capacity, PICO_ROLE_ASSISTANT, text);
    }
    if (!text || !text[0])
    {
        return true;
    }
    PicoMessage *msg = &(*messages)[*count - 1];
    size_t old = msg->source ? strlen(msg->source) : 0;
    size_t n = strlen(text);
    char *next = (char *)realloc(msg->source, old + n + 1);
    if (!next)
    {
        return false;
    }
    memcpy(next + old, text, n + 1);
    msg->source = next;
    return true;
}

static bool LoadedAddTool(PicoMessage **messages, int *count, int *capacity,
                          int message_group, int *active_group, const char *call_id,
                          const char *name, const char *args)
{
    if (StartsAssistantGroup(*messages, *count, message_group, active_group))
    {
        if (!LoadedAddMessage(messages, count, capacity, PICO_ROLE_ASSISTANT, ""))
        {
            return false;
        }
    }
    PicoMessage *msg = &(*messages)[*count - 1];
    PicoTraceLine *next =
        (PicoTraceLine *)realloc(msg->trace, (size_t)(msg->trace_count + 1) * sizeof(PicoTraceLine));
    if (!next)
    {
        return false;
    }
    msg->trace = next;
    PicoTraceLine *line = &msg->trace[msg->trace_count++];
    memset(line, 0, sizeof(*line));
    line->is_tool = true;
    line->tool_name = JsonDup(name && name[0] ? name : "tool");
    line->tool_call_id = call_id && call_id[0] ? JsonDup(call_id) : NULL;
    line->tool_args = PicoAgent_FormatToolArgs(line->tool_name, args);
    line->tool_args_json = JsonDup(args ? args : "");
    return line->tool_name != NULL && line->tool_args != NULL &&
           line->tool_args_json != NULL &&
           (!call_id || !call_id[0] || line->tool_call_id != NULL);
}

static bool LoadedAddThink(PicoMessage **messages, int *count, int *capacity,
                           const char *text, int think_ms)
{
    PicoMessage *msg;
    PicoTraceLine *line = NULL;
    size_t old;
    size_t n;
    char *next_text;

    if (!text || !text[0])
    {
        return true;
    }
    if (*count <= 0 || (*messages)[*count - 1].role != PICO_ROLE_ASSISTANT)
    {
        if (!LoadedAddMessage(messages, count, capacity, PICO_ROLE_ASSISTANT, ""))
        {
            return false;
        }
    }
    msg = &(*messages)[*count - 1];
    if (msg->trace_count > 0 && !msg->trace[msg->trace_count - 1].is_tool &&
        msg->trace[msg->trace_count - 1].think_steps == 0)
    {
        line = &msg->trace[msg->trace_count - 1];
        if (HasNonWhitespace(line->text))
        {
            return true;
        }
    }
    else
    {
        PicoTraceLine *next =
            (PicoTraceLine *)realloc(msg->trace, (size_t)(msg->trace_count + 1) * sizeof(PicoTraceLine));
        if (!next)
        {
            return false;
        }
        msg->trace = next;
        line = &msg->trace[msg->trace_count++];
        memset(line, 0, sizeof(*line));
    }
    old = line->text ? strlen(line->text) : 0;
    n = strlen(text);
    next_text = (char *)realloc(line->text, old + n + 1);
    if (!next_text)
    {
        return false;
    }
    memcpy(next_text + old, text, n + 1);
    line->text = next_text;
    if (think_ms > 0 && line->think_ms == 0)
    {
        line->think_ms = think_ms;
    }
    return true;
}

static bool LoadedAddThinkParts(PicoMessage **messages, int *count, int *capacity,
                                const JsonDoc *doc, int obj, int think_ms, bool *restored)
{
    if (restored)
    {
        *restored = false;
    }
    int parts = JsonObjGet(doc, obj, "thinking_parts");
    if (!JsonIsArray(doc, parts))
    {
        return true;
    }
    int part_count = JsonArrayLen(doc, parts);
    if (part_count <= 0)
    {
        return true;
    }
    char **copies = (char **)calloc((size_t)part_count, sizeof(char *));
    if (!copies)
    {
        return false;
    }
    bool ok = true;
    for (int i = 0; i < part_count; i++)
    {
        copies[i] = JsonStrDup(doc, JsonArrayAt(doc, parts, i));
        if (!copies[i])
        {
            ok = false;
            break;
        }
    }
    if (!ok)
    {
        for (int i = 0; i < part_count; i++)
        {
            free(copies[i]);
        }
        free(copies);
        return false;
    }
    if (*count <= 0 || (*messages)[*count - 1].role != PICO_ROLE_ASSISTANT)
    {
        if (!LoadedAddMessage(messages, count, capacity, PICO_ROLE_ASSISTANT, ""))
        {
            for (int i = 0; i < part_count; i++)
            {
                free(copies[i]);
            }
            free(copies);
            return false;
        }
    }
    PicoMessage *msg = &(*messages)[*count - 1];
    PicoTraceLine *line = NULL;
    if (msg->trace_count > 0 && !msg->trace[msg->trace_count - 1].is_tool &&
        msg->trace[msg->trace_count - 1].think_steps > 0)
    {
        line = &msg->trace[msg->trace_count - 1];
        PicoTraceLine_Release(line);
    }
    else
    {
        PicoTraceLine *next =
            (PicoTraceLine *)realloc(msg->trace, (size_t)(msg->trace_count + 1) * sizeof(PicoTraceLine));
        if (!next)
        {
            for (int i = 0; i < part_count; i++)
            {
                free(copies[i]);
            }
            free(copies);
            return false;
        }
        msg->trace = next;
        line = &msg->trace[msg->trace_count++];
        memset(line, 0, sizeof(*line));
    }
    line->think_parts = copies;
    line->think_part_count = part_count;
    line->think_steps = part_count;
    line->think_ms = think_ms > 0 ? think_ms : 0;
    line->text = JsonDup(copies[part_count - 1]);
    if (!line->text)
    {
        PicoTraceLine_Release(line);
        return false;
    }
    if (restored)
    {
        *restored = true;
    }
    return true;
}

static void LoadedSetOutput(PicoMessage *messages, int count, const char *call_id,
                            const char *output, bool is_error)
{
    if (count <= 0)
    {
        return;
    }
    for (int i = count - 1; i >= 0; i--)
    {
        PicoMessage *msg = &messages[i];
        for (int t = msg->trace_count - 1; t >= 0; t--)
        {
            PicoTraceLine *line = &msg->trace[t];
            if (line->is_tool && call_id && call_id[0] && line->tool_call_id &&
                strcmp(line->tool_call_id, call_id) == 0)
            {
                free(line->tool_output);
                line->tool_output = JsonDup(output ? output : "");
                line->tool_error = is_error;
                return;
            }
        }
    }
}

int PicoSession_LoadTranscript(const PicoWorkspace *workspace, const char *id,
                               PicoMessage **out, int *out_count)
{
    if (out)
    {
        *out = NULL;
    }
    if (out_count)
    {
        *out_count = 0;
    }
    if (!workspace || !id || !id[0] || !out || !out_count)
    {
        return -1;
    }
    char path[4096];
    if (PicoSession_Resolve(workspace, id, false, path, sizeof(path)) != 0)
    {
        return -1;
    }
    FILE *f = fopen(path, "rb");
    if (!f)
    {
        return -1;
    }
    PicoMessage *messages = NULL;
    int count = 0;
    int capacity = 0;
    char *buf = NULL;
    size_t buf_cap = 0;
    bool failed = false;
    bool valid_header = false;
    int active_group = -1;
    while (getline(&buf, &buf_cap, f) != -1)
    {
        size_t len = strlen(buf);
        while (len > 0 && (buf[len - 1] == '\n' || buf[len - 1] == '\r'))
        {
            buf[--len] = '\0';
        }
        if (len == 0)
        {
            continue;
        }
        JsonDoc doc;
        if (JsonParse(&doc, buf, len) != 0 || !JsonIsObject(&doc, 0))
        {
            if (doc.toks)
            {
                JsonFree(&doc);
            }
            continue;
        }
        char *type = JsonObjStr(&doc, 0, "type");
        if (type && strcmp(type, "session") == 0)
        {
            valid_header = JsonObjInt(&doc, 0, "version", 0) == 4;
        }
        else if (type && strcmp(type, "message") == 0)
        {
            char *role = JsonObjStr(&doc, 0, "role");
            char *content = JsonObjStr(&doc, 0, "content");
            if (role && strcmp(role, "user") == 0)
            {
                active_group = -1;
                char *display = JsonObjStr(&doc, 0, "display");
                const char *text = display && display[0] ? display : (content ? content : "");
                failed = !LoadedAddMessage(&messages, &count, &capacity, PICO_ROLE_USER, text);
                free(display);
            }
            else if (role && strcmp(role, "assistant") == 0)
            {
                char *thinking = JsonObjStr(&doc, 0, "thinking");
                int message_group = -1;
                int thinking_ms = 0;
                (void)JsonObjNonNegativeInt(&doc, 0, "thinking_ms", &thinking_ms);
                bool restored_summary = false;
                failed = !JsonObjNonNegativeInt(&doc, 0, "message_group", &message_group) ||
                         !LoadedAppendAssistant(&messages, &count, &capacity,
                                                message_group, &active_group,
                                                content ? content : "") ||
                         !LoadedAddThinkParts(&messages, &count, &capacity, &doc, 0,
                                              thinking_ms, &restored_summary) ||
                         (!restored_summary &&
                          !LoadedAddThink(&messages, &count, &capacity, thinking, thinking_ms));
                free(thinking);
            }
            free(role);
            free(content);
        }
        else if (type && strcmp(type, "tool_call") == 0)
        {
            char *call_id = JsonObjStr(&doc, 0, "call_id");
            char *name = JsonObjStr(&doc, 0, "name");
            char *args = JsonObjStr(&doc, 0, "arguments");
            int message_group = -1;
            failed = !JsonObjNonNegativeInt(&doc, 0, "message_group", &message_group) ||
                     !LoadedAddTool(&messages, &count, &capacity, message_group,
                                    &active_group, call_id, name, args);
            free(call_id);
            free(name);
            free(args);
        }
        else if (type && strcmp(type, "tool_result") == 0)
        {
            char *call_id = JsonObjStr(&doc, 0, "call_id");
            char *output = JsonObjStr(&doc, 0, "output");
            bool is_error = JsonEq(&doc, JsonObjGet(&doc, 0, "is_error"), "true");
            LoadedSetOutput(messages, count, call_id, output, is_error);
            free(call_id);
            free(output);
        }
        free(type);
        JsonFree(&doc);
        if (failed)
        {
            break;
        }
    }
    if (ferror(f) || !valid_header)
    {
        failed = true;
    }
    free(buf);
    fclose(f);
    if (failed)
    {
        LoadedTranscriptFree(messages, count);
        return -1;
    }
    *out = messages;
    *out_count = count;
    return 0;
}

int PicoSession_Resolve(const PicoWorkspace *workspace, const char *id, bool allow_prefix,
                        char *path, size_t path_cap)
{
    if (!workspace || !id || !id[0] || !path || path_cap == 0)
    {
        return -1;
    }
    PicoSessionInfo *list = NULL;
    int n = PicoSession_List(workspace, &list, false);
    const PicoSessionInfo *found = NULL;
    const PicoSessionInfo *prefix = NULL;
    int prefix_hits = 0;
    size_t id_len = strlen(id);
    for (int i = 0; i < n; i++)
    {
        if (strcmp(list[i].id, id) == 0)
        {
            found = &list[i];
            break;
        }
        if (allow_prefix && strncmp(list[i].id, id, id_len) == 0)
        {
            prefix = &list[i];
            prefix_hits++;
        }
    }
    if (!found && allow_prefix && prefix_hits == 1)
    {
        found = prefix;
    }
    char canonical[4096];
    bool ok = found && realpath(found->path, canonical) && strlen(canonical) < path_cap;
    if (ok)
    {
        snprintf(path, path_cap, "%s", canonical);
    }
    free(list);
    return ok ? 0 : -1;
}

int PicoSession_Open(PicoHost *app, PicoAgent *agent, const char *id)
{
    if (!app || !id || !id[0] || PicoAgent_IsBusy(agent))
    {
        return -1;
    }

    char path[4096];
    if (PicoSession_Resolve(SessionWorkspace(app, agent), id, true, path, sizeof(path)) != 0)
    {
        return -1;
    }
    if (agent->session_path[0] && strcmp(agent->session_path, path) == 0)
    {
        return 0;
    }
    PicoWorkspace *ws = agent->workspace;
    if (ws && PicoWorkspace_SessionReserved(ws, path, agent->id))
    {
        return -1;
    }

    PicoSession_Reset(app, agent);
    if (ws && !PicoWorkspace_ReserveSession(ws, agent->id, path))
    {
        return -1;
    }
    agent->persistence = PICO_SESSION_DURABLE;
    return PicoSession_Replay(app, agent, path, true);
}

void PicoSession_Reset(PicoHost *app, PicoAgent *agent)
{
    if (!app || !agent)
    {
        return;
    }
    PicoSession_DrainPersist(app, agent);
    if (agent->workspace)
    {
        PicoWorkspace_ReleaseSessions(agent->workspace, agent->id);
    }
    pico_run_hooks(app, PICO_HOOK_ON_SESSION_RESET, agent->id);
    PicoAgent_DismissError(agent);
    PicoAgent_ClearMessages(agent);
    PicoAgent_ClearInput(agent);
    PicoAgent_RotateCacheKey(agent);
    agent->tokens_used = 0;
    agent->tokens_cached = 0;
    agent->session_input_tokens = 0;
    agent->session_cached_tokens = 0;
    agent->activity[0] = '\0';
    free(agent->compact_summary);
    agent->compact_summary = NULL;
    if (agent->persistence != PICO_SESSION_EPHEMERAL)
    {
        agent->persistence = PICO_SESSION_DURABLE;
    }
    agent->session_id[0] = '\0';
    agent->session_path[0] = '\0';
    agent->unseen_complete = false;
}

PicoSessionWriteResult PicoSession_LogUser(PicoHost *app, PicoAgent *agent,
                                             const char *content, const char *display,
                                             const char *parts_json)
{
    char *pre = EventPrefix("message");
    JsonBuf b;
    JsonBuf_Init(&b);
    JsonBuf_Puts(&b, pre);
    JsonBuf_Puts(&b, ",\"role\":\"user\",\"content\":");
    JsonBuf_String(&b, content ? content : "");
    if (display && display[0] && (!content || strcmp(display, content) != 0))
    {
        JsonBuf_Puts(&b, ",\"display\":");
        JsonBuf_String(&b, display);
    }
    if (parts_json && parts_json[0] == '[')
    {
        JsonBuf_Puts(&b, ",\"parts\":");
        JsonBuf_Puts(&b, parts_json);
    }
    JsonBuf_Putc(&b, '}');
    char *line = JsonBuf_Steal(&b);
    PicoSessionWriteResult result = AppendLine(app, agent, line);
    free(line);
    free(pre);
    return result;
}

PicoSessionWriteResult PicoSession_LogUsage(PicoHost *app, PicoAgent *agent,
                                            int input_tokens, int cached_tokens)
{
    if (input_tokens <= 0)
    {
        return PICO_SESSION_WRITE_SKIPPED;
    }
    char *pre = EventPrefix("usage");
    JsonBuf b;
    JsonBuf_Init(&b);
    JsonBuf_Puts(&b, pre);
    JsonBuf_Puts(&b, ",\"input_tokens\":");
    JsonBuf_Int(&b, input_tokens);
    JsonBuf_Puts(&b, ",\"cached_tokens\":");
    JsonBuf_Int(&b, cached_tokens);
    JsonBuf_Putc(&b, '}');
    char *line = JsonBuf_Steal(&b);
    PicoSessionWriteResult result = AppendLine(app, agent, line);
    free(line);
    free(pre);
    return result;
}

PicoSessionWriteResult PicoSession_LogAssistant(PicoHost *app, PicoAgent *agent,
                                                int message_group, const char *content,
                                                const char *thinking,
                                                const char *thinking_signature,
                                                const char *parts_json,
                                                const char *thinking_parts_json,
                                                int thinking_ms)
{
    bool has_parts = parts_json && parts_json[0] == '[';
    bool has_thinking_parts = thinking_parts_json && thinking_parts_json[0] == '[';
    if (message_group < 0)
    {
        return PICO_SESSION_WRITE_FAILED;
    }
    if ((!content || !content[0]) && (!thinking || !thinking[0]) &&
        (!thinking_signature || !thinking_signature[0]) && !has_parts && !has_thinking_parts)
    {
        return PICO_SESSION_WRITE_SKIPPED;
    }
    char *pre = EventPrefix("message");
    JsonBuf b;
    JsonBuf_Init(&b);
    JsonBuf_Puts(&b, pre);
    JsonBuf_Puts(&b, ",\"role\":\"assistant\",\"message_group\":");
    JsonBuf_Int(&b, message_group);
    JsonBuf_Puts(&b, ",\"content\":");
    JsonBuf_String(&b, content ? content : "");
    if (thinking && thinking[0])
    {
        JsonBuf_Puts(&b, ",\"thinking\":");
        JsonBuf_String(&b, thinking);
    }
    if (has_thinking_parts)
    {
        JsonBuf_Puts(&b, ",\"thinking_parts\":");
        JsonBuf_Puts(&b, thinking_parts_json);
    }
    if (thinking_ms > 0 && ((thinking && thinking[0]) || has_thinking_parts))
    {
        JsonBuf_Puts(&b, ",\"thinking_ms\":");
        JsonBuf_Int(&b, thinking_ms);
    }
    if (thinking_signature && thinking_signature[0])
    {
        JsonBuf_Puts(&b, ",\"thinking_signature\":");
        JsonBuf_String(&b, thinking_signature);
    }
    if (has_parts)
    {
        JsonBuf_Puts(&b, ",\"parts\":");
        JsonBuf_Puts(&b, parts_json);
    }
    JsonBuf_Putc(&b, '}');
    char *line = JsonBuf_Steal(&b);
    PicoSessionWriteResult result = AppendLine(app, agent, line);
    free(line);
    free(pre);
    return result;
}

PicoSessionWriteResult PicoSession_LogToolCall(PicoHost *app, PicoAgent *agent,
                                               int message_group, const char *call_id,
                                               const char *name, const char *args,
                                               const char *item_id)
{
    if (message_group < 0)
    {
        return PICO_SESSION_WRITE_FAILED;
    }
    char *pre = EventPrefix("tool_call");
    JsonBuf b;
    JsonBuf_Init(&b);
    JsonBuf_Puts(&b, pre);
    JsonBuf_Puts(&b, ",\"message_group\":");
    JsonBuf_Int(&b, message_group);
    JsonBuf_Puts(&b, ",\"call_id\":");
    JsonBuf_String(&b, call_id ? call_id : "");
    JsonBuf_Puts(&b, ",\"name\":");
    JsonBuf_String(&b, name ? name : "");
    JsonBuf_Puts(&b, ",\"arguments\":");
    JsonBuf_String(&b, args ? args : "{}");
    if (item_id && item_id[0])
    {
        JsonBuf_Puts(&b, ",\"item_id\":");
        JsonBuf_String(&b, item_id);
    }
    JsonBuf_Putc(&b, '}');
    char *line = JsonBuf_Steal(&b);
    PicoSessionWriteResult result = AppendLine(app, agent, line);
    free(line);
    free(pre);
    return result;
}

PicoSessionWriteResult PicoSession_LogToolResult(PicoHost *app, PicoAgent *agent,
                                                 const char *call_id, const char *name,
                                                 const char *output, bool is_error,
                                                 const char *details_json)
{
    char *pre = EventPrefix("tool_result");
    JsonBuf b;
    JsonBuf_Init(&b);
    JsonBuf_Puts(&b, pre);
    JsonBuf_Puts(&b, ",\"call_id\":");
    JsonBuf_String(&b, call_id ? call_id : "");
    JsonBuf_Puts(&b, ",\"name\":");
    JsonBuf_String(&b, name ? name : "");
    JsonBuf_Puts(&b, ",\"output\":");
    JsonBuf_String(&b, output ? output : "");
    JsonBuf_Puts(&b, ",\"is_error\":");
    JsonBuf_Bool(&b, is_error);
    if (details_json && details_json[0])
    {
        JsonBuf_Puts(&b, ",\"details\":");
        JsonBuf_Puts(&b, details_json);
    }
    JsonBuf_Putc(&b, '}');
    char *line = JsonBuf_Steal(&b);
    PicoSessionWriteResult result = AppendLine(app, agent, line);
    free(line);
    free(pre);
    return result;
}

PicoSessionWriteResult PicoSession_LogCompaction(PicoHost *app, PicoAgent *agent,
                                                 const char *summary, int tokens_before)
{
    char *pre = EventPrefix("compaction");
    JsonBuf b;
    JsonBuf_Init(&b);
    JsonBuf_Puts(&b, pre);
    JsonBuf_Puts(&b, ",\"summary\":");
    JsonBuf_String(&b, summary ? summary : "");
    JsonBuf_Puts(&b, ",\"tokens_before\":");
    JsonBuf_Int(&b, tokens_before);
    JsonBuf_Putc(&b, '}');
    char *line = JsonBuf_Steal(&b);
    PicoSessionWriteResult result = AppendLine(app, agent, line);
    free(line);
    free(pre);
    return result;
}

PicoSessionWriteResult PicoSession_LogModelChange(PicoHost *app, PicoAgent *agent,
                                                  const char *model, const char *effort)
{
    char *line = BuildModelChangeJson(model, effort);
    PicoSessionWriteResult result = AppendLine(app, agent, line);
    free(line);
    return result;
}

PicoSessionWriteResult PicoSession_LogCustom(PicoHost *app, PicoAgent *agent,
                                             const char *ext, const char *data_json)
{
    char *pre = EventPrefix("custom");
    JsonBuf b;
    JsonBuf_Init(&b);
    JsonBuf_Puts(&b, pre);
    JsonBuf_Puts(&b, ",\"ext\":");
    JsonBuf_String(&b, ext ? ext : "");
    JsonBuf_Puts(&b, ",\"data\":");
    JsonBuf_Puts(&b, data_json && data_json[0] ? data_json : "{}");
    JsonBuf_Putc(&b, '}');
    char *line = JsonBuf_Steal(&b);
    PicoSessionWriteResult result = AppendLine(app, agent, line);
    free(line);
    free(pre);
    return result;
}

static bool AppendTokRaw(JsonBuf *b, const JsonDoc *doc, int tok)
{
    int start = JsonTokStart(doc, tok);
    int end = JsonTokEnd(doc, tok);
    if (!b || !doc || start < 0 || end < start || (size_t)end > doc->len)
    {
        return false;
    }
    if (start > 0 && doc->src[start - 1] == '"')
    {
        start--;
        if ((size_t)end < doc->len && doc->src[end] == '"')
        {
            end++;
        }
    }
    JsonBuf_Append(b, doc->src + start, (size_t)(end - start));
    return true;
}

static bool HeaderTitleEquals(const char *header_line, const char *title)
{
    if (!header_line || !title)
    {
        return false;
    }
    JsonDoc doc;
    memset(&doc, 0, sizeof(doc));
    if (JsonParse(&doc, header_line, strlen(header_line)) != 0 || !JsonIsObject(&doc, 0))
    {
        if (doc.toks)
        {
            JsonFree(&doc);
        }
        return false;
    }
    char *current = JsonObjStr(&doc, 0, "title");
    bool equal = JsonEq(&doc, JsonObjGet(&doc, 0, "type"), "session") &&
                 current && strcmp(current, title) == 0;
    free(current);
    JsonFree(&doc);
    return equal;
}

static char *HeaderWithTitle(const char *header_line, const char *title)
{
    if (!header_line || !title)
    {
        return NULL;
    }
    JsonDoc doc;
    memset(&doc, 0, sizeof(doc));
    if (JsonParse(&doc, header_line, strlen(header_line)) != 0 || !JsonIsObject(&doc, 0) ||
        !JsonEq(&doc, JsonObjGet(&doc, 0, "type"), "session"))
    {
        if (doc.toks)
        {
            JsonFree(&doc);
        }
        return NULL;
    }
    JsonBuf b;
    JsonBuf_Init(&b);
    JsonBuf_Putc(&b, '{');
    int n = JsonObjLen(&doc, 0);
    bool first = true;
    bool wrote_title = false;
    for (int i = 0; i < n; i++)
    {
        int key_tok = -1;
        int val_tok = -1;
        if (!JsonObjPair(&doc, 0, i, &key_tok, &val_tok))
        {
            JsonBuf_Free(&b);
            JsonFree(&doc);
            return NULL;
        }
        char *key = JsonStrDup(&doc, key_tok);
        if (!key)
        {
            JsonBuf_Free(&b);
            JsonFree(&doc);
            return NULL;
        }
        if (!first)
        {
            JsonBuf_Putc(&b, ',');
        }
        first = false;
        if (strcmp(key, "title") == 0)
        {
            JsonBuf_Puts(&b, "\"title\":");
            JsonBuf_String(&b, title);
            wrote_title = true;
            free(key);
            continue;
        }
        JsonBuf_String(&b, key);
        JsonBuf_Putc(&b, ':');
        bool ok = AppendTokRaw(&b, &doc, val_tok);
        free(key);
        if (!ok)
        {
            JsonBuf_Free(&b);
            JsonFree(&doc);
            return NULL;
        }
    }
    if (!wrote_title)
    {
        if (!first)
        {
            JsonBuf_Putc(&b, ',');
        }
        JsonBuf_Puts(&b, "\"title\":");
        JsonBuf_String(&b, title);
    }
    JsonBuf_Putc(&b, '}');
    JsonFree(&doc);
    return JsonBuf_Steal(&b);
}

static bool CopyRemainder(FILE *src, int fd)
{
    char buf[8192];
    for (;;)
    {
        size_t n = fread(buf, 1, sizeof(buf), src);
        if (n > 0 && !PicoIO_WriteAll(fd, buf, n))
        {
            return false;
        }
        if (n < sizeof(buf))
        {
            return ferror(src) == 0;
        }
    }
}

PicoSessionWriteResult PicoSession_LogTitle(PicoHost *app, PicoAgent *agent, const char *title)
{
    if (!app || !agent || !title || !title[0])
    {
        return PICO_SESSION_WRITE_FAILED;
    }
    if (agent->persistence == PICO_SESSION_EPHEMERAL)
    {
        return PICO_SESSION_WRITE_SKIPPED;
    }
    /* The rewrite must not run while appends are still queued: they would
     * land after the title event, out of call order. */
    if (!DrainPersistUiBound(app, agent))
    {
        pico_status_warn(app, "Session writes are still pending; the title was not changed.");
        return PICO_SESSION_WRITE_FAILED;
    }
    if (agent->persistence == PICO_SESSION_FAILED)
    {
        return PICO_SESSION_WRITE_FAILED;
    }
    if (!agent->session_path[0] && CreateNew(app, agent) != 0)
    {
        return PICO_SESSION_WRITE_FAILED;
    }
    if (!agent->session_path[0])
    {
        PersistenceFailed(app, agent, "session path was not created");
        return PICO_SESSION_WRITE_FAILED;
    }

    char error[256] = {0};
    int lock_fd = SessionLockAcquire(agent->session_path, error, sizeof(error));
    if (lock_fd < 0)
    {
        PersistenceFailed(app, agent, error);
        return PICO_SESSION_WRITE_FAILED;
    }

    int failure = 0;
    struct stat previous_stat;
    bool have_previous_stat = stat(agent->session_path, &previous_stat) == 0;
    bool renamed = false;
    FILE *src = NULL;
    char *header = NULL;
    char *event_line = NULL;
    int fd = -1;
    char tmp_path[sizeof(agent->session_path) + 16];
    tmp_path[0] = '\0';

    src = fopen(agent->session_path, "rb");
    if (!src)
    {
        failure = errno ? errno : EIO;
        goto done;
    }
    char *header_line = NULL;
    size_t header_cap = 0;
    ssize_t got = getline(&header_line, &header_cap, src);
    if (got <= 0)
    {
        free(header_line);
        failure = EINVAL;
        goto done;
    }
    while (got > 0 && (header_line[got - 1] == '\n' || header_line[got - 1] == '\r'))
    {
        header_line[--got] = '\0';
    }
    if (HeaderTitleEquals(header_line, title))
    {
        free(header_line);
        goto done;
    }
    header = HeaderWithTitle(header_line, title);
    free(header_line);
    if (!header)
    {
        failure = ENOMEM;
        goto done;
    }

    char *pre = EventPrefix("title");
    JsonBuf event;
    JsonBuf_Init(&event);
    JsonBuf_Puts(&event, pre);
    JsonBuf_Puts(&event, ",\"title\":");
    JsonBuf_String(&event, title);
    JsonBuf_Putc(&event, '}');
    event_line = JsonBuf_Steal(&event);
    free(pre);
    if (!event_line)
    {
        failure = ENOMEM;
        goto done;
    }

    if ((size_t)snprintf(tmp_path, sizeof(tmp_path), "%s.tmp.XXXXXX", agent->session_path) >= sizeof(tmp_path))
    {
        failure = ENAMETOOLONG;
        goto done;
    }
    if (SessionTestFail("title_temp_open") || (fd = mkstemp(tmp_path)) < 0)
    {
        failure = errno ? errno : EIO;
        goto done;
    }
    if (!PicoIO_WriteAll(fd, header, strlen(header)) || !PicoIO_WriteAll(fd, "\n", 1) ||
        !CopyRemainder(src, fd) || SessionTestFail("title_after_copy") ||
        !PicoIO_WriteAll(fd, event_line, strlen(event_line)) ||
        !PicoIO_WriteAll(fd, "\n", 1) || SessionTestFail("title_fsync") || fsync(fd) != 0)
    {
        failure = errno ? errno : EIO;
        goto done;
    }
    if (close(fd) != 0)
    {
        failure = errno ? errno : EIO;
        fd = -1;
        goto done;
    }
    fd = -1;
    if (SessionTestFail("title_before_rename") || rename(tmp_path, agent->session_path) != 0)
    {
        failure = errno ? errno : EIO;
        goto done;
    }
    renamed = true;
    if (SessionTestFail("title_dir_fsync") || !SyncParentDir(agent->session_path))
    {
        failure = errno ? errno : EIO;
        goto done;
    }

done:
    if (fd >= 0 && close(fd) != 0 && failure == 0)
    {
        failure = errno ? errno : EIO;
    }
    if (src)
    {
        fclose(src);
    }
    free(header);
    free(event_line);
    if (!renamed && tmp_path[0])
    {
        unlink(tmp_path);
    }
    if (failure == 0 && renamed)
    {
        CatalogWriteThrough(app, agent, title, NULL,
                            have_previous_stat ? &previous_stat : NULL);
        CatalogMarkChanged();
    }
    SessionLockRelease(lock_fd);
    if (failure != 0)
    {
        PersistenceFailed(app, agent, strerror(failure));
        return PICO_SESSION_WRITE_FAILED;
    }
    return PICO_SESSION_WRITE_OK;
}

PicoSessionWriteResult PicoSession_LogUnseenComplete(PicoHost *app, PicoAgent *agent, bool complete)
{
    char *pre;
    JsonBuf b;
    char *line;
    PicoSessionWriteResult result;
    if (agent)
    {
        agent->unseen_complete = complete;
    }
    pre = EventPrefix("unseen_complete");
    JsonBuf_Init(&b);
    JsonBuf_Puts(&b, pre);
    JsonBuf_Puts(&b, ",\"complete\":");
    JsonBuf_Bool(&b, complete);
    JsonBuf_Putc(&b, '}');
    line = JsonBuf_Steal(&b);
    result = AppendLine(app, agent, line);
    free(line);
    free(pre);
    return result;
}

void PicoSession_SetUnseenComplete(PicoHost *app, PicoAgent *agent, bool complete)
{
    if (!agent || agent->unseen_complete == complete)
    {
        return;
    }
    agent->unseen_complete = complete;
    (void)PicoSession_LogUnseenComplete(app, agent, complete);
}

static bool SessionsRoot(char *out, size_t cap)
{
    char cfg[4096];
    return Pico_ConfigDir(cfg, sizeof(cfg)) && PicoPath_Format(out, cap, "%s/sessions", cfg);
}

static void PathBasename(const char *path, char *out, size_t cap)
{
    const char *name;
    const char *slash;
    if (!out || cap == 0)
    {
        return;
    }
    out[0] = '\0';
    if (!path || !path[0])
    {
        snprintf(out, cap, "workspace");
        return;
    }
    slash = strrchr(path, '/');
    name = (slash && slash[1]) ? slash + 1 : path;
    if (!name[0] || strcmp(name, "/") == 0)
    {
        snprintf(out, cap, "workspace");
        return;
    }
    size_t len = strlen(name);
    if (len >= cap)
    {
        len = cap - 1;
    }
    memcpy(out, name, len);
    out[len] = '\0';
}

static bool CanonicalWorkspacePath(const char *path, char *out, size_t cap)
{
    char real[4096];
    if (!path || !path[0] || !out || cap == 0)
    {
        return false;
    }
    if (!realpath(path, real))
    {
        return false;
    }
    struct stat st;
    if (stat(real, &st) != 0 || !S_ISDIR(st.st_mode))
    {
        return false;
    }
    if (strlen(real) >= cap)
    {
        return false;
    }
    snprintf(out, cap, "%s", real);
    return true;
}

static bool CatalogKeyFromPath(const char *path, char *out, size_t cap)
{
    return EncodeCwd(path, out, cap) == 0;
}

static bool CatalogDirForPath(const char *workspace_path, char *out, size_t cap)
{
    char root[4096];
    char key[4096];
    char canonical[4096];
    const char *src = workspace_path;
    if (CanonicalWorkspacePath(workspace_path, canonical, sizeof(canonical)))
    {
        src = canonical;
    }
    return SessionsRoot(root, sizeof(root)) && CatalogKeyFromPath(src, key, sizeof(key)) &&
           PicoPath_Format(out, cap, "%s/%s", root, key);
}

static bool CatalogMetaPath(const char *dir, char *out, size_t cap)
{
    return PicoPath_Format(out, cap, "%s/.workspace.json", dir);
}

static pthread_mutex_t g_catalog_mutex = PTHREAD_MUTEX_INITIALIZER;

static int CatalogFileLockAcquire(const char *path, char *error, size_t error_cap)
{
    int fd;
    if (!path || !path[0] || pthread_mutex_lock(&g_catalog_mutex) != 0)
    {
        return -1;
    }
    fd = SessionLockAcquire(path, error, error_cap);
    if (fd < 0)
    {
        pthread_mutex_unlock(&g_catalog_mutex);
    }
    return fd;
}

static int CatalogLockAcquire(const char *dir)
{
    char meta[4096];
    char error[256];
    if (!CatalogMetaPath(dir, meta, sizeof(meta)))
    {
        return -1;
    }
    return CatalogFileLockAcquire(meta, error, sizeof(error));
}

static void CatalogLockRelease(int fd)
{
    SessionLockRelease(fd);
    pthread_mutex_unlock(&g_catalog_mutex);
}

static bool CatalogAtomicWrite(const char *path, const char *data, size_t len)
{
    char dir[4096];
    char tmp[4096];
    int fd;
    bool ok;
    size_t off;
    int dfd;
    if (!path || !path[0] || !data)
    {
        return false;
    }
    snprintf(dir, sizeof(dir), "%s", path);
    char *slash = strrchr(dir, '/');
    if (slash)
    {
        *slash = '\0';
    }
    else
    {
        snprintf(dir, sizeof(dir), ".");
    }
    if (snprintf(tmp, sizeof(tmp), "%s.tmp.XXXXXX", path) >= (int)sizeof(tmp))
    {
        return false;
    }
    fd = mkstemp(tmp);
    if (fd < 0)
    {
        return false;
    }
    (void)fchmod(fd, 0600);
    ok = PicoIO_WriteAll(fd, data, len);
    if (ok && fsync(fd) != 0)
    {
        ok = false;
    }
    if (close(fd) != 0)
    {
        ok = false;
    }
    if (!ok || rename(tmp, path) != 0)
    {
        unlink(tmp);
        return false;
    }
    dfd = open(dir, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (dfd < 0)
    {
        return false;
    }
    if (fsync(dfd) != 0)
    {
        close(dfd);
        return false;
    }
    return close(dfd) == 0;
}

static char *CatalogSerialize(const PicoCatalogWorkspace *ws)
{
    JsonBuf b;
    int i;
    if (!ws)
    {
        return NULL;
    }
    JsonBuf_Init(&b);
    JsonBuf_Puts(&b, "{\"version\":1,\"key\":");
    JsonBuf_String(&b, ws->key);
    JsonBuf_Puts(&b, ",\"path\":");
    JsonBuf_String(&b, ws->path);
    JsonBuf_Puts(&b, ",\"name\":");
    JsonBuf_String(&b, ws->name);
    JsonBuf_Puts(&b, ",\"order\":");
    JsonBuf_Int(&b, ws->order);
    JsonBuf_Puts(&b, ",\"collapsed\":");
    JsonBuf_Bool(&b, ws->collapsed);
    JsonBuf_Puts(&b, ",\"sessions\":[");
    for (i = 0; i < ws->session_count; i++)
    {
        const PicoCatalogSession *s = &ws->sessions[i];
        char number[32];
        if (i > 0)
        {
            JsonBuf_Putc(&b, ',');
        }
        JsonBuf_Puts(&b, "{\"id\":");
        JsonBuf_String(&b, s->id);
        JsonBuf_Puts(&b, ",\"title\":");
        JsonBuf_String(&b, s->title);
        JsonBuf_Puts(&b, ",\"model\":");
        JsonBuf_String(&b, s->model);
        JsonBuf_Puts(&b, ",\"effort\":");
        JsonBuf_String(&b, s->effort);
        JsonBuf_Puts(&b, ",\"kind\":");
        JsonBuf_String(&b, s->kind == PICO_AGENT_SUBAGENT ? "subagent" : "normal");
        JsonBuf_Puts(&b, ",\"mtime\":");
        snprintf(number, sizeof(number), "%lld", (long long)s->mtime);
        JsonBuf_Puts(&b, number);
        JsonBuf_Puts(&b, ",\"mtime_nsec\":");
        snprintf(number, sizeof(number), "%ld", s->mtime_nsec);
        JsonBuf_Puts(&b, number);
        JsonBuf_Puts(&b, ",\"ctime\":");
        snprintf(number, sizeof(number), "%lld", (long long)s->ctime);
        JsonBuf_Puts(&b, number);
        JsonBuf_Puts(&b, ",\"ctime_nsec\":");
        snprintf(number, sizeof(number), "%ld", s->ctime_nsec);
        JsonBuf_Puts(&b, number);
        JsonBuf_Puts(&b, ",\"inode\":");
        snprintf(number, sizeof(number), "%llu", (unsigned long long)s->inode);
        JsonBuf_Puts(&b, number);
        JsonBuf_Puts(&b, ",\"size\":");
        snprintf(number, sizeof(number), "%llu", (unsigned long long)s->size);
        JsonBuf_Puts(&b, number);
        JsonBuf_Puts(&b, ",\"unseen_complete\":");
        JsonBuf_Bool(&b, s->unseen_complete);
        JsonBuf_Putc(&b, '}');
    }
    JsonBuf_Puts(&b, "]}");
    return JsonBuf_Steal(&b);
}

static bool CatalogWrite(const PicoCatalogWorkspace *ws, const char *dir)
{
    char meta[4096];
    char *json;
    bool ok;
    if (!ws || !CatalogMetaPath(dir, meta, sizeof(meta)))
    {
        return false;
    }
    json = CatalogSerialize(ws);
    if (!json)
    {
        return false;
    }
    ok = CatalogAtomicWrite(meta, json, strlen(json));
    free(json);
    return ok;
}

static bool CatalogChangeTokenPath(char *out, size_t cap)
{
    char root[4096];
    return SessionsRoot(root, sizeof(root)) &&
           PicoPath_Format(out, cap, "%s/.catalog-change", root);
}

bool PicoCatalog_ReadChangeToken(char out[PICO_CATALOG_CHANGE_TOKEN_MAX])
{
    char path[4096];
    struct stat st;
    char *raw;
    size_t len = 0;
    if (!out)
    {
        return false;
    }
    out[0] = '\0';
    if (!CatalogChangeTokenPath(path, sizeof(path)))
    {
        return false;
    }
    if (stat(path, &st) != 0)
    {
        return errno == ENOENT;
    }
    raw = Pico_ReadFile(path, &len);
    if (!raw || len == 0 || len >= PICO_CATALOG_CHANGE_TOKEN_MAX)
    {
        free(raw);
        return false;
    }
    memcpy(out, raw, len);
    out[len] = '\0';
    free(raw);
    return true;
}

static void CatalogMarkChanged(void)
{
    char root[4096];
    char path[4096];
    char token[PICO_CATALOG_CHANGE_TOKEN_MAX] = {0};
    if (!SessionsRoot(root, sizeof(root)) ||
        !PicoPath_Format(path, sizeof(path), "%s/.catalog-change", root))
    {
        return;
    }
    Pico_MkdirP(root);
    Pico_RandomHex(token, sizeof(token));
    if (token[0])
    {
        (void)CatalogAtomicWrite(path, token, strlen(token));
    }
}

static bool CatalogWriteChanged(const PicoCatalogWorkspace *ws, const char *dir)
{
    if (!CatalogWrite(ws, dir))
    {
        return false;
    }
    CatalogMarkChanged();
    return true;
}

static bool CatalogOrderPath(char *out, size_t cap)
{
    char root[4096];
    return SessionsRoot(root, sizeof(root)) &&
           PicoPath_Format(out, cap, "%s/.workspace-order.json", root);
}

static char *CatalogOrderSerialize(const PicoCatalogWorkspace *workspaces, int count)
{
    JsonBuf b;
    int i;
    if (!workspaces || count <= 0 || count > PICO_MAX_CATALOG_WORKSPACES)
    {
        return NULL;
    }
    JsonBuf_Init(&b);
    JsonBuf_Puts(&b, "{\"version\":1,\"workspaces\":[");
    for (i = 0; i < count; i++)
    {
        if (i > 0)
        {
            JsonBuf_Putc(&b, ',');
        }
        JsonBuf_String(&b, workspaces[i].path);
    }
    JsonBuf_Puts(&b, "]}");
    return JsonBuf_Steal(&b);
}

static bool CatalogWriteOrderJson(const char *json, char *error, size_t error_cap)
{
    char root[4096];
    char path[4096];
    int lock_fd;
    bool ok;
    if (!json || !json[0] || !SessionsRoot(root, sizeof(root)) ||
        !PicoPath_Format(path, sizeof(path), "%s/.workspace-order.json", root))
    {
        snprintf(error, error_cap, "catalog order path is too long");
        return false;
    }
    Pico_MkdirP(root);
    if (SessionTestFail("catalog_order_before_write"))
    {
        snprintf(error, error_cap, "%s", strerror(errno ? errno : EIO));
        return false;
    }
    lock_fd = CatalogFileLockAcquire(path, error, error_cap);
    if (lock_fd < 0)
    {
        return false;
    }
    ok = CatalogAtomicWrite(path, json, strlen(json));
    if (ok)
    {
        CatalogMarkChanged();
    }
    if (!ok && error && error_cap > 0 && !error[0])
    {
        snprintf(error, error_cap, "%s", strerror(errno ? errno : EIO));
    }
    CatalogLockRelease(lock_fd);
    return ok;
}

static void CatalogApplyOrderFile(PicoCatalogWorkspace *list, int count)
{
    char path[4096];
    char error[256];
    char *raw;
    size_t raw_len = 0;
    JsonDoc doc;
    int workspaces;
    int order_count;
    int lock_fd;
    int i;
    if (!list || count <= 1 || !CatalogOrderPath(path, sizeof(path)))
    {
        return;
    }
    error[0] = '\0';
    lock_fd = CatalogFileLockAcquire(path, error, sizeof(error));
    if (lock_fd < 0)
    {
        return;
    }
    raw = Pico_ReadFile(path, &raw_len);
    CatalogLockRelease(lock_fd);
    if (!raw)
    {
        return;
    }
    if (JsonParse(&doc, raw, raw_len) != 0)
    {
        free(raw);
        return;
    }
    workspaces = JsonObjGet(&doc, 0, "workspaces");
    order_count = JsonArrayLen(&doc, workspaces);
    if (!JsonIsObject(&doc, 0) || JsonObjInt(&doc, 0, "version", 0) != 1 ||
        !JsonIsArray(&doc, workspaces) || order_count < 0 ||
        order_count > PICO_MAX_CATALOG_WORKSPACES)
    {
        JsonFree(&doc);
        free(raw);
        return;
    }
    for (i = 0; i < count; i++)
    {
        list[i].order = order_count + i;
    }
    for (i = 0; i < order_count; i++)
    {
        char *ordered_path = JsonStrDup(&doc, JsonArrayAt(&doc, workspaces, i));
        int j;
        if (!ordered_path)
        {
            continue;
        }
        for (j = 0; j < count; j++)
        {
            if (list[j].order >= order_count && strcmp(list[j].path, ordered_path) == 0)
            {
                list[j].order = i;
                break;
            }
        }
        free(ordered_path);
    }
    JsonFree(&doc);
    free(raw);
    qsort(list, (size_t)count, sizeof(*list), CmpCatalogOrder);
}

static void CatalogClearSessions(PicoCatalogWorkspace *ws)
{
    if (!ws)
    {
        return;
    }
    free(ws->sessions);
    ws->sessions = NULL;
    ws->session_count = 0;
}

static bool CatalogCopySession(PicoCatalogWorkspace *ws, const PicoCatalogSession *src)
{
    PicoCatalogSession *next;
    if (!ws || !src || !src->id[0] || ws->session_count >= PICO_MAX_CATALOG_SESSIONS)
    {
        return false;
    }
    next = (PicoCatalogSession *)realloc(ws->sessions,
                                         (size_t)(ws->session_count + 1) * sizeof(*next));
    if (!next)
    {
        return false;
    }
    ws->sessions = next;
    ws->sessions[ws->session_count] = *src;
    ws->session_count++;
    return true;
}

static const PicoCatalogSession *CatalogFindSession(const PicoCatalogWorkspace *ws,
                                                    const char *id)
{
    int i;
    if (!ws || !id || !id[0])
    {
        return NULL;
    }
    for (i = 0; i < ws->session_count; i++)
    {
        if (strcmp(ws->sessions[i].id, id) == 0)
        {
            return &ws->sessions[i];
        }
    }
    return NULL;
}

static bool CatalogSessionEqual(const PicoCatalogSession *a, const PicoCatalogSession *b)
{
    return a && b && strcmp(a->id, b->id) == 0 && strcmp(a->title, b->title) == 0 &&
           strcmp(a->model, b->model) == 0 && strcmp(a->effort, b->effort) == 0 &&
           a->mtime == b->mtime && a->mtime_nsec == b->mtime_nsec && a->ctime == b->ctime &&
           a->ctime_nsec == b->ctime_nsec && a->inode == b->inode && a->size == b->size &&
           a->unseen_complete == b->unseen_complete && a->kind == b->kind;
}

static bool CatalogSessionsMatch(const PicoCatalogWorkspace *a, const PicoCatalogWorkspace *b)
{
    int i;
    if (!a || !b || a->session_count != b->session_count)
    {
        return false;
    }
    for (i = 0; i < a->session_count; i++)
    {
        if (!CatalogSessionEqual(&a->sessions[i], &b->sessions[i]))
        {
            return false;
        }
    }
    return true;
}

static bool CatalogLoadMeta(const char *path, PicoCatalogWorkspace *out)
{
    size_t len = 0;
    char *raw;
    JsonDoc doc;
    int sessions;
    int i;
    if (!path || !out)
    {
        return false;
    }
    memset(out, 0, sizeof(*out));
    raw = Pico_ReadFile(path, &len);
    if (!raw || len == 0)
    {
        free(raw);
        return false;
    }
    if (JsonParse(&doc, raw, len) != 0 || !JsonIsObject(&doc, 0))
    {
        JsonFree(&doc);
        free(raw);
        return false;
    }
    {
        char *key = JsonObjStr(&doc, 0, "key");
        char *ws_path = JsonObjStr(&doc, 0, "path");
        char *name = JsonObjStr(&doc, 0, "name");
        if (key)
        {
            snprintf(out->key, sizeof(out->key), "%s", key);
        }
        if (ws_path)
        {
            snprintf(out->path, sizeof(out->path), "%s", ws_path);
        }
        if (name && name[0])
        {
            snprintf(out->name, sizeof(out->name), "%s", name);
        }
        free(key);
        free(ws_path);
        free(name);
    }
    out->order = JsonObjInt(&doc, 0, "order", 0);
    {
        int tok = JsonObjGet(&doc, 0, "collapsed");
        out->collapsed = JsonEq(&doc, tok, "true") || JsonEq(&doc, tok, "1");
    }
    sessions = JsonObjGet(&doc, 0, "sessions");
    if (JsonIsArray(&doc, sessions))
    {
        int count = JsonArrayLen(&doc, sessions);
        for (i = 0; i < count && out->session_count < PICO_MAX_CATALOG_SESSIONS; i++)
        {
            int item = JsonArrayAt(&doc, sessions, i);
            PicoCatalogSession s;
            char *id;
            char *model;
            char *effort;
            char *title;
            char *kind;
            if (!JsonIsObject(&doc, item))
            {
                continue;
            }
            memset(&s, 0, sizeof(s));
            id = JsonObjStr(&doc, item, "id");
            model = JsonObjStr(&doc, item, "model");
            effort = JsonObjStr(&doc, item, "effort");
            title = JsonObjStr(&doc, item, "title");
            kind = JsonObjStr(&doc, item, "kind");
            if (id)
            {
                snprintf(s.id, sizeof(s.id), "%s", id);
            }
            if (title)
            {
                snprintf(s.title, sizeof(s.title), "%s", title);
            }
            if (model)
            {
                snprintf(s.model, sizeof(s.model), "%s", model);
            }
            if (effort)
            {
                snprintf(s.effort, sizeof(s.effort), "%s", effort);
            }
            if (kind && strcmp(kind, "subagent") == 0)
            {
                s.kind = PICO_AGENT_SUBAGENT;
            }
            {
                char *raw_number = JsonObjRaw(&doc, item, "mtime");
                if (raw_number)
                {
                    s.mtime = (time_t)strtoll(raw_number, NULL, 10);
                }
                free(raw_number);
                raw_number = JsonObjRaw(&doc, item, "mtime_nsec");
                if (raw_number)
                {
                    s.mtime_nsec = strtol(raw_number, NULL, 10);
                }
                free(raw_number);
                raw_number = JsonObjRaw(&doc, item, "ctime");
                if (raw_number)
                {
                    s.ctime = (time_t)strtoll(raw_number, NULL, 10);
                }
                free(raw_number);
                raw_number = JsonObjRaw(&doc, item, "ctime_nsec");
                if (raw_number)
                {
                    s.ctime_nsec = strtol(raw_number, NULL, 10);
                }
                free(raw_number);
                raw_number = JsonObjRaw(&doc, item, "inode");
                if (raw_number)
                {
                    s.inode = (uint64_t)strtoull(raw_number, NULL, 10);
                }
                free(raw_number);
                raw_number = JsonObjRaw(&doc, item, "size");
                if (raw_number)
                {
                    s.size = (uint64_t)strtoull(raw_number, NULL, 10);
                }
                free(raw_number);
            }
            {
                int tok = JsonObjGet(&doc, item, "unseen_complete");
                s.unseen_complete = JsonEq(&doc, tok, "true") || JsonEq(&doc, tok, "1");
            }
            free(id);
            free(model);
            free(effort);
            free(title);
            free(kind);
            if (s.id[0])
            {
                (void)CatalogCopySession(out, &s);
            }
        }
    }
    JsonFree(&doc);
    free(raw);
    return out->path[0] || out->key[0];
}

static int CmpCatalogOrder(const void *a, const void *b)
{
    const PicoCatalogWorkspace *x = (const PicoCatalogWorkspace *)a;
    const PicoCatalogWorkspace *y = (const PicoCatalogWorkspace *)b;
    if (x->order < y->order)
    {
        return -1;
    }
    if (x->order > y->order)
    {
        return 1;
    }
    return strcmp(x->name, y->name);
}

static int CountSessionDirs(const char *root)
{
    DIR *d;
    int n = 0;
    struct dirent *ent;
    if (!root)
    {
        return 0;
    }
    d = opendir(root);
    if (!d)
    {
        return 0;
    }
    while ((ent = readdir(d)))
    {
        char path[4096];
        struct stat st;
        if (!ent->d_name[0] || ent->d_name[0] == '.')
        {
            continue;
        }
        if (!PicoPath_Format(path, sizeof(path), "%s/%s", root, ent->d_name))
        {
            continue;
        }
        if (stat(path, &st) == 0 && S_ISDIR(st.st_mode))
        {
            n++;
        }
    }
    closedir(d);
    return n;
}

void PicoCatalog_Free(PicoCatalogWorkspace *list, int n)
{
    int i;
    if (!list)
    {
        return;
    }
    for (i = 0; i < n; i++)
    {
        CatalogClearSessions(&list[i]);
    }
    free(list);
}

int PicoCatalog_Ensure(const char *workspace_path)
{
    char canonical[4096];
    char root[4096];
    char dir[4096];
    char meta[4096];
    char key[4096];
    PicoCatalogWorkspace loaded;
    PicoCatalogWorkspace fresh;
    int lock_fd;
    int result = -1;
    if (!CanonicalWorkspacePath(workspace_path, canonical, sizeof(canonical)) ||
        !SessionsRoot(root, sizeof(root)) || !CatalogKeyFromPath(canonical, key, sizeof(key)) ||
        !PicoPath_Format(dir, sizeof(dir), "%s/%s", root, key))
    {
        return -1;
    }
    Pico_MkdirP(dir);
    if (!CatalogMetaPath(dir, meta, sizeof(meta)) || (lock_fd = CatalogLockAcquire(dir)) < 0)
    {
        return -1;
    }
    memset(&fresh, 0, sizeof(fresh));
    snprintf(fresh.key, sizeof(fresh.key), "%s", key);
    snprintf(fresh.path, sizeof(fresh.path), "%s", canonical);
    PathBasename(canonical, fresh.name, sizeof(fresh.name));
    memset(&loaded, 0, sizeof(loaded));
    if (CatalogLoadMeta(meta, &loaded))
    {
        bool same_path = strcmp(loaded.path, canonical) == 0;
        if (same_path && loaded.name[0])
        {
            result = 0;
            goto done;
        }
        snprintf(fresh.key, sizeof(fresh.key), "%s", loaded.key[0] ? loaded.key : key);
        snprintf(fresh.name, sizeof(fresh.name), "%s", loaded.name[0] ? loaded.name : fresh.name);
        fresh.order = loaded.order;
        fresh.collapsed = loaded.collapsed;
        fresh.sessions = loaded.sessions;
        fresh.session_count = loaded.session_count;
        loaded.sessions = NULL;
        loaded.session_count = 0;
        result = CatalogWriteChanged(&fresh, dir) ? 0 : -1;
        goto done;
    }
    fresh.order = CountSessionDirs(root) - 1;
    if (fresh.order < 0)
    {
        fresh.order = 0;
    }
    result = CatalogWriteChanged(&fresh, dir) ? 0 : -1;

done:
    CatalogClearSessions(&loaded);
    CatalogClearSessions(&fresh);
    CatalogLockRelease(lock_fd);
    return result;
}

int PicoCatalog_SetCollapsed(const char *workspace_path, bool collapsed)
{
    char dir[4096];
    char meta[4096];
    PicoCatalogWorkspace ws;
    int lock_fd;
    int result = -1;
    if (PicoCatalog_Ensure(workspace_path) != 0 || !CatalogDirForPath(workspace_path, dir, sizeof(dir)) ||
        !CatalogMetaPath(dir, meta, sizeof(meta)) || (lock_fd = CatalogLockAcquire(dir)) < 0)
    {
        return -1;
    }
    memset(&ws, 0, sizeof(ws));
    if (CatalogLoadMeta(meta, &ws))
    {
        ws.collapsed = collapsed;
        result = CatalogWriteChanged(&ws, dir) ? 0 : -1;
    }
    CatalogClearSessions(&ws);
    CatalogLockRelease(lock_fd);
    return result;
}

int PicoCatalog_SetSessionModel(const char *workspace_path, const char *session_id,
                                const char *model, const char *effort)
{
    char dir[4096];
    char meta[4096];
    PicoCatalogWorkspace ws;
    int i;
    int lock_fd;
    int result = -1;
    bool found = false;
    if (!session_id || !session_id[0] || PicoCatalog_Ensure(workspace_path) != 0 ||
        !CatalogDirForPath(workspace_path, dir, sizeof(dir)) ||
        !CatalogMetaPath(dir, meta, sizeof(meta)) || (lock_fd = CatalogLockAcquire(dir)) < 0)
    {
        return -1;
    }
    memset(&ws, 0, sizeof(ws));
    if (!CatalogLoadMeta(meta, &ws))
    {
        goto done;
    }
    for (i = 0; i < ws.session_count; i++)
    {
        if (strcmp(ws.sessions[i].id, session_id) == 0)
        {
            snprintf(ws.sessions[i].model, sizeof(ws.sessions[i].model), "%s", model ? model : "");
            snprintf(ws.sessions[i].effort, sizeof(ws.sessions[i].effort), "%s", effort ? effort : "");
            found = true;
            break;
        }
    }
    if (!found)
    {
        PicoCatalogSession s;
        memset(&s, 0, sizeof(s));
        snprintf(s.id, sizeof(s.id), "%s", session_id);
        snprintf(s.model, sizeof(s.model), "%s", model ? model : "");
        snprintf(s.effort, sizeof(s.effort), "%s", effort ? effort : "");
        if (!CatalogCopySession(&ws, &s))
        {
            goto done;
        }
    }
    result = CatalogWriteChanged(&ws, dir) ? 0 : -1;

done:
    CatalogClearSessions(&ws);
    CatalogLockRelease(lock_fd);
    return result;
}

static bool CatalogApplyEvent(PicoCatalogSession *row, const char *event_json)
{
    JsonDoc doc;
    char *type;
    bool ok = true;
    if (!row || !event_json || JsonParse(&doc, event_json, strlen(event_json)) != 0 ||
        !JsonIsObject(&doc, 0))
    {
        return false;
    }
    type = JsonObjStr(&doc, 0, "type");
    if (!type)
    {
        ok = false;
    }
    else if (strcmp(type, "message") == 0)
    {
        char *role = JsonObjStr(&doc, 0, "role");
        if (role && strcmp(role, "user") == 0 &&
            (!row->title[0] || strcmp(row->title, "Untitled") == 0))
        {
            /* The cache does not retain whether "Untitled" came from an explicit header title. */
            ok = false;
        }
        free(role);
    }
    else if (strcmp(type, "model_change") == 0)
    {
        char *model = JsonObjStr(&doc, 0, "model");
        char *effort = JsonObjStr(&doc, 0, "effort");
        if (model && model[0])
        {
            snprintf(row->model, sizeof(row->model), "%s", model);
        }
        if (effort && effort[0])
        {
            snprintf(row->effort, sizeof(row->effort), "%s", effort);
        }
        free(model);
        free(effort);
    }
    else if (strcmp(type, "unseen_complete") == 0)
    {
        int tok = JsonObjGet(&doc, 0, "complete");
        row->unseen_complete = JsonEq(&doc, tok, "true") || JsonEq(&doc, tok, "1");
    }
    free(type);
    JsonFree(&doc);
    return ok;
}

/* Queued session writes accumulate into one '\n'-separated buffer. Apply
 * each record in order; any line that cannot be applied forces a rescan of
 * the file the persist thread has just written. */
static bool CatalogApplyEventLines(PicoCatalogSession *row, const char *event_json)
{
    const char *p = event_json;
    if (!event_json || !event_json[0])
    {
        return false;
    }
    while (p && *p)
    {
        const char *nl = strchr(p, '\n');
        size_t len = nl ? (size_t)(nl - p) : strlen(p);
        if (len > 0)
        {
            char *line = (char *)malloc(len + 1);
            if (!line)
            {
                return false;
            }
            memcpy(line, p, len);
            line[len] = '\0';
            bool ok = CatalogApplyEvent(row, line);
            free(line);
            if (!ok)
            {
                return false;
            }
        }
        p = nl ? nl + 1 : NULL;
    }
    return true;
}

static void CatalogRowFromFile(const char *path, PicoCatalogSession *row)
{
    PicoSessionInfo info;
    if (!path || !row)
    {
        return;
    }
    memset(&info, 0, sizeof(info));
    ScanSessionFile(path, &info, false);
    snprintf(row->id, sizeof(row->id), "%s", info.id);
    snprintf(row->title, sizeof(row->title), "%s", info.title);
    snprintf(row->model, sizeof(row->model), "%s", info.model);
    snprintf(row->effort, sizeof(row->effort), "%s", info.effort);
    row->unseen_complete = info.unseen_complete;
    row->kind = info.kind;
}

static void CatalogWriteThroughFields(PicoAgentKind kind, PicoSessionPersistence persistence,
                                      const char *session_id, const char *session_path,
                                      const char *ws_path, const char *title_override,
                                      const char *event_json, const struct stat *previous_stat)
{
    char dir[4096];
    char meta[4096];
    PicoCatalogWorkspace ws;
    PicoCatalogSession row;
    const PicoCatalogSession *previous;
    struct stat current_stat;
    int lock_fd;
    int index = -1;
    bool row_ready = false;
    if (kind != PICO_AGENT_MAIN || persistence != PICO_SESSION_DURABLE || !session_id ||
        !session_id[0] || !session_path || !session_path[0])
    {
        return;
    }
    if (!ws_path || !ws_path[0] || stat(session_path, &current_stat) != 0 ||
        !S_ISREG(current_stat.st_mode) || PicoCatalog_Ensure(ws_path) != 0 ||
        !CatalogDirForPath(ws_path, dir, sizeof(dir)) ||
        !CatalogMetaPath(dir, meta, sizeof(meta)) || (lock_fd = CatalogLockAcquire(dir)) < 0)
    {
        return;
    }
#ifdef PICO_SESSION_TEST_HOOKS
    (void)PicoSession_TestHook("catalog_before_upsert");
#endif
    memset(&ws, 0, sizeof(ws));
    memset(&row, 0, sizeof(row));
    if (!CatalogLoadMeta(meta, &ws))
    {
        goto done;
    }
    previous = CatalogFindSession(&ws, session_id);
    if (previous && previous_stat && CatalogGenerationMatches(previous, previous_stat))
    {
        row = *previous;
        row_ready = title_override && title_override[0];
        if (row_ready)
        {
            snprintf(row.title, sizeof(row.title), "%s", title_override);
        }
        else
        {
            row_ready = CatalogApplyEventLines(&row, event_json);
        }
    }
    if (!row_ready)
    {
        CatalogRowFromFile(session_path, &row);
    }
    if (!row.id[0])
    {
        snprintf(row.id, sizeof(row.id), "%s", session_id);
    }
    row.kind = kind;
    CopyStatToCatalog(&row, &current_stat);
    for (int i = 0; i < ws.session_count; i++)
    {
        if (strcmp(ws.sessions[i].id, row.id) == 0)
        {
            index = i;
            break;
        }
    }
    if (index >= 0)
    {
        ws.sessions[index] = row;
    }
    else if (!CatalogCopySession(&ws, &row))
    {
        goto done;
    }
    (void)CatalogWrite(&ws, dir);

done:
    CatalogClearSessions(&ws);
    CatalogLockRelease(lock_fd);
}

static void CatalogWriteThrough(PicoHost *app, const PicoAgent *agent,
                                const char *title_override, const char *event_json,
                                const struct stat *previous_stat)
{
    if (!app || !agent)
    {
        return;
    }
    CatalogWriteThroughFields(agent->kind, agent->persistence, agent->session_id, agent->session_path,
                              PicoWorkspace_Path(SessionWorkspace(app, agent)), title_override, event_json,
                              previous_stat);
}

static bool CatalogScanDir(const char *dir, const char *key, PicoCatalogWorkspace *out)
{
    char meta[4096];
    char canonical[4096];
    PicoCatalogWorkspace ws;
    PicoCatalogWorkspace loaded;
    PicoSessionInfo *files = NULL;
    int lock_fd;
    int file_n;
    bool recovered = false;
    bool had_meta;
    bool result = false;
    if (!dir || !key || !out || (lock_fd = CatalogLockAcquire(dir)) < 0)
    {
        return false;
    }
    memset(&ws, 0, sizeof(ws));
    memset(&loaded, 0, sizeof(loaded));
    snprintf(ws.key, sizeof(ws.key), "%s", key);
    had_meta = CatalogMetaPath(dir, meta, sizeof(meta)) && CatalogLoadMeta(meta, &loaded);
    if (had_meta)
    {
        snprintf(ws.path, sizeof(ws.path), "%s", loaded.path);
        snprintf(ws.name, sizeof(ws.name), "%s", loaded.name);
        ws.order = loaded.order;
        ws.collapsed = loaded.collapsed;
        if (loaded.key[0])
        {
            snprintf(ws.key, sizeof(ws.key), "%s", loaded.key);
        }
    }
    file_n = ListSessionsInDir(dir, &files, true, had_meta ? &loaded : NULL,
                               PICO_MAX_CATALOG_SESSIONS);
    for (int i = 0; i < file_n; i++)
    {
        PicoCatalogSession s;
        memset(&s, 0, sizeof(s));
        snprintf(s.id, sizeof(s.id), "%s", files[i].id);
        snprintf(s.title, sizeof(s.title), "%s", files[i].title);
        snprintf(s.model, sizeof(s.model), "%s", files[i].model);
        snprintf(s.effort, sizeof(s.effort), "%s", files[i].effort);
        s.mtime = files[i].mtime;
        s.mtime_nsec = files[i].mtime_nsec;
        s.ctime = files[i].ctime;
        s.ctime_nsec = files[i].ctime_nsec;
        s.inode = files[i].inode;
        s.size = files[i].size;
        s.unseen_complete = files[i].unseen_complete;
        s.kind = files[i].kind;
        if (!ws.path[0] && files[i].cwd[0])
        {
            snprintf(ws.path, sizeof(ws.path), "%s", files[i].cwd);
            recovered = true;
        }
        (void)CatalogCopySession(&ws, &s);
    }
    free(files);
    if (!ws.path[0] || !CanonicalWorkspacePath(ws.path, canonical, sizeof(canonical)))
    {
        goto done;
    }
    if (!ws.name[0])
    {
        PathBasename(ws.path, ws.name, sizeof(ws.name));
    }
    if (!had_meta || recovered || !CatalogSessionsMatch(&ws, &loaded))
    {
        (void)CatalogWrite(&ws, dir);
    }
    *out = ws;
    ws.sessions = NULL;
    ws.session_count = 0;
    result = true;

done:
    CatalogClearSessions(&ws);
    CatalogClearSessions(&loaded);
    CatalogLockRelease(lock_fd);
    return result;
}

int PicoCatalog_Scan(PicoCatalogWorkspace **out)
{
#ifdef PICO_SESSION_TEST_HOOKS
    (void)PicoSession_TestHook("catalog_scan");
#endif
    char root[4096];
    DIR *d;
    struct dirent *ent;
    PicoCatalogWorkspace *list = NULL;
    int n = 0;
    if (out)
    {
        *out = NULL;
    }
    if (!out || !SessionsRoot(root, sizeof(root)))
    {
        return 0;
    }
    d = opendir(root);
    if (!d)
    {
        return 0;
    }
    while ((ent = readdir(d)) && n < PICO_MAX_CATALOG_WORKSPACES)
    {
        char dir[4096];
        struct stat st;
        PicoCatalogWorkspace ws;
        PicoCatalogWorkspace *next;
        if (!ent->d_name[0] || ent->d_name[0] == '.' ||
            !PicoPath_Format(dir, sizeof(dir), "%s/%s", root, ent->d_name) ||
            stat(dir, &st) != 0 || !S_ISDIR(st.st_mode) ||
            !CatalogScanDir(dir, ent->d_name, &ws))
        {
            continue;
        }
        next = (PicoCatalogWorkspace *)realloc(list, (size_t)(n + 1) * sizeof(*next));
        if (!next)
        {
            CatalogClearSessions(&ws);
            break;
        }
        list = next;
        list[n++] = ws;
    }
    closedir(d);
    if (n > 1)
    {
        qsort(list, (size_t)n, sizeof(*list), CmpCatalogOrder);
        CatalogApplyOrderFile(list, n);
    }
    *out = list;
    return n;
}

static void PersistJobClear(PicoSessionPersistJob *job)
{
    if (!job)
    {
        return;
    }
    free(job->header_json);
    free(job->event_json);
    free(job->catalog_order_json);
    memset(job, 0, sizeof(*job));
}

static PicoAgent *PersistFindAgent(PicoHost *host, PicoAgentId id)
{
    int i;
    int j;
    if (!host || id == 0)
    {
        return NULL;
    }
    for (i = 0; i < host->workspace_count; i++)
    {
        PicoWorkspace *workspace = host->workspaces[i];
        if (!workspace)
        {
            continue;
        }
        for (j = 0; j < workspace->count; j++)
        {
            PicoAgent *agent = workspace->agents[j];
            if (agent && agent->id == id)
            {
                return agent;
            }
        }
    }
    return NULL;
}

static bool PersistHasWorkLocked(const PicoHost *host, PicoAgentId id)
{
    int i;
    if (!host || id == 0 || !host->persist_pending)
    {
        return false;
    }
    if (host->persist_flight_agent_id == id)
    {
        return true;
    }
    for (i = 0; i < host->persist_pending_count; i++)
    {
        if (host->persist_pending[i].job_kind == PICO_PERSIST_JOB_SESSION &&
            host->persist_pending[i].agent_id == id)
        {
            return true;
        }
    }
    return false;
}

static int PersistTakeFailuresLocked(PicoHost *host, PicoAgentId id,
                                     PicoSessionPersistFailure *out, int cap)
{
    int i;
    int n = 0;
    int kept = 0;
    if (!host || !out || cap <= 0)
    {
        return 0;
    }
    for (i = 0; i < host->persist_failure_count; i++)
    {
        PicoSessionPersistFailure *item = &host->persist_failures[i];
        if (id == 0 || item->agent_id == id)
        {
            if (n < cap)
            {
                out[n++] = *item;
            }
        }
        else
        {
            if (kept != i)
            {
                host->persist_failures[kept] = *item;
            }
            kept++;
        }
    }
    host->persist_failure_count = kept;
    return n;
}

static void PersistApplyFailures(PicoHost *host, const PicoSessionPersistFailure *items, int n)
{
    int i;
    for (i = 0; i < n; i++)
    {
        PicoAgent *agent = PersistFindAgent(host, items[i].agent_id);
        if (!agent)
        {
            continue;
        }
        if (items[i].session_id[0] && strcmp(items[i].session_id, agent->session_id) != 0)
        {
            /* The agent moved on to a new session after a reset; the failure
             * belongs to its previous session file and must not fail the
             * fresh one. */
            char line[320];
            snprintf(line, sizeof(line), "Session persistence failed for a previous session: %s",
                     items[i].error);
            pico_status_warn(host, line);
            continue;
        }
        PersistenceFailed(host, agent, items[i].error);
    }
}

static PicoSessionPersistJob *PersistPendingForAgentLocked(PicoHost *host, PicoAgentId id,
                                                           const char *session_id)
{
    int i;
    if (!host || id == 0 || !session_id || !session_id[0] || !host->persist_pending)
    {
        return NULL;
    }
    for (i = 0; i < host->persist_pending_count; i++)
    {
        if (host->persist_pending[i].job_kind == PICO_PERSIST_JOB_SESSION &&
            host->persist_pending[i].agent_id == id &&
            strcmp(host->persist_pending[i].session_id, session_id) == 0)
        {
            return &host->persist_pending[i];
        }
    }
    return NULL;
}

static PicoSessionPersistJob *PersistPendingCatalogOrderLocked(PicoHost *host)
{
    int i;
    if (!host || !host->persist_pending)
    {
        return NULL;
    }
    for (i = 0; i < host->persist_pending_count; i++)
    {
        if (host->persist_pending[i].job_kind == PICO_PERSIST_JOB_CATALOG_ORDER)
        {
            return &host->persist_pending[i];
        }
    }
    return NULL;
}

static bool PersistTakeNextLocked(PicoHost *host, PicoSessionPersistJob *out)
{
    int i;
    if (!host || !out || !host->persist_pending || host->persist_pending_count <= 0)
    {
        return false;
    }
    *out = host->persist_pending[0];
    for (i = 1; i < host->persist_pending_count; i++)
    {
        host->persist_pending[i - 1] = host->persist_pending[i];
    }
    host->persist_pending_count--;
    memset(&host->persist_pending[host->persist_pending_count], 0,
           sizeof(host->persist_pending[0]));
    return true;
}

static void PersistDropPendingForAgentLocked(PicoHost *host, PicoAgentId agent_id)
{
    int i = 0;
    if (!host || agent_id == 0)
    {
        return;
    }
    while (i < host->persist_pending_count)
    {
        if (host->persist_pending[i].job_kind != PICO_PERSIST_JOB_SESSION ||
            host->persist_pending[i].agent_id != agent_id)
        {
            i++;
            continue;
        }
        PersistJobClear(&host->persist_pending[i]);
        for (int j = i + 1; j < host->persist_pending_count; j++)
        {
            host->persist_pending[j - 1] = host->persist_pending[j];
        }
        host->persist_pending_count--;
        memset(&host->persist_pending[host->persist_pending_count], 0,
               sizeof(host->persist_pending[0]));
    }
}

static void PersistRecordFailureLocked(PicoHost *host, PicoAgentId agent_id,
                                       const char *session_id, const char *error)
{
    PicoSessionPersistFailure *item;
    if (!host || agent_id == 0)
    {
        return;
    }
    for (int i = 0; i < host->persist_failure_count; i++)
    {
        if (host->persist_failures[i].agent_id == agent_id &&
            strcmp(host->persist_failures[i].session_id, session_id ? session_id : "") == 0)
        {
            return;
        }
    }
    if (host->persist_failure_count >= PICO_MAX_TOTAL_AGENTS)
    {
        return;
    }
    item = &host->persist_failures[host->persist_failure_count++];
    memset(item, 0, sizeof(*item));
    item->agent_id = agent_id;
    snprintf(item->session_id, sizeof(item->session_id), "%s", session_id ? session_id : "");
    snprintf(item->error, sizeof(item->error), "%s", error ? error : "");
}

/* True once the persist thread has recorded a write failure for this exact
 * session, even before the failure is applied to the agent. */
static bool PersistHasFailureLocked(const PicoHost *host, PicoAgentId id, const char *session_id)
{
    int i;
    if (!host || id == 0 || !session_id || !session_id[0])
    {
        return false;
    }
    for (i = 0; i < host->persist_failure_count; i++)
    {
        if (host->persist_failures[i].agent_id == id &&
            strcmp(host->persist_failures[i].session_id, session_id) == 0)
        {
            return true;
        }
    }
    return false;
}

static void *PersistThreadMain(void *arg)
{
    PicoHost *host = (PicoHost *)arg;
    while (host)
    {
        PicoSessionPersistJob job;
        char error[256];
        bool failed = false;
        memset(&job, 0, sizeof(job));
        pthread_mutex_lock(&host->persist_mu);
        while (!host->persist_stop && host->persist_pending_count == 0)
        {
            pthread_cond_wait(&host->persist_cv, &host->persist_mu);
        }
        if (host->persist_pending_count == 0)
        {
            pthread_mutex_unlock(&host->persist_mu);
            break;
        }
        if (!PersistTakeNextLocked(host, &job))
        {
            pthread_mutex_unlock(&host->persist_mu);
            continue;
        }
        if (job.job_kind == PICO_PERSIST_JOB_CATALOG_ORDER)
        {
            host->persist_flight_catalog_order = true;
        }
        else
        {
            host->persist_flight_agent_id = job.agent_id;
        }
        pthread_mutex_unlock(&host->persist_mu);

        error[0] = '\0';
        if (job.job_kind == PICO_PERSIST_JOB_CATALOG_ORDER)
        {
            failed = !CatalogWriteOrderJson(job.catalog_order_json, error, sizeof(error));
        }
        else if (job.header_json && job.header_json[0] &&
                 (!EnsureSessionParent(job.session_path, error, sizeof(error)) ||
                  !WriteLineAtPath(job.session_path, job.header_json, false, job.kind, job.persistence,
                                   job.session_id, job.workspace_path, error, sizeof(error))))
        {
            failed = true;
        }
        else if (job.event_json && job.event_json[0] &&
                 !WriteLineAtPath(job.session_path, job.event_json, true, job.kind, job.persistence,
                                  job.session_id, job.workspace_path, error, sizeof(error)))
        {
            failed = true;
        }

        pthread_mutex_lock(&host->persist_mu);
        if (job.job_kind == PICO_PERSIST_JOB_CATALOG_ORDER)
        {
            PicoSessionPersistJob *newer = PersistPendingCatalogOrderLocked(host);
            host->persist_catalog_completed_generation = job.catalog_order_generation;
            if (failed)
            {
                host->persist_catalog_failed_generation = job.catalog_order_generation;
                if (!newer || newer->catalog_order_generation <= job.catalog_order_generation)
                {
                    snprintf(host->persist_catalog_error, sizeof(host->persist_catalog_error),
                             "%s", error[0] ? error : "unknown error");
                }
            }
            host->persist_flight_catalog_order = false;
        }
        else
        {
            if (failed)
            {
                PersistDropPendingForAgentLocked(host, job.agent_id);
                PersistRecordFailureLocked(host, job.agent_id, job.session_id, error);
            }
            host->persist_flight_agent_id = 0;
        }
        pthread_cond_broadcast(&host->persist_cv);
        pthread_mutex_unlock(&host->persist_mu);
        PersistJobClear(&job);
    }
    return NULL;
}

void PicoSessionPersist_Init(PicoHost *host)
{
    if (!host || host->persist_ready)
    {
        return;
    }
    if (pthread_mutex_init(&host->persist_mu, NULL) != 0)
    {
        return;
    }
    if (pthread_cond_init(&host->persist_cv, NULL) != 0)
    {
        pthread_mutex_destroy(&host->persist_mu);
        return;
    }
    host->persist_pending = (PicoSessionPersistJob *)calloc(PICO_PERSIST_QUEUE_CAPACITY,
                                                            sizeof(*host->persist_pending));
    if (!host->persist_pending)
    {
        pthread_cond_destroy(&host->persist_cv);
        pthread_mutex_destroy(&host->persist_mu);
        return;
    }
    host->persist_stop = false;
    host->persist_pending_count = 0;
    host->persist_flight_agent_id = 0;
    host->persist_flight_catalog_order = false;
    host->persist_catalog_next_generation = 0;
    host->persist_catalog_completed_generation = 0;
    host->persist_catalog_failed_generation = 0;
    host->persist_catalog_error[0] = '\0';
    host->persist_failure_count = 0;
    host->persist_ready = true;
    if (pthread_create(&host->persist_thread, NULL, PersistThreadMain, host) != 0)
    {
        host->persist_ready = false;
        free(host->persist_pending);
        host->persist_pending = NULL;
        pthread_cond_destroy(&host->persist_cv);
        pthread_mutex_destroy(&host->persist_mu);
        return;
    }
}

void PicoSessionPersist_Shutdown(PicoHost *host)
{
    int i;
    if (!host)
    {
        return;
    }
    if (host->persist_ready)
    {
        pthread_mutex_lock(&host->persist_mu);
        host->persist_stop = true;
        pthread_cond_broadcast(&host->persist_cv);
        pthread_mutex_unlock(&host->persist_mu);
    }
    if (host->persist_ready)
    {
        pthread_join(host->persist_thread, NULL);
        for (i = 0; i < host->persist_pending_count; i++)
        {
            PersistJobClear(&host->persist_pending[i]);
        }
        host->persist_pending_count = 0;
        free(host->persist_pending);
        host->persist_pending = NULL;
        host->persist_flight_agent_id = 0;
        host->persist_flight_catalog_order = false;
        host->persist_catalog_next_generation = 0;
        host->persist_catalog_completed_generation = 0;
        host->persist_catalog_failed_generation = 0;
        host->persist_catalog_error[0] = '\0';
        host->persist_failure_count = 0;
        pthread_cond_destroy(&host->persist_cv);
        pthread_mutex_destroy(&host->persist_mu);
        host->persist_ready = false;
        host->persist_stop = false;
    }
}

void PicoSessionPersist_Pump(PicoHost *host)
{
    PicoSessionPersistFailure local[PICO_MAX_TOTAL_AGENTS];
    char catalog_error[256];
    int n = 0;
    if (!host || !host->persist_ready)
    {
        return;
    }
    catalog_error[0] = '\0';
    pthread_mutex_lock(&host->persist_mu);
    n = PersistTakeFailuresLocked(host, 0, local, PICO_MAX_TOTAL_AGENTS);
    if (host->persist_catalog_error[0])
    {
        snprintf(catalog_error, sizeof(catalog_error), "%s", host->persist_catalog_error);
        host->persist_catalog_error[0] = '\0';
    }
    pthread_mutex_unlock(&host->persist_mu);
    PersistApplyFailures(host, local, n);
    if (catalog_error[0])
    {
        char line[320];
        snprintf(line, sizeof(line), "Workspace order persistence failed: %s", catalog_error);
        pico_status_warn(host, line);
    }
}

static bool PersistHasCatalogOrderLocked(const PicoHost *host)
{
    int i;
    if (!host || !host->persist_pending)
    {
        return false;
    }
    if (host->persist_flight_catalog_order)
    {
        return true;
    }
    for (i = 0; i < host->persist_pending_count; i++)
    {
        if (host->persist_pending[i].job_kind == PICO_PERSIST_JOB_CATALOG_ORDER)
        {
            return true;
        }
    }
    return false;
}

bool PicoCatalog_DrainOrderPersistBefore(PicoHost *host, const struct timespec *deadline)
{
    bool drained;
    if (!host || !host->persist_ready)
    {
        return true;
    }
    pthread_mutex_lock(&host->persist_mu);
    while (PersistHasCatalogOrderLocked(host))
    {
        int wait_result = deadline
            ? pthread_cond_timedwait(&host->persist_cv, &host->persist_mu, deadline)
            : pthread_cond_wait(&host->persist_cv, &host->persist_mu);
        if (wait_result != 0 && PersistHasCatalogOrderLocked(host))
        {
            break;
        }
    }
    drained = !PersistHasCatalogOrderLocked(host);
    pthread_mutex_unlock(&host->persist_mu);
    return drained;
}

bool PicoSession_DrainPersistBefore(PicoHost *app, PicoAgent *agent, const struct timespec *deadline)
{
    PicoSessionPersistFailure local[PICO_MAX_TOTAL_AGENTS];
    bool drained;
    int n = 0;
    if (!app || !agent || !app->persist_ready || agent->id == 0)
    {
        return true;
    }
    pthread_mutex_lock(&app->persist_mu);
    while (PersistHasWorkLocked(app, agent->id))
    {
        int wait_result = deadline
            ? pthread_cond_timedwait(&app->persist_cv, &app->persist_mu, deadline)
            : pthread_cond_wait(&app->persist_cv, &app->persist_mu);
        if (wait_result != 0 && PersistHasWorkLocked(app, agent->id))
        {
            break;
        }
    }
    drained = !PersistHasWorkLocked(app, agent->id);
    n = PersistTakeFailuresLocked(app, agent->id, local, PICO_MAX_TOTAL_AGENTS);
    pthread_mutex_unlock(&app->persist_mu);
    PersistApplyFailures(app, local, n);
    return drained;
}

/* UI-thread callers must never wait on the persist thread without a bound:
 * a stalled disk or a stuck session lock must not freeze the host. On timeout
 * the queued records still carry their own copies and land later. */
#define PICO_PERSIST_DRAIN_TIMEOUT_SEC 1

static bool DrainPersistUiBound(PicoHost *app, PicoAgent *agent)
{
    struct timespec deadline;
    clock_gettime(CLOCK_REALTIME, &deadline);
    deadline.tv_sec += PICO_PERSIST_DRAIN_TIMEOUT_SEC;
    return PicoSession_DrainPersistBefore(app, agent, &deadline);
}

void PicoSession_DrainPersist(PicoHost *app, PicoAgent *agent)
{
    (void)DrainPersistUiBound(app, agent);
}

uint64_t PicoCatalog_EnqueueOrder(PicoHost *host,
                                  const PicoCatalogWorkspace *workspaces, int count)
{
    PicoSessionPersistJob *pending;
    PicoSessionPersistJob job;
    char *json;
    uint64_t generation;
    if (!host || !workspaces || count <= 0 || count > PICO_MAX_CATALOG_WORKSPACES)
    {
        return 0;
    }
    json = CatalogOrderSerialize(workspaces, count);
    if (!json)
    {
        pico_status_warn(host, "Could not prepare workspace order persistence.");
        return 0;
    }
    if (!host->persist_ready)
    {
        free(json);
        pico_status_warn(host, "Workspace order persistence is unavailable.");
        return 0;
    }

    memset(&job, 0, sizeof(job));
    job.job_kind = PICO_PERSIST_JOB_CATALOG_ORDER;
    job.catalog_order_json = json;
    pthread_mutex_lock(&host->persist_mu);
    generation = ++host->persist_catalog_next_generation;
    if (generation == 0)
    {
        generation = ++host->persist_catalog_next_generation;
    }
    job.catalog_order_generation = generation;
    pending = PersistPendingCatalogOrderLocked(host);
    if (pending)
    {
        free(pending->catalog_order_json);
        pending->catalog_order_json = job.catalog_order_json;
        pending->catalog_order_generation = generation;
        job.catalog_order_json = NULL;
        PersistJobClear(&job);
    }
    else if (host->persist_pending &&
             host->persist_pending_count < PICO_PERSIST_QUEUE_CAPACITY)
    {
        host->persist_pending[host->persist_pending_count++] = job;
        pthread_cond_signal(&host->persist_cv);
    }
    else
    {
        pthread_mutex_unlock(&host->persist_mu);
        PersistJobClear(&job);
        pico_status_warn(host, "Workspace order persist queue is full.");
        return 0;
    }
    pthread_mutex_unlock(&host->persist_mu);
    return generation;
}

PicoCatalogPersistStatus PicoCatalog_OrderPersistStatus(PicoHost *host,
                                                        uint64_t generation)
{
    PicoCatalogPersistStatus status = PICO_CATALOG_PERSIST_PENDING;
    if (!host || !host->persist_ready || generation == 0)
    {
        return PICO_CATALOG_PERSIST_FAILED;
    }
    pthread_mutex_lock(&host->persist_mu);
    if (host->persist_catalog_completed_generation >= generation)
    {
        status = host->persist_catalog_failed_generation == generation
            ? PICO_CATALOG_PERSIST_FAILED
            : PICO_CATALOG_PERSIST_SUCCEEDED;
    }
    pthread_mutex_unlock(&host->persist_mu);
    return status;
}

/* Queue one JSONL record on the persist thread. Lines queued for the same
 * agent accumulate in FIFO order in its pending slot, so the on-disk order
 * always matches the call order and the calling thread never touches the
 * session file. Write failures surface asynchronously through the persist
 * failure pump, which moves the agent to PICO_SESSION_FAILED. */
static PicoSessionWriteResult QueueSessionLine(PicoHost *app, PicoAgent *agent, const char *json)
{
    PicoSessionPersistJob *pending;
    PicoSessionPersistJob job;
    bool new_identity;
    if (!app || !agent || !json || !json[0])
    {
        return PICO_SESSION_WRITE_FAILED;
    }
    if (agent->persistence == PICO_SESSION_EPHEMERAL)
    {
        return PICO_SESSION_WRITE_SKIPPED;
    }
    if (agent->persistence == PICO_SESSION_FAILED)
    {
        return PICO_SESSION_WRITE_FAILED;
    }
    if (!app->persist_ready)
    {
        return PICO_SESSION_WRITE_FAILED;
    }

    memset(&job, 0, sizeof(job));
    job.job_kind = PICO_PERSIST_JOB_SESSION;
    job.event_json = JsonDup(json);
    if (!job.event_json)
    {
        PersistenceFailed(app, agent, "out of memory while queueing the session write");
        return PICO_SESSION_WRITE_FAILED;
    }
    new_identity = !agent->session_path[0];
    if (AssignSessionIdentity(app, agent) != 0 || !agent->session_path[0] || agent->id == 0)
    {
        PersistJobClear(&job);
        return PICO_SESSION_WRITE_FAILED;
    }
    if (new_identity)
    {
        job.header_json = BuildSessionHeaderJson(app, agent);
        if (!job.header_json)
        {
            PersistJobClear(&job);
            PersistenceFailed(app, agent, "out of memory while creating the session header");
            return PICO_SESSION_WRITE_FAILED;
        }
    }
    job.agent_id = agent->id;
    job.kind = agent->kind;
    job.persistence = agent->persistence;
    snprintf(job.session_id, sizeof(job.session_id), "%s", agent->session_id);
    snprintf(job.session_path, sizeof(job.session_path), "%s", agent->session_path);
    snprintf(job.workspace_path, sizeof(job.workspace_path), "%s",
             PicoWorkspace_Path(SessionWorkspace(app, agent)));

    pthread_mutex_lock(&app->persist_mu);
    /* A write failure already recorded for this session (not yet applied to
     * the agent) rejects further appends so no records are written past a
     * dropped one. */
    if (PersistHasFailureLocked(app, agent->id, agent->session_id))
    {
        pthread_mutex_unlock(&app->persist_mu);
        PersistJobClear(&job);
        return PICO_SESSION_WRITE_FAILED;
    }
    pending = PersistPendingForAgentLocked(app, agent->id, agent->session_id);
    if (pending)
    {
        size_t have = pending->event_json ? strlen(pending->event_json) : 0;
        size_t add = strlen(job.event_json);
        char *merged = (char *)realloc(pending->event_json, have + (have ? 1 : 0) + add + 1);
        if (!merged)
        {
            pthread_mutex_unlock(&app->persist_mu);
            PersistJobClear(&job);
            PersistenceFailed(app, agent, "out of memory while queueing the session write");
            return PICO_SESSION_WRITE_FAILED;
        }
        if (have)
        {
            merged[have] = '\n';
        }
        memcpy(merged + have + (have ? 1 : 0), job.event_json, add + 1);
        pending->event_json = merged;
        if (!pending->header_json && job.header_json)
        {
            pending->header_json = job.header_json;
            job.header_json = NULL;
        }
        PersistJobClear(&job);
    }
    else if (app->persist_pending &&
             app->persist_pending_count < PICO_PERSIST_QUEUE_CAPACITY)
    {
        app->persist_pending[app->persist_pending_count++] = job;
        pthread_cond_signal(&app->persist_cv);
    }
    else
    {
        pthread_mutex_unlock(&app->persist_mu);
        PersistJobClear(&job);
        PersistenceFailed(app, agent, "session persist queue is full");
        return PICO_SESSION_WRITE_FAILED;
    }
    pthread_mutex_unlock(&app->persist_mu);
    return PICO_SESSION_WRITE_OK;
}

void PicoSession_EnqueueModelChange(PicoHost *app, PicoAgent *agent)
{
    char *json;
    if (!app || !agent || agent->persistence != PICO_SESSION_DURABLE)
    {
        return;
    }
    if (!app->persist_ready)
    {
        (void)PicoSession_LogModelChange(app, agent, agent->model,
                                         agent->effort[0] ? agent->effort : "none");
        return;
    }
    json = BuildModelChangeJson(agent->model, agent->effort[0] ? agent->effort : "none");
    if (!json)
    {
        PersistenceFailed(app, agent, "out of memory while logging the model change");
        return;
    }
    (void)QueueSessionLine(app, agent, json);
    free(json);
}
