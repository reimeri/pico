#define _DEFAULT_SOURCE
#define _POSIX_C_SOURCE 200809L

#include "session.h"
#include "agent.h"
#include "agent_manager.h"
#include "json.h"
#include "path.h"
#include "settings.h"
#include "usage.h"
#include "workspace.h"

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

static bool SessionDirForKey(const PicoApp *app, const char *key, const char *path,
                             char *out, size_t cap)
{
    if (!app || !app->workspaces || !key || !key[0] || !path || !path[0])
    {
        return false;
    }
    const PicoWorkspace *workspace = PicoWorkspaceRegistry_FindKey(app->workspaces, key);
    return workspace && workspace->available && strcmp(workspace->path, path) == 0 &&
           PicoWorkspace_SessionDir(app->workspaces, workspace->key, out, cap);
}

static bool SessionDir(const PicoApp *app, const PicoAgent *agent, char *out, size_t cap)
{
    return agent && SessionDirForKey(app, agent->workspace_key, agent->workspace_path, out, cap);
}

static int ReadSessionHeaderFd(int fd, PicoSessionHeader *out);

static bool IsSessionJsonl(const char *name)
{
    size_t len = name ? strlen(name) : 0;
    return len >= 7 && name[0] != '.' && strcmp(name + len - 6, ".jsonl") == 0;
}

static void IdFromName(const char *name, char *out, size_t cap)
{
    out[0] = '\0';
    if (!name || cap < 2)
    {
        return;
    }
    const char *us = strrchr(name, '_');
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
    info->kind = PICO_AGENT_NORMAL;
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

int PicoSession_ListWorkspace(const PicoApp *app, const char *workspace_key,
                              PicoSessionInfo **out, bool parents_only)
{
    if (out)
    {
        *out = NULL;
    }
    if (!app || !app->workspaces || !workspace_key || !workspace_key[0] || !out)
    {
        return 0;
    }
    const PicoWorkspace *workspace = PicoWorkspaceRegistry_FindKey(app->workspaces, workspace_key);
    if (!workspace)
    {
        return 0;
    }
    char dir[4096];
    if (!SessionDirForKey(app, workspace->key, workspace->path, dir, sizeof(dir)))
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
        if (lstat(s->path, &st) != 0 || !S_ISREG(st.st_mode))
        {
            continue;
        }
        char filename_id[40];
        IdFromName(ent->d_name, filename_id, sizeof(filename_id));
        PicoSessionHeader header;
        if (!filename_id[0] || PicoSession_ReadHeader(s->path, &header) != 0 ||
            strcmp(filename_id, header.id) != 0 ||
            strcmp(header.cwd, workspace->path) != 0)
        {
            continue;
        }
        s->mtime = st.st_mtime;
        ScanSessionFile(s->path, s);
        snprintf(s->id, sizeof(s->id), "%s", header.id);
        s->kind = header.kind;
        n++;
    }
    closedir(d);
    for (int i = 0; i < n; i++)
    {
        for (int j = i + 1; j < n; j++)
        {
            if (list[i].id[0] && strcmp(list[i].id, list[j].id) == 0)
            {
                list[i].id[0] = '\0';
                list[j].id[0] = '\0';
            }
        }
    }
    int valid = 0;
    for (int i = 0; i < n; i++)
    {
        if (list[i].id[0] && (!parents_only || list[i].kind == PICO_AGENT_NORMAL))
        {
            list[valid++] = list[i];
        }
    }
    n = valid;
    if (n > 1)
    {
        qsort(list, (size_t)n, sizeof(*list), CmpMtimeDesc);
    }
    *out = list;
    return n;
}

int PicoSession_List(const PicoApp *app, const PicoAgent *agent,
                     PicoSessionInfo **out, bool parents_only)
{
    if (!agent)
    {
        if (out)
        {
            *out = NULL;
        }
        return 0;
    }
    return PicoSession_ListWorkspace(app, agent->workspace_key, out, parents_only);
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

static void CloseSessionLock(PicoAgent *agent)
{
    if (agent && agent->session_lock_fd >= 0)
    {
        close(agent->session_lock_fd);
        agent->session_lock_fd = -1;
    }
}

static bool AcquireSessionLock(PicoAgent *agent, const char *path, bool create)
{
    int flags = O_RDWR | O_APPEND | O_CLOEXEC | O_NOFOLLOW;
    if (create)
    {
        flags |= O_CREAT | O_EXCL;
    }
    int fd = open(path, flags, 0600);
    if (fd < 0 || flock(fd, LOCK_EX | LOCK_NB) != 0)
    {
        if (fd >= 0)
        {
            close(fd);
        }
        return false;
    }
    CloseSessionLock(agent);
    agent->session_lock_fd = fd;
    return true;
}

static bool WriteLine(PicoAgent *agent, const char *json, char *error, size_t error_cap)
{
    int failure = 0;
    int fd = agent ? agent->session_lock_fd : -1;
    struct stat path_stat;
    struct stat fd_stat;
    if (fd < 0 || lstat(agent->session_path, &path_stat) != 0 ||
        !S_ISREG(path_stat.st_mode) || fstat(fd, &fd_stat) != 0 ||
        path_stat.st_dev != fd_stat.st_dev || path_stat.st_ino != fd_stat.st_ino)
    {
        failure = errno ? errno : EIO;
    }
    else
    {
        off_t original_size = lseek(fd, 0, SEEK_END);
        size_t len = strlen(json);
        if (original_size < 0 || !WriteAll(fd, json, len) || !WriteAll(fd, "\n", 1) ||
            fsync(fd) != 0)
        {
            failure = errno ? errno : EIO;
            if (original_size >= 0)
            {
                int rollback_result = ftruncate(fd, original_size);
                (void)rollback_result;
            }
        }
    }
    if (failure != 0 && error && error_cap > 0)
    {
        snprintf(error, error_cap, "%s", strerror(failure));
    }
    return failure == 0;
}

static void PersistenceFailed(PicoApp *app, PicoAgent *agent, const char *reason)
{
    if (!agent || agent->persistence == PICO_SESSION_FAILED)
    {
        return;
    }
    agent->persistence = PICO_SESSION_FAILED;
    CloseSessionLock(agent);
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

static int CreateNew(PicoApp *app, PicoAgent *agent)
{
    char dir[4096];
    if (!SessionDir(app, agent, dir, sizeof(dir)))
    {
        PersistenceFailed(app, agent, "workspace is not registered or session directory path is too long");
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
    if (!AcquireSessionLock(agent, agent->session_path, true))
    {
        if (app->agents)
        {
            PicoAgentManager_ReleaseSessions(app->agents, agent->id);
        }
        PersistenceFailed(app, agent, "session is locked by another Pico process");
        return -1;
    }

    char ts[40];
    Pico_IsoTime(ts, sizeof(ts), false);
    JsonBuf b;
    JsonBuf_Init(&b);
    JsonBuf_Puts(&b, "{\"type\":\"session\",\"version\":3,\"id\":");
    JsonBuf_String(&b, agent->session_id);
    JsonBuf_Puts(&b, ",\"timestamp\":");
    JsonBuf_String(&b, ts);
    JsonBuf_Puts(&b, ",\"cwd\":");
    JsonBuf_String(&b, agent->workspace_path);
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

static PicoSessionWriteResult AppendLine(PicoApp *app, PicoAgent *agent, const char *json)
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

static void ApplyHeader(PicoApp *app, PicoAgent *agent, const JsonDoc *doc, int obj)
{
    char *kind = JsonObjStr(doc, obj, "kind");
    agent->kind = kind && strcmp(kind, "subagent") == 0 ? PICO_AGENT_SUBAGENT : PICO_AGENT_NORMAL;
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

static void ApplyToolDetails(PicoApp *app, PicoAgent *agent, const char *name,
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
                (void)tool->apply(app, agent->id, details, true);
            }
            return;
        }
    }
}

static void ReplayLine(PicoApp *app, PicoAgent *agent, const JsonDoc *doc, int obj, bool into_input)
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
            char *display = JsonObjStr(doc, obj, "display");
            PicoAgent_AddMessage(app, agent, PICO_ROLE_USER,
                               display && display[0] ? display : (content ? content : ""));
            if (into_input)
            {
                PicoAgent_PushHistoryUser(agent, content ? content : "");
            }
            free(display);
        }
        else if (role && strcmp(role, "assistant") == 0)
        {
            PicoAgent_AppendAssistant(app, agent, content ? content : "");
            if (into_input && content && content[0])
            {
                PicoAgent_PushHistoryAssistant(agent, content);
            }
        }
        free(role);
        free(content);
    }
    else if (strcmp(type, "tool_call") == 0)
    {
        char *call_id = JsonObjStr(doc, obj, "call_id");
        char *name = JsonObjStr(doc, obj, "name");
        char *args = JsonObjStr(doc, obj, "arguments");
        PicoAgent_AddToolCall(app, agent, name, args);
        if (into_input)
        {
            PicoAgent_PushHistoryFunctionCall(agent, call_id, name, args);
        }
        free(call_id);
        free(name);
        free(args);
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
        PicoAgent_SetLastToolOutput(agent, output, is_error);
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

static bool LockedPathMatches(const PicoAgent *agent)
{
    struct stat path_stat;
    struct stat fd_stat;
    return agent && agent->session_lock_fd >= 0 &&
           lstat(agent->session_path, &path_stat) == 0 && S_ISREG(path_stat.st_mode) &&
           fstat(agent->session_lock_fd, &fd_stat) == 0 &&
           path_stat.st_dev == fd_stat.st_dev && path_stat.st_ino == fd_stat.st_ino;
}

static bool ValidateSessionLocation(const PicoApp *app, const PicoAgent *agent,
                                    const char *path, char *canonical, size_t canonical_cap)
{
    if (!app || !agent || !path || !canonical || !realpath(path, canonical) ||
        strlen(canonical) >= canonical_cap)
    {
        return false;
    }
    struct stat st;
    if (lstat(path, &st) != 0 || !S_ISREG(st.st_mode))
    {
        return false;
    }
    char directory[4096];
    char canonical_directory[4096];
    char parent[4096];
    if (!SessionDir(app, agent, directory, sizeof(directory)) ||
        !realpath(directory, canonical_directory) ||
        !PicoPath_Format(parent, sizeof(parent), "%s", canonical))
    {
        return false;
    }
    char *slash = strrchr(parent, '/');
    if (!slash || slash == parent || !IsSessionJsonl(slash + 1))
    {
        return false;
    }
    *slash = '\0';
    return strcmp(parent, canonical_directory) == 0;
}

int PicoSession_Replay(PicoApp *app, PicoAgent *agent, const char *path,
                       bool append_interrupted)
{
    PicoSessionHeader header;
    char canonical[4096];
    if (!ValidateSessionLocation(app, agent, path, canonical, sizeof(canonical)) ||
        !AcquireSessionLock(agent, canonical, false))
    {
        return -1;
    }
    snprintf(agent->session_path, sizeof(agent->session_path), "%s", canonical);
    struct stat locked_stat;
    if (!LockedPathMatches(agent) || fstat(agent->session_lock_fd, &locked_stat) != 0 ||
        !S_ISREG(locked_stat.st_mode) || ReadSessionHeaderFd(agent->session_lock_fd, &header) != 0 ||
        header.kind != agent->kind || strcmp(header.cwd, agent->workspace_path) != 0)
    {
        CloseSessionLock(agent);
        agent->session_path[0] = '\0';
        return -1;
    }
    const char *filename = strrchr(canonical, '/');
    char filename_id[40];
    IdFromName(filename ? filename + 1 : canonical, filename_id, sizeof(filename_id));
    if (!filename_id[0] || strcmp(filename_id, header.id) != 0)
    {
        CloseSessionLock(agent);
        agent->session_path[0] = '\0';
        return -1;
    }
    int read_fd = dup(agent->session_lock_fd);
    FILE *f = read_fd >= 0 ? fdopen(read_fd, "rb") : NULL;
    if (!f)
    {
        if (read_fd >= 0)
        {
            close(read_fd);
        }
        CloseSessionLock(agent);
        return -1;
    }
    if (lseek(read_fd, 0, SEEK_SET) < 0)
    {
        fclose(f);
        CloseSessionLock(agent);
        agent->session_path[0] = '\0';
        return -1;
    }

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
                           header_id && header_id[0] && version == 3 &&
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
    for (int i = 0; i < n; i++)
    {
        JsonDoc doc;
        if (JsonParse(&doc, lines[i], strlen(lines[i])) != 0)
        {
            continue;
        }
        bool into_input = (last_compact < 0) || (i >= last_compact);
        ReplayLine(app, agent, &doc, 0, into_input);
        JsonFree(&doc);
    }

    for (int i = 0; i < n; i++)
    {
        free(lines[i]);
    }
    free(lines);
    agent->ui.chat_follow_bottom = true;
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
    CloseSessionLock(agent);
    agent->session_path[0] = '\0';
    agent->session_id[0] = '\0';
    return -1;
}

static FILE *OpenLockedSessionRead(PicoAgent *agent)
{
    if (!LockedPathMatches(agent))
    {
        return NULL;
    }
    int fd = dup(agent->session_lock_fd);
    if (fd < 0 || lseek(fd, 0, SEEK_SET) < 0)
    {
        if (fd >= 0) close(fd);
        return NULL;
    }
    FILE *file = fdopen(fd, "rb");
    if (!file)
    {
        close(fd);
    }
    return file;
}

void PicoSession_AppendInterrupted(PicoApp *app, PicoAgent *agent)
{
    if (!app || !agent || !agent->session_path[0])
    {
        return;
    }
    FILE *f = OpenLockedSessionRead(agent);
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

void PicoSession_ReplayToolDetails(PicoApp *app, PicoAgent *agent)
{
    if (!app || !agent->session_path[0])
    {
        return;
    }
    FILE *f = OpenLockedSessionRead(agent);
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

void PicoSession_Start(PicoApp *app, PicoAgent *agent, PicoSessionStart start, const char *session_file)
{
    if (!app || !agent)
    {
        return;
    }
    if (start == PICO_SESSION_NONE)
    {
        CloseSessionLock(agent);
        agent->persistence = PICO_SESSION_EPHEMERAL;
        agent->session_path[0] = '\0';
        return;
    }
    agent->persistence = PICO_SESSION_DURABLE;
    if (session_file && session_file[0])
    {
        char canonical[4096];
        bool reserved = ValidateSessionLocation(app, agent, session_file,
                                                canonical, sizeof(canonical)) &&
                        (!app->agents || PicoAgentManager_ReserveSession(
                                             app->agents, agent->id, canonical));
        if (!reserved || PicoSession_Replay(app, agent, canonical, true) != 0)
        {
            if (app->agents)
            {
                PicoAgentManager_ReleaseSessions(app->agents, agent->id);
            }
            CloseSessionLock(agent);
            agent->session_path[0] = '\0';
            agent->session_id[0] = '\0';
            agent->persistence = PICO_SESSION_FAILED;
            pico_status_warn(app, "Could not open the requested session file.");
        }
        return;
    }
    if (start == PICO_SESSION_RESUME || app->settings.resume_last)
    {
        PicoSessionInfo *sessions = NULL;
        int count = PicoSession_List(app, agent, &sessions, agent->kind == PICO_AGENT_NORMAL);
        if (count > 0)
        {
            bool reserved = !app->agents || PicoAgentManager_ReserveSession(
                                                app->agents, agent->id, sessions[0].path);
            if (!reserved || PicoSession_Replay(app, agent, sessions[0].path, true) != 0)
            {
                if (app->agents)
                {
                    PicoAgentManager_ReleaseSessions(app->agents, agent->id);
                }
                CloseSessionLock(agent);
                agent->session_path[0] = '\0';
                agent->session_id[0] = '\0';
            }
        }
        free(sessions);
    }
}

static int ParseSessionHeaderLine(const char *line, size_t length, PicoSessionHeader *out)
{
    memset(out, 0, sizeof(*out));
    JsonDoc doc;
    if (!line || JsonParse(&doc, line, length) != 0 ||
        !JsonEq(&doc, JsonObjGet(&doc, 0, "type"), "session"))
    {
        if (line && doc.toks)
        {
            JsonFree(&doc);
        }
        return -1;
    }
    out->version = JsonObjInt(&doc, 0, "version", 0);
    char *id = JsonObjStr(&doc, 0, "id");
    char *kind = JsonObjStr(&doc, 0, "kind");
    char *profile = JsonObjStr(&doc, 0, "profile");
    char *purpose = JsonObjStr(&doc, 0, "initial_purpose");
    char *parent = JsonObjStr(&doc, 0, "parent_session_id");
    char *model = JsonObjStr(&doc, 0, "model");
    char *cwd = JsonObjStr(&doc, 0, "cwd");
    if (id) snprintf(out->id, sizeof(out->id), "%s", id);
    bool kind_valid = kind && (strcmp(kind, "normal") == 0 || strcmp(kind, "subagent") == 0);
    out->kind = kind && strcmp(kind, "subagent") == 0 ? PICO_AGENT_SUBAGENT : PICO_AGENT_NORMAL;
    if (profile) snprintf(out->profile, sizeof(out->profile), "%s", profile);
    if (purpose) snprintf(out->initial_purpose, sizeof(out->initial_purpose), "%s", purpose);
    if (parent) snprintf(out->parent_session_id, sizeof(out->parent_session_id), "%s", parent);
    if (model) snprintf(out->model, sizeof(out->model), "%s", model);
    if (cwd) snprintf(out->cwd, sizeof(out->cwd), "%s", cwd);
    bool valid = out->version == 3 && out->id[0] && out->cwd[0] == '/' && kind_valid &&
                 (out->kind == PICO_AGENT_NORMAL || (out->profile[0] && out->initial_purpose[0]));
    free(id); free(kind); free(profile); free(purpose); free(parent); free(model); free(cwd);
    JsonFree(&doc);
    return valid ? 0 : -1;
}

static int ReadSessionHeaderFd(int fd, PicoSessionHeader *out)
{
    int copy = dup(fd);
    if (copy < 0 || lseek(copy, 0, SEEK_SET) < 0)
    {
        if (copy >= 0) close(copy);
        return -1;
    }
    FILE *file = fdopen(copy, "rb");
    if (!file)
    {
        close(copy);
        return -1;
    }
    char *line = NULL;
    size_t capacity = 0;
    ssize_t got = getline(&line, &capacity, file);
    int result = got > 0 ? ParseSessionHeaderLine(line, (size_t)got, out) : -1;
    free(line);
    fclose(file);
    return result;
}

int PicoSession_ReadHeader(const char *path, PicoSessionHeader *out)
{
    struct stat st;
    if (!path || !out || lstat(path, &st) != 0 || !S_ISREG(st.st_mode))
    {
        return -1;
    }
    int fd = open(path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0)
    {
        return -1;
    }
    int result = ReadSessionHeaderFd(fd, out);
    close(fd);
    return result;
}

int PicoSession_Resolve(const PicoApp *app, const PicoAgent *agent,
                        const char *id, bool allow_prefix, char *path, size_t path_cap)
{
    if (!app || !agent || !id || !id[0] || !path || path_cap == 0)
    {
        return -1;
    }
    PicoSessionInfo *list = NULL;
    int n = PicoSession_List(app, agent, &list, false);
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

int PicoSession_Preflight(const char *workspace, const char *session_file,
                          PicoSessionHeader *out)
{
    if (!workspace || !workspace[0] || !session_file || !session_file[0] || !out)
    {
        return -1;
    }
    char canonical_workspace[4096];
    char canonical_file[4096];
    if (!realpath(workspace, canonical_workspace) || !realpath(session_file, canonical_file))
    {
        return -1;
    }
    struct stat st;
    if (lstat(session_file, &st) != 0 || !S_ISREG(st.st_mode) ||
        PicoSession_ReadHeader(canonical_file, out) != 0 ||
        out->kind != PICO_AGENT_NORMAL || strcmp(out->cwd, canonical_workspace) != 0)
    {
        return -1;
    }
    char key[4096];
    char config[4096];
    char directory[4096];
    char canonical_directory[4096];
    char parent[4096];
    if (!PicoWorkspace_EncodePath(canonical_workspace, key, sizeof(key)) ||
        !Pico_ConfigDir(config, sizeof(config)) ||
        !PicoPath_Format(directory, sizeof(directory), "%s/sessions/%s", config, key) ||
        !realpath(directory, canonical_directory) ||
        !PicoPath_Format(parent, sizeof(parent), "%s", canonical_file))
    {
        return -1;
    }
    char *slash = strrchr(parent, '/');
    if (!slash || slash == parent || !IsSessionJsonl(slash + 1))
    {
        return -1;
    }
    *slash = '\0';
    const char *filename = strrchr(canonical_file, '/');
    char filename_id[40];
    IdFromName(filename ? filename + 1 : canonical_file, filename_id, sizeof(filename_id));
    return strcmp(parent, canonical_directory) == 0 && filename_id[0] &&
                   strcmp(filename_id, out->id) == 0
               ? 0
               : -1;
}

int PicoSession_Open(PicoApp *app, PicoAgent *agent, const char *id)
{
    if (!app || !agent || !id || !id[0])
    {
        return -1;
    }
    if (app->agents)
    {
        return PicoAgentManager_OpenSession(app, agent, id, true, true) == PICO_AGENT_RESULT_OK
                   ? 0
                   : -1;
    }

    char path[4096];
    if (PicoSession_Resolve(app, agent, id, true, path, sizeof(path)) != 0)
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
    if (PicoSession_Replay(app, agent, path, true) != 0)
    {
        if (app->agents)
        {
            PicoAgentManager_ReleaseSessions(app->agents, agent->id);
        }
        CloseSessionLock(agent);
        agent->session_path[0] = '\0';
        agent->session_id[0] = '\0';
        return -1;
    }
    return 0;
}

void PicoSession_Reset(PicoApp *app, PicoAgent *agent)
{
    if (!app || !agent)
    {
        return;
    }
    if (app->agents)
    {
        PicoAgentManager_ReleaseSessions(app->agents, agent->id);
    }
    CloseSessionLock(agent);
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

PicoSessionWriteResult PicoSession_LogUser(PicoApp *app, PicoAgent *agent,
                                             const char *content, const char *display)
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
    JsonBuf_Putc(&b, '}');
    char *line = JsonBuf_Steal(&b);
    PicoSessionWriteResult result = AppendLine(app, agent, line);
    free(line);
    free(pre);
    return result;
}

PicoSessionWriteResult PicoSession_LogUsage(PicoApp *app, PicoAgent *agent,
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

PicoSessionWriteResult PicoSession_LogAssistant(PicoApp *app, PicoAgent *agent,
                                                const char *content)
{
    if (!content || !content[0])
    {
        return PICO_SESSION_WRITE_SKIPPED;
    }
    char *pre = EventPrefix("message");
    JsonBuf b;
    JsonBuf_Init(&b);
    JsonBuf_Puts(&b, pre);
    JsonBuf_Puts(&b, ",\"role\":\"assistant\",\"content\":");
    JsonBuf_String(&b, content);
    JsonBuf_Putc(&b, '}');
    char *line = JsonBuf_Steal(&b);
    PicoSessionWriteResult result = AppendLine(app, agent, line);
    free(line);
    free(pre);
    return result;
}

PicoSessionWriteResult PicoSession_LogToolCall(PicoApp *app, PicoAgent *agent,
                                               const char *call_id, const char *name,
                                               const char *args)
{
    char *pre = EventPrefix("tool_call");
    JsonBuf b;
    JsonBuf_Init(&b);
    JsonBuf_Puts(&b, pre);
    JsonBuf_Puts(&b, ",\"call_id\":");
    JsonBuf_String(&b, call_id ? call_id : "");
    JsonBuf_Puts(&b, ",\"name\":");
    JsonBuf_String(&b, name ? name : "");
    JsonBuf_Puts(&b, ",\"arguments\":");
    JsonBuf_String(&b, args ? args : "{}");
    JsonBuf_Putc(&b, '}');
    char *line = JsonBuf_Steal(&b);
    PicoSessionWriteResult result = AppendLine(app, agent, line);
    free(line);
    free(pre);
    return result;
}

PicoSessionWriteResult PicoSession_LogToolResult(PicoApp *app, PicoAgent *agent,
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

PicoSessionWriteResult PicoSession_LogCompaction(PicoApp *app, PicoAgent *agent,
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

PicoSessionWriteResult PicoSession_LogModelChange(PicoApp *app, PicoAgent *agent,
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

PicoSessionWriteResult PicoSession_LogCustom(PicoApp *app, PicoAgent *agent,
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
