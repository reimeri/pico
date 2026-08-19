#define _POSIX_C_SOURCE 200809L

#include "agent.h"
#include "json.h"
#include "session.h"
#include "settings.h"

#include <curl/curl.h>
#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define PICO_MAX_PENDING_CALLS 16

typedef struct PicoInputItem {
    char *json;
} PicoInputItem;

typedef struct PicoPendingCall {
    char *call_id;
    char *name;
    char *arguments;
} PicoPendingCall;

typedef enum PicoAgentEvType {
    PICO_AEV_LLM_DONE = 0,
    PICO_AEV_LLM_FAIL,
    PICO_AEV_LLM_CANCEL,
    PICO_AEV_TOOL_DONE,
    PICO_AEV_TOOL_FAIL,
} PicoAgentEvType;

typedef struct PicoAgentEv {
    PicoAgentEvType type;
    char *text;
    char *payload;
    int tokens;
    int cached;
} PicoAgentEv;

typedef enum PicoWorkKind {
    PICO_WORK_IDLE = 0,
    PICO_WORK_LLM,
    PICO_WORK_TOOL,
} PicoWorkKind;

struct PicoAgentRt {
    PicoApp *app;
    pthread_t thread;
    pthread_mutex_t mu;
    pthread_cond_t cv;
    bool stop;
    bool started;
    bool busy;
    bool cancel;

    PicoWorkKind work;
    PicoProviderStreamFn work_stream;
    char *work_model;
    char *work_base_url;
    char *work_api_key;
    char *work_effort;
    char *work_instructions;
    char *work_cache_key;
    char **work_input;
    int work_input_count;
    bool work_compact;
    bool work_include_tools;
    char *work_tool_name;
    char *work_tool_args;
    char *work_call_id;
    PicoToolFn work_tool_fn;

    char *stream;
    size_t stream_len;
    size_t stream_cap;
    char *think;
    size_t think_len;
    size_t think_cap;
    char status[128];

    PicoAgentEv *events;
    int event_count;
    int event_cap;

    PicoInputItem *input;
    int input_count;
    int input_cap;
    char cache_key[33];
    char *instructions;
    char *turn_provider;
    bool compacting;
    bool compact_no_tools;

    PicoPendingCall pending[PICO_MAX_PENDING_CALLS];
    int pending_count;
    int pending_next;

    int stream_msg;
    bool stream_dirty;
};

static void SetErrorState(PicoApp *app, const char *msg);
static void FinishAssistantHistory(PicoApp *app);

static char *Dup(const char *s)
{
    return JsonDup(s ? s : "");
}

static void PushInput(PicoAgentRt *rt, char *json)
{
    if (!json)
    {
        return;
    }
    if (rt->input_count >= rt->input_cap)
    {
        int cap = rt->input_cap == 0 ? 8 : rt->input_cap * 2;
        PicoInputItem *next = (PicoInputItem *)realloc(rt->input, (size_t)cap * sizeof(PicoInputItem));
        if (!next)
        {
            free(json);
            return;
        }
        rt->input = next;
        rt->input_cap = cap;
    }
    rt->input[rt->input_count++].json = json;
}

static void ClearPending(PicoAgentRt *rt)
{
    for (int i = 0; i < rt->pending_count; i++)
    {
        free(rt->pending[i].call_id);
        free(rt->pending[i].name);
        free(rt->pending[i].arguments);
        memset(&rt->pending[i], 0, sizeof(rt->pending[i]));
    }
    rt->pending_count = 0;
    rt->pending_next = 0;
}

/* Ends the work item: `busy` has to clear in the same critical section that
 * publishes the event, or the main thread can pump the event and queue the next
 * request while this thread still looks busy. Exactly one call per work item. */
static void PostEvent(PicoAgentRt *rt, PicoAgentEvType type, char *text, char *payload, int tokens, int cached)
{
    pthread_mutex_lock(&rt->mu);
    if (rt->event_count >= rt->event_cap)
    {
        int cap = rt->event_cap == 0 ? 8 : rt->event_cap * 2;
        PicoAgentEv *next = (PicoAgentEv *)realloc(rt->events, (size_t)cap * sizeof(PicoAgentEv));
        if (!next)
        {
            rt->busy = false;
            pthread_cond_signal(&rt->cv);
            pthread_mutex_unlock(&rt->mu);
            free(text);
            free(payload);
            return;
        }
        rt->events = next;
        rt->event_cap = cap;
    }
    PicoAgentEv *ev = &rt->events[rt->event_count++];
    ev->type = type;
    ev->text = text;
    ev->payload = payload;
    ev->tokens = tokens;
    ev->cached = cached;
    rt->busy = false;
    pthread_cond_signal(&rt->cv);
    pthread_mutex_unlock(&rt->mu);
}

static bool CancelCb(void *user)
{
    PicoAgentRt *rt = (PicoAgentRt *)user;
    pthread_mutex_lock(&rt->mu);
    bool c = rt->cancel;
    pthread_mutex_unlock(&rt->mu);
    return c;
}

static void BufAppend(char **buf, size_t *len, size_t *cap, const char *s, size_t n)
{
    if (!s || n == 0)
    {
        return;
    }
    if (*len + n + 1 > *cap)
    {
        size_t next_cap = *cap ? *cap : 256;
        while (next_cap < *len + n + 1)
        {
            next_cap *= 2;
        }
        char *next = (char *)realloc(*buf, next_cap);
        if (!next)
        {
            return;
        }
        *buf = next;
        *cap = next_cap;
    }
    memcpy(*buf + *len, s, n);
    *len += n;
    (*buf)[*len] = '\0';
}

static void SetActivity(PicoApp *app, const char *msg)
{
    snprintf(app->agent_activity, sizeof(app->agent_activity), "%s", msg ? msg : "");
}

static char *EncodeResult(const PicoLlmResult *r)
{
    JsonBuf b;
    JsonBuf_Init(&b);
    JsonBuf_Puts(&b, "{\"assistant_text\":");
    JsonBuf_String(&b, r && r->assistant_text ? r->assistant_text : "");
    JsonBuf_Puts(&b, ",\"think_text\":");
    JsonBuf_String(&b, r && r->think_text ? r->think_text : "");
    JsonBuf_Puts(&b, ",\"calls\":[");
    int n = r ? r->call_count : 0;
    for (int i = 0; i < n; i++)
    {
        if (i)
        {
            JsonBuf_Putc(&b, ',');
        }
        JsonBuf_Puts(&b, "{\"call_id\":");
        JsonBuf_String(&b, r->calls[i].call_id ? r->calls[i].call_id : "");
        JsonBuf_Puts(&b, ",\"name\":");
        JsonBuf_String(&b, r->calls[i].name ? r->calls[i].name : "");
        JsonBuf_Puts(&b, ",\"arguments\":");
        JsonBuf_String(&b, r->calls[i].arguments ? r->calls[i].arguments : "{}");
        JsonBuf_Putc(&b, '}');
    }
    JsonBuf_Puts(&b, "],\"raw\":[");
    n = r ? r->raw_count : 0;
    for (int i = 0; i < n; i++)
    {
        if (i)
        {
            JsonBuf_Putc(&b, ',');
        }
        JsonBuf_Puts(&b, r->raw_items[i] && r->raw_items[i][0] ? r->raw_items[i] : "{}");
    }
    JsonBuf_Puts(&b, "]}");
    return JsonBuf_Steal(&b);
}

static int ResultCallCount(const char *payload)
{
    if (!payload)
    {
        return 0;
    }
    JsonDoc doc;
    if (JsonParse(&doc, payload, strlen(payload)) != 0)
    {
        return 0;
    }
    int arr = JsonObjGet(&doc, 0, "calls");
    int n = JsonIsArray(&doc, arr) ? JsonArrayLen(&doc, arr) : 0;
    JsonFree(&doc);
    return n;
}

static char *ResultStr(const char *payload, const char *key)
{
    if (!payload)
    {
        return NULL;
    }
    JsonDoc doc;
    if (JsonParse(&doc, payload, strlen(payload)) != 0)
    {
        return NULL;
    }
    char *s = JsonObjStr(&doc, 0, key);
    JsonFree(&doc);
    return s;
}

static void DeltaCb(void *user, PicoLlmDeltaKind kind, const char *s, size_t n)
{
    PicoAgentRt *rt = (PicoAgentRt *)user;
    if (!s || n == 0)
    {
        return;
    }
    pthread_mutex_lock(&rt->mu);
    if (kind == PICO_LLM_DELTA_THINKING)
    {
        BufAppend(&rt->think, &rt->think_len, &rt->think_cap, s, n);
    }
    else if (kind == PICO_LLM_DELTA_STATUS)
    {
        size_t copy = n < sizeof(rt->status) - 1 ? n : sizeof(rt->status) - 1;
        memcpy(rt->status, s, copy);
        rt->status[copy] = '\0';
    }
    else
    {
        BufAppend(&rt->stream, &rt->stream_len, &rt->stream_cap, s, n);
    }
    pthread_mutex_unlock(&rt->mu);
}

static void *WorkerMain(void *arg)
{
    PicoAgentRt *rt = (PicoAgentRt *)arg;
    for (;;)
    {
        pthread_mutex_lock(&rt->mu);
        while (!rt->stop && rt->work == PICO_WORK_IDLE)
        {
            pthread_cond_wait(&rt->cv, &rt->mu);
        }
        if (rt->stop)
        {
            pthread_mutex_unlock(&rt->mu);
            break;
        }
        PicoWorkKind kind = rt->work;
        PicoProviderStreamFn stream_fn = rt->work_stream;
        char *model = rt->work_model;
        char *base_url = rt->work_base_url;
        char *api_key = rt->work_api_key;
        char *effort = rt->work_effort;
        char *instructions = rt->work_instructions;
        char *cache_key = rt->work_cache_key;
        char **input = rt->work_input;
        int input_count = rt->work_input_count;
        bool compact = rt->work_compact;
        bool include_tools = rt->work_include_tools;
        char *tool_name = rt->work_tool_name;
        char *tool_args = rt->work_tool_args;
        char *call_id = rt->work_call_id;
        PicoToolFn tool_fn = rt->work_tool_fn;
        rt->work = PICO_WORK_IDLE;
        rt->work_stream = NULL;
        rt->work_model = NULL;
        rt->work_base_url = NULL;
        rt->work_api_key = NULL;
        rt->work_effort = NULL;
        rt->work_instructions = NULL;
        rt->work_cache_key = NULL;
        rt->work_input = NULL;
        rt->work_input_count = 0;
        rt->work_compact = false;
        rt->work_include_tools = false;
        rt->work_tool_name = NULL;
        rt->work_tool_args = NULL;
        rt->work_call_id = NULL;
        rt->work_tool_fn = NULL;
        pthread_mutex_unlock(&rt->mu);

        if (kind == PICO_WORK_LLM)
        {
            PicoLlmTurn turn;
            memset(&turn, 0, sizeof(turn));
            turn.model = model;
            turn.base_url = base_url;
            turn.api_key = api_key;
            turn.instructions = instructions;
            turn.cache_key = cache_key;
            turn.effort = effort;
            turn.compact = compact;
            turn.include_tools = include_tools;
            turn.input_json = (const char *const *)input;
            turn.input_count = input_count;
            /* Safe to read from this thread only because PicoPlugins_Reload
             * defers itself while the agent is busy. */
            turn.tools = rt->app->tools;
            turn.tool_count = include_tools ? rt->app->tool_count : 0;
            PicoLlmResult result;
            memset(&result, 0, sizeof(result));
            int rc = stream_fn ? stream_fn(rt->app, &turn, CancelCb, DeltaCb, rt, &result) : PICO_LLM_FAIL;
            if (rc == PICO_LLM_CANCEL)
            {
                PostEvent(rt, PICO_AEV_LLM_CANCEL, NULL, NULL, 0, 0);
                pico_llm_result_free(&result);
            }
            else if (rc != PICO_LLM_OK)
            {
                PostEvent(rt, PICO_AEV_LLM_FAIL, result.error ? result.error : Dup("LLM request failed"), NULL,
                          0, 0);
                result.error = NULL;
                pico_llm_result_free(&result);
            }
            else
            {
                char *payload = EncodeResult(&result);
                PostEvent(rt, PICO_AEV_LLM_DONE, NULL, payload, result.input_tokens, result.cached_tokens);
                pico_llm_result_free(&result);
            }
        }
        else if (kind == PICO_WORK_TOOL)
        {
            char *out = NULL;
            if (tool_fn)
            {
                tool_fn(rt->app, tool_args ? tool_args : "{}", &out);
            }
            else
            {
                JsonBuf b;
                JsonBuf_Init(&b);
                JsonBuf_Puts(&b, "unknown tool: ");
                JsonBuf_Puts(&b, tool_name ? tool_name : "?");
                out = JsonBuf_Steal(&b);
                PostEvent(rt, PICO_AEV_TOOL_FAIL, out, call_id, 0, 0);
                call_id = NULL;
                out = NULL;
            }
            if (out || call_id)
            {
                PostEvent(rt, PICO_AEV_TOOL_DONE, out ? out : Dup(""), call_id, 0, 0);
                call_id = NULL;
            }
        }

        free(model);
        free(base_url);
        free(api_key);
        free(effort);
        free(instructions);
        free(cache_key);
        for (int i = 0; i < input_count; i++)
        {
            free(input[i]);
        }
        free(input);
        free(tool_name);
        free(tool_args);
        free(call_id);
    }
    return NULL;
}

static bool QueueLlm(PicoApp *app, bool compact, bool include_tools)
{
    PicoAgentRt *rt = app->agent;
    PicoModel *m = PicoSettings_ActiveModel(app);
    if (!m || !m->provider[0])
    {
        SetErrorState(app, "Active model has no provider. Set `provider` on the model in settings.json.");
        return false;
    }
    const PicoProvider *p = pico_find_provider(app, m->provider);
    if (!p || !p->stream)
    {
        char buf[256];
        snprintf(buf, sizeof(buf), "No provider `%s` for model `%s`.", m->provider,
                 m->id[0] ? m->id : "?");
        SetErrorState(app, buf);
        return false;
    }

    char **input = NULL;
    int input_count = rt->input_count;
    if (input_count > 0)
    {
        input = (char **)calloc((size_t)input_count, sizeof(char *));
        if (!input)
        {
            SetErrorState(app, "out of memory");
            return false;
        }
        for (int i = 0; i < input_count; i++)
        {
            input[i] = Dup(rt->input[i].json);
        }
    }

    pthread_mutex_lock(&rt->mu);
    if (rt->busy || rt->stop)
    {
        pthread_mutex_unlock(&rt->mu);
        for (int i = 0; i < input_count; i++)
        {
            free(input[i]);
        }
        free(input);
        return false;
    }
    rt->work = PICO_WORK_LLM;
    rt->work_stream = p->stream;
    free(rt->turn_provider);
    rt->turn_provider = Dup(p->name);
    rt->work_model = Dup(m->id);
    rt->work_base_url = Dup(m->base_url);
    rt->work_api_key = Dup(app->settings.api_key);
    rt->work_effort = Dup(PicoSettings_ActiveEffort(app));
    rt->work_instructions = Dup(rt->instructions ? rt->instructions : "");
    rt->work_cache_key = Dup(rt->cache_key);
    rt->work_compact = compact;
    rt->work_include_tools = include_tools;
    rt->work_input = input;
    rt->work_input_count = input_count;
    rt->busy = true;
    rt->cancel = false;
    pthread_cond_signal(&rt->cv);
    pthread_mutex_unlock(&rt->mu);
    return true;
}

static bool QueueTool(PicoAgentRt *rt, const char *name, const char *args, const char *call_id, PicoToolFn fn)
{
    pthread_mutex_lock(&rt->mu);
    if (rt->busy || rt->stop)
    {
        pthread_mutex_unlock(&rt->mu);
        return false;
    }
    rt->work = PICO_WORK_TOOL;
    rt->work_tool_name = Dup(name);
    rt->work_tool_args = Dup(args ? args : "{}");
    rt->work_call_id = Dup(call_id);
    rt->work_tool_fn = fn;
    rt->busy = true;
    pthread_cond_signal(&rt->cv);
    pthread_mutex_unlock(&rt->mu);
    return true;
}

static PicoToolFn FindTool(PicoApp *app, const char *name)
{
    if (!name)
    {
        return NULL;
    }
    for (int i = 0; i < app->tool_count; i++)
    {
        if (app->tools[i].name && strcmp(app->tools[i].name, name) == 0)
        {
            return app->tools[i].run;
        }
    }
    return NULL;
}

static char *BuildUserItem(const char *text)
{
    JsonBuf b;
    JsonBuf_Init(&b);
    JsonBuf_Puts(&b, "{\"type\":\"user\",\"text\":");
    JsonBuf_String(&b, text ? text : "");
    JsonBuf_Putc(&b, '}');
    return JsonBuf_Steal(&b);
}

static char *BuildAssistantItem(const char *text)
{
    JsonBuf b;
    JsonBuf_Init(&b);
    JsonBuf_Puts(&b, "{\"type\":\"assistant\",\"text\":");
    JsonBuf_String(&b, text ? text : "");
    JsonBuf_Putc(&b, '}');
    return JsonBuf_Steal(&b);
}

static char *BuildToolCall(const char *call_id, const char *name, const char *args)
{
    JsonBuf b;
    JsonBuf_Init(&b);
    JsonBuf_Puts(&b, "{\"type\":\"tool_call\",\"call_id\":");
    JsonBuf_String(&b, call_id ? call_id : "");
    JsonBuf_Puts(&b, ",\"name\":");
    JsonBuf_String(&b, name ? name : "");
    JsonBuf_Puts(&b, ",\"arguments\":");
    JsonBuf_String(&b, args ? args : "{}");
    JsonBuf_Putc(&b, '}');
    return JsonBuf_Steal(&b);
}

static char *BuildToolResult(const char *call_id, const char *output)
{
    JsonBuf b;
    JsonBuf_Init(&b);
    JsonBuf_Puts(&b, "{\"type\":\"tool_result\",\"call_id\":");
    JsonBuf_String(&b, call_id ? call_id : "");
    JsonBuf_Puts(&b, ",\"output\":");
    JsonBuf_String(&b, output ? output : "");
    JsonBuf_Putc(&b, '}');
    return JsonBuf_Steal(&b);
}

static char *BuildRawItem(const char *provider, const char *json)
{
    JsonBuf b;
    JsonBuf_Init(&b);
    JsonBuf_Puts(&b, "{\"type\":\"raw\",\"provider\":");
    JsonBuf_String(&b, provider ? provider : "");
    JsonBuf_Puts(&b, ",\"json\":");
    JsonBuf_Puts(&b, json && json[0] ? json : "{}");
    JsonBuf_Putc(&b, '}');
    return JsonBuf_Steal(&b);
}

static void AppendMessageText(PicoApp *app, int idx, const char *s, size_t n)
{
    if (idx < 0 || idx >= app->message_count || !s || n == 0)
    {
        return;
    }
    PicoMessage *m = &app->messages[idx];
    size_t old = m->source ? strlen(m->source) : 0;
    char *next = (char *)realloc(m->source, old + n + 1);
    if (!next)
    {
        return;
    }
    memcpy(next + old, s, n);
    next[old + n] = '\0';
    m->source = next;
}

static void ReparseMessage(PicoApp *app, int idx)
{
    if (idx < 0 || idx >= app->message_count)
    {
        return;
    }
    PicoMessage *m = &app->messages[idx];
    MdDocument_Free(&m->doc);
    size_t len = m->source ? strlen(m->source) : 0;
    m->doc = MdDocument_ParseEx(m->source ? m->source : "", len, MD_PARSE_DEFAULT);
}

static void SetMessageText(PicoApp *app, int idx, const char *text)
{
    if (idx < 0 || idx >= app->message_count)
    {
        return;
    }
    PicoMessage *m = &app->messages[idx];
    free(m->source);
    m->source = Dup(text ? text : "");
    ReparseMessage(app, idx);
}

static void PopLastMessage(PicoApp *app)
{
    if (app->message_count <= 0)
    {
        return;
    }
    int i = --app->message_count;
    free(app->messages[i].source);
    for (int t = 0; t < app->messages[i].trace_count; t++)
    {
        free(app->messages[i].trace[t].text);
        free(app->messages[i].trace[t].tool_name);
        free(app->messages[i].trace[t].tool_args);
        free(app->messages[i].trace[t].tool_output);
    }
    free(app->messages[i].trace);
    MdDocument_Free(&app->messages[i].doc);
    memset(&app->messages[i], 0, sizeof(app->messages[i]));
}

static bool Blank(const char *s)
{
    if (!s)
    {
        return true;
    }
    while (*s == ' ' || *s == '\n' || *s == '\t' || *s == '\r')
    {
        s++;
    }
    return *s == '\0';
}

static bool MessageSourceEmpty(const PicoApp *app, int idx)
{
    if (idx < 0 || idx >= app->message_count)
    {
        return true;
    }
    return Blank(app->messages[idx].source);
}

static bool MessageEmpty(const PicoApp *app, int idx)
{
    if (idx < 0 || idx >= app->message_count)
    {
        return true;
    }
    if (!Blank(app->messages[idx].source))
    {
        return false;
    }
    for (int t = 0; t < app->messages[idx].trace_count; t++)
    {
        if (!Blank(app->messages[idx].trace[t].text) || !Blank(app->messages[idx].trace[t].tool_name))
        {
            return false;
        }
    }
    return true;
}

static bool HasThinkTrace(const PicoMessage *m)
{
    for (int t = 0; t < m->trace_count; t++)
    {
        if (!m->trace[t].is_tool && !Blank(m->trace[t].text))
        {
            return true;
        }
    }
    return false;
}

static PicoTraceLine *TracePush(PicoMessage *m, bool is_tool)
{
    PicoTraceLine *next =
        (PicoTraceLine *)realloc(m->trace, (size_t)(m->trace_count + 1) * sizeof(PicoTraceLine));
    if (!next)
    {
        return NULL;
    }
    m->trace = next;
    PicoTraceLine *line = &m->trace[m->trace_count++];
    memset(line, 0, sizeof(*line));
    line->is_tool = is_tool;
    return line;
}

static void TraceAppendThink(PicoApp *app, int idx, const char *s, size_t n)
{
    if (idx < 0 || idx >= app->message_count || !s || n == 0)
    {
        return;
    }
    PicoMessage *m = &app->messages[idx];
    PicoTraceLine *line = NULL;
    if (m->trace_count > 0 && !m->trace[m->trace_count - 1].is_tool)
    {
        line = &m->trace[m->trace_count - 1];
    }
    else
    {
        line = TracePush(m, false);
    }
    if (!line)
    {
        return;
    }
    size_t old = line->text ? strlen(line->text) : 0;
    char *next = (char *)realloc(line->text, old + n + 1);
    if (!next)
    {
        return;
    }
    memcpy(next + old, s, n);
    next[old + n] = '\0';
    line->text = next;
}

static void FlattenPut(JsonBuf *b, const char *s, size_t max)
{
    if (!s)
    {
        return;
    }
    for (; *s && b->len < max; s++)
    {
        char c = (*s == '\n' || *s == '\r' || *s == '\t') ? ' ' : *s;
        JsonBuf_Putc(b, c);
    }
}

static char *FormatToolProps(const char *args_json)
{
    JsonBuf b;
    JsonBuf_Init(&b);
    if (!args_json || !args_json[0])
    {
        return JsonBuf_Steal(&b);
    }
    JsonDoc doc;
    if (JsonParse(&doc, args_json, strlen(args_json)) != 0)
    {
        FlattenPut(&b, args_json, 240);
        return JsonBuf_Steal(&b);
    }
    if (JsonIsObject(&doc, 0))
    {
        int n = JsonObjLen(&doc, 0);
        for (int i = 0; i < n; i++)
        {
            int key_tok = -1;
            int val_tok = -1;
            if (!JsonObjPair(&doc, 0, i, &key_tok, &val_tok))
            {
                continue;
            }
            if (b.len)
            {
                JsonBuf_Puts(&b, "  ");
            }
            char *key = JsonStrDup(&doc, key_tok);
            FlattenPut(&b, key, 240);
            free(key);
            JsonBuf_Puts(&b, ": ");
            char *val = NULL;
            if (JsonIsObject(&doc, val_tok) || JsonIsArray(&doc, val_tok))
            {
                val = JsonRawDup(&doc, val_tok);
            }
            else
            {
                val = JsonStrDup(&doc, val_tok);
                if (!val)
                {
                    val = JsonRawDup(&doc, val_tok);
                }
            }
            FlattenPut(&b, val, 240);
            free(val);
            if (b.len > 240)
            {
                JsonBuf_Puts(&b, "...");
                break;
            }
        }
    }
    else
    {
        char *raw = JsonRawDup(&doc, 0);
        FlattenPut(&b, raw, 240);
        free(raw);
    }
    JsonFree(&doc);
    return JsonBuf_Steal(&b);
}

static void TraceAddTool(PicoApp *app, int idx, const char *name, const char *args_json)
{
    if (idx < 0 || idx >= app->message_count)
    {
        return;
    }
    PicoTraceLine *line = TracePush(&app->messages[idx], true);
    if (!line)
    {
        return;
    }
    line->tool_name = Dup(name && name[0] ? name : "tool");
    line->tool_args = FormatToolProps(args_json);
}

static void TraceSetLastToolOutput(PicoApp *app, int idx, const char *output)
{
    if (idx < 0 || idx >= app->message_count)
    {
        return;
    }
    PicoMessage *m = &app->messages[idx];
    for (int t = m->trace_count - 1; t >= 0; t--)
    {
        if (m->trace[t].is_tool)
        {
            free(m->trace[t].tool_output);
            m->trace[t].tool_output = Dup(output ? output : "");
            return;
        }
    }
}

static char *FormatToolLine(const char *name, const char *args_json)
{
    char *detail = NULL;
    if (args_json && args_json[0])
    {
        JsonDoc doc;
        if (JsonParse(&doc, args_json, strlen(args_json)) == 0)
        {
            detail = JsonObjStr(&doc, 0, "command");
            JsonFree(&doc);
        }
    }
    JsonBuf b;
    JsonBuf_Init(&b);
    JsonBuf_Puts(&b, "Running ");
    JsonBuf_Puts(&b, name && name[0] ? name : "tool");
    if (detail && detail[0])
    {
        JsonBuf_Puts(&b, ": ");
        for (const char *p = detail; *p; p++)
        {
            char c = (*p == '\n' || *p == '\r' || *p == '\t') ? ' ' : *p;
            JsonBuf_Putc(&b, c);
            if (b.len > 200)
            {
                JsonBuf_Puts(&b, "...");
                break;
            }
        }
    }
    free(detail);
    return JsonBuf_Steal(&b);
}

static void PushFunctionOutput(PicoAgentRt *rt, const char *call_id, const char *output)
{
    PushInput(rt, BuildToolResult(call_id, output));
}

static void AbortRemainingCalls(PicoAgentRt *rt)
{
    for (int i = rt->pending_next; i < rt->pending_count; i++)
    {
        PushFunctionOutput(rt, rt->pending[i].call_id, "(interrupted)");
        PicoSession_LogToolResult(rt->app, rt->pending[i].call_id, "(interrupted)", true);
    }
    ClearPending(rt);
}

static void GoIdle(PicoApp *app)
{
    PicoAgentRt *rt = app->agent;
    app->agent_state = PICO_AGENT_IDLE;
    rt->stream_msg = -1;
    rt->stream_dirty = false;
    rt->compacting = false;
    rt->compact_no_tools = false;
    app->agent_activity[0] = '\0';
}

static void ApplyCancel(PicoApp *app)
{
    PicoAgentRt *rt = app->agent;
    if (rt->compacting)
    {
        GoIdle(app);
        return;
    }
    FinishAssistantHistory(app);
    if (rt->stream_msg >= 0 && !MessageSourceEmpty(app, rt->stream_msg))
    {
        PicoSession_LogAssistant(app, app->messages[rt->stream_msg].source, 0, 0);
    }
    if (rt->stream_msg >= 0 && MessageEmpty(app, rt->stream_msg))
    {
        PopLastMessage(app);
    }
    AbortRemainingCalls(rt);
    GoIdle(app);
}

static int CompactThreshold(const PicoApp *app)
{
    if (!app->settings.compact_enabled || app->tokens_limit <= 0)
    {
        return 0;
    }
    int t = (int)((double)app->tokens_limit * app->settings.compact_ratio);
    return t > 0 ? t : 0;
}

static void ApplyCompaction(PicoApp *app, const char *summary)
{
    int before = app->tokens_used;
    PicoAgent_ClearInput(app);
    JsonBuf b;
    JsonBuf_Init(&b);
    JsonBuf_Puts(&b, "Briefing:\n");
    JsonBuf_Puts(&b, summary ? summary : "");
    PicoAgent_PushHistoryUser(app, b.data ? b.data : "Briefing:\n");
    JsonBuf_Free(&b);
    PicoAgent_RotateCacheKey(app);
    PicoSession_LogCompaction(app, summary, before);
    app->tokens_used = 0;
    app->tokens_cached = 0;
    free(app->compact_summary);
    app->compact_summary = NULL;
}

static void StartCompact(PicoApp *app)
{
    PicoAgentRt *rt = app->agent;
    rt->stream_msg = -1;
    rt->stream_dirty = false;
    app->agent_state = PICO_AGENT_COMPACT_WAIT;
    SetActivity(app, "Compacting…");
    free(app->compact_summary);
    app->compact_summary = NULL;
    pico_run_hooks(app, PICO_HOOK_ON_COMPACT);
    if (app->compact_summary && app->compact_summary[0])
    {
        ApplyCompaction(app, app->compact_summary);
        GoIdle(app);
        return;
    }
    rt->compacting = true;
    rt->compact_no_tools = false;
    if (!QueueLlm(app, true, true))
    {
        SetErrorState(app, "Failed to start compaction");
    }
}

static void FinishTurn(PicoApp *app)
{
    PicoAgentRt *rt = app->agent;
    if (rt->stream_msg >= 0 && MessageEmpty(app, rt->stream_msg))
    {
        PopLastMessage(app);
    }
    if (CompactThreshold(app) > 0 && app->tokens_used >= CompactThreshold(app))
    {
        StartCompact(app);
        return;
    }
    GoIdle(app);
}

static void SetErrorState(PicoApp *app, const char *msg)
{
    PicoAgentRt *rt = app->agent;
    free(app->agent_error);
    app->agent_error = Dup(msg ? msg : "agent error");
    app->agent_state = PICO_AGENT_ERROR;
    if (rt->stream_msg >= 0 && MessageEmpty(app, rt->stream_msg))
    {
        SetMessageText(app, rt->stream_msg, app->agent_error);
    }
    rt->stream_msg = -1;
    rt->stream_dirty = false;
    ClearPending(rt);
    rt->compacting = false;
    rt->compact_no_tools = false;
    app->agent_activity[0] = '\0';
}

static void StartLlm(PicoApp *app);

static void StartNextTool(PicoApp *app)
{
    PicoAgentRt *rt = app->agent;
    if (rt->pending_next >= rt->pending_count)
    {
        ClearPending(rt);
        StartLlm(app);
        return;
    }
    PicoPendingCall *call = &rt->pending[rt->pending_next];
    app->agent_state = PICO_AGENT_TOOL_WAIT;
    char *line = FormatToolLine(call->name, call->arguments);
    SetActivity(app, line);
    if (rt->stream_msg >= 0)
    {
        TraceAddTool(app, rt->stream_msg, call->name, call->arguments);
        app->chat_follow_bottom = true;
    }
    if (!QueueTool(rt, call->name, call->arguments, call->call_id, FindTool(app, call->name)))
    {
        SetErrorState(app, "Failed to start tool");
    }
    free(line);
}

static void StartLlm(PicoApp *app)
{
    PicoAgentRt *rt = app->agent;
    int last = app->message_count - 1;
    if (last >= 0 && app->messages[last].role == PICO_ROLE_ASSISTANT && MessageSourceEmpty(app, last))
    {
        rt->stream_msg = last;
    }
    else
    {
        PicoApp_AddMessage(app, PICO_ROLE_ASSISTANT, "");
        rt->stream_msg = app->message_count - 1;
    }
    rt->stream_dirty = false;
    app->agent_state = PICO_AGENT_LLM_WAIT;
    SetActivity(app, "Thinking…");
    free(app->agent_error);
    app->agent_error = NULL;
    if (!QueueLlm(app, false, true))
    {
        SetErrorState(app, "Failed to start model request");
    }
}

static void FinishAssistantHistory(PicoApp *app)
{
    PicoAgentRt *rt = app->agent;
    if (rt->stream_msg >= 0 && !MessageSourceEmpty(app, rt->stream_msg))
    {
        PushInput(rt, BuildAssistantItem(app->messages[rt->stream_msg].source));
    }
}

static void IngestResult(PicoApp *app, const char *payload)
{
    PicoAgentRt *rt = app->agent;
    if (!payload)
    {
        FinishAssistantHistory(app);
        return;
    }
    JsonDoc doc;
    if (JsonParse(&doc, payload, strlen(payload)) != 0)
    {
        FinishAssistantHistory(app);
        return;
    }

    char *assistant = JsonObjStr(&doc, 0, "assistant_text");
    if (rt->stream_msg >= 0 && MessageSourceEmpty(app, rt->stream_msg) && assistant && assistant[0])
    {
        SetMessageText(app, rt->stream_msg, assistant);
    }

    char *think = JsonObjStr(&doc, 0, "think_text");
    if (think && think[0] && rt->stream_msg >= 0 && !HasThinkTrace(&app->messages[rt->stream_msg]))
    {
        TraceAppendThink(app, rt->stream_msg, think, strlen(think));
    }

    const char *prov = rt->turn_provider ? rt->turn_provider : "";
    int raw = JsonObjGet(&doc, 0, "raw");
    if (JsonIsArray(&doc, raw))
    {
        int n = JsonArrayLen(&doc, raw);
        for (int i = 0; i < n; i++)
        {
            char *item = JsonRawDup(&doc, JsonArrayAt(&doc, raw, i));
            PushInput(rt, BuildRawItem(prov, item));
            free(item);
        }
    }

    FinishAssistantHistory(app);

    int calls = JsonObjGet(&doc, 0, "calls");
    int n = JsonIsArray(&doc, calls) ? JsonArrayLen(&doc, calls) : 0;
    for (int i = 0; i < n && rt->pending_count < PICO_MAX_PENDING_CALLS; i++)
    {
        int item = JsonArrayAt(&doc, calls, i);
        PicoPendingCall *call = &rt->pending[rt->pending_count++];
        call->call_id = JsonObjStr(&doc, item, "call_id");
        call->name = JsonObjStr(&doc, item, "name");
        call->arguments = JsonObjStr(&doc, item, "arguments");
        PushInput(rt, BuildToolCall(call->call_id, call->name, call->arguments));
    }

    free(assistant);
    free(think);
    JsonFree(&doc);
}

static void OnLlmDone(PicoApp *app, PicoAgentEv *ev)
{
    PicoAgentRt *rt = app->agent;
    if (ev->tokens > 0)
    {
        app->tokens_used = ev->tokens;
        app->tokens_cached = ev->cached;
    }
    if (rt->compacting)
    {
        if (ResultCallCount(ev->payload) > 0 && !rt->compact_no_tools)
        {
            rt->compact_no_tools = true;
            if (!QueueLlm(app, true, false))
            {
                SetErrorState(app, "Failed to start compaction");
            }
            return;
        }
        char *text = ResultStr(ev->payload, "assistant_text");
        if ((!text || !text[0]) && rt->stream_msg >= 0 && !MessageSourceEmpty(app, rt->stream_msg))
        {
            free(text);
            text = Dup(app->messages[rt->stream_msg].source);
        }
        if (text && text[0])
        {
            ApplyCompaction(app, text);
            free(text);
            GoIdle(app);
            return;
        }
        free(text);
        rt->compacting = false;
        SetErrorState(app, "Compaction failed");
        return;
    }
    IngestResult(app, ev->payload);
    if (rt->stream_msg >= 0 && !MessageSourceEmpty(app, rt->stream_msg))
    {
        PicoSession_LogAssistant(app, app->messages[rt->stream_msg].source, ev->tokens, ev->cached);
    }
    for (int i = 0; i < rt->pending_count; i++)
    {
        PicoSession_LogToolCall(app, rt->pending[i].call_id, rt->pending[i].name, rt->pending[i].arguments);
    }
    if (rt->pending_count > 0)
    {
        rt->pending_next = 0;
        StartNextTool(app);
        return;
    }
    FinishTurn(app);
}

static void OnToolDone(PicoApp *app, PicoAgentEv *ev, bool failed)
{
    PicoAgentRt *rt = app->agent;
    const char *call_id = ev->payload;
    if (rt->pending_next < rt->pending_count && rt->pending[rt->pending_next].call_id)
    {
        if (!call_id)
        {
            call_id = rt->pending[rt->pending_next].call_id;
        }
    }
    const char *output = ev->text ? ev->text : (failed ? "tool failed" : "");
    PushFunctionOutput(rt, call_id, output);
    PicoSession_LogToolResult(app, call_id, output, failed);
    if (rt->stream_msg >= 0)
    {
        TraceSetLastToolOutput(app, rt->stream_msg, output);
    }
    rt->pending_next++;
    bool cancel;
    pthread_mutex_lock(&rt->mu);
    cancel = rt->cancel;
    pthread_mutex_unlock(&rt->mu);
    if (cancel)
    {
        AbortRemainingCalls(rt);
        GoIdle(app);
        return;
    }
    StartNextTool(app);
}

bool PicoAgent_BlocksReload(const PicoApp *app)
{
    return app->agent_state == PICO_AGENT_LLM_WAIT || app->agent_state == PICO_AGENT_TOOL_WAIT ||
           app->agent_state == PICO_AGENT_COMPACT_WAIT;
}

void PicoAgent_Compact(PicoApp *app)
{
    if (!app || !app->agent || PicoAgent_BlocksReload(app))
    {
        return;
    }
    StartCompact(app);
}

void PicoAgent_Init(PicoApp *app)
{
    PicoAgentRt *rt = (PicoAgentRt *)calloc(1, sizeof(PicoAgentRt));
    app->agent = rt;
    if (!rt)
    {
        return;
    }
    rt->app = app;
    rt->stream_msg = -1;
    Pico_RandomHex(rt->cache_key, sizeof(rt->cache_key));
    pthread_mutex_init(&rt->mu, NULL);
    pthread_cond_init(&rt->cv, NULL);
    curl_global_init(CURL_GLOBAL_DEFAULT);
    if (pthread_create(&rt->thread, NULL, WorkerMain, rt) == 0)
    {
        rt->started = true;
    }
}

void PicoAgent_Shutdown(PicoApp *app)
{
    PicoAgentRt *rt = app->agent;
    if (!rt)
    {
        return;
    }
    pthread_mutex_lock(&rt->mu);
    rt->stop = true;
    rt->cancel = true;
    pthread_cond_signal(&rt->cv);
    struct timespec until;
    clock_gettime(CLOCK_REALTIME, &until);
    until.tv_sec += 1;
    while (rt->busy)
    {
        if (pthread_cond_timedwait(&rt->cv, &rt->mu, &until) == ETIMEDOUT)
        {
            break;
        }
    }
    bool done = !rt->busy;
    pthread_mutex_unlock(&rt->mu);
    if (!done)
    {
        if (rt->started)
        {
            pthread_detach(rt->thread);
        }
        app->agent = NULL;
        return;
    }
    if (rt->started)
    {
        pthread_join(rt->thread, NULL);
    }
    for (int i = 0; i < rt->event_count; i++)
    {
        free(rt->events[i].text);
        free(rt->events[i].payload);
    }
    free(rt->events);
    for (int i = 0; i < rt->input_count; i++)
    {
        free(rt->input[i].json);
    }
    free(rt->input);
    ClearPending(rt);
    free(rt->stream);
    free(rt->think);
    free(rt->work_model);
    free(rt->work_base_url);
    free(rt->work_api_key);
    free(rt->work_effort);
    free(rt->work_instructions);
    free(rt->work_cache_key);
    for (int i = 0; i < rt->work_input_count; i++)
    {
        free(rt->work_input[i]);
    }
    free(rt->work_input);
    free(rt->work_tool_name);
    free(rt->work_tool_args);
    free(rt->work_call_id);
    free(rt->instructions);
    free(rt->turn_provider);
    pthread_mutex_destroy(&rt->mu);
    pthread_cond_destroy(&rt->cv);
    curl_global_cleanup();
    free(rt);
    app->agent = NULL;
}

void PicoAgent_StartTurn(PicoApp *app, const char *user_text)
{
    if (!app->agent || !user_text || !user_text[0])
    {
        return;
    }
    if (PicoAgent_BlocksReload(app))
    {
        return;
    }
    PicoSettings_Load(app);
    PicoAgent_DismissError(app);
    free(app->agent->instructions);
    app->agent->instructions = PicoSettings_LoadSystemPrompt(app);
    PushInput(app->agent, BuildUserItem(user_text));
    StartLlm(app);
}

void PicoAgent_Cancel(PicoApp *app)
{
    PicoAgentRt *rt = app->agent;
    if (!rt || !PicoAgent_BlocksReload(app))
    {
        return;
    }
    pthread_mutex_lock(&rt->mu);
    rt->cancel = true;
    pthread_mutex_unlock(&rt->mu);
}

void PicoAgent_DismissError(PicoApp *app)
{
    if (app->agent_state == PICO_AGENT_ERROR)
    {
        app->agent_state = PICO_AGENT_IDLE;
    }
    free(app->agent_error);
    app->agent_error = NULL;
}

void PicoAgent_Pump(PicoApp *app)
{
    PicoAgentRt *rt = app->agent;
    if (!rt)
    {
        return;
    }

    pthread_mutex_lock(&rt->mu);
    char *stream = rt->stream;
    size_t stream_len = rt->stream_len;
    rt->stream = NULL;
    rt->stream_len = 0;
    rt->stream_cap = 0;
    char *think = rt->think;
    size_t think_len = rt->think_len;
    rt->think = NULL;
    rt->think_len = 0;
    rt->think_cap = 0;
    char status[128];
    memcpy(status, rt->status, sizeof(status));
    rt->status[0] = '\0';
    PicoAgentEv *events = rt->events;
    int event_count = rt->event_count;
    rt->events = NULL;
    rt->event_count = 0;
    rt->event_cap = 0;
    pthread_mutex_unlock(&rt->mu);

    if (stream && stream_len && rt->stream_msg >= 0)
    {
        AppendMessageText(app, rt->stream_msg, stream, stream_len);
        rt->stream_dirty = true;
        app->chat_follow_bottom = true;
    }
    free(stream);

    if (think && think_len && rt->stream_msg >= 0)
    {
        TraceAppendThink(app, rt->stream_msg, think, think_len);
        app->chat_follow_bottom = true;
    }
    free(think);

    if (status[0])
    {
        char line[192];
        snprintf(line, sizeof(line), "Calling `%s`…", status);
        SetActivity(app, line);
        app->chat_follow_bottom = true;
    }

    if (rt->stream_dirty)
    {
        ReparseMessage(app, rt->stream_msg);
        rt->stream_dirty = false;
    }

    for (int i = 0; i < event_count; i++)
    {
        PicoAgentEv *ev = &events[i];
        switch (ev->type)
        {
        case PICO_AEV_LLM_DONE:
            OnLlmDone(app, ev);
            break;
        case PICO_AEV_LLM_FAIL:
            SetErrorState(app, ev->text ? ev->text : "LLM request failed");
            break;
        case PICO_AEV_LLM_CANCEL:
            ApplyCancel(app);
            break;
        case PICO_AEV_TOOL_DONE:
            OnToolDone(app, ev, false);
            break;
        case PICO_AEV_TOOL_FAIL:
            OnToolDone(app, ev, true);
            break;
        }
        free(ev->text);
        free(ev->payload);
    }
    free(events);
}

const char *PicoAgent_CacheKey(const PicoApp *app)
{
    return app && app->agent ? app->agent->cache_key : "";
}

void PicoAgent_SetCacheKey(PicoApp *app, const char *key)
{
    if (!app || !app->agent || !key)
    {
        return;
    }
    snprintf(app->agent->cache_key, sizeof(app->agent->cache_key), "%s", key);
}

void PicoAgent_RotateCacheKey(PicoApp *app)
{
    if (!app || !app->agent)
    {
        return;
    }
    Pico_RandomHex(app->agent->cache_key, sizeof(app->agent->cache_key));
}

void PicoAgent_ClearInput(PicoApp *app)
{
    PicoAgentRt *rt = app ? app->agent : NULL;
    if (!rt)
    {
        return;
    }
    for (int i = 0; i < rt->input_count; i++)
    {
        free(rt->input[i].json);
        rt->input[i].json = NULL;
    }
    rt->input_count = 0;
}

void PicoAgent_PushHistoryUser(PicoApp *app, const char *text)
{
    if (!app || !app->agent)
    {
        return;
    }
    PushInput(app->agent, BuildUserItem(text));
}

void PicoAgent_PushHistoryAssistant(PicoApp *app, const char *text)
{
    if (!app || !app->agent)
    {
        return;
    }
    PushInput(app->agent, BuildAssistantItem(text));
}

void PicoAgent_PushHistoryFunctionCall(PicoApp *app, const char *call_id, const char *name, const char *args)
{
    if (!app || !app->agent)
    {
        return;
    }
    PushInput(app->agent, BuildToolCall(call_id, name, args));
}

void PicoAgent_PushHistoryFunctionOutput(PicoApp *app, const char *call_id, const char *output)
{
    if (!app || !app->agent)
    {
        return;
    }
    PushFunctionOutput(app->agent, call_id, output);
}
