#define _DEFAULT_SOURCE
#define _POSIX_C_SOURCE 200809L

#include "session.h"
#include "agent.h"
#include "agent_manager.h"
#include "json.h"
#include "path.h"
#include "settings.h"
#include "usage.h"
#include "host_internal.h"

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

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

static void ScanSessionFile(const char *path, PicoSessionInfo *info)
{
    if (!info)
    {
        return;
    }
    info->title[0] = '\0';
    info->kind = PICO_AGENT_MAIN;
    FILE *f = fopen(path, "rb");
    if (!f)
    {
        snprintf(info->title, sizeof(info->title), "Untitled");
        return;
    }
    char *buf = NULL;
    size_t buf_cap = 0;
    bool got_title = false;
    while (!got_title && getline(&buf, &buf_cap, f) != -1)
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
        }
        else if (type && strcmp(type, "message") == 0)
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

static int CmpMtimeDesc(const void *a, const void *b)
{
    const PicoSessionInfo *x = (const PicoSessionInfo *)a;
    const PicoSessionInfo *y = (const PicoSessionInfo *)b;
    if (x->mtime > y->mtime)
    {
        return -1;
    }
    if (x->mtime < y->mtime)
    {
        return 1;
    }
    return strcmp(y->path, x->path);
}

int PicoSession_List(const PicoWorkspace *workspace, PicoSessionInfo **out, bool parents_only)
{
    if (out)
    {
        *out = NULL;
    }
    if (!workspace || !out)
    {
        return 0;
    }
    char dir[4096];
    if (!SessionDir(workspace, dir, sizeof(dir)))
    {
        return 0;
    }
    DIR *d = opendir(dir);
    if (!d)
    {
        return 0;
    }
    PicoSessionInfo *list = NULL;
    int n = 0;
    int cap = 0;
    struct dirent *ent;
    while ((ent = readdir(d)))
    {
        if (!IsSessionJsonl(ent->d_name))
        {
            continue;
        }
        if (n >= cap)
        {
            int next_cap = cap == 0 ? 8 : cap * 2;
            PicoSessionInfo *next = (PicoSessionInfo *)realloc(list, (size_t)next_cap * sizeof(*list));
            if (!next)
            {
                break;
            }
            list = next;
            cap = next_cap;
        }
        PicoSessionInfo *s = &list[n];
        memset(s, 0, sizeof(*s));
        if (!PicoPath_Format(s->path, sizeof(s->path), "%s/%s", dir, ent->d_name))
        {
            continue;
        }
        struct stat st;
        if (stat(s->path, &st) != 0 || !S_ISREG(st.st_mode))
        {
            continue;
        }
        s->mtime = st.st_mtime;
        IdFromName(ent->d_name, s->id, sizeof(s->id));
        ScanSessionFile(s->path, s);
        if (!s->id[0] || (parents_only && s->kind == PICO_AGENT_SUBAGENT))
        {
            continue;
        }
        n++;
    }
    closedir(d);
    if (n > 1)
    {
        qsort(list, (size_t)n, sizeof(*list), CmpMtimeDesc);
    }
    *out = list;
    return n;
}

static bool WriteAll(int fd, const char *data, size_t len)
{
    size_t offset = 0;
    while (offset < len)
    {
        ssize_t wrote = write(fd, data + offset, len - offset);
        if (wrote > 0)
        {
            offset += (size_t)wrote;
            continue;
        }
        if (wrote < 0 && errno == EINTR)
        {
            continue;
        }
        return false;
    }
    return true;
}

#ifdef PICO_SESSION_TEST_HOOKS
extern bool PicoSession_TestHook(const char *stage);

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

static bool WriteLine(PicoAgent *agent, const char *json, char *error, size_t error_cap)
{
    int failure = 0;
    int lock_fd = SessionLockAcquire(agent->session_path, error, error_cap);
    if (lock_fd < 0)
    {
        return false;
    }
    int fd = open(agent->session_path, O_WRONLY | O_APPEND | O_CREAT, 0600);
    off_t original_size = -1;
    if (fd < 0)
    {
        failure = errno ? errno : EIO;
    }
    else
    {
        original_size = lseek(fd, 0, SEEK_END);
        size_t len = strlen(json);
        if (SessionTestFail("append_write") || !WriteAll(fd, json, len) || !WriteAll(fd, "\n", 1) ||
            fsync(fd) != 0)
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
    SessionLockRelease(lock_fd);
    if (failure != 0 && error && error_cap > 0)
    {
        snprintf(error, error_cap, "%s", strerror(failure));
    }
    return failure == 0;
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

static int CreateNew(PicoHost *app, PicoAgent *agent)
{
    char dir[4096];
    if (!SessionDir(SessionWorkspace(app, agent), dir, sizeof(dir)))
    {
        PersistenceFailed(app, agent, "session directory path is too long");
        return -1;
    }
    Pico_MkdirP(dir);
    Pico_RandomHex(agent->session_id, sizeof(agent->session_id));
    char stamp[40];
    Pico_IsoTime(stamp, sizeof(stamp), true);
    char canonical_dir[4096];
    if (!realpath(dir, canonical_dir))
    {
        PersistenceFailed(app, agent, strerror(errno ? errno : EIO));
        return -1;
    }
    if ((size_t)snprintf(agent->session_path, sizeof(agent->session_path), "%s/%s_%s.jsonl",
                         canonical_dir, stamp, agent->session_id) >= sizeof(agent->session_path))
    {
        PersistenceFailed(app, agent, "session path is too long");
        return -1;
    }
    if (app->agents && !PicoAgentManager_ReserveSession(app->agents, agent->id, agent->session_path))
    {
        PersistenceFailed(app, agent, "session path is already reserved");
        return -1;
    }

    char ts[40];
    Pico_IsoTime(ts, sizeof(ts), false);
    JsonBuf b;
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
    char *line = JsonBuf_Steal(&b);
    if (!line)
    {
        PersistenceFailed(app, agent, "out of memory while creating the session header");
        return -1;
    }
    char error[256] = {0};
    bool wrote = WriteLine(agent, line, error, sizeof(error));
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
    if (!WriteLine(agent, json, error, sizeof(error)))
    {
        PersistenceFailed(app, agent, error);
        return PICO_SESSION_WRITE_FAILED;
    }
    return PICO_SESSION_WRITE_OK;
}

static void ApplyHeader(PicoHost *app, PicoAgent *agent, const JsonDoc *doc, int obj)
{
    char *kind = JsonObjStr(doc, obj, "kind");
    agent->kind = kind && strcmp(kind, "subagent") == 0 ? PICO_AGENT_SUBAGENT : PICO_AGENT_MAIN;
    free(kind);
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
        PicoSettings_SyncAgent(app, agent);
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
    for (int i = 0; i < app->tool_count; i++)
    {
        PicoTool *tool = &app->tools[i];
        if (tool->name && strcmp(tool->name, name) == 0)
        {
            if (tool->apply)
            {
                (void)tool->apply(PicoHost_PrimaryWorkspace(app), agent->id, details, true, tool->state);
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
        PicoSettings_SyncAgent(app, agent);
        free(model);
        free(effort);
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
            valid_header = type && strcmp(type, "session") == 0 &&
                           header_id && header_id[0] && version == 4 &&
                           (normal || subagent);
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
    if (session_file && session_file[0])
    {
        char canonical[4096];
        if (!realpath(session_file, canonical) ||
            (app->agents && !PicoAgentManager_ReserveSession(app->agents, agent->id, canonical)) ||
            PicoSession_Replay(app, agent, canonical, true) != 0)
        {
            agent->persistence = PICO_SESSION_FAILED;
            pico_status_warn(app, "Could not open the requested session file.");
        }
        return;
    }
    if (start == PICO_SESSION_RESUME || app->settings.resume_last)
    {
        char dir[4096];
        char latest[4096];
        if (SessionDir(SessionWorkspace(app, agent), dir, sizeof(dir)) &&
            FindLatest(dir, latest, sizeof(latest)) == 0)
        {
            char canonical[4096];
            if (realpath(latest, canonical) &&
                (!app->agents || PicoAgentManager_ReserveSession(app->agents, agent->id, canonical)))
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
    line->tool_args = JsonDup(args ? args : "");
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
    if (app->agents && PicoAgentManager_SessionReserved(app->agents, path, agent->id))
    {
        return -1;
    }

    PicoSession_Reset(app, agent);
    if (app->agents && !PicoAgentManager_ReserveSession(app->agents, agent->id, path))
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
    if (app->agents)
    {
        PicoAgentManager_ReleaseSessions(app->agents, agent->id);
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
    PicoSessionWriteResult result = AppendLine(app, agent, line);
    free(line);
    free(pre);
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
        if (n > 0 && !WriteAll(fd, buf, n))
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
    if (!WriteAll(fd, header, strlen(header)) || !WriteAll(fd, "\n", 1) ||
        !CopyRemainder(src, fd) || SessionTestFail("title_after_copy") ||
        !WriteAll(fd, event_line, strlen(event_line)) || !WriteAll(fd, "\n", 1) ||
        SessionTestFail("title_fsync") || fsync(fd) != 0)
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
    SessionLockRelease(lock_fd);
    if (failure != 0)
    {
        PersistenceFailed(app, agent, strerror(failure));
        return PICO_SESSION_WRITE_FAILED;
    }
    return PICO_SESSION_WRITE_OK;
}
