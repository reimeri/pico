#define _POSIX_C_SOURCE 200809L

#include "agent.h"
#include "json.h"
#include "llm.h"
#include "settings.h"

#include <curl/curl.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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
    char *work_url;
    char *work_key;
    char *work_body;
    char *work_tool_name;
    char *work_tool_args;
    char *work_call_id;
    PicoToolFn work_tool_fn;

    char *stream;
    size_t stream_len;
    size_t stream_cap;

    PicoAgentEv *events;
    int event_count;
    int event_cap;

    PicoInputItem *input;
    int input_count;
    int input_cap;

    PicoPendingCall pending[PICO_MAX_PENDING_CALLS];
    int pending_count;
    int pending_next;

    int stream_msg;
    bool stream_dirty;
};

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

static void PostEvent(PicoAgentRt *rt, PicoAgentEvType type, char *text, char *payload, int tokens)
{
    pthread_mutex_lock(&rt->mu);
    if (rt->event_count >= rt->event_cap)
    {
        int cap = rt->event_cap == 0 ? 8 : rt->event_cap * 2;
        PicoAgentEv *next = (PicoAgentEv *)realloc(rt->events, (size_t)cap * sizeof(PicoAgentEv));
        if (!next)
        {
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

static void DeltaCb(void *user, const char *s, size_t n)
{
    PicoAgentRt *rt = (PicoAgentRt *)user;
    if (!s || n == 0)
    {
        return;
    }
    pthread_mutex_lock(&rt->mu);
    if (rt->stream_len + n + 1 > rt->stream_cap)
    {
        size_t cap = rt->stream_cap ? rt->stream_cap : 256;
        while (cap < rt->stream_len + n + 1)
        {
            cap *= 2;
        }
        char *next = (char *)realloc(rt->stream, cap);
        if (!next)
        {
            pthread_mutex_unlock(&rt->mu);
            return;
        }
        rt->stream = next;
        rt->stream_cap = cap;
    }
    memcpy(rt->stream + rt->stream_len, s, n);
    rt->stream_len += n;
    rt->stream[rt->stream_len] = '\0';
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
        char *url = rt->work_url;
        char *key = rt->work_key;
        char *body = rt->work_body;
        char *tool_name = rt->work_tool_name;
        char *tool_args = rt->work_tool_args;
        char *call_id = rt->work_call_id;
        PicoToolFn tool_fn = rt->work_tool_fn;
        rt->work = PICO_WORK_IDLE;
        rt->work_url = NULL;
        rt->work_key = NULL;
        rt->work_body = NULL;
        rt->work_tool_name = NULL;
        rt->work_tool_args = NULL;
        rt->work_call_id = NULL;
        rt->work_tool_fn = NULL;
        pthread_mutex_unlock(&rt->mu);

        if (kind == PICO_WORK_LLM)
        {
            char *items = NULL;
            char *err = NULL;
            int tokens = 0;
            int rc = PicoLlm_Stream(url, key, body, CancelCb, DeltaCb, rt, &items, &tokens, &err);
            if (rc == PICO_LLM_CANCEL)
            {
                PostEvent(rt, PICO_AEV_LLM_CANCEL, NULL, NULL, 0);
                free(items);
                free(err);
            }
            else if (rc != PICO_LLM_OK)
            {
                PostEvent(rt, PICO_AEV_LLM_FAIL, err, NULL, 0);
                free(items);
            }
            else
            {
                PostEvent(rt, PICO_AEV_LLM_DONE, NULL, items, tokens);
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
                PostEvent(rt, PICO_AEV_TOOL_FAIL, out, call_id, 0);
                call_id = NULL;
                out = NULL;
            }
            if (out || call_id)
            {
                PostEvent(rt, PICO_AEV_TOOL_DONE, out ? out : Dup(""), call_id, 0);
                call_id = NULL;
            }
        }

        free(url);
        free(key);
        free(body);
        free(tool_name);
        free(tool_args);
        free(call_id);

        pthread_mutex_lock(&rt->mu);
        rt->busy = false;
        pthread_cond_signal(&rt->cv);
        pthread_mutex_unlock(&rt->mu);
    }
    return NULL;
}

static void QueueLlm(PicoAgentRt *rt, const char *url, const char *key, char *body)
{
    pthread_mutex_lock(&rt->mu);
    while (rt->busy && !rt->stop)
    {
        pthread_cond_wait(&rt->cv, &rt->mu);
    }
    rt->work = PICO_WORK_LLM;
    rt->work_url = Dup(url);
    rt->work_key = Dup(key ? key : "");
    rt->work_body = body;
    rt->busy = true;
    rt->cancel = false;
    pthread_cond_signal(&rt->cv);
    pthread_mutex_unlock(&rt->mu);
}

static void QueueTool(PicoAgentRt *rt, const char *name, const char *args, const char *call_id, PicoToolFn fn)
{
    pthread_mutex_lock(&rt->mu);
    while (rt->busy && !rt->stop)
    {
        pthread_cond_wait(&rt->cv, &rt->mu);
    }
    rt->work = PICO_WORK_TOOL;
    rt->work_tool_name = Dup(name);
    rt->work_tool_args = Dup(args ? args : "{}");
    rt->work_call_id = Dup(call_id);
    rt->work_tool_fn = fn;
    rt->busy = true;
    pthread_cond_signal(&rt->cv);
    pthread_mutex_unlock(&rt->mu);
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

static char *BuildRequest(PicoApp *app)
{
    PicoAgentRt *rt = app->agent;
    char *instructions = PicoSettings_LoadSystemPrompt(app);
    JsonBuf b;
    JsonBuf_Init(&b);
    JsonBuf_Puts(&b, "{\"model\":");
    JsonBuf_String(&b, app->settings.model);
    JsonBuf_Puts(&b, ",\"stream\":true,\"instructions\":");
    JsonBuf_String(&b, instructions ? instructions : "");
    JsonBuf_Puts(&b, ",\"input\":[");
    for (int i = 0; i < rt->input_count; i++)
    {
        if (i)
        {
            JsonBuf_Putc(&b, ',');
        }
        JsonBuf_Puts(&b, rt->input[i].json);
    }
    JsonBuf_Puts(&b, "]");
    if (app->tool_count > 0)
    {
        JsonBuf_Puts(&b, ",\"tools\":[");
        for (int i = 0; i < app->tool_count; i++)
        {
            if (i)
            {
                JsonBuf_Putc(&b, ',');
            }
            JsonBuf_Puts(&b, "{\"type\":\"function\",\"name\":");
            JsonBuf_String(&b, app->tools[i].name ? app->tools[i].name : "");
            JsonBuf_Puts(&b, ",\"description\":");
            JsonBuf_String(&b, app->tools[i].description ? app->tools[i].description : "");
            JsonBuf_Puts(&b, ",\"parameters\":");
            JsonBuf_Puts(&b, app->tools[i].params_json && app->tools[i].params_json[0]
                                 ? app->tools[i].params_json
                                 : "{\"type\":\"object\",\"properties\":{}}");
            JsonBuf_Putc(&b, '}');
        }
        JsonBuf_Putc(&b, ']');
    }
    JsonBuf_Putc(&b, '}');
    free(instructions);
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
    MdDocument_Free(&app->messages[i].doc);
    memset(&app->messages[i], 0, sizeof(app->messages[i]));
}

static bool MessageEmpty(const PicoApp *app, int idx)
{
    if (idx < 0 || idx >= app->message_count || !app->messages[idx].source)
    {
        return true;
    }
    const char *s = app->messages[idx].source;
    while (*s == ' ' || *s == '\n' || *s == '\t' || *s == '\r')
    {
        s++;
    }
    return *s == '\0';
}

static char *ContentText(const JsonDoc *doc, int content)
{
    if (JsonIsArray(doc, content))
    {
        JsonBuf b;
        JsonBuf_Init(&b);
        int n = JsonArrayLen(doc, content);
        for (int i = 0; i < n; i++)
        {
            int part = JsonArrayAt(doc, content, i);
            char *text = JsonObjStr(doc, part, "text");
            if (text)
            {
                JsonBuf_Puts(&b, text);
                free(text);
            }
        }
        return JsonBuf_Steal(&b);
    }
    return JsonStrDup(doc, content);
}

static char *MessageTextFromItems(const char *items_json)
{
    if (!items_json)
    {
        return NULL;
    }
    JsonDoc doc;
    if (JsonParse(&doc, items_json, strlen(items_json)) != 0)
    {
        return NULL;
    }
    JsonBuf b;
    JsonBuf_Init(&b);
    int n = JsonArrayLen(&doc, 0);
    for (int i = 0; i < n; i++)
    {
        int item = JsonArrayAt(&doc, 0, i);
        if (!JsonEq(&doc, JsonObjGet(&doc, item, "type"), "message"))
        {
            continue;
        }
        char *text = ContentText(&doc, JsonObjGet(&doc, item, "content"));
        if (text && text[0])
        {
            JsonBuf_Puts(&b, text);
        }
        free(text);
    }
    JsonFree(&doc);
    if (!b.len)
    {
        JsonBuf_Free(&b);
        return NULL;
    }
    return JsonBuf_Steal(&b);
}

static void PushFunctionOutput(PicoAgentRt *rt, const char *call_id, const char *output)
{
    JsonBuf b;
    JsonBuf_Init(&b);
    JsonBuf_Puts(&b, "{\"type\":\"function_call_output\",\"call_id\":");
    JsonBuf_String(&b, call_id ? call_id : "");
    JsonBuf_Puts(&b, ",\"output\":");
    JsonBuf_String(&b, output ? output : "");
    JsonBuf_Putc(&b, '}');
    PushInput(rt, JsonBuf_Steal(&b));
}

static void AbortRemainingCalls(PicoAgentRt *rt)
{
    for (int i = rt->pending_next; i < rt->pending_count; i++)
    {
        PushFunctionOutput(rt, rt->pending[i].call_id, "(interrupted)");
    }
    ClearPending(rt);
}

static void GoIdle(PicoApp *app)
{
    PicoAgentRt *rt = app->agent;
    app->agent_state = PICO_AGENT_IDLE;
    rt->stream_msg = -1;
    rt->stream_dirty = false;
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
}

static char *BuildMessageItem(const char *role, const char *text, bool assistant)
{
    JsonBuf b;
    JsonBuf_Init(&b);
    JsonBuf_Puts(&b, "{\"type\":\"message\",\"role\":");
    JsonBuf_String(&b, role ? role : "user");
    JsonBuf_Puts(&b, ",\"content\":[{\"type\":");
    JsonBuf_String(&b, assistant ? "output_text" : "input_text");
    JsonBuf_Puts(&b, ",\"text\":");
    JsonBuf_String(&b, text ? text : "");
    JsonBuf_Puts(&b, "}]}");
    return JsonBuf_Steal(&b);
}

static char *BuildUserItem(const char *text)
{
    return BuildMessageItem("user", text, false);
}

static char *BuildAssistantItem(const char *text)
{
    return BuildMessageItem("assistant", text, true);
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
    QueueTool(rt, call->name, call->arguments, call->call_id, FindTool(app, call->name));
}

static void StartLlm(PicoApp *app)
{
    PicoAgentRt *rt = app->agent;
    PicoApp_AddMessage(app, PICO_ROLE_ASSISTANT, "");
    rt->stream_msg = app->message_count - 1;
    rt->stream_dirty = false;
    app->agent_state = PICO_AGENT_LLM_WAIT;
    free(app->agent_error);
    app->agent_error = NULL;

    char url[1024];
    PicoLlm_ResolveUrl(app->settings.base_url, url, sizeof(url));
    QueueLlm(rt, url, app->settings.api_key, BuildRequest(app));
}

static bool ItemIsNonEmptyMessage(const JsonDoc *doc, int item)
{
    if (!JsonEq(doc, JsonObjGet(doc, item, "type"), "message"))
    {
        return false;
    }
    char *text = ContentText(doc, JsonObjGet(doc, item, "content"));
    bool ok = text && text[0];
    free(text);
    return ok;
}

static void IngestOutputItems(PicoApp *app, const char *items_json)
{
    PicoAgentRt *rt = app->agent;
    if (!items_json)
    {
        return;
    }
    JsonDoc doc;
    if (JsonParse(&doc, items_json, strlen(items_json)) != 0)
    {
        return;
    }
    int n = JsonArrayLen(&doc, 0);
    for (int i = 0; i < n; i++)
    {
        int item = JsonArrayAt(&doc, 0, i);
        if (JsonEq(&doc, JsonObjGet(&doc, item, "type"), "message") && !ItemIsNonEmptyMessage(&doc, item))
        {
            continue;
        }
        char *raw = JsonRawDup(&doc, item);
        PushInput(rt, raw);
        if (!JsonEq(&doc, JsonObjGet(&doc, item, "type"), "function_call"))
        {
            continue;
        }
        if (rt->pending_count >= PICO_MAX_PENDING_CALLS)
        {
            continue;
        }
        PicoPendingCall *call = &rt->pending[rt->pending_count++];
        call->call_id = JsonObjStr(&doc, item, "call_id");
        call->name = JsonObjStr(&doc, item, "name");
        call->arguments = JsonObjStr(&doc, item, "arguments");
    }
    JsonFree(&doc);
}

static void FinishAssistantHistory(PicoApp *app, const char *items_json)
{
    PicoAgentRt *rt = app->agent;
    bool have_message_item = false;
    if (items_json)
    {
        JsonDoc doc;
        if (JsonParse(&doc, items_json, strlen(items_json)) == 0)
        {
            int n = JsonArrayLen(&doc, 0);
            for (int i = 0; i < n; i++)
            {
                if (ItemIsNonEmptyMessage(&doc, JsonArrayAt(&doc, 0, i)))
                {
                    have_message_item = true;
                    break;
                }
            }
            JsonFree(&doc);
        }
    }
    if (!have_message_item && rt->stream_msg >= 0 && !MessageEmpty(app, rt->stream_msg))
    {
        PushInput(rt, BuildAssistantItem(app->messages[rt->stream_msg].source));
    }
}

static void OnLlmDone(PicoApp *app, PicoAgentEv *ev)
{
    PicoAgentRt *rt = app->agent;
    if (ev->tokens > 0)
    {
        app->tokens_used = ev->tokens;
    }
    if (rt->stream_msg >= 0 && MessageEmpty(app, rt->stream_msg))
    {
        char *text = MessageTextFromItems(ev->payload);
        if (text && text[0])
        {
            SetMessageText(app, rt->stream_msg, text);
        }
        free(text);
    }
    FinishAssistantHistory(app, ev->payload);
    IngestOutputItems(app, ev->payload);
    if (rt->pending_count > 0)
    {
        if (rt->stream_msg >= 0 && MessageEmpty(app, rt->stream_msg))
        {
            PopLastMessage(app);
            rt->stream_msg = -1;
        }
        rt->pending_next = 0;
        StartNextTool(app);
        return;
    }
    if (rt->stream_msg >= 0 && MessageEmpty(app, rt->stream_msg))
    {
        PopLastMessage(app);
    }
    GoIdle(app);
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
    PushFunctionOutput(rt, call_id, ev->text ? ev->text : (failed ? "tool failed" : ""));
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
    pthread_mutex_unlock(&rt->mu);
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
    free(rt->work_url);
    free(rt->work_key);
    free(rt->work_body);
    free(rt->work_tool_name);
    free(rt->work_tool_args);
    free(rt->work_call_id);
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
    if (!app->settings.api_key[0])
    {
        SetErrorState(app,
                      "No API key. Set `PICO_API_KEY` or `OPENAI_API_KEY`, or put `api_key` in "
                      "`~/.config/pico/settings.json`. Optional: `PICO_BASE_URL` / `base_url` "
                      "(default `https://api.openai.com/v1`) and `PICO_MODEL` / `model`.");
        PicoApp_AddMessage(app, PICO_ROLE_ASSISTANT, app->agent_error);
        return;
    }
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
            FinishAssistantHistory(app, NULL);
            if (rt->stream_msg >= 0 && MessageEmpty(app, rt->stream_msg))
            {
                PopLastMessage(app);
            }
            GoIdle(app);
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
