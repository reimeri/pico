#define _POSIX_C_SOURCE 200809L

#include "session.h"
#include "agent.h"
#include "json.h"
#include "settings.h"

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
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

static int FindLatest(const char *dir, char *out, size_t cap)
{
    DIR *d = opendir(dir);
    if (!d)
    {
        return -1;
    }
    char best[256];
    best[0] = '\0';
    struct dirent *ent;
    while ((ent = readdir(d)))
    {
        const char *n = ent->d_name;
        size_t len = strlen(n);
        if (len < 7 || strcmp(n + len - 6, ".jsonl") != 0)
        {
            continue;
        }
        if (!best[0] || strcmp(n, best) > 0)
        {
            snprintf(best, sizeof(best), "%s", n);
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
            PicoApp_AddMessage(app, PICO_ROLE_USER, content ? content : "");
            if (into_input)
            {
                PicoAgent_PushHistoryUser(app, content ? content : "");
            }
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
        if (model && model[0])
        {
            snprintf(app->settings.model, sizeof(app->settings.model), "%s", model);
            app->model_name = app->settings.model;
        }
        free(model);
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

void PicoSession_LogUser(PicoApp *app, const char *content)
{
    char *pre = EventPrefix("message");
    JsonBuf b;
    JsonBuf_Init(&b);
    JsonBuf_Puts(&b, pre);
    JsonBuf_Puts(&b, ",\"role\":\"user\",\"content\":");
    JsonBuf_String(&b, content ? content : "");
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

void PicoSession_LogModelChange(PicoApp *app, const char *model)
{
    char *pre = EventPrefix("model_change");
    JsonBuf b;
    JsonBuf_Init(&b);
    JsonBuf_Puts(&b, pre);
    JsonBuf_Puts(&b, ",\"model\":");
    JsonBuf_String(&b, model ? model : "");
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
