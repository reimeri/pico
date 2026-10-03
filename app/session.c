#define _DEFAULT_SOURCE
#define _POSIX_C_SOURCE 200809L

#include "session.h"
#include "agent.h"
#include "workspace_internal.h"
#include "worktree.h"
#include "json.h"
#include "path.h"
#include "posix_io.h"
#include "settings.h"
#include "usage.h"
#include "host_internal.h"
#include "overlay.h"

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <pthread.h>
#include <sqlite3.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

static bool CatalogMetaPath(const char *dir, char *out, size_t cap);
static int CatalogLockAcquire(const char *dir);
static int CatalogDeletionGuardAcquire(char *error, size_t error_cap);
static void CatalogDeletionGuardRelease(int fd);
static void PathBasename(const char *path, char *out, size_t cap);
static bool CatalogKeyFromPath(const char *path, char *out, size_t cap);
static void CatalogLockRelease(int fd);
static void CatalogRowFromFile(const char *path, PicoCatalogSession *row);
static bool CatalogProjectMetaPath(const char *project, char *out, size_t cap);
static int CmpCatalogOrder(const void *a, const void *b);
static void CatalogClearSessions(PicoCatalogWorkspace *ws);
static void CatalogMarkChanged(void);
static void CatalogWriteThrough(PicoHost *app, const PicoAgent *agent,
                                const char *title_override, const char *event_json,
                                const struct stat *previous_stat);
static void CatalogWriteThroughFields(PicoAgentKind kind, PicoSessionPersistence persistence,
                                      const char *session_id, const char *session_path,
                                      const char *ws_path, const char *title_override,
                                      const char *event_json, const struct stat *previous_stat);
static sqlite3 *CatalogDbOpen(void);
static bool CatalogDbExec(sqlite3 *db, const char *sql);
static bool CatalogDbPrepare(sqlite3 *db, sqlite3_stmt **stmt, const char *sql);
static const char *CatalogDbText(sqlite3_stmt *stmt, int column);
static bool CatalogDbReadProject(sqlite3 *db, const char *path, PicoCatalogWorkspace *ws);
static bool CatalogDbWorkspace(sqlite3 *db, const char *path, const char *key,
                               const char *project, const char *checkout, bool worktree);
static bool CatalogDbReconcileDir(sqlite3 *db, const char *dir, const char *workspace,
                                  const atomic_bool *cancelled, bool force);
static bool CatalogDbSeed(sqlite3 *db, const char *path);
static int CatalogDbList(sqlite3 *db, const char *workspace, const char *search,
                         PicoSessionInfo **out, bool parents_only, int maximum);
static PicoSessionWriteResult QueueSessionLine(PicoHost *app, PicoAgent *agent,
                                                 const char *json);
static PicoSessionWriteResult QueueSessionTitle(PicoHost *app, PicoAgent *agent,
                                                const char *title);
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
            char *project_path = JsonObjStr(&doc, 0, "project_path");
            if (project_path && project_path[0])
                snprintf(info->project_path, sizeof(info->project_path), "%s", project_path);
            free(project_path);
            {
                int worktree = JsonObjGet(&doc, 0, "worktree");
                info->worktree = JsonEq(&doc, worktree, "true") || JsonEq(&doc, worktree, "1");
            }
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

static bool CatalogCancelled(const atomic_bool *cancelled)
{
    return cancelled && atomic_load(cancelled);
}

int PicoSession_List(const PicoWorkspace *workspace, PicoSessionInfo **out, bool parents_only)
{
    sqlite3 *db;
    char dir[4096];
    int n = 0;
    if (out) *out = NULL;
    if (!workspace || !out || !SessionDir(workspace, dir, sizeof(dir))) return 0;
    db = CatalogDbOpen();
    if (!db) return 0;
    if (!CatalogDbSeed(db, PicoWorkspace_Path(workspace)))
    { sqlite3_close(db); return 0; }
    int lock = CatalogLockAcquire(dir);
    if (lock >= 0)
    {
        if (CatalogDbReconcileDir(db, dir, PicoWorkspace_Path(workspace), NULL, true))
            n = CatalogDbList(db, PicoWorkspace_Path(workspace), NULL, out, parents_only, 0);
        CatalogLockRelease(lock);
    }
    sqlite3_close(db);
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
    int deletion_fd = -1;
    if (!session_path || !session_path[0] || !json)
    {
        if (error && error_cap > 0)
        {
            snprintf(error, error_cap, "session path is missing");
        }
        return false;
    }
    if (persistence == PICO_SESSION_DURABLE)
    {
        deletion_fd = CatalogDeletionGuardAcquire(error, error_cap);
        if (deletion_fd < 0) return false;
    }
    lock_fd = SessionLockAcquire(session_path, error, error_cap);
    if (lock_fd < 0)
    {
        if (deletion_fd >= 0) CatalogDeletionGuardRelease(deletion_fd);
        return false;
    }
    /* An old agent must not recreate a session deleted by another process. */
    if (write_catalog && access(session_path, F_OK) != 0)
    {
        failure = ENOENT;
        goto finish;
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
finish:
    if (failure == 0 && write_catalog)
    {
#ifdef PICO_SESSION_TEST_HOOKS
        (void)PicoSession_TestHook("catalog_before_upsert");
#endif
        CatalogWriteThroughFields(kind, persistence, session_id, session_path, workspace_path, NULL, json,
                                  have_previous_stat ? &previous_stat : NULL);
    }
    if (failure == 0 && kind == PICO_AGENT_MAIN && persistence == PICO_SESSION_DURABLE)
    {
        CatalogMarkChanged();
    }
    SessionLockRelease(lock_fd);
    if (deletion_fd >= 0) CatalogDeletionGuardRelease(deletion_fd);
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
    PicoWorkspace *session_workspace = agent->workspace;
    JsonBuf_Puts(&b, ",\"project_path\":");
    JsonBuf_String(&b, session_workspace && session_workspace->project_path[0]
                            ? session_workspace->project_path
                            : PicoWorkspace_Path(session_workspace));
    JsonBuf_Puts(&b, ",\"worktree\":");
    JsonBuf_Bool(&b, session_workspace && session_workspace->worktree);
    JsonBuf_Puts(&b, ",\"model\":");
    JsonBuf_String(&b, agent->model);
    JsonBuf_Puts(&b, ",\"fast\":");
    JsonBuf_Bool(&b, agent->fast);
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

static char *BuildModelChangeJson(const char *model, const char *effort, bool fast)
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
    JsonBuf_Puts(&b, ",\"fast\":");
    JsonBuf_Bool(&b, fast);
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

static void ApplyHeader(PicoAgent *agent, const JsonDoc *doc, int obj)
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
        agent->fast = JsonEq(doc, JsonObjGet(doc, obj, "fast"), "true");
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

static bool ReadNoticeSeverity(const JsonDoc *doc, int obj, PicoNoticeSeverity *out)
{
    int token = JsonObjGet(doc, obj, "severity");
    if (JsonEq(doc, token, "info")) *out = PICO_NOTICE_INFO;
    else if (JsonEq(doc, token, "warning")) *out = PICO_NOTICE_WARNING;
    else if (JsonEq(doc, token, "error")) *out = PICO_NOTICE_ERROR;
    else return false;
    return true;
}

typedef struct ReplayPreparedMessage {
    char *content;
    char *display;
    char *rendered;
    MdDocument doc;
} ReplayPreparedMessage;

static void ReplayLine(PicoHost *app, PicoAgent *agent, const JsonDoc *doc, int obj,
                       bool into_input, int *active_group,
                       ReplayPreparedMessage *prepared)
{
    char *type = JsonObjStr(doc, obj, "type");
    if (!type)
    {
        return;
    }
    if (strcmp(type, "session") == 0)
    {
        ApplyHeader(agent, doc, obj);
    }
    else if (strcmp(type, "usage") == 0)
    {
        int input_tokens = JsonObjInt(doc, obj, "input_tokens", 0);
        int cached_tokens = JsonObjInt(doc, obj, "cached_tokens", 0);
        PicoUsage_Apply(agent, input_tokens, cached_tokens, NULL);
        char *tier = JsonObjStr(doc, obj, "service_tier");
        snprintf(agent->last_service_tier, sizeof(agent->last_service_tier), "%s", tier ? tier : "");
        free(tier);
    }
    else if (strcmp(type, "notice") == 0)
    {
        PicoNoticeSeverity severity;
        if (active_group) *active_group = -1;
        char *content = JsonObjStr(doc, obj, "content");
        if (content && ReadNoticeSeverity(doc, obj, &severity))
            PicoAgent_AddNotice(app, agent, severity, content);
        free(content);
    }
    else if (strcmp(type, "message") == 0)
    {
        char *role = JsonObjStr(doc, obj, "role");
        char *content = prepared && prepared->content ? prepared->content
                                                        : JsonObjStr(doc, obj, "content");
        if (role && strcmp(role, "user") == 0)
        {
            if (active_group)
            {
                *active_group = -1;
            }
            char *display = prepared && prepared->display ? prepared->display
                                                           : JsonObjStr(doc, obj, "display");
            char *parts = JsonObjRaw(doc, obj, "parts");
            if (prepared && prepared->doc.arena.primary.memory)
                PicoAgent_AddMessagePrepared(app, agent, PICO_ROLE_USER,
                    display && display[0] ? display : (content ? content : ""), &prepared->doc, prepared->rendered);
            else
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
            if (!prepared || display != prepared->display) free(display);
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
                if (prepared && prepared->doc.arena.primary.memory)
                    PicoAgent_AddMessagePrepared(app, agent, PICO_ROLE_ASSISTANT, text, &prepared->doc, prepared->rendered);
                else PicoAgent_AddMessage(app, agent, PICO_ROLE_ASSISTANT, text);
            }
            else
            {
                if (prepared && prepared->doc.arena.primary.memory)
                    PicoAgent_AppendAssistantPrepared(app, agent, text, &prepared->doc, prepared->rendered);
                else PicoAgent_AppendAssistant(app, agent, text);
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
        if (!prepared || content != prepared->content) free(content);
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
        agent->fast = JsonEq(doc, JsonObjGet(doc, obj, "fast"), "true");
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

/* Only owned file data and JSON tokens live here; workers never hold a host,
 * agent, workspace, or registration pointer. */
typedef struct PicoSessionReplay {
    char **lines;
    JsonDoc *docs;
    int count;
    int cursor;
    int last_compact;
    int last_tool_call;
    int last_tool_result;
    int tool_calls;
    int tool_results;
    int active_group;
    ReplayPreparedMessage *prepared;
} PicoSessionReplay;

static void ReplayReleaseRecord(PicoSessionReplay *replay, int i)
{
    JsonFree(&replay->docs[i]);
    free(replay->lines[i]);
    replay->lines[i] = NULL;
    if (replay->prepared)
    {
        ReplayPreparedMessage *item = &replay->prepared[i];
        free(item->content);
        free(item->display);
        free(item->rendered);
        MdDocument_Free(&item->doc);
        memset(item, 0, sizeof(*item));
    }
}

void PicoSession_ReplayFree(PicoSessionReplay *replay)
{
    if (!replay) return;
    for (int i = 0; i < replay->count; i++)
        ReplayReleaseRecord(replay, i);
    free(replay->docs);
    free(replay->lines);
    free(replay->prepared);
    free(replay);
}

/* Reading and strict validation are worker-safe. All ReplayLine calls remain
 * on the main thread, including historical extension tool apply callbacks. */
static PicoSessionReplay *ReplayPrepareBefore(const char *path, PicoAgentKind kind,
                                              const atomic_bool *cancelled,
                                              bool prepare_messages)
{
    FILE *f = fopen(path, "rb");
    if (!f)
    {
        return NULL;
    }


    JsonDoc *docs = NULL;
    char **lines = NULL;
    int n = 0;
    int cap = 0;
    char *buf = NULL;
    size_t buf_cap = 0;
    bool read_failed = false;
    while ((!cancelled || !atomic_load(cancelled)) && getline(&buf, &buf_cap, f) != -1)
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
    if (read_failed || n == 0 || (cancelled && atomic_load(cancelled)))
    {
        goto invalid;
    }

    docs = (JsonDoc *)calloc((size_t)n, sizeof(JsonDoc));
    if (!docs) goto invalid;
    int last_compact = -1;
    int last_tool_call = -1;
    int last_tool_result = -1;
    int tool_calls = 0;
    int tool_results = 0;
    bool valid_header = false;
    for (int i = 0; i < n; i++)
    {
        if (cancelled && atomic_load(cancelled)) goto invalid;
        JsonDoc *doc = &docs[i];
        if (JsonParse(doc, lines[i], strlen(lines[i])) != 0 || !JsonIsObject(doc, 0))
        {
            goto invalid;
        }
        char *type = JsonObjStr(doc, 0, "type");
        if (i == 0)
        {
            char *header_id = JsonObjStr(doc, 0, "id");
            char *header_kind = JsonObjStr(doc, 0, "kind");
            char *profile = JsonObjStr(doc, 0, "profile");
            char *purpose = JsonObjStr(doc, 0, "initial_purpose");
            int version = JsonObjInt(doc, 0, "version", 0);
            bool normal = header_kind && strcmp(header_kind, "normal") == 0;
            bool subagent = header_kind && strcmp(header_kind, "subagent") == 0 &&
                            profile && profile[0] && purpose && purpose[0];
            bool compatible_kind = (normal && kind == PICO_AGENT_MAIN) ||
                                   (subagent && kind == PICO_AGENT_SUBAGENT);
            valid_header = type && strcmp(type, "session") == 0 &&
                           header_id && header_id[0] && version == 4 &&
                           compatible_kind;
            free(header_id);
            free(header_kind);
            free(profile);
            free(purpose);
            if (!valid_header)
            {
                free(type);
                goto invalid;
            }
        }
        bool requires_group = type && strcmp(type, "tool_call") == 0;
        if (type && strcmp(type, "message") == 0)
        {
            char *role = JsonObjStr(doc, 0, "role");
            requires_group = role && strcmp(role, "assistant") == 0;
            free(role);
        }
        if (requires_group && !JsonObjNonNegativeInt(doc, 0, "message_group", NULL))
        {
            free(type);
            goto invalid;
        }
        if (type && strcmp(type, "compaction") == 0)
        {
            last_compact = i;
        }
        else if (type && strcmp(type, "tool_call") == 0)
        {
            last_tool_call = i;
            tool_calls++;
        }
        else if (type && strcmp(type, "tool_result") == 0)
        {
            last_tool_result = i;
            tool_results++;
        }
        free(type);

    }
    if (!valid_header) goto invalid;

    PicoSessionReplay *replay = (PicoSessionReplay *)calloc(1, sizeof(*replay));
    if (!replay) goto invalid;
    replay->lines = lines;
    replay->docs = docs;
    replay->count = n;
    replay->last_compact = last_compact;
    replay->last_tool_call = last_tool_call;
    replay->last_tool_result = last_tool_result;
    replay->tool_calls = tool_calls;
    replay->tool_results = tool_results;
    replay->active_group = -1;
    if (prepare_messages)
    {
        replay->prepared = calloc((size_t)n, sizeof(*replay->prepared));
        if (!replay->prepared) { PicoSession_ReplayFree(replay); return NULL; }
        int group = -1;
        int prepared_docs = 0;
        /* Keep worker preparation from multiplying one arena per JSONL
         * record across an arbitrarily long session. Unprepared records
         * retain the existing main-thread replay behavior. */
        const int max_prepared_docs = 16;
        char *assembled = NULL;
        size_t assembled_len = 0;
        for (int i = 0; i < n; i++)
        {
            if (cancelled && atomic_load(cancelled)) break;
            JsonDoc *doc = &replay->docs[i];
            ReplayPreparedMessage *item = &replay->prepared[i];
            if (JsonEq(doc, JsonObjGet(doc, 0, "type"), "message"))
            {
                if (JsonEq(doc, JsonObjGet(doc, 0, "role"), "user"))
                {
                    free(assembled); assembled = NULL; assembled_len = 0; group = -1;
                    item->content = JsonObjStr(doc, 0, "content");
                    item->display = JsonObjStr(doc, 0, "display");
                    const char *shown = item->display && item->display[0]
                                            ? item->display : item->content;
                    if (shown && strlen(shown) > 8192 && prepared_docs < max_prepared_docs)
                    {
                        prepared_docs++;
                        item->rendered = JsonDup(shown);
                        if (item->rendered)
                            item->doc = MdDocument_ParseEx(shown, strlen(shown),
                                                          MD_PARSE_PRESERVE_NEWLINES);
                    }
                }
                else if (JsonEq(doc, JsonObjGet(doc, 0, "role"), "assistant"))
                {
                    int next_group = -1;
                    (void)JsonObjNonNegativeInt(doc, 0, "message_group", &next_group);
                    if (group != next_group)
                    {
                        free(assembled); assembled = NULL; assembled_len = 0;
                        group = next_group;
                    }
                    item->content = JsonObjStr(doc, 0, "content");
                    size_t length = item->content ? strlen(item->content) : 0;
                    char *next = realloc(assembled, assembled_len + length + 1);
                    if (next)
                    {
                        assembled = next;
                        if (length) memcpy(assembled + assembled_len, item->content, length);
                        assembled_len += length;
                        assembled[assembled_len] = '\0';
                        /* Do not retain a 1 MiB parse arena for every tiny
                         * fragment of one long assistant turn. Small chunks
                         * keep the existing batched UI replay path. */
                        if (length > 8192 && prepared_docs < max_prepared_docs)
                        {
                            prepared_docs++;
                            item->rendered = JsonDup(assembled);
                            if (item->rendered)
                                item->doc = MdDocument_ParseEx(assembled, assembled_len,
                                                               MD_PARSE_DEFAULT);
                        }
                    }
                }
            }
            else if (JsonEq(doc, JsonObjGet(doc, 0, "type"), "notice"))
            {
                free(assembled); assembled = NULL; assembled_len = 0; group = -1;
            }
            else if (JsonEq(doc, JsonObjGet(doc, 0, "type"), "tool_call"))
            {
                int next_group = -1;
                (void)JsonObjNonNegativeInt(doc, 0, "message_group", &next_group);
                if (group != next_group)
                {
                    free(assembled); assembled = NULL; assembled_len = 0;
                    group = next_group;
                }
            }
        }
        free(assembled);
    }
    return replay;
invalid:
    for (int i = 0; i < n; i++)
    {
        if (docs) JsonFree(&docs[i]);
        free(lines[i]);
    }
    free(docs);
    free(lines);
    return NULL;
}

PicoSessionReplay *PicoSession_ReplayPrepare(const char *path, PicoAgentKind kind)
{
    return ReplayPrepareBefore(path, kind, NULL, false);
}

/* Returns true when complete. Call on the main thread only. */
bool PicoSession_ReplayBatch(PicoHost *app, PicoAgent *agent, PicoSessionReplay *replay,
                             int max_records)
{
    if (!app || !agent || !replay || max_records <= 0) return false;
    int end = replay->cursor + max_records;
    if (end > replay->count) end = replay->count;
    for (int i = replay->cursor; i < end; i++)
    {
        bool into_input = replay->last_compact < 0 || i >= replay->last_compact;
        ReplayLine(app, agent, &replay->docs[i], 0, into_input, &replay->active_group,
                   replay->prepared ? &replay->prepared[i] : NULL);
        /* The final unmatched call is needed by ReplayFinish to append its
         * interrupted result; every other record can be released now. */
        if (i != replay->last_tool_call ||
            replay->last_tool_call <= replay->last_tool_result ||
            replay->tool_calls <= replay->tool_results)
            ReplayReleaseRecord(replay, i);
    }
    replay->cursor = end;
    return end == replay->count;
}

static void ReplayAppendInterrupted(PicoHost *app, PicoAgent *agent,
                                    const PicoSessionReplay *replay)
{
    if (replay->last_tool_call <= replay->last_tool_result ||
        replay->tool_calls <= replay->tool_results) return;
    const JsonDoc *doc = &replay->docs[replay->last_tool_call];
    char *call_id = JsonObjStr(doc, 0, "call_id");
    char *name = JsonObjStr(doc, 0, "name");
    PicoSession_LogToolResult(app, agent, call_id, name, "(interrupted)", true, NULL);
    PicoAgent_SetLastToolOutput(agent, "(interrupted)", true);
    PicoAgent_PushHistoryFunctionOutput(agent, call_id, name, "(interrupted)", true);
    free(call_id);
    free(name);
}

void PicoSession_ReplayFinish(PicoHost *app, PicoAgent *agent,
                               const PicoSessionReplay *replay, bool append_interrupted)
{
    if (append_interrupted) ReplayAppendInterrupted(app, agent, replay);
    agent->accepted_submit = true;
}

void PicoSession_ReplayPrepared(PicoHost *app, PicoAgent *agent, const char *path,
                                 PicoSessionReplay *replay, bool append_interrupted)
{
    snprintf(agent->session_path, sizeof(agent->session_path), "%s", path);
    PicoAgent_ClearInput(agent);
    (void)PicoSession_ReplayBatch(app, agent, replay, replay->count);
    app->chat_follow_bottom = true;
    PicoSession_ReplayFinish(app, agent, replay, append_interrupted);
    for (int i = 0; i < agent->message_count; i++)
        for (int t = 0; t < agent->messages[i].trace_count; t++)
            agent->messages[i].trace[t].tool_done_t0 = 0.0;
}

int PicoSession_Replay(PicoHost *app, PicoAgent *agent, const char *path,
                       bool append_interrupted)
{
    PicoSessionReplay *replay = PicoSession_ReplayPrepare(path, agent->kind);
    if (!replay)
    {
        agent->session_path[0] = '\0';
        return -1;
    }
    PicoSession_ReplayPrepared(app, agent, path, replay, append_interrupted);
    PicoSession_ReplayFree(replay);
    return 0;
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
    msg->source_len = msg->source ? strlen(msg->source) : 0;
    msg->source_cap = msg->source ? msg->source_len + 1 : 0;
    msg->revision = 1;
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
    size_t old = msg->source_len;
    size_t n = strlen(text);
    size_t need = old + n + 1;
    if (need > msg->source_cap)
    {
        size_t cap = msg->source_cap ? msg->source_cap : 16;
        char *grown;
        while (cap < need)
        {
            if (cap > (size_t)-1 / 2)
            {
                return false;
            }
            cap *= 2;
        }
        grown = (char *)realloc(msg->source, cap);
        if (!grown)
        {
            return false;
        }
        msg->source = grown;
        msg->source_cap = cap;
    }
    memcpy(msg->source + old, text, n + 1);
    msg->source_len = old + n;
    msg->revision++;
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
        else if (type && strcmp(type, "notice") == 0)
        {
            PicoNoticeSeverity severity;
            active_group = -1;
            char *content = JsonObjStr(&doc, 0, "content");
            if (content && ReadNoticeSeverity(&doc, 0, &severity))
            {
                failed = !LoadedAddMessage(&messages, &count, &capacity, PICO_ROLE_NOTICE, content);
                if (!failed) messages[count - 1].notice_severity = severity;
            }
            free(content);
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

int PicoSession_Search(const PicoWorkspace *workspace, const char *search,
                       PicoSessionInfo **out, bool parents_only, int maximum)
{
    sqlite3 *db;
    if (out) *out = NULL;
    if (!workspace || !out || maximum < 1 || !(db = CatalogDbOpen())) return 0;
    char dir[4096];
    if (!SessionDir(workspace, dir, sizeof(dir)))
    { sqlite3_close(db); return 0; }
    int count = 0;
    if (CatalogDbSeed(db, PicoWorkspace_Path(workspace)))
    {
        int lock = CatalogLockAcquire(dir);
        if (lock >= 0)
        {
            if (CatalogDbReconcileDir(db, dir, PicoWorkspace_Path(workspace), NULL, false))
                count = CatalogDbList(db, PicoWorkspace_Path(workspace), search,
                                      out, parents_only, maximum);
            CatalogLockRelease(lock);
        }
    }
    sqlite3_close(db);
    return count;
}

int PicoSession_Resolve(const PicoWorkspace *workspace, const char *id, bool allow_prefix,
                        char *path, size_t path_cap)
{
    sqlite3 *db;
    sqlite3_stmt *stmt = NULL;
    char dir[4096], exact[4096] = {0};
    bool ok = false;
    if (!workspace || !id || !id[0] || !path || path_cap == 0 ||
        !SessionDir(workspace, dir, sizeof(dir)) || !(db = CatalogDbOpen())) return -1;
    if (!CatalogDbSeed(db, PicoWorkspace_Path(workspace)))
    { sqlite3_close(db); return -1; }
    /* Exact IDs must win regardless of the number of prefix matches. */
    for (int attempt = 0; attempt < 2 && !ok; attempt++)
    {
        if (!CatalogDbPrepare(db, &stmt,
            "SELECT file_path FROM sessions WHERE workspace_path=? AND id=?")) break;
        sqlite3_bind_text(stmt, 1, PicoWorkspace_Path(workspace), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 2, id, -1, SQLITE_TRANSIENT);
        if (sqlite3_step(stmt) == SQLITE_ROW)
            snprintf(exact, sizeof(exact), "%s", CatalogDbText(stmt, 0));
        sqlite3_finalize(stmt); stmt = NULL;
        if (!exact[0] && allow_prefix && CatalogDbPrepare(db, &stmt,
            "SELECT file_path FROM sessions WHERE workspace_path=? AND"
            " substr(id,1,length(?))=? LIMIT 2"))
        {
            sqlite3_bind_text(stmt, 1, PicoWorkspace_Path(workspace), -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(stmt, 2, id, -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(stmt, 3, id, -1, SQLITE_TRANSIENT);
            if (sqlite3_step(stmt) == SQLITE_ROW)
            {
                snprintf(exact, sizeof(exact), "%s", CatalogDbText(stmt, 0));
                if (sqlite3_step(stmt) == SQLITE_ROW) exact[0] = '\0';
            }
            sqlite3_finalize(stmt); stmt = NULL;
        }
        char canonical[4096];
        ok = exact[0] && realpath(exact, canonical) && strlen(canonical) < path_cap;
        if (ok) snprintf(path, path_cap, "%s", canonical);
        if (!ok && attempt == 0)
        {
            exact[0] = '\0';
            int lock = CatalogLockAcquire(dir);
            if (lock >= 0)
            {
                (void)CatalogDbReconcileDir(db, dir, PicoWorkspace_Path(workspace), NULL, true);
                CatalogLockRelease(lock);
            }
        }
    }
    sqlite3_finalize(stmt);
    sqlite3_close(db);
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
    agent->fast = false;
    agent->last_service_tier[0] = '\0';
    agent->activity[0] = '\0';
    free(agent->compact_summary);
    agent->compact_summary = NULL;
    if (agent->persistence != PICO_SESSION_EPHEMERAL)
    {
        agent->persistence = PICO_SESSION_DURABLE;
    }
    free(agent->turn_user_request);
    agent->turn_user_request = NULL;
    free(agent->originating_user_request);
    agent->originating_user_request = NULL;
    agent->session_id[0] = '\0';
    agent->session_path[0] = '\0';
    agent->accepted_submit = false;
    agent->unseen_complete = false;
}

PicoSessionWriteResult PicoSession_LogNotice(PicoHost *app, PicoAgent *agent,
                                            PicoNoticeSeverity severity, const char *content)
{
    const char *name;
    switch (severity)
    {
    case PICO_NOTICE_INFO: name = "info"; break;
    case PICO_NOTICE_WARNING: name = "warning"; break;
    case PICO_NOTICE_ERROR: name = "error"; break;
    default: return PICO_SESSION_WRITE_FAILED;
    }
    char *pre = EventPrefix("notice");
    JsonBuf b;
    JsonBuf_Init(&b);
    JsonBuf_Puts(&b, pre);
    JsonBuf_Puts(&b, ",\"severity\":");
    JsonBuf_String(&b, name);
    JsonBuf_Puts(&b, ",\"content\":");
    JsonBuf_String(&b, content ? content : "");
    JsonBuf_Putc(&b, '}');
    char *line = JsonBuf_Steal(&b);
    PicoSessionWriteResult result = AppendLine(app, agent, line);
    free(line);
    free(pre);
    return result;
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
                                            int input_tokens, int cached_tokens,
                                            bool fast, const char *service_tier)
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
    JsonBuf_Puts(&b, ",\"fast\":");
    JsonBuf_Bool(&b, fast);
    JsonBuf_Puts(&b, ",\"service_tier\":");
    JsonBuf_String(&b, service_tier ? service_tier : "");
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
    char *line = BuildModelChangeJson(model, effort, agent->fast);
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

static bool RewriteSessionTitleAtPath(const char *session_path, const char *title,
                                      PicoAgentKind kind, PicoSessionPersistence persistence,
                                      const char *session_id, const char *workspace_path,
                                      char *error, size_t error_cap)
{
    char error_buf[256];
    int lock_fd;
    int deletion_fd = -1;
    int failure = 0;
    struct stat previous_stat;
    bool have_previous_stat = false;
    bool renamed = false;
    FILE *src = NULL;
    char *header = NULL;
    char *event_line = NULL;
    int fd = -1;
    char tmp_path[4096 + 16];

    if (error && error_cap > 0)
    {
        error[0] = '\0';
    }
    if (!error || error_cap == 0)
    {
        error = error_buf;
        error_cap = sizeof(error_buf);
        error[0] = '\0';
    }
    tmp_path[0] = '\0';
    if (!session_path || !session_path[0] || !title || !title[0])
    {
        snprintf(error, error_cap, "session path is missing");
        return false;
    }

    if (persistence == PICO_SESSION_DURABLE)
    {
        deletion_fd = CatalogDeletionGuardAcquire(error, error_cap);
        if (deletion_fd < 0) return false;
    }
    lock_fd = SessionLockAcquire(session_path, error, error_cap);
    if (lock_fd < 0)
    {
        if (deletion_fd >= 0) CatalogDeletionGuardRelease(deletion_fd);
        return false;
    }

    have_previous_stat = stat(session_path, &previous_stat) == 0;
    src = fopen(session_path, "rb");
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

    if ((size_t)snprintf(tmp_path, sizeof(tmp_path), "%s.tmp.XXXXXX", session_path) >= sizeof(tmp_path))
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
    if (SessionTestFail("title_before_rename") || rename(tmp_path, session_path) != 0)
    {
        failure = errno ? errno : EIO;
        goto done;
    }
    renamed = true;
    if (SessionTestFail("title_dir_fsync") || !SyncParentDir(session_path))
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
        CatalogWriteThroughFields(kind, persistence, session_id, session_path, workspace_path, title, NULL,
                                  have_previous_stat ? &previous_stat : NULL);
        CatalogMarkChanged();
    }
    SessionLockRelease(lock_fd);
    if (deletion_fd >= 0) CatalogDeletionGuardRelease(deletion_fd);
    if (failure != 0)
    {
        snprintf(error, error_cap, "%s", strerror(failure));
        return false;
    }
    return true;
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
    /* With a persist thread, title rewrites queue behind earlier records so the
     * UI never blocks on the session lock, transcript copy, or fsync. */
    if (app->persist_ready)
    {
        return QueueSessionTitle(app, agent, title);
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
    if (!RewriteSessionTitleAtPath(agent->session_path, title, agent->kind, agent->persistence,
                                   agent->session_id, PicoWorkspace_Path(SessionWorkspace(app, agent)),
                                   error, sizeof(error)))
    {
        PersistenceFailed(app, agent, error);
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

/* Held while a JSONL is written or a project is removed. File locks coordinate
 * independent Pico processes; the mutex supplies the missing thread-level
 * serialization (POSIX record locks are process-scoped). */
static pthread_mutex_t g_catalog_deletion_mu = PTHREAD_MUTEX_INITIALIZER;

static int CatalogDeletionGuardAcquire(char *error, size_t error_cap)
{
    char root[4096], path[4096];
    if (!SessionsRoot(root, sizeof(root)) ||
        !PicoPath_Format(path, sizeof(path), "%s/.catalog-deletion", root))
    {
        if (error && error_cap) snprintf(error, error_cap, "catalog deletion lock path is too long");
        return -1;
    }
    Pico_MkdirP(root);
    pthread_mutex_lock(&g_catalog_deletion_mu);
    int fd = SessionLockAcquire(path, error, error_cap);
    if (fd < 0) pthread_mutex_unlock(&g_catalog_deletion_mu);
    return fd;
}

static void CatalogDeletionGuardRelease(int fd)
{
    SessionLockRelease(fd);
    pthread_mutex_unlock(&g_catalog_deletion_mu);
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

/* Project-level presentation survives even when only linked worktrees have catalogs. */
static bool CatalogProjectMetaPath(const char *project, char *out, size_t cap)
{
    char root[4096], key[4096];
    return project && project[0] && SessionsRoot(root, sizeof(root)) &&
           CatalogKeyFromPath(project, key, sizeof(key)) &&
           PicoPath_Format(out, cap, "%s/.project-%s.json", root, key);
}

static int CatalogProjectUpdate(const char *project, const char *name, int stash)
{
    char canonical[4096];
    sqlite3 *db;
    sqlite3_stmt *stmt = NULL;
    bool ok;
    if (!project || project[0] != '/' ||
        (CanonicalWorkspacePath(project, canonical, sizeof(canonical)) && strcmp(project, canonical) != 0) ||
        !(db = CatalogDbOpen())) return -1;
    sqlite3_busy_timeout(db, 30);
    ok = CatalogDbPrepare(db, &stmt,
        "INSERT INTO projects(path,name,stashed) VALUES(?,?,?)"
        " ON CONFLICT(path) DO UPDATE SET name=CASE WHEN ? THEN excluded.name ELSE projects.name END,"
        "stashed=CASE WHEN ? THEN excluded.stashed ELSE projects.stashed END");
    if (ok)
    {
        sqlite3_bind_text(stmt, 1, project, -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 2, name ? name : "", -1, SQLITE_TRANSIENT);
        sqlite3_bind_int(stmt, 3, stash > 0);
        sqlite3_bind_int(stmt, 4, name != NULL);
        sqlite3_bind_int(stmt, 5, stash >= 0);
        ok = sqlite3_step(stmt) == SQLITE_DONE;
    }
    sqlite3_finalize(stmt);
    sqlite3_close(db);
    if (ok) CatalogMarkChanged();
    return ok ? 0 : -1;
}

int PicoCatalog_SetProjectName(const char *project, const char *name)
{
    size_t n = name ? strlen(name) : 0;
    if (!n || n >= PICO_CATALOG_NAME_MAX) return -1;
    for (size_t i = 0; i < n; i++) if ((unsigned char)name[i] < 32) return -1;
    return CatalogProjectUpdate(project, name, -1);
}

int PicoCatalog_SetProjectStashed(const char *project, bool stashed)
{
    return CatalogProjectUpdate(project, NULL, stashed ? 1 : 0);
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
    sqlite3 *db = NULL;
    sqlite3_stmt *stmt = NULL;
    JsonDoc doc;
    bool ok;
    if (!json || !json[0] || JsonParse(&doc, json, strlen(json)) != 0)
    {
        snprintf(error, error_cap, "invalid workspace order");
        return false;
    }
    int items = JsonObjGet(&doc, 0, "workspaces");
    ok = JsonIsArray(&doc, items);
#ifdef PICO_SESSION_TEST_HOOKS
    if (ok && PicoSession_TestHook("catalog_order_before_write")) ok = false;
#endif
    if (ok && (db = CatalogDbOpen()))
        ok = CatalogDbExec(db, "BEGIN IMMEDIATE") &&
             CatalogDbExec(db, "DELETE FROM workspace_order") &&
             CatalogDbPrepare(db, &stmt, "INSERT INTO workspace_order(path,position) VALUES(?,?)");
    else ok = false;
    for (int i = 0; ok && i < JsonArrayLen(&doc, items); i++)
    {
        char *path = JsonStrDup(&doc, JsonArrayAt(&doc, items, i));
        if (!path) { ok = false; break; }
        sqlite3_bind_text(stmt, 1, path, -1, SQLITE_TRANSIENT);
        sqlite3_bind_int(stmt, 2, i);
        ok = sqlite3_step(stmt) == SQLITE_DONE;
        sqlite3_reset(stmt);
        sqlite3_clear_bindings(stmt);
        free(path);
    }
    sqlite3_finalize(stmt);
    if (db)
    {
        if (ok) ok = CatalogDbExec(db, "COMMIT");
        if (!ok)
        {
            snprintf(error, error_cap, "%s", sqlite3_errmsg(db));
            CatalogDbExec(db, "ROLLBACK");
        }
        sqlite3_close(db);
    }
    else if (!ok) snprintf(error, error_cap, "could not open catalog database");
    JsonFree(&doc);
    if (ok) CatalogMarkChanged();
    return ok;
}

static bool CatalogApplyOrder(sqlite3 *db, PicoCatalogWorkspace *list, int count)
{
    sqlite3_stmt *stmt = NULL;
    if (!list || count < 2) return true;
    bool ok = CatalogDbPrepare(db, &stmt, "SELECT path,position FROM workspace_order ORDER BY position");
    if (ok)
    {
        for (int i = 0; i < count; i++) list[i].order = count + i;
        int index = 0;
        int rc;
        while ((rc = sqlite3_step(stmt)) == SQLITE_ROW)
        {
            const char *path = CatalogDbText(stmt, 0);
            for (int j = 0; j < count; j++)
                if (list[j].order >= count &&
                    (!strcmp(list[j].path, path) || !strcmp(list[j].project_path, path)))
                    list[j].order = index;
            index++;
        }
        ok = rc == SQLITE_DONE;
        if (ok) qsort(list, (size_t)count, sizeof(*list), CmpCatalogOrder);
    }
    sqlite3_finalize(stmt);
    return ok;
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
    ws->session_capacity = 0;
}

static bool CatalogGrowSessions(PicoCatalogWorkspace *ws, int need)
{
    PicoCatalogSession *next;
    int cap;
    if (!ws)
    {
        return false;
    }
    if (need <= ws->session_capacity)
    {
        return true;
    }
    cap = ws->session_capacity > 0 ? ws->session_capacity : 8;
    while (cap < need)
    {
        if (cap > INT_MAX / 2)
        {
            return false;
        }
        cap *= 2;
    }
    next = (PicoCatalogSession *)realloc(ws->sessions, (size_t)cap * sizeof(*next));
    if (!next)
    {
        return false;
    }
    ws->sessions = next;
    ws->session_capacity = cap;
    return true;
}

static bool CatalogAppendSession(PicoCatalogWorkspace *ws, const PicoCatalogSession *src)
{
    if (!ws || !src || !src->id[0])
    {
        return false;
    }
    if (!CatalogGrowSessions(ws, ws->session_count + 1))
    {
        return false;
    }
    ws->sessions[ws->session_count++] = *src;
    return true;
}

/* SQLite is the catalog's index and presentation store. JSONL remains the
 * durable source of session contents. Each operation owns its own connection. */
static bool CatalogDbExec(sqlite3 *db, const char *sql)
{
    return sqlite3_exec(db, sql, NULL, NULL, NULL) == SQLITE_OK;
}

static bool CatalogDbPrepare(sqlite3 *db, sqlite3_stmt **stmt, const char *sql)
{
    return sqlite3_prepare_v2(db, sql, -1, stmt, NULL) == SQLITE_OK;
}

static const char *CatalogDbText(sqlite3_stmt *stmt, int column)
{
    const unsigned char *value = sqlite3_column_text(stmt, column);
    return value ? (const char *)value : "";
}

static bool CatalogDbImportWorkspace(sqlite3 *db, const char *dir, const char *key)
{
    char path[4096];
    char *raw;
    size_t len = 0;
    JsonDoc doc;
    sqlite3_stmt *stmt = NULL;
    bool ok;
    if (!CatalogMetaPath(dir, path, sizeof(path))) return false;
    raw = Pico_ReadFile(path, &len);
    if (!raw) return errno == ENOENT;
    if (JsonParse(&doc, raw, len) != 0) { free(raw); return false; }
    char *ws_path = JsonObjStr(&doc, 0, "path");
    char *project = JsonObjStr(&doc, 0, "project_path");
    char *checkout = JsonObjStr(&doc, 0, "checkout_name");
    char *name = JsonObjStr(&doc, 0, "name");
    ok = ws_path && ws_path[0] && JsonIsObject(&doc, 0) &&
         CatalogDbPrepare(db, &stmt,
             "INSERT INTO workspaces(path,key,project_path,checkout_name,worktree,name,collapsed,ord)"
             " VALUES(?,?,?,?,?,?,?,?)");
    if (ok)
    {
        sqlite3_bind_text(stmt, 1, ws_path, -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 2, key, -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 3, project && project[0] ? project : ws_path, -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 4, checkout ? checkout : "", -1, SQLITE_TRANSIENT);
        sqlite3_bind_int(stmt, 5, JsonEq(&doc, JsonObjGet(&doc, 0, "worktree"), "true"));
        sqlite3_bind_text(stmt, 6, name ? name : "", -1, SQLITE_TRANSIENT);
        sqlite3_bind_int(stmt, 7, JsonEq(&doc, JsonObjGet(&doc, 0, "collapsed"), "true"));
        sqlite3_bind_int(stmt, 8, JsonObjInt(&doc, 0, "order", 0));
        ok = sqlite3_step(stmt) == SQLITE_DONE;
    }
    sqlite3_finalize(stmt);
    free(ws_path); free(project); free(checkout); free(name);
    JsonFree(&doc);
    free(raw);
    return ok;
}

static bool CatalogDbImportProject(sqlite3 *db, const char *path)
{
    char *raw;
    size_t len = 0;
    JsonDoc doc;
    sqlite3_stmt *stmt = NULL;
    bool ok;
    raw = Pico_ReadFile(path, &len);
    if (!raw) return false;
    if (JsonParse(&doc, raw, len) != 0) { free(raw); return false; }
    char *project = JsonObjStr(&doc, 0, "path");
    char *name = JsonObjStr(&doc, 0, "name");
    ok = project && project[0] && JsonIsObject(&doc, 0) &&
         CatalogDbPrepare(db, &stmt, "INSERT INTO projects(path,name,stashed) VALUES(?,?,?)");
    if (ok)
    {
        sqlite3_bind_text(stmt, 1, project, -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 2, name ? name : "", -1, SQLITE_TRANSIENT);
        sqlite3_bind_int(stmt, 3, JsonEq(&doc, JsonObjGet(&doc, 0, "stashed"), "true"));
        ok = sqlite3_step(stmt) == SQLITE_DONE;
    }
    sqlite3_finalize(stmt);
    free(project); free(name); JsonFree(&doc); free(raw);
    return ok;
}

static bool CatalogDbImportOrder(sqlite3 *db, const char *root)
{
    char path[4096];
    size_t len = 0;
    char *raw;
    JsonDoc doc;
    sqlite3_stmt *stmt = NULL;
    bool ok = true;
    if (!PicoPath_Format(path, sizeof(path), "%s/.workspace-order.json", root)) return false;
    raw = Pico_ReadFile(path, &len);
    if (!raw) return errno == ENOENT;
    if (JsonParse(&doc, raw, len) != 0) { free(raw); return false; }
    int items = JsonObjGet(&doc, 0, "workspaces");
    if (!JsonIsArray(&doc, items)) ok = false;
    if (ok) ok = CatalogDbPrepare(db, &stmt, "INSERT OR REPLACE INTO workspace_order(path,position) VALUES(?,?)");
    for (int i = 0; ok && i < JsonArrayLen(&doc, items); i++)
    {
        char *name = JsonStrDup(&doc, JsonArrayAt(&doc, items, i));
        if (!name) { ok = false; break; }
        sqlite3_bind_text(stmt, 1, name, -1, SQLITE_TRANSIENT);
        sqlite3_bind_int(stmt, 2, i);
        ok = sqlite3_step(stmt) == SQLITE_DONE;
        sqlite3_reset(stmt);
        sqlite3_clear_bindings(stmt);
        free(name);
    }
    sqlite3_finalize(stmt); JsonFree(&doc); free(raw);
    return ok;
}

static bool CatalogDbImport(sqlite3 *db, const char *root)
{
    DIR *d = opendir(root);
    struct dirent *ent;
    bool ok = d != NULL;
    if (!ok) return false;
    while (ok)
    {
        errno = 0;
        ent = readdir(d);
        if (!ent) { if (errno != 0) ok = false; break; }
        char path[4096];
        struct stat st;
        if (ent->d_name[0] != '.')
        {
            if (PicoPath_Format(path, sizeof(path), "%s/%s", root, ent->d_name) &&
                lstat(path, &st) == 0 && S_ISDIR(st.st_mode))
                ok = CatalogDbImportWorkspace(db, path, ent->d_name);
        }
        else if (strncmp(ent->d_name, ".project-", 9) == 0 &&
                 strlen(ent->d_name) > 14 &&
                 strcmp(ent->d_name + strlen(ent->d_name) - 5, ".json") == 0)
        {
            if (PicoPath_Format(path, sizeof(path), "%s/%s", root, ent->d_name))
                ok = CatalogDbImportProject(db, path);
        }
    }
    closedir(d);
    return ok && CatalogDbImportOrder(db, root);
}

static pthread_mutex_t g_catalog_retire_mu = PTHREAD_MUTEX_INITIALIZER;
static char g_catalog_retired_root[4096];

static void CatalogDbRetireLegacy(const char *root)
{
    pthread_mutex_lock(&g_catalog_retire_mu);
    if (strcmp(g_catalog_retired_root, root) == 0)
    {
        pthread_mutex_unlock(&g_catalog_retire_mu);
        return;
    }
    DIR *d = opendir(root);
    if (!d) { pthread_mutex_unlock(&g_catalog_retire_mu); return; }
    struct dirent *ent;
    while ((ent = readdir(d)))
    {
        char path[4096];
        struct stat st;
        if (!PicoPath_Format(path, sizeof(path), "%s/%s", root, ent->d_name)) continue;
        if (ent->d_name[0] != '.')
        {
            if (lstat(path, &st) != 0 || !S_ISDIR(st.st_mode)) continue;
            char old_meta[4096];
            if (CatalogMetaPath(path, old_meta, sizeof(old_meta))) (void)unlink(old_meta);
        }
        else if (!strcmp(ent->d_name, ".workspace-order.json") ||
                 (strncmp(ent->d_name, ".project-", 9) == 0 &&
                  strlen(ent->d_name) > 14 &&
                  strcmp(ent->d_name + strlen(ent->d_name) - 5, ".json") == 0))
            (void)unlink(path);
    }
    closedir(d);
    snprintf(g_catalog_retired_root, sizeof(g_catalog_retired_root), "%s", root);
    pthread_mutex_unlock(&g_catalog_retire_mu);
}

static bool CatalogDbMarkInstalled(const char *root)
{
    char marker[4096];
    if (!PicoPath_Format(marker, sizeof(marker), "%s/.catalog-installed", root)) return false;
    return access(marker, F_OK) == 0 || CatalogAtomicWrite(marker, "1\n", 2);
}

enum { CATALOG_DB_VERSION = 2 };

static sqlite3 *CatalogDbOpenUnlocked(void)
{
    char root[4096], path[4096], marker[4096], version_sql[64];
    sqlite3 *db = NULL;
    sqlite3_stmt *stmt = NULL;
    int version = 0;
    if (!SessionsRoot(root, sizeof(root)) ||
        !PicoPath_Format(path, sizeof(path), "%s/.catalog.sqlite3", root) ||
        !PicoPath_Format(marker, sizeof(marker), "%s/.catalog-installed", root)) return NULL;
    Pico_MkdirP(root);
    if (access(path, F_OK) != 0 && access(marker, F_OK) == 0)
    {
        fprintf(stderr, "Pico catalog database is missing: %s (preferences not reset)\n", path);
        return NULL;
    }
    if (sqlite3_open_v2(path, &db, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE |
                       SQLITE_OPEN_FULLMUTEX, NULL) != SQLITE_OK) goto fail;
    sqlite3_busy_timeout(db, 1000);
    if (!CatalogDbExec(db, "PRAGMA foreign_keys=ON") ||
        !CatalogDbPrepare(db, &stmt, "PRAGMA user_version")) goto fail;
    if (sqlite3_step(stmt) == SQLITE_ROW) version = sqlite3_column_int(stmt, 0);
    sqlite3_finalize(stmt); stmt = NULL;
    if (version == CATALOG_DB_VERSION)
    {
        if (!CatalogDbMarkInstalled(root))
        { fprintf(stderr, "Could not mark Pico catalog installation in %s\n", root); goto fail; }
        CatalogDbRetireLegacy(root);
        return db;
    }
    if (version != 0) goto incompatible;
    if (access(marker, F_OK) == 0)
    {
        fprintf(stderr, "Pico catalog database lost its schema: %s (preferences not reset)\n", path);
        goto fail;
    }
    if (!CatalogDbExec(db, "PRAGMA journal_mode=WAL") ||
        !CatalogDbExec(db, "BEGIN IMMEDIATE")) goto fail;
    /* Another process may have completed initialization while BEGIN waited. */
    if (!CatalogDbPrepare(db, &stmt, "PRAGMA user_version")) goto fail;
    if (sqlite3_step(stmt) == SQLITE_ROW) version = sqlite3_column_int(stmt, 0);
    sqlite3_finalize(stmt); stmt = NULL;
    if (version == CATALOG_DB_VERSION)
    {
        if (!CatalogDbExec(db, "COMMIT")) goto fail;
        if (!CatalogDbMarkInstalled(root))
        { fprintf(stderr, "Could not mark Pico catalog installation in %s\n", root); goto fail; }
        CatalogDbRetireLegacy(root);
        return db;
    }
    if (version != 0) goto incompatible;
    snprintf(version_sql, sizeof(version_sql), "PRAGMA user_version=%d", CATALOG_DB_VERSION);
    bool ok = CatalogDbExec(db,
        "CREATE TABLE workspaces("
        "path TEXT PRIMARY KEY,key TEXT NOT NULL,project_path TEXT NOT NULL,"
        "checkout_name TEXT NOT NULL,worktree INTEGER NOT NULL,name TEXT NOT NULL,"
        "collapsed INTEGER NOT NULL,ord INTEGER NOT NULL,"
        "last_reconcile INTEGER NOT NULL DEFAULT 0);"
        "CREATE UNIQUE INDEX workspaces_key ON workspaces(key);"
        "CREATE TABLE projects(path TEXT PRIMARY KEY,name TEXT NOT NULL,stashed INTEGER NOT NULL);"
        "CREATE TABLE workspace_order(path TEXT PRIMARY KEY,position INTEGER NOT NULL);"
        "CREATE TABLE sessions("
        "workspace_path TEXT NOT NULL,project_path TEXT NOT NULL,id TEXT NOT NULL,file_path TEXT NOT NULL,"
        "title TEXT NOT NULL,model TEXT NOT NULL,effort TEXT NOT NULL,kind INTEGER NOT NULL,"
        "mtime INTEGER NOT NULL,mtime_nsec INTEGER NOT NULL,ctime INTEGER NOT NULL,"
        "ctime_nsec INTEGER NOT NULL,inode INTEGER NOT NULL,size INTEGER NOT NULL,"
        "unseen INTEGER NOT NULL,seen INTEGER NOT NULL DEFAULT 0,"
        "PRIMARY KEY(workspace_path,id),"
        "FOREIGN KEY(workspace_path) REFERENCES workspaces(path) ON DELETE CASCADE);"
        "CREATE INDEX sessions_recent ON sessions(workspace_path,mtime DESC,mtime_nsec DESC,id DESC);"
        "CREATE INDEX sessions_project_recent ON sessions(project_path,kind,mtime DESC,mtime_nsec DESC,id DESC);") &&
        CatalogDbImport(db, root) && CatalogDbExec(db, version_sql) &&
        CatalogDbExec(db, "COMMIT");
    if (!ok)
    {
        fprintf(stderr, "Pico catalog initialization failed: %s\n", sqlite3_errmsg(db));
        (void)CatalogDbExec(db, "ROLLBACK");
        goto fail;
    }
    if (!CatalogDbMarkInstalled(root))
    { fprintf(stderr, "Could not mark Pico catalog installation in %s\n", root); goto fail; }
    CatalogDbRetireLegacy(root);
    return db;
incompatible:
    fprintf(stderr, "Pico catalog database has incompatible schema version %d (expected %d): %s "
                    "(preferences not reset)\n", version, CATALOG_DB_VERSION, path);
fail:
    sqlite3_finalize(stmt);
    if (db)
    {
        if (sqlite3_errcode(db) != SQLITE_OK)
            fprintf(stderr, "Pico catalog database %s: %s\n", path, sqlite3_errmsg(db));
        sqlite3_close(db);
    }
    return NULL;
}

/* sqlite3's first WAL-mode transition can briefly lock other first opens.
 * Serialize schema/open setup between host and catalog workers in this process. */
static pthread_mutex_t g_catalog_open_mu = PTHREAD_MUTEX_INITIALIZER;

static sqlite3 *CatalogDbOpen(void)
{
    pthread_mutex_lock(&g_catalog_open_mu);
    sqlite3 *db = CatalogDbOpenUnlocked();
    pthread_mutex_unlock(&g_catalog_open_mu);
    return db;
}

static bool CatalogDbWorkspace(sqlite3 *db, const char *path, const char *key,
                               const char *project, const char *checkout, bool worktree)
{
    sqlite3_stmt *stmt = NULL;
    bool ok = CatalogDbPrepare(db, &stmt,
        "INSERT INTO workspaces(path,key,project_path,checkout_name,worktree,name,collapsed,ord)"
        " VALUES(?,?,?,?,?,?,0,(SELECT count(*) FROM workspaces))"
        " ON CONFLICT(path) DO UPDATE SET project_path=excluded.project_path,"
        "checkout_name=excluded.checkout_name,worktree=excluded.worktree"
        " WHERE workspaces.project_path<>excluded.project_path OR"
        " workspaces.checkout_name<>excluded.checkout_name OR workspaces.worktree<>excluded.worktree");
    if (ok)
    {
        sqlite3_bind_text(stmt, 1, path, -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 2, key, -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 3, project, -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 4, checkout, -1, SQLITE_TRANSIENT);
        sqlite3_bind_int(stmt, 5, worktree);
        sqlite3_bind_text(stmt, 6, "", -1, SQLITE_STATIC);
        ok = sqlite3_step(stmt) == SQLITE_DONE;
    }
    sqlite3_finalize(stmt);
    stmt = NULL;
    if (ok && sqlite3_changes(db) > 0 && CatalogDbPrepare(db, &stmt,
        "UPDATE sessions SET project_path=? WHERE workspace_path=?"))
    {
        sqlite3_bind_text(stmt, 1, project, -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 2, path, -1, SQLITE_TRANSIENT);
        ok = sqlite3_step(stmt) == SQLITE_DONE;
    }
    sqlite3_finalize(stmt);
    return ok;
}

static bool CatalogDbSessionRead(sqlite3 *db, const char *workspace, const char *id,
                                 PicoCatalogSession *row, const struct stat *st)
{
    sqlite3_stmt *stmt = NULL;
    bool valid = false;
    if (!CatalogDbPrepare(db, &stmt,
        "SELECT title,model,effort,kind,mtime,mtime_nsec,ctime,ctime_nsec,inode,size,unseen"
        " FROM sessions WHERE workspace_path=? AND id=?")) return false;
    sqlite3_bind_text(stmt, 1, workspace, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, id, -1, SQLITE_TRANSIENT);
    if (sqlite3_step(stmt) == SQLITE_ROW)
    {
        memset(row, 0, sizeof(*row));
        snprintf(row->id, sizeof(row->id), "%s", id);
        snprintf(row->title, sizeof(row->title), "%s", CatalogDbText(stmt, 0));
        snprintf(row->model, sizeof(row->model), "%s", CatalogDbText(stmt, 1));
        snprintf(row->effort, sizeof(row->effort), "%s", CatalogDbText(stmt, 2));
        row->kind = (PicoAgentKind)sqlite3_column_int(stmt, 3);
        row->mtime = (time_t)sqlite3_column_int64(stmt, 4);
        row->mtime_nsec = (long)sqlite3_column_int64(stmt, 5);
        row->ctime = (time_t)sqlite3_column_int64(stmt, 6);
        row->ctime_nsec = (long)sqlite3_column_int64(stmt, 7);
        row->inode = (uint64_t)sqlite3_column_int64(stmt, 8);
        row->size = (uint64_t)sqlite3_column_int64(stmt, 9);
        row->unseen_complete = sqlite3_column_int(stmt, 10) != 0;
        valid = !st || CatalogGenerationMatches(row, st);
    }
    sqlite3_finalize(stmt);
    return valid;
}

static bool CatalogDbSessionPut(sqlite3 *db, const char *workspace, const char *path,
                                const PicoCatalogSession *row, sqlite3_int64 seen)
{
    sqlite3_stmt *stmt = NULL;
    bool ok = CatalogDbPrepare(db, &stmt,
        "INSERT INTO sessions(workspace_path,project_path,id,file_path,title,model,effort,kind,"
        "mtime,mtime_nsec,ctime,ctime_nsec,inode,size,unseen,seen)"
        " VALUES(?,(SELECT project_path FROM workspaces WHERE path=?),?,?,?,?,?,?,?,?,?,?,?,?,?,?)"
        " ON CONFLICT(workspace_path,id) DO UPDATE SET project_path=excluded.project_path,"
        "file_path=excluded.file_path,"
        "title=excluded.title,model=excluded.model,effort=excluded.effort,"
        "kind=excluded.kind,mtime=excluded.mtime,mtime_nsec=excluded.mtime_nsec,"
        "ctime=excluded.ctime,ctime_nsec=excluded.ctime_nsec,inode=excluded.inode,"
        "size=excluded.size,unseen=excluded.unseen,seen=excluded.seen");
    if (ok)
    {
        sqlite3_bind_text(stmt, 1, workspace, -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 2, workspace, -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 3, row->id, -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 4, path, -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 5, row->title, -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 6, row->model, -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 7, row->effort, -1, SQLITE_TRANSIENT);
        sqlite3_bind_int(stmt, 8, row->kind);
        sqlite3_bind_int64(stmt, 9, (sqlite3_int64)row->mtime);
        sqlite3_bind_int64(stmt, 10, (sqlite3_int64)row->mtime_nsec);
        sqlite3_bind_int64(stmt, 11, (sqlite3_int64)row->ctime);
        sqlite3_bind_int64(stmt, 12, (sqlite3_int64)row->ctime_nsec);
        sqlite3_bind_int64(stmt, 13, (sqlite3_int64)row->inode);
        sqlite3_bind_int64(stmt, 14, (sqlite3_int64)row->size);
        sqlite3_bind_int(stmt, 15, row->unseen_complete);
        sqlite3_bind_int64(stmt, 16, seen);
        ok = sqlite3_step(stmt) == SQLITE_DONE;
    }
    sqlite3_finalize(stmt);
    return ok;
}

static bool CatalogDbReconcileDir(sqlite3 *db, const char *dir, const char *workspace,
                                  const atomic_bool *cancelled, bool force)
{
    sqlite3_stmt *status = NULL;
    sqlite3_stmt *mark = NULL;
    if (!force)
    {
        sqlite3_stmt *last_reconcile = NULL;
        bool recent = false;
        if (CatalogDbPrepare(db, &last_reconcile,
                "SELECT last_reconcile FROM workspaces WHERE path=?"))
        {
            sqlite3_bind_text(last_reconcile, 1, workspace, -1, SQLITE_TRANSIENT);
            time_t now = time(NULL);
            bool found = sqlite3_step(last_reconcile) == SQLITE_ROW;
            time_t last = found ? (time_t)sqlite3_column_int64(last_reconcile, 0) : 0;
            recent = found && last > 0 && last <= now && now - last < 60;
        }
        sqlite3_finalize(last_reconcile);
        if (recent) return true;
    }
    DIR *d = opendir(dir);
    struct dirent *ent;
    bool ok = d != NULL;
    if (!ok) return false;
    /* Unique per scan: a failed/cancelled scan never sweeps unseen rows. */
    bool writing = false;
    int batch = 0;
    sqlite3_int64 epoch = 0;
    sqlite3_randomness((int)sizeof(epoch), &epoch);
    if (!epoch) epoch = 1;
    while (ok && (ent = readdir(d)))
    {
        char path[4096], id[40];
        struct stat st, after;
        PicoCatalogSession row;
        if (CatalogCancelled(cancelled)) { ok = false; break; }
        if (!IsSessionJsonl(ent->d_name)) continue;
        if (!PicoPath_Format(path, sizeof(path), "%s/%s", dir, ent->d_name) ||
            stat(path, &st) != 0)
        {
            ok = false;
            break;
        }
        if (!S_ISREG(st.st_mode)) continue;
        IdFromName(ent->d_name, id, sizeof(id));
        if (!id[0])
        {
            PicoSessionInfo header = {0};
            ScanSessionFile(path, &header, true);
            snprintf(id, sizeof(id), "%s", header.id);
        }
        if (!id[0]) { ok = false; break; }
        bool cached = CatalogDbSessionRead(db, workspace, id, &row, &st);
        if (!cached)
        {
            /* A cache miss may parse a long transcript. Never hold a SQLite
             * write transaction across that file read. */
            if (writing)
            {
                ok = CatalogDbExec(db, "COMMIT");
                writing = false; batch = 0;
                if (!ok) break;
            }
            memset(&row, 0, sizeof(row));
            CatalogRowFromFile(path, &row);
            if (!row.id[0]) snprintf(row.id, sizeof(row.id), "%s", id);
            CopyStatToCatalog(&row, &st);
            /* A file modified during parsing cannot publish a stale row. */
            if (stat(path, &after) != 0 || after.st_ino != st.st_ino ||
                after.st_size != st.st_size || after.st_mtime != st.st_mtime ||
                StatMtimeNsec(&after) != StatMtimeNsec(&st) ||
                StatCtimeNsec(&after) != StatCtimeNsec(&st))
            {
                ok = false;
                break;
            }
        }
        if (!writing)
        {
            ok = CatalogDbExec(db, "BEGIN IMMEDIATE");
            if (!ok) break;
            writing = true;
        }
        if (cached)
        {
            if (!mark)
                ok = CatalogDbPrepare(db, &mark,
                    "UPDATE sessions SET seen=? WHERE workspace_path=? AND id=?");
            if (ok)
            {
                sqlite3_bind_int64(mark, 1, epoch);
                sqlite3_bind_text(mark, 2, workspace, -1, SQLITE_TRANSIENT);
                sqlite3_bind_text(mark, 3, id, -1, SQLITE_TRANSIENT);
                ok = sqlite3_step(mark) == SQLITE_DONE;
                sqlite3_reset(mark);
                sqlite3_clear_bindings(mark);
            }
        }
        else ok = CatalogDbSessionPut(db, workspace, path, &row, epoch);
        if (ok && ++batch == 128)
        {
            ok = CatalogDbExec(db, "COMMIT");
            writing = false; batch = 0;
        }
    }
    closedir(d);
    sqlite3_finalize(mark);
    if (writing)
    {
        if (ok && !CatalogCancelled(cancelled)) ok = CatalogDbExec(db, "COMMIT");
        else (void)CatalogDbExec(db, "ROLLBACK");
    }
    if (ok && !CatalogCancelled(cancelled))
        ok = CatalogDbExec(db, "BEGIN IMMEDIATE");
    if (ok && !CatalogCancelled(cancelled))
    {
        sqlite3_stmt *sweep = NULL;
        ok = CatalogDbPrepare(db, &sweep, "DELETE FROM sessions WHERE workspace_path=? AND seen<>?");
        if (ok)
        {
            sqlite3_bind_text(sweep, 1, workspace, -1, SQLITE_TRANSIENT);
            sqlite3_bind_int64(sweep, 2, epoch);
            ok = sqlite3_step(sweep) == SQLITE_DONE;
        }
        sqlite3_finalize(sweep);
    }
    if (ok && !CatalogCancelled(cancelled))
    {
        ok = CatalogDbPrepare(db, &status,
            "UPDATE workspaces SET last_reconcile=? WHERE path=?");
        if (ok)
        {
            sqlite3_bind_int64(status, 1, (sqlite3_int64)time(NULL));
            sqlite3_bind_text(status, 2, workspace, -1, SQLITE_TRANSIENT);
            ok = sqlite3_step(status) == SQLITE_DONE;
        }
    }
    sqlite3_finalize(status);
    if (ok && !CatalogCancelled(cancelled)) ok = CatalogDbExec(db, "COMMIT");
    else (void)CatalogDbExec(db, "ROLLBACK");
    return ok && !CatalogCancelled(cancelled);
}

static void CatalogDbFillInfo(sqlite3_stmt *stmt, PicoSessionInfo *info)
{
    memset(info, 0, sizeof(*info));
    snprintf(info->path, sizeof(info->path), "%s", CatalogDbText(stmt, 0));
    snprintf(info->id, sizeof(info->id), "%s", CatalogDbText(stmt, 1));
    snprintf(info->title, sizeof(info->title), "%s", CatalogDbText(stmt, 2));
    snprintf(info->model, sizeof(info->model), "%s", CatalogDbText(stmt, 3));
    snprintf(info->effort, sizeof(info->effort), "%s", CatalogDbText(stmt, 4));
    info->kind = (PicoAgentKind)sqlite3_column_int(stmt, 5);
    info->mtime = (time_t)sqlite3_column_int64(stmt, 6);
    info->mtime_nsec = (long)sqlite3_column_int64(stmt, 7);
    info->ctime = (time_t)sqlite3_column_int64(stmt, 8);
    info->ctime_nsec = (long)sqlite3_column_int64(stmt, 9);
    info->inode = (uint64_t)sqlite3_column_int64(stmt, 10);
    info->size = (uint64_t)sqlite3_column_int64(stmt, 11);
    info->unseen_complete = sqlite3_column_int(stmt, 12) != 0;
}

static int CatalogDbList(sqlite3 *db, const char *workspace, const char *search,
                         PicoSessionInfo **out, bool parents_only, int maximum)
{
    sqlite3_stmt *stmt = NULL;
    PicoSessionInfo *list = NULL;
    int count = 0;
    int cap = 0;
    *out = NULL;
    bool ok = CatalogDbPrepare(db, &stmt,
        "SELECT file_path,id,title,model,effort,kind,mtime,mtime_nsec,ctime,ctime_nsec,inode,size,unseen"
        " FROM sessions WHERE workspace_path=? AND (?=0 OR kind=0)"
        " AND (?='' OR instr(lower(title),lower(?))>0 OR instr(lower(id),lower(?))>0)"
        " ORDER BY mtime DESC,mtime_nsec DESC,file_path DESC LIMIT ?");
    if (!ok) return 0;
    sqlite3_bind_text(stmt, 1, workspace, -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(stmt, 2, parents_only);
    sqlite3_bind_text(stmt, 3, search ? search : "", -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 4, search ? search : "", -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 5, search ? search : "", -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(stmt, 6, maximum > 0 ? maximum : INT_MAX);
    int rc;
    while ((rc = sqlite3_step(stmt)) == SQLITE_ROW)
    {
        if (count >= cap)
        {
            int next_cap = cap ? cap * 2 : 16;
            if (maximum > 0 && next_cap > maximum) next_cap = maximum;
            PicoSessionInfo *next = realloc(list, (size_t)next_cap * sizeof(*next));
            if (!next) { ok = false; break; }
            list = next; cap = next_cap;
        }
        CatalogDbFillInfo(stmt, &list[count++]);
    }
    if (rc != SQLITE_DONE) ok = false;
    sqlite3_finalize(stmt);
    if (!ok) { free(list); return -1; }
    *out = list;
    return count;
}

static void CatalogDbFillWorkspace(sqlite3_stmt *stmt, PicoCatalogWorkspace *ws)
{
    memset(ws, 0, sizeof(*ws));
    snprintf(ws->path, sizeof(ws->path), "%s", CatalogDbText(stmt, 0));
    snprintf(ws->key, sizeof(ws->key), "%s", CatalogDbText(stmt, 1));
    snprintf(ws->project_path, sizeof(ws->project_path), "%s", CatalogDbText(stmt, 2));
    snprintf(ws->checkout_name, sizeof(ws->checkout_name), "%s", CatalogDbText(stmt, 3));
    ws->worktree = sqlite3_column_int(stmt, 4) != 0;
    snprintf(ws->name, sizeof(ws->name), "%s", CatalogDbText(stmt, 5));
    ws->collapsed = sqlite3_column_int(stmt, 6) != 0;
    ws->order = sqlite3_column_int(stmt, 7);
}

static int CatalogDbReadWorkspace(sqlite3 *db, const char *key, PicoCatalogWorkspace *ws)
{
    sqlite3_stmt *stmt = NULL;
    int found = 0;
    if (!CatalogDbPrepare(db, &stmt,
        "SELECT path,key,project_path,checkout_name,worktree,name,collapsed,ord"
        " FROM workspaces WHERE key=?")) return -1;
    sqlite3_bind_text(stmt, 1, key, -1, SQLITE_TRANSIENT);
    int rc = sqlite3_step(stmt);
    if (rc == SQLITE_ROW)
    {
        CatalogDbFillWorkspace(stmt, ws);
        found = 1;
    }
    else if (rc != SQLITE_DONE) found = -1;
    sqlite3_finalize(stmt);
    return found;
}

static bool CatalogDbSeed(sqlite3 *db, const char *path)
{
    char key[4096], checkout[256];
    PicoCatalogWorkspace existing = {0};
    PicoWorktreeInfo info;
    if (!path || !path[0] || !CatalogKeyFromPath(path, key, sizeof(key))) return false;
    int existing_state = CatalogDbReadWorkspace(db, key, &existing);
    if (existing_state < 0) return false;
    if (existing_state > 0) return strcmp(existing.path, path) == 0;
    bool linked = PicoWorktree_Discover(path, &info) && info.linked;
    PathBasename(path, checkout, sizeof(checkout));
    return CatalogDbWorkspace(db, path, key, linked ? info.project_path : path, checkout, linked);
}

static bool CatalogDbReadProject(sqlite3 *db, const char *path, PicoCatalogWorkspace *ws)
{
    sqlite3_stmt *stmt = NULL;
    if (!CatalogDbPrepare(db, &stmt, "SELECT name,stashed FROM projects WHERE path=?")) return false;
    sqlite3_bind_text(stmt, 1, path, -1, SQLITE_TRANSIENT);
    int rc = sqlite3_step(stmt);
    if (rc == SQLITE_ROW)
    {
        if (CatalogDbText(stmt, 0)[0])
            snprintf(ws->name, sizeof(ws->name), "%s", CatalogDbText(stmt, 0));
        ws->stashed = sqlite3_column_int(stmt, 1) != 0;
    }
    sqlite3_finalize(stmt);
    return rc == SQLITE_ROW || rc == SQLITE_DONE;
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
    char canonical[4096], dir[4096], key[4096], checkout[256];
    PicoWorktreeInfo worktree;
    sqlite3 *db;
    bool ok;
    if (!CanonicalWorkspacePath(workspace_path, canonical, sizeof(canonical)) ||
        !CatalogDirForPath(canonical, dir, sizeof(dir)) ||
        !CatalogKeyFromPath(canonical, key, sizeof(key))) return -1;
    Pico_MkdirP(dir);
    db = CatalogDbOpen();
    if (!db) return -1;
    bool linked = PicoWorktree_Discover(canonical, &worktree) && worktree.linked;
    const char *project = linked ? worktree.project_path : canonical;
    PathBasename(canonical, checkout, sizeof(checkout));
    int changes_before = sqlite3_total_changes(db);
    ok = CatalogDbWorkspace(db, canonical, key, project, checkout, linked);
    bool changed = sqlite3_total_changes(db) != changes_before;
    sqlite3_close(db);
    if (ok && changed) CatalogMarkChanged();
    return ok ? 0 : -1;
}

int PicoCatalog_SetCollapsed(const char *workspace_path, bool collapsed)
{
    sqlite3 *db;
    sqlite3_stmt *stmt = NULL;
    bool ok;
    if (!workspace_path || !(db = CatalogDbOpen())) return -1;
    sqlite3_busy_timeout(db, 30);
    ok = CatalogDbPrepare(db, &stmt, "UPDATE workspaces SET collapsed=? WHERE path=?");
    if (ok)
    {
        sqlite3_bind_int(stmt, 1, collapsed);
        sqlite3_bind_text(stmt, 2, workspace_path, -1, SQLITE_TRANSIENT);
        ok = sqlite3_step(stmt) == SQLITE_DONE;
    }
    sqlite3_finalize(stmt);
    bool changed = sqlite3_changes(db) != 0;
    sqlite3_close(db);
    if (ok && changed) CatalogMarkChanged();
    return ok && changed ? 0 : -1;
}

int PicoCatalog_SetSessionModel(const char *workspace_path, const char *session_id,
                                const char *model, const char *effort)
{
    char dir[4096];
    sqlite3 *db;
    sqlite3_stmt *stmt = NULL;
    int lock;
    bool ok = false;
    if (!session_id || !session_id[0] || PicoCatalog_Ensure(workspace_path) != 0 ||
        !CatalogDirForPath(workspace_path, dir, sizeof(dir)) ||
        (lock = CatalogLockAcquire(dir)) < 0) return -1;
    db = CatalogDbOpen();
    if (db)
    {
        ok = CatalogDbPrepare(db, &stmt,
            "UPDATE sessions SET model=?,effort=? WHERE workspace_path=? AND id=?");
        if (ok)
        {
            sqlite3_bind_text(stmt, 1, model ? model : "", -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(stmt, 2, effort ? effort : "", -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(stmt, 3, workspace_path, -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(stmt, 4, session_id, -1, SQLITE_TRANSIENT);
            ok = sqlite3_step(stmt) == SQLITE_DONE;
        }
        sqlite3_finalize(stmt);
        sqlite3_close(db);
    }
    CatalogLockRelease(lock);
    if (ok) CatalogMarkChanged();
    return ok ? 0 : -1;
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
    struct stat current_stat;
    PicoCatalogSession row = {0};
    sqlite3 *db = NULL;
    bool ready = false;
    int lock;
    if (kind != PICO_AGENT_MAIN || persistence != PICO_SESSION_DURABLE ||
        !session_id || !session_id[0] || !session_path || !ws_path ||
        stat(session_path, &current_stat) != 0 || !S_ISREG(current_stat.st_mode) ||
        PicoCatalog_Ensure(ws_path) != 0 || !CatalogDirForPath(ws_path, dir, sizeof(dir)) ||
        (lock = CatalogLockAcquire(dir)) < 0) return;
    db = CatalogDbOpen();
    if (!db) goto done;
    if (previous_stat && CatalogDbSessionRead(db, ws_path, session_id, &row, previous_stat))
    {
        ready = title_override && title_override[0];
        if (ready) snprintf(row.title, sizeof(row.title), "%s", title_override);
        else ready = CatalogApplyEventLines(&row, event_json);
    }
    if (!ready) CatalogRowFromFile(session_path, &row);
    if (!row.id[0]) snprintf(row.id, sizeof(row.id), "%s", session_id);
    row.kind = kind;
    CopyStatToCatalog(&row, &current_stat);
    (void)CatalogDbSessionPut(db, ws_path, session_path, &row, 1);
done:
    if (db) sqlite3_close(db);
    CatalogLockRelease(lock);
}

static void CatalogWriteThrough(PicoHost *app, const PicoAgent *agent,
                                const char *title_override, const char *event_json,
                                const struct stat *previous_stat)
{
    if (!app || !agent) return;
    CatalogWriteThroughFields(agent->kind, agent->persistence, agent->session_id, agent->session_path,
                              PicoWorkspace_Path(SessionWorkspace(app, agent)), title_override, event_json,
                              previous_stat);
}

static bool CatalogScanDir(sqlite3 *db, const char *dir, const char *key, PicoCatalogWorkspace *out,
                           const atomic_bool *cancelled, int session_limit, bool *failed)
{
    PicoCatalogWorkspace ws = {0};
    PicoSessionInfo *rows = NULL;
    int lock_fd, count;
    bool ok = false;
    if (failed) *failed = false;
    if (!dir || !key || !out || CatalogCancelled(cancelled)) return false;
    lock_fd = CatalogLockAcquire(dir);
    if (lock_fd < 0) { if (failed) *failed = true; return false; }
    int workspace_state = CatalogDbReadWorkspace(db, key, &ws);
    if (workspace_state < 0) { if (failed) *failed = true; goto done_db; }
    if (!workspace_state)
    {
        /* Metadata can be reconstructed after a missing index from a session
         * header, but an empty checkout must first be registered by Ensure. */
        DIR *d = opendir(dir);
        struct dirent *ent;
        bool recovered = false;
        if (d)
        {
            while (!recovered && (ent = readdir(d)))
            {
                char file[4096];
                PicoSessionInfo header = {0};
                if (!IsSessionJsonl(ent->d_name) ||
                    !PicoPath_Format(file, sizeof(file), "%s/%s", dir, ent->d_name)) continue;
                ScanSessionFile(file, &header, true);
                if (!header.cwd[0]) continue;
                char checkout[256] = {0};
                const char *project = header.project_path[0] ? header.project_path : header.cwd;
                PathBasename(header.cwd, checkout, sizeof(checkout));
                recovered = CatalogDbWorkspace(db, header.cwd, key, project, checkout, header.worktree);
            }
            closedir(d);
        }
        if (!recovered) goto done_db;
        workspace_state = CatalogDbReadWorkspace(db, key, &ws);
        if (workspace_state < 0) { if (failed) *failed = true; goto done_db; }
        if (!workspace_state) goto done_db;
    }
    if (!ws.path[0]) goto done_db;
    char canonical[4096];
    if (!CanonicalWorkspacePath(ws.path, canonical, sizeof(canonical)))
    {
        if (!ws.worktree) goto done_db;
        ws.missing = true;
    }
    else if (strcmp(canonical, ws.path) != 0) goto done_db;
    if (!ws.name[0]) PathBasename(ws.path, ws.name, sizeof(ws.name));
    if (!ws.checkout_name[0]) PathBasename(ws.path, ws.checkout_name, sizeof(ws.checkout_name));
    if (!CatalogDbReconcileDir(db, dir, ws.path, cancelled, cancelled == NULL))
    { if (failed && !CatalogCancelled(cancelled)) *failed = true; goto done_db; }
    count = session_limit > 0 ? CatalogDbList(db, ws.path, NULL, &rows, true, session_limit) : 0;
    if (count < 0) { if (failed) *failed = true; goto done_db; }
    for (int i = 0; i < count; i++)
    {
        PicoCatalogSession row = {0};
        snprintf(row.id, sizeof(row.id), "%s", rows[i].id);
        snprintf(row.title, sizeof(row.title), "%s", rows[i].title);
        snprintf(row.model, sizeof(row.model), "%s", rows[i].model);
        snprintf(row.effort, sizeof(row.effort), "%s", rows[i].effort);
        row.kind = rows[i].kind;
        row.mtime = rows[i].mtime; row.mtime_nsec = rows[i].mtime_nsec;
        row.ctime = rows[i].ctime; row.ctime_nsec = rows[i].ctime_nsec;
        row.inode = rows[i].inode; row.size = rows[i].size;
        row.unseen_complete = rows[i].unseen_complete;
        if (!CatalogAppendSession(&ws, &row))
        { if (failed) *failed = true; goto done_db; }
    }
    *out = ws;
    ws.sessions = NULL;
    ws.session_count = 0;
    ok = true;
done_db:
    free(rows);
    CatalogClearSessions(&ws);
    CatalogLockRelease(lock_fd);
    return ok;
}

/* limit <= 0 enumerates every catalog; project deletion must not truncate. */
static int CatalogScanN(sqlite3 *db, PicoCatalogWorkspace **out, int limit, const atomic_bool *cancelled, int session_limit)
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
    if (!out || CatalogCancelled(cancelled) || !SessionsRoot(root, sizeof(root)))
    {
#ifdef PICO_SESSION_TEST_HOOKS
        if (!CatalogCancelled(cancelled))
        {
            (void)PicoSession_TestHook("catalog_scan_done");
        }
#endif
        return 0;
    }
    d = opendir(root);
    if (!d)
    {
#ifdef PICO_SESSION_TEST_HOOKS
        (void)PicoSession_TestHook("catalog_scan_done");
#endif
        return 0;
    }
    while ((ent = readdir(d)) && (limit <= 0 || n < limit))
    {
        char dir[4096];
        struct stat st;
        PicoCatalogWorkspace ws;
        PicoCatalogWorkspace *next;
        bool failed = false;
        if (CatalogCancelled(cancelled))
        {
            closedir(d);
            PicoCatalog_Free(list, n);
            if (out)
            {
                *out = NULL;
            }
            return 0;
        }
        if (!ent->d_name[0] || ent->d_name[0] == '.' ||
            !PicoPath_Format(dir, sizeof(dir), "%s/%s", root, ent->d_name) ||
            stat(dir, &st) != 0 || !S_ISDIR(st.st_mode) ||
            !CatalogScanDir(db, dir, ent->d_name, &ws, cancelled, session_limit, &failed))
        {
            if (failed)
            {
                closedir(d);
                PicoCatalog_Free(list, n);
                *out = NULL;
                return -1;
            }
            continue;
        }
        next = (PicoCatalogWorkspace *)realloc(list, (size_t)(n + 1) * sizeof(*next));
        if (!next)
        {
            CatalogClearSessions(&ws);
            closedir(d);
            PicoCatalog_Free(list, n);
            return -1;
        }
        list = next;
        list[n++] = ws;
    }
    closedir(d);
    if (CatalogCancelled(cancelled))
    {
        PicoCatalog_Free(list, n);
        if (out)
        {
            *out = NULL;
        }
        return 0;
    }
    if (n > 1)
    {
        qsort(list, (size_t)n, sizeof(*list), CmpCatalogOrder);
        if (!CatalogApplyOrder(db, list, n))
        {
            PicoCatalog_Free(list, n);
            return -1;
        }
    }
#ifdef PICO_SESSION_TEST_HOOKS
    (void)PicoSession_TestHook("catalog_scan_done");
#endif
    *out = list;
    return n;
}

int PicoCatalog_Scan(PicoCatalogWorkspace **out)
{
    if (out) *out = NULL;
    if (!out) return 0;
    sqlite3 *db = CatalogDbOpen();
    if (!db) return -1;
    int count = CatalogScanN(db, out, PICO_MAX_CATALOG_WORKSPACES, NULL, PICO_MAX_CATALOG_SESSIONS);
    sqlite3_close(db);
    return count;
}

/* Pico-owned catalog root files: meta, sessions, and atomic-write residue
 * ("<name>.tmp.XXXXXX") left behind by an interrupted write. */
static bool CatalogOwnedRootName(const char *name, size_t len)
{
    const char *tmp;
    size_t base;
    if (!strcmp(name, ".workspace.json")) return true;
    if (len >= 6 && !strcmp(name + len - 6, ".jsonl")) return true;
    tmp = strstr(name, ".tmp.");
    if (!tmp || tmp == name) return false;
    base = (size_t)(tmp - name);
    return (base >= 6 && !memcmp(tmp - 6, ".jsonl", 6)) ||
           (base == strlen(".workspace.json") && !memcmp(name, ".workspace.json", base));
}

/* Keep lock inodes stable; never traverse symlinks, including a swapped parent. */
static bool CatalogOwnedData(int dirfd, bool root, bool remove_files)
{
    DIR *d = fdopendir(dup(dirfd));
    struct dirent *ent;
    bool ok = d != NULL;
    if (!d) return false;
    while ((ent = readdir(d)))
    {
        struct stat st;
        const char *name = ent->d_name;
        size_t len = strlen(name);
        if (!strcmp(name, ".") || !strcmp(name, "..")) continue;
        if (root && len >= 5 && !strcmp(name + len - 5, ".lock")) continue;
        if (root && !CatalogOwnedRootName(name, len))
        {
            ok = false; /* unknown files are not Pico session data */
            continue;
        }
        if (fstatat(dirfd, name, &st, AT_SYMLINK_NOFOLLOW) != 0) { ok = false; continue; }
        if (S_ISDIR(st.st_mode) && !root)
        {
            int child = openat(dirfd, name, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
            if (child < 0) { ok = false; continue; }
            if (!CatalogOwnedData(child, false, remove_files)) ok = false;
            close(child);
            if (remove_files && unlinkat(dirfd, name, AT_REMOVEDIR) != 0) ok = false;
        }
        else if (S_ISDIR(st.st_mode)) ok = false;
        else if (remove_files && unlinkat(dirfd, name, 0) != 0) ok = false;
    }
    closedir(d);
    return ok;
}

static bool CatalogRemoveDataTree(const char *dir, bool root)
{
    int fd = open(dir, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    bool ok;
    if (fd < 0) return false;
    /* Reject unexpected entries before modifying a catalog. */
    ok = !root || CatalogOwnedData(fd, true, false);
    if (ok && lseek(fd, 0, SEEK_SET) < 0) ok = false;
    if (ok) ok = CatalogOwnedData(fd, root, true);
    close(fd);
    if (ok && !root) ok = rmdir(dir) == 0;
    return ok;
}

static bool CatalogValidateDataTree(const char *dir)
{
    int fd = open(dir, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (fd < 0) return false;
    bool ok = CatalogOwnedData(fd, true, false);
    close(fd);
    return ok;
}

static bool CatalogRemoveSessionMedia(const char *checkout, const char *id)
{
    int pico = -1, media = -1, session = -1;
    bool ok = false;
    if (!id || !id[0]) return false;
    for (const unsigned char *p = (const unsigned char *)id; *p; p++)
        if (!isalnum(*p) && *p != '-' && *p != '_') return false;
    pico = open(checkout, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (pico < 0) return errno == ENOENT; /* a removed worktree has no media */
    int pico_dir = openat(pico, ".pico", O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (pico_dir < 0) { ok = errno == ENOENT; goto done; }
    media = openat(pico_dir, "media", O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    int media_error = errno;
    close(pico_dir);
    if (media < 0)
    {
        ok = media_error == ENOENT;
        goto done;
    }
    session = openat(media, id, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (session < 0)
    {
        ok = errno == ENOENT;
        goto done;
    }
    ok = CatalogOwnedData(session, false, true) && unlinkat(media, id, AT_REMOVEDIR) == 0;
done:
    if (session >= 0) close(session);
    if (media >= 0) close(media);
    close(pico);
    return ok;
}

static int CatalogDeleteProjectLocked(PicoHost *host, const char *project)
{
    PicoCatalogWorkspace *leaves = NULL;
    char project_meta[4096];
    char canonical[4096];
    int count, matched = 0;
    bool ok = true;
    if (!host || !project || project[0] != '/' ||
        (CanonicalWorkspacePath(project, canonical, sizeof(canonical)) && strcmp(project, canonical) != 0) ||
        !CatalogProjectMetaPath(project, project_meta, sizeof(project_meta))) return -1;
    for (int w = 0; w < host->workspace_count; w++)
    {
        const PicoWorkspace *workspace = host->workspaces[w];
        if (!workspace) continue;
        const char *identity = workspace->project_path[0] ? workspace->project_path : workspace->path;
        if (workspace->count > 0 && !strcmp(identity, project)) return -1;
    }
    /* Agent close can precede completion of its queued persist job. Never
     * remove a catalog while this host still has a writer in flight. */
    if (host->persist_ready)
    {
        bool busy;
        pthread_mutex_lock(&host->persist_mu);
        busy = host->persist_flight_agent_id != 0;
        for (int i = 0; i < host->persist_pending_count; i++)
            if (host->persist_pending[i].job_kind == PICO_PERSIST_JOB_SESSION) busy = true;
        pthread_mutex_unlock(&host->persist_mu);
        if (busy) return -1;
    }
    sqlite3 *scan_db = CatalogDbOpen();
    if (!scan_db) return -1;
    count = CatalogScanN(scan_db, &leaves, 0, NULL, 0); /* deletion must see every checkout in the group */
    sqlite3_close(scan_db);
    if (count < 0) return -1;
    int *locks = malloc((size_t)(count > 0 ? count : 1) * sizeof(*locks));
    if (!locks) { PicoCatalog_Free(leaves, count); return -1; }
    for (int i = 0; i < count; i++) locks[i] = -1;
    /* All catalog access uses this mutex and the stable per-checkout lock
     * inode. No scan/write can interleave with validation and removal. */
    pthread_mutex_lock(&g_catalog_mutex);
    /* Fail before touching any checkout if a catalog contains unknown data. */
    for (int i = 0; i < count; i++)
    {
        const PicoCatalogWorkspace *ws = &leaves[i];
        char dir[4096];
        const char *group = ws->project_path[0] ? ws->project_path : ws->path;
        if (strcmp(group, project)) continue;
        char root[4096];
        char meta[4096], error[256] = {0};
        if (!SessionsRoot(root, sizeof(root)) ||
            !PicoPath_Format(dir, sizeof(dir), "%s/%s", root, ws->key) ||
            !CatalogMetaPath(dir, meta, sizeof(meta)) ||
            (locks[i] = SessionLockAcquire(meta, error, sizeof(error))) < 0 ||
            !CatalogValidateDataTree(dir))
        {
            ok = false;
            goto unlock;
        }
    }
    sqlite3 *db = CatalogDbOpen();
    if (!db) { ok = false; goto unlock; }
    for (int i = 0; i < count; i++)
    {
        const PicoCatalogWorkspace *ws = &leaves[i];
        const char *group = ws->project_path[0] ? ws->project_path : ws->path;
        if (strcmp(group, project)) continue;
        matched++;
        if (!CatalogRemoveSessionMedia(ws->path, "composer")) ok = false;
        sqlite3_stmt *items = NULL;
        if (!CatalogDbPrepare(db, &items,
            "SELECT id FROM sessions WHERE workspace_path=?")) ok = false;
        if (items)
        {
            sqlite3_bind_text(items, 1, ws->path, -1, SQLITE_TRANSIENT);
            int rc;
            while ((rc = sqlite3_step(items)) == SQLITE_ROW)
                if (!CatalogRemoveSessionMedia(ws->path, CatalogDbText(items, 0))) ok = false;
            if (rc != SQLITE_DONE) ok = false;
            sqlite3_finalize(items);
        }
    }
    if (!ok || !matched) goto close_db;
    /* Once media cleanup succeeds everywhere, remove each checkout's history. */
    for (int i = 0; i < count; i++)
    {
        const PicoCatalogWorkspace *ws = &leaves[i];
        const char *group = ws->project_path[0] ? ws->project_path : ws->path;
        if (strcmp(group, project)) continue;
        char dir[4096], root[4096];
        if (!SessionsRoot(root, sizeof(root)) ||
            !PicoPath_Format(dir, sizeof(dir), "%s/%s", root, ws->key) ||
            !CatalogRemoveDataTree(dir, true)) ok = false;
        else
        {
            sqlite3_stmt *order = NULL;
            if (!CatalogDbPrepare(db, &order, "DELETE FROM workspace_order WHERE path=?")) ok = false;
            else
            {
                sqlite3_bind_text(order, 1, ws->path, -1, SQLITE_TRANSIENT);
                if (sqlite3_step(order) != SQLITE_DONE) ok = false;
            }
            sqlite3_finalize(order);
            sqlite3_stmt *delete = NULL;
            if (!CatalogDbPrepare(db, &delete, "DELETE FROM workspaces WHERE path=?")) ok = false;
            else
            {
                sqlite3_bind_text(delete, 1, ws->path, -1, SQLITE_TRANSIENT);
                if (sqlite3_step(delete) != SQLITE_DONE) ok = false;
            }
            sqlite3_finalize(delete);
        }
        if (!ok) break;
    }
    if (ok && matched)
    {
        sqlite3_stmt *order = NULL;
        if (CatalogDbPrepare(db, &order,
            "DELETE FROM workspace_order WHERE path=?"))
        {
            sqlite3_bind_text(order, 1, project, -1, SQLITE_TRANSIENT);
            if (sqlite3_step(order) != SQLITE_DONE) ok = false;
        }
        else ok = false;
        sqlite3_finalize(order);
        sqlite3_stmt *delete = NULL;
        if (!CatalogDbPrepare(db, &delete, "DELETE FROM projects WHERE path=?")) ok = false;
        else
        {
            sqlite3_bind_text(delete, 1, project, -1, SQLITE_TRANSIENT);
            if (sqlite3_step(delete) != SQLITE_DONE) ok = false;
        }
        sqlite3_finalize(delete);
    }
close_db:
    sqlite3_close(db);
    if (matched) CatalogMarkChanged(); /* also publish partially deleted catalogs */
unlock:
    for (int i = 0; i < count; i++) SessionLockRelease(locks[i]);
    pthread_mutex_unlock(&g_catalog_mutex);
    free(locks);
    PicoCatalog_Free(leaves, count);
    if (!matched || !ok) return -1;
    if (ok)
    {
        char error[256];
        int lock = CatalogFileLockAcquire(project_meta, error, sizeof(error));
        if (lock < 0) ok = false;
        else
        {
            if (unlink(project_meta) != 0 && errno != ENOENT) ok = false;
            if (ok) CatalogMarkChanged();
            CatalogLockRelease(lock);
        }
    }
    return ok ? 0 : -1;
}

int PicoCatalog_DeleteProject(PicoHost *host, const char *project)
{
    char error[256] = {0};
    if (!host || !project) return -1;
#ifdef PICO_SESSION_TEST_HOOKS
    (void)PicoSession_TestHook("catalog_delete_before_lock");
#endif
    int lock = CatalogDeletionGuardAcquire(error, sizeof(error));
    if (lock < 0) return -1;
    int result = CatalogDeleteProjectLocked(host, project);
    CatalogDeletionGuardRelease(lock);
    return result;
}

static bool CatalogDbGroupRows(sqlite3 *db, PicoCatalogWorkspace *group, int requested,
                               const atomic_bool *cancelled)
{
    sqlite3_stmt *stmt = NULL;
    if (!CatalogDbPrepare(db, &stmt,
        "SELECT s.id,s.title,s.model,s.effort,s.kind,s.mtime,s.mtime_nsec,s.ctime,s.ctime_nsec,"
        "s.inode,s.size,s.unseen,w.path,w.checkout_name,w.worktree"
        " FROM sessions s JOIN workspaces w ON w.path=s.workspace_path"
        " WHERE s.project_path=? AND s.kind=0"
        " ORDER BY s.mtime DESC,s.mtime_nsec DESC,s.id DESC")) return false;
    sqlite3_bind_text(stmt, 1, group->path, -1, SQLITE_TRANSIENT);
    int rc;
    while ((rc = sqlite3_step(stmt)) == SQLITE_ROW && !CatalogCancelled(cancelled))
    {
        PicoCatalogSession row = {0};
        snprintf(row.id, sizeof(row.id), "%s", CatalogDbText(stmt, 0));
        snprintf(row.title, sizeof(row.title), "%s", CatalogDbText(stmt, 1));
        snprintf(row.model, sizeof(row.model), "%s", CatalogDbText(stmt, 2));
        snprintf(row.effort, sizeof(row.effort), "%s", CatalogDbText(stmt, 3));
        row.kind = (PicoAgentKind)sqlite3_column_int(stmt, 4);
        row.mtime = (time_t)sqlite3_column_int64(stmt, 5);
        row.mtime_nsec = (long)sqlite3_column_int64(stmt, 6);
        row.ctime = (time_t)sqlite3_column_int64(stmt, 7);
        row.ctime_nsec = (long)sqlite3_column_int64(stmt, 8);
        row.inode = (uint64_t)sqlite3_column_int64(stmt, 9);
        row.size = (uint64_t)sqlite3_column_int64(stmt, 10);
        row.unseen_complete = sqlite3_column_int(stmt, 11) != 0;
        snprintf(row.checkout_path, sizeof(row.checkout_path), "%s", CatalogDbText(stmt, 12));
        snprintf(row.checkout_name, sizeof(row.checkout_name), "%s", CatalogDbText(stmt, 13));
        row.worktree = sqlite3_column_int(stmt, 14) != 0;
        struct stat st;
        row.missing_checkout = stat(row.checkout_path, &st) != 0 || !S_ISDIR(st.st_mode);
        if (row.missing_checkout && !row.worktree) continue;
        char canonical[4096];
        if (!row.missing_checkout &&
            (!CanonicalWorkspacePath(row.checkout_path, canonical, sizeof(canonical)) ||
             strcmp(canonical, row.checkout_path))) continue;
        /* Count the look-ahead row only after filtering unavailable normal
         * checkouts, so they cannot consume a page or its has-more indicator. */
        if (group->session_count >= requested) { group->has_more_sessions = true; break; }
        if (!CatalogAppendSession(group, &row)) break;
    }
    bool ok = group->has_more_sessions || rc == SQLITE_DONE;
    sqlite3_finalize(stmt);
    return ok;
}

static int CatalogGroupSnapshot(sqlite3 *db, PicoCatalogWorkspace *leaves, int count,
                                 PicoCatalogWorkspace **out, const atomic_bool *cancelled,
                                 const PicoCatalogPage *pages, int page_count)
{
    PicoCatalogWorkspace *groups = NULL;
    int group_count = 0;
    if (out) *out = NULL;
    if (!out) { PicoCatalog_Free(leaves, count); return 0; }
    if (CatalogCancelled(cancelled) || count < 0) { PicoCatalog_Free(leaves, count); return count < 0 ? -1 : 0; }
    for (int i = 0; i < count; i++)
    {
        PicoCatalogWorkspace *leaf = &leaves[i];
        const char *project = leaf->project_path[0] ? leaf->project_path : leaf->path;
        int gi = -1;
        for (int j = 0; j < group_count; j++)
            if (strcmp(groups[j].path, project) == 0) { gi = j; break; }
        if (gi < 0)
        {
            PicoCatalogWorkspace *next = realloc(groups, (size_t)(group_count + 1) * sizeof(*next));
            if (!next)
            {
                PicoCatalog_Free(leaves, count);
                PicoCatalog_Free(groups, group_count);
                return -1;
            }
            groups = next;
            gi = group_count++;
            memset(&groups[gi], 0, sizeof(groups[gi]));
            snprintf(groups[gi].path, sizeof(groups[gi].path), "%s", project);
            snprintf(groups[gi].project_path, sizeof(groups[gi].project_path), "%s", project);
            snprintf(groups[gi].key, sizeof(groups[gi].key), "%s", project);
            PathBasename(project, groups[gi].name, sizeof(groups[gi].name));
            groups[gi].order = leaf->order;
            groups[gi].collapsed = leaf->collapsed;
            struct stat st;
            groups[gi].missing = stat(project, &st) != 0 || !S_ISDIR(st.st_mode);
        }
        if (leaf->order < groups[gi].order) groups[gi].order = leaf->order;
        if (strcmp(leaf->path, project) == 0) groups[gi].collapsed = leaf->collapsed;
    }
    PicoCatalog_Free(leaves, count);
    if (CatalogCancelled(cancelled)) { PicoCatalog_Free(groups, group_count); return 0; }
    bool failed = false;
    for (int i = 0; i < group_count; i++)
    {
        if (!CatalogDbReadProject(db, groups[i].path, &groups[i]))
        { failed = true; break; }
        int requested = 10;
        for (int j = 0; j < page_count; j++)
            if (!strcmp(pages[j].path, groups[i].path) && pages[j].shown > requested)
                requested = pages[j].shown;
        if (requested >= INT_MAX) requested = INT_MAX - 1;
        if (!CatalogDbGroupRows(db, &groups[i], requested, cancelled))
        { failed = true; break; }
    }
    if (failed) { PicoCatalog_Free(groups, group_count); return -1; }
    if (group_count > 1) qsort(groups, (size_t)group_count, sizeof(*groups), CmpCatalogOrder);
    if (CatalogCancelled(cancelled)) { PicoCatalog_Free(groups, group_count); return 0; }
    *out = groups;
    return group_count;
}

/* Snapshot reads never enumerate or reconcile transcripts. A read transaction
 * keeps workspace preferences, project grouping and pages from one generation. */
int PicoCatalog_ReadGroupedPaged(PicoCatalogWorkspace **out, const atomic_bool *cancelled,
                                 const PicoCatalogPage *pages, int page_count)
{
#ifdef PICO_SESSION_TEST_HOOKS
    (void)PicoSession_TestHook("catalog_snapshot");
#endif
    PicoCatalogWorkspace *leaves = NULL;
    sqlite3_stmt *stmt = NULL;
    int count = 0, rc = SQLITE_DONE, result = -1;
    if (out) *out = NULL;
    if (!out || CatalogCancelled(cancelled)) return 0;
    sqlite3 *db = CatalogDbOpen();
    if (!db) return -1;
    bool ok = CatalogDbExec(db, "BEGIN") && CatalogDbPrepare(db, &stmt,
        "SELECT path,key,project_path,checkout_name,worktree,name,collapsed,ord"
        " FROM workspaces ORDER BY ord,path");
    while (ok && count < PICO_MAX_CATALOG_WORKSPACES &&
           (rc = sqlite3_step(stmt)) == SQLITE_ROW && !CatalogCancelled(cancelled))
    {
        PicoCatalogWorkspace ws = {0};
        char canonical[4096];
        CatalogDbFillWorkspace(stmt, &ws);
        if (!CanonicalWorkspacePath(ws.path, canonical, sizeof(canonical)))
        {
            if (!ws.worktree) continue;
            ws.missing = true;
        }
        else if (strcmp(canonical, ws.path) != 0) continue;
        if (!ws.name[0]) PathBasename(ws.path, ws.name, sizeof(ws.name));
        if (!ws.checkout_name[0]) PathBasename(ws.path, ws.checkout_name, sizeof(ws.checkout_name));
        PicoCatalogWorkspace *next = realloc(leaves, (size_t)(count + 1) * sizeof(*next));
        if (!next) { ok = false; break; }
        leaves = next;
        leaves[count++] = ws;
    }
    if (rc != SQLITE_ROW && rc != SQLITE_DONE) ok = false;
    sqlite3_finalize(stmt);
    if (ok) ok = CatalogApplyOrder(db, leaves, count);
    if (ok)
    {
        result = CatalogGroupSnapshot(db, leaves, count, out, cancelled, pages, page_count);
        leaves = NULL;
    }
    PicoCatalog_Free(leaves, count);
    if (!CatalogDbExec(db, "COMMIT"))
    {
        PicoCatalog_Free(*out, result);
        *out = NULL;
        result = -1;
    }
    sqlite3_close(db);
#ifdef PICO_SESSION_TEST_HOOKS
    (void)PicoSession_TestHook("catalog_snapshot_done");
#endif
    return result;
}

int PicoCatalog_ScanGroupedPaged(PicoCatalogWorkspace **out, const atomic_bool *cancelled,
                                 const PicoCatalogPage *pages, int page_count)
{
    PicoCatalogWorkspace *leaves = NULL;
    if (out) *out = NULL;
    if (!out || CatalogCancelled(cancelled)) return 0;
    sqlite3 *db = CatalogDbOpen();
    if (!db) return -1;
    int count = CatalogScanN(db, &leaves, PICO_MAX_CATALOG_WORKSPACES, cancelled, 0);
    int result = CatalogGroupSnapshot(db, leaves, count, out, cancelled, pages, page_count);
    sqlite3_close(db);
    return result;
}

int PicoCatalog_ScanGroupedInterruptible(PicoCatalogWorkspace **out, const atomic_bool *cancelled)
{
    return PicoCatalog_ScanGroupedPaged(out, cancelled, NULL, 0);
}

int PicoCatalog_ScanGrouped(PicoCatalogWorkspace **out)
{
    return PicoCatalog_ScanGroupedInterruptible(out, NULL);
}


static void PersistJobClear(PicoSessionPersistJob *job)
{
    PicoSessionPersistJob *chain;
    if (!job)
    {
        return;
    }
    chain = job->next;
    job->next = NULL;
    free(job->header_json);
    free(job->event_json);
    free(job->title);
    free(job->catalog_order_json);
    memset(job, 0, sizeof(*job));
    while (chain)
    {
        PicoSessionPersistJob *next = chain->next;
        chain->next = NULL;
        PersistJobClear(chain);
        free(chain);
        chain = next;
    }
}

static PicoSessionPersistJob *PersistJobTail(PicoSessionPersistJob *job)
{
    if (!job)
    {
        return NULL;
    }
    while (job->next)
    {
        job = job->next;
    }
    return job;
}

static PicoSessionPersistJob *PersistJobExtend(PicoSessionPersistJob *last, PicoSessionPersistJob *src)
{
    PicoSessionPersistJob *node;
    if (!last || !src)
    {
        return NULL;
    }
    node = (PicoSessionPersistJob *)calloc(1, sizeof(*node));
    if (!node)
    {
        return NULL;
    }
    node->job_kind = PICO_PERSIST_JOB_SESSION;
    node->agent_id = src->agent_id;
    node->kind = src->kind;
    node->persistence = src->persistence;
    snprintf(node->session_id, sizeof(node->session_id), "%s", src->session_id);
    snprintf(node->session_path, sizeof(node->session_path), "%s", src->session_path);
    snprintf(node->workspace_path, sizeof(node->workspace_path), "%s", src->workspace_path);
    node->header_json = src->header_json;
    node->event_json = src->event_json;
    node->event_len = src->event_len;
    node->event_capacity = src->event_capacity;
    node->title = src->title;
    src->header_json = NULL;
    src->event_json = NULL;
    src->event_len = src->event_capacity = 0;
    src->title = NULL;
    last->next = node;
    return node;
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
    for (i = host->persist_pending_count - 1; i >= 0; i--)
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
    host->persist_pending_bytes -= out->queued_bytes;
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
        host->persist_pending_bytes -= host->persist_pending[i].queued_bytes;
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
        else
        {
            PicoSessionPersistJob *cur = &job;
            while (cur && !failed)
            {
                if (cur->header_json && cur->header_json[0] &&
                    (!EnsureSessionParent(cur->session_path, error, sizeof(error)) ||
                     !WriteLineAtPath(cur->session_path, cur->header_json, false, cur->kind,
                                      cur->persistence, cur->session_id, cur->workspace_path, error,
                                      sizeof(error))))
                {
                    failed = true;
                }
                else if (cur->event_json && cur->event_json[0] &&
                         !WriteLineAtPath(cur->session_path, cur->event_json, true, cur->kind,
                                          cur->persistence, cur->session_id, cur->workspace_path, error,
                                          sizeof(error)))
                {
                    failed = true;
                }
                else if (cur->title && cur->title[0] &&
                         !RewriteSessionTitleAtPath(cur->session_path, cur->title, cur->kind,
                                                    cur->persistence, cur->session_id,
                                                    cur->workspace_path, error, sizeof(error)))
                {
                    failed = true;
                }
                cur = cur->next;
            }
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
        pico_host_wakeup(NULL);
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
    host->persist_pending_bytes = 0;
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
 * session file. A pending title rewrite is a barrier: later appends start a
 * new job so they cannot land inside that rewrite. Write failures surface
 * asynchronously through the persist failure pump, which moves the agent to
 * PICO_SESSION_FAILED. */
/* Byte budgets cover queued session payloads, including title continuations.
 * In-flight work is owned by the writer and no longer consumes queue space. */
#define PICO_PERSIST_SESSION_BYTES (16u * 1024u * 1024u)
#define PICO_PERSIST_TOTAL_BYTES (64u * 1024u * 1024u)

static bool PersistCanQueueLocked(const PicoHost *app, const PicoSessionPersistJob *pending,
                                  size_t bytes)
{
    size_t session = pending ? pending->queued_bytes : 0;
    return bytes <= PICO_PERSIST_SESSION_BYTES - session &&
           bytes <= PICO_PERSIST_TOTAL_BYTES - app->persist_pending_bytes;
}

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
    job.event_len = strlen(json);
    job.event_capacity = job.event_len + 1;
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

    job.queued_bytes = job.event_len + (job.header_json ? strlen(job.header_json) : 0);
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
    if (pending && job.header_json && PersistJobTail(pending)->header_json)
    {
        job.queued_bytes -= strlen(job.header_json);
        free(job.header_json);
        job.header_json = NULL;
    }
    if (!PersistCanQueueLocked(app, pending, job.queued_bytes +
                               (pending && !PersistJobTail(pending)->title &&
                                PersistJobTail(pending)->event_len ? 1 : 0)))
    {
        pthread_mutex_unlock(&app->persist_mu);
        PersistJobClear(&job);
        PersistenceFailed(app, agent, "session persist pending-byte budget exceeded");
        return PICO_SESSION_WRITE_FAILED;
    }
    if (pending)
    {
        PicoSessionPersistJob *last = PersistJobTail(pending);
        if (last && !last->title)
        {
            size_t have = last->event_len;
            size_t add = job.event_len;
            size_t need = have + (have ? 1 : 0) + add + 1;
            if (need > last->event_capacity)
            {
                size_t capacity = last->event_capacity ? last->event_capacity : 64;
                while (capacity < need)
                    capacity = capacity > PICO_PERSIST_SESSION_BYTES / 2 ? need : capacity * 2;
                char *merged = (char *)realloc(last->event_json, capacity);
                if (!merged)
                {
                    pthread_mutex_unlock(&app->persist_mu);
                    PersistJobClear(&job);
                    PersistenceFailed(app, agent, "out of memory while queueing the session write");
                    return PICO_SESSION_WRITE_FAILED;
                }
                last->event_json = merged;
                last->event_capacity = capacity;
            }
            if (have) last->event_json[have] = '\n';
            memcpy(last->event_json + have + (have ? 1 : 0), job.event_json, add + 1);
            last->event_len = need - 1;
            if (!last->header_json && job.header_json)
            {
                last->header_json = job.header_json;
                job.header_json = NULL;
            }
            pending->queued_bytes += job.queued_bytes + (have ? 1 : 0);
            app->persist_pending_bytes += job.queued_bytes + (have ? 1 : 0);
            PersistJobClear(&job);
        }
        else if (!PersistJobExtend(last, &job))
        {
            pthread_mutex_unlock(&app->persist_mu);
            PersistJobClear(&job);
            PersistenceFailed(app, agent, "out of memory while queueing the session write");
            return PICO_SESSION_WRITE_FAILED;
        }
        else
        {
            pending->queued_bytes += job.queued_bytes;
            app->persist_pending_bytes += job.queued_bytes;
            PersistJobClear(&job);
        }
    }
    else if (app->persist_pending &&
             app->persist_pending_count < PICO_PERSIST_QUEUE_CAPACITY)
    {
        app->persist_pending[app->persist_pending_count++] = job;
        app->persist_pending_bytes += job.queued_bytes;
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

/* Queue a title rewrite behind earlier records for this session. Distinct
 * titles are not coalesced: each accepted change is its own rewrite/event.
 * A title can ride on the current pending append job when that job does not
 * already have a rewrite, which keeps FIFO order without cutting in front of
 * already-queued events. Later appends start a new job so they cannot land
 * inside the rewrite. */
static PicoSessionWriteResult QueueSessionTitle(PicoHost *app, PicoAgent *agent, const char *title)
{
    PicoSessionPersistJob *pending;
    PicoSessionPersistJob job;
    bool new_identity;
    if (!app || !agent || !title || !title[0])
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
    job.title = JsonDup(title);
    if (!job.title)
    {
        PersistenceFailed(app, agent, "out of memory while queueing the session title");
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

    job.queued_bytes = strlen(job.title) + (job.header_json ? strlen(job.header_json) : 0);
    pthread_mutex_lock(&app->persist_mu);
    if (PersistHasFailureLocked(app, agent->id, agent->session_id))
    {
        pthread_mutex_unlock(&app->persist_mu);
        PersistJobClear(&job);
        return PICO_SESSION_WRITE_FAILED;
    }
    pending = PersistPendingForAgentLocked(app, agent->id, agent->session_id);
    if (pending && job.header_json && PersistJobTail(pending)->header_json)
    {
        job.queued_bytes -= strlen(job.header_json);
        free(job.header_json);
        job.header_json = NULL;
    }
    if (!PersistCanQueueLocked(app, pending, job.queued_bytes))
    {
        pthread_mutex_unlock(&app->persist_mu);
        PersistJobClear(&job);
        PersistenceFailed(app, agent, "session persist pending-byte budget exceeded");
        return PICO_SESSION_WRITE_FAILED;
    }
    if (pending)
    {
        PicoSessionPersistJob *last = PersistJobTail(pending);
        if (last && !last->title)
        {
            last->title = job.title;
            job.title = NULL;
            if (!last->header_json && job.header_json)
            {
                last->header_json = job.header_json;
                job.header_json = NULL;
            }
            pending->queued_bytes += job.queued_bytes;
            app->persist_pending_bytes += job.queued_bytes;
            PersistJobClear(&job);
        }
        else if (!PersistJobExtend(last, &job))
        {
            pthread_mutex_unlock(&app->persist_mu);
            PersistJobClear(&job);
            PersistenceFailed(app, agent, "out of memory while queueing the session title");
            return PICO_SESSION_WRITE_FAILED;
        }
        else
        {
            pending->queued_bytes += job.queued_bytes;
            app->persist_pending_bytes += job.queued_bytes;
            PersistJobClear(&job);
        }
    }
    else if (app->persist_pending &&
             app->persist_pending_count < PICO_PERSIST_QUEUE_CAPACITY)
    {
        app->persist_pending[app->persist_pending_count++] = job;
        app->persist_pending_bytes += job.queued_bytes;
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
    json = BuildModelChangeJson(agent->model, agent->effort[0] ? agent->effort : "none", agent->fast);
    if (!json)
    {
        PersistenceFailed(app, agent, "out of memory while logging the model change");
        return;
    }
    (void)QueueSessionLine(app, agent, json);
    free(json);
}

/* UI-initiated loads only. Public create/resume and extension reload retain their
 * synchronous contract. The worker owns only copied paths and parsed JSON. */
typedef struct PicoSessionLoadWorker {
    atomic_bool cancelled;
    uint64_t serial;
    char workspace_path[4096];
    char requested[4096];
    bool latest;
    bool explicit_path;
    bool explicit_in_workspace;
    bool allow_prefix;
    bool no_session;
    char path[4096];
    PicoSessionReplay *replay;
} PicoSessionLoadWorker;

typedef struct PicoSessionLoad {
    uint64_t serial;
    PicoWorkspaceId workspace_id;
    PicoAgentId replace_id;
    PicoAgentId selected_at_start;
    bool startup;
    bool cancelled;
    bool processing;
    PicoSessionLoadWorker *worker;
    PicoAgent *candidate;
    PicoSessionReplay *replay;
    char path[4096];
    char target_id[40];
    int finish_message;
} PicoSessionLoad;

static void *SessionLoadRead(void *arg)
{
    PicoSessionLoadWorker *worker = arg;
#ifdef PICO_SESSION_TEST_HOOKS
    (void)PicoSession_TestHook("async_replay_before_read");
#endif
    if (atomic_load(&worker->cancelled)) return NULL;
    if (worker->explicit_path)
    {
        if (!realpath(worker->requested, worker->path)) return NULL;
        PicoWorkspace *lookup = calloc(1, sizeof(*lookup));
        if (lookup)
        {
            char dir[4096], canonical[4096];
            snprintf(lookup->path, sizeof(lookup->path), "%s", worker->workspace_path);
            if (SessionDir(lookup, dir, sizeof(dir)) && realpath(dir, canonical))
            {
                size_t len = strlen(canonical);
                worker->explicit_in_workspace = strncmp(worker->path, canonical, len) == 0 &&
                                                worker->path[len] == '/';
            }
            free(lookup);
        }
    }
    else
    {
        /* Only the copied path is used; no live workspace state crosses the
         * thread boundary. Listing and latest-session scans remain worker I/O. */
        PicoWorkspace *lookup = calloc(1, sizeof(*lookup));
        if (!lookup) return NULL;
        snprintf(lookup->path, sizeof(lookup->path), "%s", worker->workspace_path);
        if (worker->latest)
        {
            char dir[4096], latest[4096];
            if (!SessionDir(lookup, dir, sizeof(dir)) ||
                FindLatest(dir, latest, sizeof(latest)) != 0)
                worker->no_session = true;
            else if (!realpath(latest, worker->path)) worker->path[0] = '\0';
        }
        else if (PicoSession_Resolve(lookup, worker->requested,
                                     worker->allow_prefix, worker->path,
                                     sizeof(worker->path)) != 0)
            worker->path[0] = '\0';
        free(lookup);
    }
    if (worker->path[0] && !atomic_load(&worker->cancelled))
        worker->replay = ReplayPrepareBefore(worker->path, PICO_AGENT_MAIN,
                                              &worker->cancelled, true);
    return NULL;
}

static void SessionLoadWorkerCancel(void *arg)
{
    atomic_store(&((PicoSessionLoadWorker *)arg)->cancelled, true);
}

static void SessionLoadWorkerDestroy(void *arg)
{
    PicoSessionLoadWorker *worker = arg;
    PicoSession_ReplayFree(worker->replay);
    free(worker);
}

static void SessionLoadDiscard(PicoHost *host, PicoSessionLoad *load)
{
    if (!load) return;
    if (load->candidate)
    {
        PicoWorkspace *ws = load->candidate->workspace;
        PicoWorkspace_ReleaseSessions(ws, load->candidate->id);
        if (load->replay && load->replay->cursor > 0)
            PicoWorkspace_RunHooks(ws, PICO_HOOK_ON_AGENT_DESTROY, load->candidate->id);
        (void)PicoAgent_Destroy(load->candidate);
    }
    PicoSession_ReplayFree(load->replay);
    if (host && host->session_load == load) host->session_load = NULL;
    free(load);
}

bool PicoSession_LoadTargetsWorkspace(const PicoHost *host, PicoWorkspaceId id)
{
    return host && host->session_load && host->session_load->workspace_id == id;
}

bool PicoSession_LoadReplacesAgent(const PicoHost *host, PicoAgentId id)
{
    return host && id && host->session_load && host->session_load->replace_id == id;
}

void PicoSession_LoadCancel(PicoHost *host)
{
    if (!host || !host->session_load) return;
    PicoSessionLoad *load = host->session_load;
    host->session_load = NULL;
    load->cancelled = true;
    if (load->worker) atomic_store(&load->worker->cancelled, true);
    /* Workers have no pointer to this load; a stale completion is discarded. */
    if (!load->processing) SessionLoadDiscard(host, load);
}

void PicoSession_LoadCancelWorkspace(PicoHost *host, PicoWorkspaceId id)
{
    if (host && host->session_load && host->session_load->workspace_id == id)
        PicoSession_LoadCancel(host);
}

static void SessionLoadCompleted(PicoHost *host, void *arg)
{
    PicoSessionLoadWorker *worker = arg;
    PicoSessionLoad *load = host->session_load;
    if (!load || load->serial != worker->serial || load->cancelled) return;
    load->worker = NULL;
    if (worker->no_session && load->startup)
    {
        PicoSession_LoadCancel(host);
        return;
    }
    if (!worker->replay)
    {
        PicoOverlay_Notify(host, "Could not open that session.");
        PicoSession_LoadCancel(host);
        return;
    }
    load->replay = worker->replay;
    worker->replay = NULL;
#ifdef PICO_SESSION_TEST_HOOKS
    (void)PicoSession_TestHook("async_replay_after_adopt");
#endif
    snprintf(load->path, sizeof(load->path), "%s", worker->path);
    const char *filename = strrchr(worker->path, '/');
    if (!worker->explicit_path || worker->explicit_in_workspace)
        IdFromName(filename ? filename + 1 : worker->path, load->target_id, sizeof(load->target_id));
}

PicoResult PicoSession_LoadAsync(PicoHost *host, PicoWorkspaceId workspace_id,
                                 PicoAgentId replace_id, const char *requested,
                                 bool allow_prefix, bool latest, bool explicit_path,
                                 bool startup)
{
    PicoWorkspace *ws = PicoHost_FindWorkspace(host, workspace_id);
    if (!host || !ws || (!latest && (!requested || !requested[0])) ||
        ws->state != PICO_WORKSPACE_OPEN) return PICO_INVALID;
    if (replace_id)
    {
        PicoAgent *old = PicoWorkspace_FindAgent(ws, replace_id);
        if (!old) return PICO_NOT_FOUND;
        if (PicoAgent_IsBusy(old)) return PICO_BUSY;
    }
    /* Cancellation of the previous candidate also dispatches destroy hooks.
     * Keep the new target alive until the installed load takes over protection. */
    int hold = PicoHost_EvictHoldPush(host, ws, replace_id);
    if (!replace_id && (ws->count >= PICO_MAX_AGENTS ||
                        PicoHost_TotalAgentCount(host) >= PICO_MAX_TOTAL_AGENTS))
    {
        if (!PicoHost_EvictIdleAgent(host, ws))
        {
            PicoHost_EvictHoldPop(host, hold);
            return PICO_LIMIT;
        }
    }
    PicoSession_LoadCancel(host);
    PicoSessionLoad *load = calloc(1, sizeof(*load));
    PicoSessionLoadWorker *worker = calloc(1, sizeof(*worker));
    if (!load || !worker)
    {
        free(load); free(worker);
        PicoHost_EvictHoldPop(host, hold);
        return PICO_NO_MEMORY;
    }
    worker->serial = load->serial = ++host->next_session_load_serial;
    snprintf(worker->workspace_path, sizeof(worker->workspace_path), "%s", ws->path);
    snprintf(worker->requested, sizeof(worker->requested), "%s", requested ? requested : "");
    worker->allow_prefix = allow_prefix;
    worker->latest = latest;
    worker->explicit_path = explicit_path;
    load->workspace_id = workspace_id;
    if (!allow_prefix && !latest && !explicit_path)
        snprintf(load->target_id, sizeof(load->target_id), "%s", requested);
    load->replace_id = replace_id;
    load->selected_at_start = host->selected_agent_id;
    load->startup = startup;
    if (!PicoHost_StartTaskCompleted(host, SessionLoadRead, worker,
                                      SessionLoadWorkerCancel, SessionLoadCompleted,
                                      SessionLoadWorkerDestroy))
    {
        free(worker); free(load);
        PicoHost_EvictHoldPop(host, hold);
        return PICO_NO_MEMORY;
    }
    load->worker = worker;
    host->session_load = load;
    PicoHost_EvictHoldPop(host, hold);
    return PICO_OK;
}

bool PicoSession_LoadPending(const PicoHost *host)
{
    return host && host->session_load != NULL;
}

bool PicoSession_LoadTarget(const PicoHost *host, const char **workspace_path,
                            const char **session_id)
{
    const PicoSessionLoad *load = host ? host->session_load : NULL;
    PicoWorkspace *ws = load ? PicoHost_FindWorkspace((PicoHost *)host, load->workspace_id) : NULL;
    if (!ws || !load->target_id[0]) return false;
    if (workspace_path) *workspace_path = PicoWorkspace_Path(ws);
    if (session_id) *session_id = load->target_id;
    return true;
}

bool PicoSession_LoadBlocksSubmit(const PicoHost *host, PicoAgentId id)
{
    return host && host->session_load && host->session_load->startup &&
           host->session_load->replace_id == id;
}

/* A per-frame time budget complements the row budget: small records should
 * not aggregate into a long frame. A single callback/record is uninterruptible. */
void PicoSession_LoadPump(PicoHost *host)
{
    PicoSessionLoad *load = host ? host->session_load : NULL;
    if (!load || !load->replay) return;
    PicoWorkspace *ws = PicoHost_FindWorkspace(host, load->workspace_id);
    PicoAgent *old = load->replace_id && ws ? PicoWorkspace_FindAgent(ws, load->replace_id) : NULL;
    if (!ws || ws->state != PICO_WORKSPACE_OPEN ||
        (load->replace_id && (!old || PicoAgent_IsBusy(old))))
    {
        PicoSession_LoadCancel(host);
        return;
    }
    if (old && old->session_path[0] && strcmp(old->session_path, load->path) == 0)
    {
        PicoSession_LoadCancel(host);
        return;
    }
    load->processing = true;
    pico_host_request_redraw(host);
    if (!load->candidate)
    {
        if (!load->replace_id &&
            (ws->count >= PICO_MAX_AGENTS || PicoHost_TotalAgentCount(host) >= PICO_MAX_TOTAL_AGENTS))
            goto failed;
        load->candidate = PicoAgent_Create(host, ws);
        if (!load->candidate) goto failed;
        load->candidate->persistence = PICO_SESSION_DURABLE;
        if (!PicoWorkspace_ReserveSession(ws, load->candidate->id, load->path))
        {
            PicoOverlay_Notify(host, PicoWorkspace_SessionReserved(ws, load->path,
                                  load->candidate->id) ? "Session is already open by another agent."
                                                        : "Could not reserve that session.");
            goto cancelled;
        }
        snprintf(load->candidate->session_path, sizeof(load->candidate->session_path),
                 "%s", load->path);
        PicoAgent_ClearInput(load->candidate);
    }
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    double deadline = (double)now.tv_sec + now.tv_nsec / 1e9 + 0.004;
    for (int budget = 0; budget < 16 && !load->cancelled; budget++)
    {
        host->session_replay_agent = load->candidate;
        bool finished = PicoSession_ReplayBatch(host, load->candidate, load->replay, 1);
        host->session_replay_agent = NULL;
        if (!finished)
        {
            clock_gettime(CLOCK_MONOTONIC, &now);
            if ((double)now.tv_sec + now.tv_nsec / 1e9 >= deadline) break;
        }
        else break;
    }
    if (load->cancelled) goto cancelled;
    if (load->replay->cursor < load->replay->count) goto done;
    /* Historical tool rows must not retain their live completion dwell. */
    for (int budget = 0; budget < 32 &&
                         load->finish_message < load->candidate->message_count; budget++)
    {
        PicoMessage *msg = &load->candidate->messages[load->finish_message++];
        for (int t = 0; t < msg->trace_count; t++) msg->trace[t].tool_done_t0 = 0.0;
        clock_gettime(CLOCK_MONOTONIC, &now);
        if ((double)now.tv_sec + now.tv_nsec / 1e9 >= deadline) break;
    }
    if (load->finish_message < load->candidate->message_count) goto done;
    PicoSession_ReplayFinish(host, load->candidate, load->replay, false);
    if (load->cancelled) goto cancelled;
    PicoAgent *published = load->candidate;
    PicoAgentId published_id = published->id;
    if (!PicoWorkspace_CommitLoadedSession(host, load->workspace_id, load->replace_id,
                                           published,
                                           load->selected_at_start == host->selected_agent_id))
        goto failed;
    load->candidate = NULL;
    /* Session hooks may close the just-published agent or supersede this load. */
    PicoAgent *live = PicoHost_FindAgent(host, published_id);
    if (live) ReplayAppendInterrupted(host, live, load->replay);
    goto cancelled;
failed:
    PicoOverlay_Notify(host, "Could not open that session.");
cancelled:
    load->processing = false;
    SessionLoadDiscard(host, load);
    return;
done:
    load->processing = false;
    if (load->cancelled) SessionLoadDiscard(host, load);
}
