#define _DEFAULT_SOURCE
#define _POSIX_C_SOURCE 200809L

#include "session.h"
#include "agent.h"
#include "json.h"
#include "settings.h"

#include <ctype.h>
#include <dirent.h>
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
    if (!realpath(src, real))
    {
        snprintf(real, sizeof(real), "%s", src);
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

static void SessionDir(const PicoApp *app, char *out, size_t cap)
{
    char cfg[4096];
    char enc[4096];
    Pico_ConfigDir(cfg, sizeof(cfg));
    EncodeCwd(app->workspace, enc, sizeof(enc));
    snprintf(out, cap, "%s/sessions/%s", cfg, enc);
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
        snprintf(path, sizeof(path), "%s/%s", dir, n);
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
    snprintf(out, cap, "%s/%s", dir, best);
    return 0;
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

static void ScanSessionFile(const char *path, char *id, size_t id_cap, char *title, size_t title_cap)
{
    if (title && title_cap)
    {
        title[0] = '\0';
    }
    FILE *f = fopen(path, "rb");
    if (!f)
    {
        if (title && title_cap)
        {
            snprintf(title, title_cap, "Untitled");
        }
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
            if (sid && sid[0] && id && id_cap)
            {
                snprintf(id, id_cap, "%s", sid);
            }
            free(sid);
        }
        else if (type && strcmp(type, "message") == 0)
        {
            char *role = JsonObjStr(&doc, 0, "role");
            if (role && strcmp(role, "user") == 0)
            {
                char *display = JsonObjStr(&doc, 0, "display");
                char *content = JsonObjStr(&doc, 0, "content");
                const char *src = (display && display[0]) ? display : content;
                MakeTitle(title, title_cap, src);
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
    if (title && title_cap && !title[0])
    {
        snprintf(title, title_cap, "Untitled");
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

int PicoSession_List(const PicoApp *app, PicoSessionInfo **out)
{
    if (out)
    {
        *out = NULL;
    }
    if (!app || !out)
    {
        return 0;
    }
    char dir[4096];
    SessionDir(app, dir, sizeof(dir));
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
        snprintf(s->path, sizeof(s->path), "%s/%s", dir, ent->d_name);
        struct stat st;
        if (stat(s->path, &st) != 0 || !S_ISREG(st.st_mode))
        {
            continue;
        }
        s->mtime = st.st_mtime;
        IdFromName(ent->d_name, s->id, sizeof(s->id));
        ScanSessionFile(s->path, s->id, sizeof(s->id), s->title, sizeof(s->title));
        if (!s->id[0])
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

static void WriteLine(PicoApp *app, const char *json)
{
    FILE *f = fopen(app->session_path, "ab");
    if (!f)
    {
        return;
    }
    fwrite(json, 1, strlen(json), f);
    fputc('\n', f);
    fflush(f);
    fsync(fileno(f));
    fclose(f);
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

static int CreateNew(PicoApp *app)
{
    char dir[4096];
    SessionDir(app, dir, sizeof(dir));
    Pico_MkdirP(dir);
    Pico_RandomHex(app->session_id, sizeof(app->session_id));
    char stamp[40];
    Pico_IsoTime(stamp, sizeof(stamp), true);
    snprintf(app->session_path, sizeof(app->session_path), "%s/%s_%s.jsonl", dir, stamp, app->session_id);

    char ts[40];
    Pico_IsoTime(ts, sizeof(ts), false);
    JsonBuf b;
    JsonBuf_Init(&b);
    JsonBuf_Puts(&b, "{\"type\":\"session\",\"version\":1,\"id\":");
    JsonBuf_String(&b, app->session_id);
    JsonBuf_Puts(&b, ",\"timestamp\":");
    JsonBuf_String(&b, ts);
    JsonBuf_Puts(&b, ",\"cwd\":");
    JsonBuf_String(&b, app->workspace);
    JsonBuf_Puts(&b, ",\"model\":");
    JsonBuf_String(&b, app->settings.model);
    const char *cache_key = PicoAgent_CacheKey(app);
    if (cache_key && cache_key[0])
    {
        JsonBuf_Puts(&b, ",\"prompt_cache_key\":");
        JsonBuf_String(&b, cache_key);
    }
    JsonBuf_Putc(&b, '}');
    char *line = JsonBuf_Steal(&b);
    WriteLine(app, line);
    free(line);
    return 0;
}

static void AppendLine(PicoApp *app, const char *json)
{
    if (!app || app->session_ephemeral || !json || !json[0])
    {
        return;
    }
    if (!app->session_path[0] && CreateNew(app) != 0)
    {
        return;
    }
    if (!app->session_path[0])
    {
        return;
    }
    WriteLine(app, json);
}

static void ApplyHeader(PicoApp *app, const JsonDoc *doc, int obj)
{
    char *id = JsonObjStr(doc, obj, "id");
    if (id && id[0])
    {
        snprintf(app->session_id, sizeof(app->session_id), "%s", id);
    }
    free(id);
    char *model = JsonObjStr(doc, obj, "model");
    if (model && model[0])
    {
        snprintf(app->settings.model, sizeof(app->settings.model), "%s", model);
        app->model_name = app->settings.model;
    }
    free(model);
    char *key = JsonObjStr(doc, obj, "prompt_cache_key");
    if (key && key[0] && app->agent)
    {
        PicoAgent_SetCacheKey(app, key);
    }
    free(key);
}

static void ReplayLine(PicoApp *app, const JsonDoc *doc, int obj, bool into_input)
{
    char *type = JsonObjStr(doc, obj, "type");
    if (!type)
    {
        return;
    }
    if (strcmp(type, "session") == 0)
    {
        ApplyHeader(app, doc, obj);
    }
    else if (strcmp(type, "message") == 0)
    {
        char *role = JsonObjStr(doc, obj, "role");
        char *content = JsonObjStr(doc, obj, "content");
        int input_tok = 0;
        int usage = JsonObjGet(doc, obj, "usage");
        if (JsonIsObject(doc, usage))
        {
            input_tok = JsonObjInt(doc, usage, "input_tokens", 0);
            int cached = JsonObjInt(doc, usage, "cached_tokens", 0);
            if (input_tok > 0)
            {
                app->tokens_used = input_tok;
                app->tokens_cached = cached;
            }
        }
        if (role && strcmp(role, "user") == 0)
        {
            char *display = JsonObjStr(doc, obj, "display");
            PicoApp_AddMessage(app, PICO_ROLE_USER,
                               display && display[0] ? display : (content ? content : ""));
            if (into_input)
            {
                PicoAgent_PushHistoryUser(app, content ? content : "");
            }
            free(display);
        }
        else if (role && strcmp(role, "assistant") == 0)
        {
            PicoApp_AppendAssistant(app, content ? content : "");
            if (into_input && content && content[0])
            {
                PicoAgent_PushHistoryAssistant(app, content);
            }
        }
        free(role);
        free(content);
        (void)input_tok;
    }
    else if (strcmp(type, "tool_call") == 0)
    {
        char *call_id = JsonObjStr(doc, obj, "call_id");
        char *name = JsonObjStr(doc, obj, "name");
        char *args = JsonObjStr(doc, obj, "arguments");
        PicoApp_AddToolCall(app, name, args);
        if (into_input)
        {
            PicoAgent_PushHistoryFunctionCall(app, call_id, name, args);
        }
        free(call_id);
        free(name);
        free(args);
    }
    else if (strcmp(type, "tool_result") == 0)
    {
        char *call_id = JsonObjStr(doc, obj, "call_id");
        char *output = JsonObjStr(doc, obj, "output");
        PicoApp_SetLastToolOutput(app, output);
        if (into_input)
        {
            PicoAgent_PushHistoryFunctionOutput(app, call_id, output);
        }
        free(call_id);
        free(output);
    }
    else if (strcmp(type, "compaction") == 0)
    {
        char *summary = JsonObjStr(doc, obj, "summary");
        if (into_input)
        {
            PicoAgent_ClearInput(app);
            JsonBuf b;
            JsonBuf_Init(&b);
            JsonBuf_Puts(&b, "Briefing:\n");
            JsonBuf_Puts(&b, summary ? summary : "");
            PicoAgent_PushHistoryUser(app, b.data ? b.data : "Briefing:\n");
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
            snprintf(app->settings.model, sizeof(app->settings.model), "%s", model);
        }
        if (effort && effort[0])
        {
            PicoModel *m = PicoSettings_ActiveModel(app);
            if (m)
            {
                snprintf(m->selected_effort, sizeof(m->selected_effort), "%s", effort);
            }
        }
        PicoSettings_SyncActive(app);
        free(model);
        free(effort);
    }
    free(type);
}

static int ReplayFile(PicoApp *app, const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f)
    {
        return -1;
    }
    snprintf(app->session_path, sizeof(app->session_path), "%s", path);

    char **lines = NULL;
    int n = 0;
    int cap = 0;
    char *buf = NULL;
    size_t buf_cap = 0;
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
                break;
            }
            lines = next;
        }
        lines[n++] = JsonDup(buf);
    }
    free(buf);
    fclose(f);

    int last_compact = -1;
    int last_tool_call = -1;
    int last_tool_result = -1;
    for (int i = 0; i < n; i++)
    {
        JsonDoc doc;
        if (JsonParse(&doc, lines[i], strlen(lines[i])) != 0)
        {
            continue;
        }
        char *type = JsonObjStr(&doc, 0, "type");
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

    PicoAgent_ClearInput(app);
    for (int i = 0; i < n; i++)
    {
        JsonDoc doc;
        if (JsonParse(&doc, lines[i], strlen(lines[i])) != 0)
        {
            continue;
        }
        bool into_input = (last_compact < 0) || (i >= last_compact);
        ReplayLine(app, &doc, 0, into_input);
        JsonFree(&doc);
    }

    if (last_tool_call > last_tool_result)
    {
        JsonDoc doc;
        if (JsonParse(&doc, lines[last_tool_call], strlen(lines[last_tool_call])) == 0)
        {
            char *call_id = JsonObjStr(&doc, 0, "call_id");
            PicoSession_LogToolResult(app, call_id, "(interrupted)", true);
            PicoApp_SetLastToolOutput(app, "(interrupted)");
            PicoAgent_PushHistoryFunctionOutput(app, call_id, "(interrupted)");
            free(call_id);
            JsonFree(&doc);
        }
    }

    for (int i = 0; i < n; i++)
    {
        free(lines[i]);
    }
    free(lines);
    app->chat_follow_bottom = true;
    return 0;
}

void PicoSession_Start(PicoApp *app, PicoSessionStart start, const char *session_file)
{
    if (!app)
    {
        return;
    }
    if (start == PICO_SESSION_NONE)
    {
        app->session_ephemeral = true;
        app->session_path[0] = '\0';
        return;
    }
    app->session_ephemeral = false;
    if (session_file && session_file[0])
    {
        ReplayFile(app, session_file);
        return;
    }
    if (start == PICO_SESSION_RESUME || app->settings.resume_last)
    {
        char dir[4096];
        char latest[4096];
        SessionDir(app, dir, sizeof(dir));
        if (FindLatest(dir, latest, sizeof(latest)) == 0)
        {
            ReplayFile(app, latest);
        }
    }
}

int PicoSession_Open(PicoApp *app, const char *id)
{
    if (!app || !id || !id[0] || PicoAgent_IsBusy(app))
    {
        return -1;
    }

    PicoSessionInfo *list = NULL;
    int n = PicoSession_List(app, &list);
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
        if (strncmp(list[i].id, id, id_len) == 0)
        {
            prefix = &list[i];
            prefix_hits++;
        }
    }
    if (!found && prefix_hits == 1)
    {
        found = prefix;
    }
    if (!found)
    {
        free(list);
        return -1;
    }

    if (app->session_path[0] && strcmp(app->session_path, found->path) == 0)
    {
        free(list);
        return 0;
    }

    char path[4096];
    snprintf(path, sizeof(path), "%s", found->path);
    free(list);

    PicoSession_Reset(app);
    app->session_ephemeral = false;
    return ReplayFile(app, path);
}

void PicoSession_Reset(PicoApp *app)
{
    if (!app)
    {
        return;
    }
    PicoAgent_DismissError(app);
    PicoApp_ClearMessages(app);
    PicoAgent_ClearInput(app);
    PicoAgent_RotateCacheKey(app);
    app->tokens_used = 0;
    app->tokens_cached = 0;
    app->agent_activity[0] = '\0';
    free(app->compact_summary);
    app->compact_summary = NULL;
    app->session_id[0] = '\0';
    app->session_path[0] = '\0';
}

void PicoSession_LogUser(PicoApp *app, const char *content, const char *display)
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
    AppendLine(app, line);
    free(line);
    free(pre);
}

void PicoSession_LogAssistant(PicoApp *app, const char *content, int input_tokens, int cached_tokens)
{
    if (!content || !content[0])
    {
        return;
    }
    char *pre = EventPrefix("message");
    JsonBuf b;
    JsonBuf_Init(&b);
    JsonBuf_Puts(&b, pre);
    JsonBuf_Puts(&b, ",\"role\":\"assistant\",\"content\":");
    JsonBuf_String(&b, content);
    if (input_tokens > 0)
    {
        JsonBuf_Puts(&b, ",\"usage\":{\"input_tokens\":");
        JsonBuf_Int(&b, input_tokens);
        JsonBuf_Puts(&b, ",\"cached_tokens\":");
        JsonBuf_Int(&b, cached_tokens);
        JsonBuf_Putc(&b, '}');
    }
    JsonBuf_Putc(&b, '}');
    char *line = JsonBuf_Steal(&b);
    AppendLine(app, line);
    free(line);
    free(pre);
}

void PicoSession_LogToolCall(PicoApp *app, const char *call_id, const char *name, const char *args)
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
    AppendLine(app, line);
    free(line);
    free(pre);
}

void PicoSession_LogToolResult(PicoApp *app, const char *call_id, const char *output, bool is_error)
{
    char *pre = EventPrefix("tool_result");
    JsonBuf b;
    JsonBuf_Init(&b);
    JsonBuf_Puts(&b, pre);
    JsonBuf_Puts(&b, ",\"call_id\":");
    JsonBuf_String(&b, call_id ? call_id : "");
    JsonBuf_Puts(&b, ",\"output\":");
    JsonBuf_String(&b, output ? output : "");
    JsonBuf_Puts(&b, ",\"is_error\":");
    JsonBuf_Bool(&b, is_error);
    JsonBuf_Putc(&b, '}');
    char *line = JsonBuf_Steal(&b);
    AppendLine(app, line);
    free(line);
    free(pre);
}

void PicoSession_LogCompaction(PicoApp *app, const char *summary, int tokens_before)
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
    AppendLine(app, line);
    free(line);
    free(pre);
}

void PicoSession_LogModelChange(PicoApp *app, const char *model, const char *effort)
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
    AppendLine(app, line);
    free(line);
    free(pre);
}

void PicoSession_LogCustom(PicoApp *app, const char *ext, const char *data_json)
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
    AppendLine(app, line);
    free(line);
    free(pre);
}
