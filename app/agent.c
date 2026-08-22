#define _POSIX_C_SOURCE 200809L

#include "agent.h"
#include "agent_manager.h"
#include "json.h"
#include "session.h"
#include "settings.h"
#include "usage.h"

#include <curl/curl.h>
#include <errno.h>
#include <pthread.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

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
    PICO_AEV_TOOL_START,
    PICO_AEV_TOOL_DONE,
    PICO_AEV_TOOL_FAIL,
} PicoAgentEvType;

typedef struct PicoAgentEv {
    PicoAgentEvType type;
    char *text;
    char *payload;
    char *tool_args;
    char *tool_details;
    int tokens;
    int cached;
    bool executed;
    bool is_error;
} PicoAgentEv;

typedef enum PicoWorkKind {
    PICO_WORK_IDLE = 0,
    PICO_WORK_LLM,
    PICO_WORK_TOOL,
} PicoWorkKind;

struct PicoAgentContext {
    PicoAgentRt *runtime;
    PicoAgentManager *manager;
    PicoAgentId agent_id;
    uint64_t runtime_generation;
    char workspace[4096];
    char session_id[40];
    char profile[65];
    char purpose[1025];
    bool safe_mode;
    struct PicoAuthStore *auth_store;
};

struct PicoAgentRt {
    /* Heap-owned worker services and callback-scoped public context. */
    PicoAgentContext context;
    PicoToolBeforeFn tool_before_hooks[PICO_MAX_TOOL_HOOKS];
    int tool_before_hook_count;
    pthread_t thread;
    pthread_mutex_t mu;
    pthread_cond_t cv;
    bool stop;
    bool started;
    bool busy;
    bool cancel;
    bool retired;
    pid_t tool_child;
    struct PicoAgentRt *zombie_next;

    PicoWorkKind work;
    PicoProviderStreamFn work_stream;
    char *work_model;
    char *work_base_url;
    char *work_effort;
    char *work_instructions;
    char *work_cache_key;
    char **work_input;
    int work_input_count;
    bool work_compact;
    bool work_include_tools;
    PicoTool *work_tools;
    int work_tool_count;
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
    char *summary;
    size_t summary_len;
    size_t summary_cap;
    int summary_steps;
    bool summary_new_step;
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
    PicoTool *offered_tools;
    int offered_tool_count;

    int stream_msg;
    bool stream_dirty;

    uint64_t ask_id;
    char *ask_request;
    char *ask_answer;
    bool ask_waiting;
    bool ask_done;

    uint64_t snap_id;
    char *snap_request;
    bool snap_retired;
};

static pthread_mutex_t g_ask_id_mu = PTHREAD_MUTEX_INITIALIZER;
static uint64_t g_ask_next_id;
static __thread PicoAgentRt *t_worker_rt;
static __thread PicoAgentContext *t_agent_context;

typedef enum PicoWorkerContext {
    PICO_WORKER_NONE = 0,
    PICO_WORKER_PROVIDER,
    PICO_WORKER_TOOL,
} PicoWorkerContext;
static __thread PicoWorkerContext t_worker_context;

static void SetErrorState(PicoApp *app, PicoAgent *agent, const char *msg);
static void FinishAssistantHistory(PicoApp *app, PicoAgent *agent);
static void RefreshWorkerContext(PicoAgentRt *rt, const PicoApp *app, const PicoAgent *agent);
static char *BuildUserItem(const char *text);
static void *WorkerMain(void *arg);
static bool AgentContextActive(const PicoAgentContext *ctx);

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

static void ClearOfferedTools(PicoAgentRt *rt)
{
    free(rt->offered_tools);
    rt->offered_tools = NULL;
    rt->offered_tool_count = 0;
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
static void PostEventEx(PicoAgentRt *rt, PicoAgentEvType type, char *text, char *payload, char *tool_args,
                        char *tool_details, int tokens, int cached, bool finish, bool executed, bool is_error)
{
    pthread_mutex_lock(&rt->mu);
    if (rt->event_count >= rt->event_cap)
    {
        int cap = rt->event_cap == 0 ? 8 : rt->event_cap * 2;
        PicoAgentEv *next = (PicoAgentEv *)realloc(rt->events, (size_t)cap * sizeof(PicoAgentEv));
        if (!next)
        {
            if (finish)
            {
                rt->busy = false;
            }
            pthread_cond_broadcast(&rt->cv);
            pthread_mutex_unlock(&rt->mu);
            free(text);
            free(payload);
            free(tool_args);
            free(tool_details);
            return;
        }
        rt->events = next;
        rt->event_cap = cap;
    }
    PicoAgentEv *ev = &rt->events[rt->event_count++];
    ev->type = type;
    ev->text = text;
    ev->payload = payload;
    ev->tool_args = tool_args;
    ev->tool_details = tool_details;
    ev->tokens = tokens;
    ev->cached = cached;
    ev->executed = executed;
    ev->is_error = is_error;
    if (finish)
    {
        rt->busy = false;
    }
    pthread_cond_broadcast(&rt->cv);
    pthread_mutex_unlock(&rt->mu);
}

static void PostEvent(PicoAgentRt *rt, PicoAgentEvType type, char *text, char *payload, int tokens, int cached)
{
    PostEventEx(rt, type, text, payload, NULL, NULL, tokens, cached, true, false, false);
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

static void BufSet(char **buf, size_t *len, size_t *cap, const char *s, size_t n)
{
    if (!s)
    {
        n = 0;
    }
    if (n + 1 > *cap)
    {
        size_t next_cap = *cap ? *cap : 256;
        while (next_cap < n + 1)
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
    if (*buf)
    {
        if (n)
        {
            memcpy(*buf, s, n);
        }
        (*buf)[n] = '\0';
        *len = n;
    }
}

static void SetActivity(PicoApp *app, PicoAgent *agent, const char *msg)
{
    snprintf(agent->activity, sizeof(agent->activity), "%s", msg ? msg : "");
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
    pthread_mutex_lock(&rt->mu);
    if (kind == PICO_LLM_DELTA_THINKING_SUMMARY)
    {
        if (n == 0)
        {
            rt->summary_new_step = true;
        }
        else if (s)
        {
            if (rt->summary_steps < 1 || rt->summary_new_step)
            {
                rt->summary_steps++;
                rt->summary_new_step = false;
            }
            BufSet(&rt->summary, &rt->summary_len, &rt->summary_cap, s, n);
        }
        pthread_mutex_unlock(&rt->mu);
        return;
    }
    if (!s || n == 0)
    {
        pthread_mutex_unlock(&rt->mu);
        return;
    }
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

static bool WorkerIsCancelled(PicoAgentRt *rt)
{
    pthread_mutex_lock(&rt->mu);
    bool c = rt->cancel || rt->stop;
    pthread_mutex_unlock(&rt->mu);
    return c;
}

static char *RunToolBeforeHooks(PicoAgentRt *rt, const char *name, const char *call_id, char **args_inout,
                                bool *denied)
{
    *denied = false;
    char *args = *args_inout;
    if (!args)
    {
        args = Dup("{}");
        *args_inout = args;
    }
    for (int i = 0; i < rt->tool_before_hook_count; i++)
    {
        if (!rt->tool_before_hooks[i])
        {
            continue;
        }
        if (WorkerIsCancelled(rt))
        {
            break;
        }
        PicoToolEvent ev;
        memset(&ev, 0, sizeof(ev));
        ev.name = name ? name : "";
        ev.call_id = call_id ? call_id : "";
        ev.args_json = args ? args : "{}";
        t_agent_context = &rt->context;
        rt->tool_before_hooks[i](&rt->context, &ev);
        t_agent_context = NULL;
        if (ev.args_json_out)
        {
            if (args != ev.args_json_out)
            {
                free(args);
            }
            args = ev.args_json_out;
            *args_inout = args;
        }
        if (ev.deny)
        {
            *denied = true;
            *args_inout = args;
            return ev.result ? ev.result : Dup("User denied this tool.");
        }
        free(ev.result);
    }
    *args_inout = args;
    return NULL;
}

static char *RunToolAfterHooks(PicoApp *app, PicoAgentId agent_id, const char *name,
                               const char *call_id, const char *args, const char *output,
                               const char *details_json, bool executed, bool is_error)
{
    char *cur = Dup(output ? output : "");
    for (int i = 0; i < app->tool_after_hook_count; i++)
    {
        if (!app->tool_after_hooks[i])
        {
            continue;
        }
        PicoToolEvent ev;
        memset(&ev, 0, sizeof(ev));
        ev.name = name ? name : "";
        ev.call_id = call_id ? call_id : "";
        ev.args_json = args ? args : "{}";
        ev.output = cur ? cur : "";
        ev.details_json = details_json;
        ev.executed = executed;
        ev.is_error = is_error;
        app->tool_after_hooks[i](app, agent_id, &ev);
        free(ev.args_json_out);
        if (ev.result)
        {
            free(cur);
            cur = ev.result;
        }
    }
    return cur;
}

static char *AppendParagraph(char *base, char *extra)
{
    if (!extra || !extra[0])
    {
        free(extra);
        return base;
    }
    if (!base || !base[0])
    {
        free(base);
        return extra;
    }
    size_t nb = strlen(base);
    size_t ne = strlen(extra);
    char *out = (char *)malloc(nb + 2 + ne + 1);
    if (!out)
    {
        free(extra);
        return base;
    }
    memcpy(out, base, nb);
    memcpy(out + nb, "\n\n", 2);
    memcpy(out + nb + 2, extra, ne + 1);
    free(base);
    free(extra);
    return out;
}

static bool AgentAllowsTool(const PicoAgent *agent, const char *name)
{
    if (!agent || !name)
    {
        return false;
    }
    if (!agent->allowed_tools)
    {
        return true;
    }
    for (int i = 0; i < agent->allowed_tool_count; i++)
    {
        if (agent->allowed_tools[i] && strcmp(agent->allowed_tools[i], name) == 0)
        {
            return true;
        }
    }
    return false;
}

static void RunLlmHooks(PicoApp *app, PicoAgent *agent, bool compact, bool include_tools,
                        const char *base, char **instructions, PicoTool **tools, int *tool_count)
{
    char *instr = Dup(base);
    PicoTool eligible[PICO_MAX_TOOLS];
    int ntools = 0;
    for (int i = 0; i < app->tool_count && ntools < PICO_MAX_TOOLS; i++)
    {
        if (AgentAllowsTool(agent, app->tools[i].name))
        {
            eligible[ntools++] = app->tools[i];
        }
    }
    bool exclude[PICO_MAX_TOOLS];
    memset(exclude, 0, sizeof(exclude));
    /* Filtering pass: collect exclusions so every hook sees the final catalog. */
    for (int i = 0; i < app->llm_hook_count; i++)
    {
        if (!app->llm_hooks[i])
        {
            continue;
        }
        PicoLlmEvent ev;
        memset(&ev, 0, sizeof(ev));
        ev.compact = compact;
        ev.include_tools = include_tools;
        ev.tools = eligible;
        ev.tool_count = ntools;
        ev.exclude = include_tools ? exclude : NULL;
        ev.instructions = instr ? instr : "";
        app->llm_hooks[i](app, agent ? agent->id : 0, &ev);
        free(ev.extra_instructions);
    }
    /* Instructions pass: extras go under one heading; later hooks see the section. */
    bool extras = false;
    for (int i = 0; i < app->llm_hook_count; i++)
    {
        if (!app->llm_hooks[i])
        {
            continue;
        }
        PicoLlmEvent ev;
        memset(&ev, 0, sizeof(ev));
        ev.compact = compact;
        ev.include_tools = include_tools;
        ev.tools = eligible;
        ev.tool_count = ntools;
        ev.exclude = include_tools ? exclude : NULL;
        ev.instructions = instr ? instr : "";
        app->llm_hooks[i](app, agent ? agent->id : 0, &ev);
        if (ev.extra_instructions)
        {
            if (ev.extra_instructions[0] && !extras)
            {
                extras = true;
                instr = AppendParagraph(instr, Dup("## Additional instructions"));
            }
            instr = AppendParagraph(instr, ev.extra_instructions);
        }
    }
    *instructions = instr;
    if (!include_tools)
    {
        *tools = NULL;
        *tool_count = 0;
        return;
    }
    int kept = 0;
    for (int i = 0; i < ntools; i++)
    {
        if (!exclude[i])
        {
            kept++;
        }
    }
    PicoTool *copy = NULL;
    if (kept > 0)
    {
        copy = (PicoTool *)calloc((size_t)kept, sizeof(PicoTool));
        if (copy)
        {
            int j = 0;
            for (int i = 0; i < ntools; i++)
            {
                if (!exclude[i])
                {
                    copy[j++] = eligible[i];
                }
            }
        }
        else
        {
            kept = 0;
        }
    }
    *tools = copy;
    *tool_count = kept;
}

static char *ValidateLlmToolCalls(const PicoLlmResult *result)
{
    if (!result || result->call_count < 0 || result->call_count > PICO_MAX_PENDING_CALLS)
    {
        return Dup("provider returned too many or an invalid number of tool calls");
    }
    if (result->call_count > 0 && !result->calls)
    {
        return Dup("provider returned a malformed tool call array");
    }
    for (int i = 0; i < result->call_count; i++)
    {
        const PicoLlmToolCall *call = &result->calls[i];
        if (!call->call_id || !call->call_id[0])
        {
            return Dup("provider returned a tool call with an empty call id");
        }
        if (!call->name || !call->name[0] || !call->arguments)
        {
            return Dup("provider returned a malformed tool call");
        }
        for (int j = 0; j < i; j++)
        {
            if (strcmp(result->calls[j].call_id, call->call_id) == 0)
            {
                return Dup("provider returned duplicate tool call ids");
            }
        }
    }
    return NULL;
}

static char *ValidateToolDetails(char *details, bool *valid)
{
    *valid = false;
    if (!details)
    {
        *valid = true;
        return NULL;
    }
    size_t len = strlen(details);
    if (len > PICO_TOOL_DETAILS_MAX || !JsonValidUtf8(details, len))
    {
        free(details);
        return NULL;
    }
    JsonDoc doc;
    memset(&doc, 0, sizeof(doc));
    if (JsonParse(&doc, details, len) != 0 || !JsonIsObject(&doc, 0) || JsonSkip(&doc, 0) != doc.ntoks)
    {
        if (doc.toks)
        {
            JsonFree(&doc);
        }
        free(details);
        return NULL;
    }
    int end = JsonTokEnd(&doc, 0);
    for (size_t i = end >= 0 ? (size_t)end : len; i < len; i++)
    {
        char c = details[i];
        if (c != ' ' && c != '\t' && c != '\r' && c != '\n')
        {
            JsonFree(&doc);
            free(details);
            return NULL;
        }
    }
    char *canonical = JsonRawDup(&doc, 0);
    JsonFree(&doc);
    free(details);
    if (!canonical)
    {
        return NULL;
    }
    *valid = true;
    return canonical;
}

static void *WorkerMain(void *arg)
{
    PicoAgentRt *rt = (PicoAgentRt *)arg;
    t_worker_rt = rt;
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
        char *effort = rt->work_effort;
        char *instructions = rt->work_instructions;
        char *cache_key = rt->work_cache_key;
        char **input = rt->work_input;
        int input_count = rt->work_input_count;
        bool compact = rt->work_compact;
        bool include_tools = rt->work_include_tools;
        PicoTool *work_tools = rt->work_tools;
        int work_tool_count = rt->work_tool_count;
        char *tool_name = rt->work_tool_name;
        char *tool_args = rt->work_tool_args;
        char *call_id = rt->work_call_id;
        PicoToolFn tool_fn = rt->work_tool_fn;
        rt->work = PICO_WORK_IDLE;
        rt->work_stream = NULL;
        rt->work_model = NULL;
        rt->work_base_url = NULL;
        rt->work_effort = NULL;
        rt->work_instructions = NULL;
        rt->work_cache_key = NULL;
        rt->work_input = NULL;
        rt->work_input_count = 0;
        rt->work_compact = false;
        rt->work_include_tools = false;
        rt->work_tools = NULL;
        rt->work_tool_count = 0;
        rt->work_tool_name = NULL;
        rt->work_tool_args = NULL;
        rt->work_call_id = NULL;
        rt->work_tool_fn = NULL;
        pthread_mutex_unlock(&rt->mu);

        if (kind == PICO_WORK_LLM)
        {
            pthread_mutex_lock(&rt->mu);
            free(rt->summary);
            rt->summary = NULL;
            rt->summary_len = 0;
            rt->summary_cap = 0;
            rt->summary_steps = 0;
            rt->summary_new_step = false;
            pthread_mutex_unlock(&rt->mu);
            PicoLlmTurn turn;
            memset(&turn, 0, sizeof(turn));
            turn.model = model;
            turn.base_url = base_url;
            turn.instructions = instructions;
            turn.cache_key = cache_key;
            turn.effort = effort;
            turn.compact = compact;
            turn.include_tools = include_tools;
            turn.input_json = (const char *const *)input;
            turn.input_count = input_count;
            turn.tools = work_tools;
            turn.tool_count = include_tools ? work_tool_count : 0;
            PicoLlmResult result;
            memset(&result, 0, sizeof(result));
            t_worker_context = PICO_WORKER_PROVIDER;
            t_agent_context = &rt->context;
            int rc = stream_fn ? stream_fn(&rt->context, &turn, CancelCb, DeltaCb, rt, &result)
                               : PICO_LLM_FAIL;
            t_agent_context = NULL;
            t_worker_context = PICO_WORKER_NONE;
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
                char *call_error = ValidateLlmToolCalls(&result);
                if (call_error)
                {
                    PostEvent(rt, PICO_AEV_LLM_FAIL, call_error, NULL, 0, 0);
                    if (!result.calls || result.call_count < 0)
                    {
                        /* A missing or negative-length array cannot be traversed safely. */
                        result.calls = NULL;
                        result.call_count = 0;
                    }
                }
                else
                {
                    char *payload = EncodeResult(&result);
                    PostEvent(rt, PICO_AEV_LLM_DONE, NULL, payload,
                              result.input_tokens, result.cached_tokens);
                }
                pico_llm_result_free(&result);
            }
        }
        else if (kind == PICO_WORK_TOOL)
        {
            bool skip_run = false;
            t_worker_context = PICO_WORKER_TOOL;
            bool denied = false;
            char *deny_result = RunToolBeforeHooks(rt, tool_name, call_id, &tool_args, &denied);
            if (WorkerIsCancelled(rt))
            {
                free(deny_result);
                PostEventEx(rt, PICO_AEV_TOOL_DONE, Dup("(interrupted)"), call_id,
                            Dup(tool_args ? tool_args : "{}"), NULL, 0, 0, true, false, true);
                call_id = NULL;
                skip_run = true;
            }
            else if (denied)
            {
                PostEventEx(rt, PICO_AEV_TOOL_DONE,
                            deny_result ? deny_result : Dup("User denied this tool."), call_id,
                            Dup(tool_args ? tool_args : "{}"), NULL, 0, 0, true, false, true);
                call_id = NULL;
                skip_run = true;
            }
            else
            {
                free(deny_result);
            }
            if (!skip_run)
            {
                PostEventEx(rt, PICO_AEV_TOOL_START, Dup(tool_name ? tool_name : ""),
                            Dup(tool_args ? tool_args : "{}"), NULL, NULL, 0, 0, false, false, false);
                PicoToolResult result;
                memset(&result, 0, sizeof(result));
                if (tool_fn)
                {
                    t_agent_context = &rt->context;
                    tool_fn(&rt->context, tool_args ? tool_args : "{}", &result);
                    t_agent_context = NULL;
                    bool details_valid = false;
                    result.details_json = ValidateToolDetails(result.details_json, &details_valid);
                    if (!details_valid)
                    {
                        free(result.output);
                        result.output = Dup("tool returned invalid details");
                        result.is_error = true;
                    }
                }
                else
                {
                    JsonBuf b;
                    JsonBuf_Init(&b);
                    JsonBuf_Puts(&b, "unknown tool: ");
                    JsonBuf_Puts(&b, tool_name ? tool_name : "?");
                    result.output = JsonBuf_Steal(&b);
                    result.is_error = true;
                }
                PostEventEx(rt, PICO_AEV_TOOL_DONE, result.output ? result.output : Dup(""), call_id,
                            Dup(tool_args ? tool_args : "{}"), result.details_json, 0, 0, true,
                            tool_fn != NULL, result.is_error);
                call_id = NULL;
            }
            t_worker_context = PICO_WORKER_NONE;
        }

        free(model);
        free(base_url);
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

static void RunContextHooks(PicoApp *app, PicoAgent *agent, bool compact,
                            const PicoTool *tools, int tool_count,
                            char ***input_inout, int *count_inout)
{
    char **input = *input_inout;
    int base_count = *count_inout;
    char *extras[PICO_MAX_CONTEXT_HOOKS];
    int extra_count = 0;
    memset(extras, 0, sizeof(extras));

    for (int i = 0; i < app->context_hook_count; i++)
    {
        if (!app->context_hooks[i])
        {
            continue;
        }
        PicoContextEvent ev;
        memset(&ev, 0, sizeof(ev));
        ev.compact = compact;
        ev.history_json = (const char *const *)input;
        ev.history_count = base_count;
        ev.tools = tools;
        ev.tool_count = tool_count;
        app->context_hooks[i](app, agent ? agent->id : 0, &ev);
        if (ev.extra_context && ev.extra_context[0])
        {
            extras[extra_count++] = ev.extra_context;
        }
        else
        {
            free(ev.extra_context);
        }
    }
    if (extra_count == 0)
    {
        return;
    }
    char **next = (char **)realloc(input, (size_t)(base_count + extra_count) * sizeof(char *));
    if (!next)
    {
        for (int i = 0; i < extra_count; i++)
        {
            free(extras[i]);
        }
        return;
    }
    input = next;
    for (int i = 0; i < extra_count; i++)
    {
        input[base_count + i] = BuildUserItem(extras[i]);
        free(extras[i]);
    }
    *input_inout = input;
    *count_inout = base_count + extra_count;
}

static bool QueueLlm(PicoApp *app, PicoAgent *agent, bool compact, bool include_tools)
{
    PicoAgentRt *rt = agent->runtime;
    RefreshWorkerContext(rt, app, agent);
    PicoModel *m = PicoSettings_ActiveModel(app, agent);
    if (!m || !m->provider[0])
    {
        SetErrorState(app, agent, "Active model has no provider. Set `provider` on the model in settings.json.");
        return false;
    }
    const PicoProvider *p = pico_find_provider(app, m->provider);
    if (!p || !p->stream)
    {
        char buf[256];
        snprintf(buf, sizeof(buf), "No provider `%s` for model `%s`.", m->provider,
                 m->id[0] ? m->id : "?");
        SetErrorState(app, agent, buf);
        return false;
    }

    char **input = NULL;
    int input_count = rt->input_count;
    if (input_count > 0)
    {
        input = (char **)calloc((size_t)input_count, sizeof(char *));
        if (!input)
        {
            SetErrorState(app, agent, "out of memory");
            return false;
        }
        for (int i = 0; i < input_count; i++)
        {
            input[i] = Dup(rt->input[i].json);
        }
    }

    char *instructions = NULL;
    PicoTool *tools = NULL;
    int tool_count = 0;
    RunLlmHooks(app, agent, compact, include_tools, rt->instructions, &instructions, &tools, &tool_count);
    RunContextHooks(app, agent, compact, tools, tool_count, &input, &input_count);

    pthread_mutex_lock(&rt->mu);
    if (rt->busy || rt->stop)
    {
        pthread_mutex_unlock(&rt->mu);
        for (int i = 0; i < input_count; i++)
        {
            free(input[i]);
        }
        free(input);
        free(instructions);
        free(tools);
        return false;
    }
    rt->work = PICO_WORK_LLM;
    rt->work_stream = p->stream;
    free(rt->turn_provider);
    rt->turn_provider = Dup(p->name);
    rt->work_model = Dup(m->id);
    rt->work_base_url = Dup(m->base_url);
    rt->work_effort = Dup(PicoSettings_ActiveEffort(agent));
    rt->work_instructions = instructions;
    rt->work_cache_key = Dup(rt->cache_key);
    rt->work_compact = compact;
    rt->work_include_tools = include_tools;
    ClearOfferedTools(rt);
    rt->offered_tools = tools;
    rt->offered_tool_count = tool_count;
    rt->work_tools = rt->offered_tools;
    rt->work_tool_count = rt->offered_tool_count;
    rt->work_input = input;
    rt->work_input_count = input_count;
    rt->busy = true;
    rt->cancel = false;
    pthread_cond_broadcast(&rt->cv);
    pthread_mutex_unlock(&rt->mu);
    return true;
}

static bool QueueTool(PicoApp *app, PicoAgent *agent, const char *name, const char *args,
                      const char *call_id, PicoToolFn fn)
{
    PicoAgentRt *rt = agent->runtime;
    RefreshWorkerContext(rt, app, agent);
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
    rt->work_tools = NULL;
    rt->work_tool_count = 0;
    rt->tool_child = 0;
    rt->busy = true;
    pthread_cond_broadcast(&rt->cv);
    pthread_mutex_unlock(&rt->mu);
    return true;
}

static PicoTool *FindOfferedTool(PicoAgentRt *rt, const char *name)
{
    if (!rt || !name)
    {
        return NULL;
    }
    for (int i = 0; i < rt->offered_tool_count; i++)
    {
        if (rt->offered_tools[i].name && strcmp(rt->offered_tools[i].name, name) == 0)
        {
            return &rt->offered_tools[i];
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

static char *BuildToolResult(const char *call_id, const char *name, const char *output, bool is_error)
{
    JsonBuf b;
    JsonBuf_Init(&b);
    JsonBuf_Puts(&b, "{\"type\":\"tool_result\",\"call_id\":");
    JsonBuf_String(&b, call_id ? call_id : "");
    JsonBuf_Puts(&b, ",\"name\":");
    JsonBuf_String(&b, name ? name : "");
    JsonBuf_Puts(&b, ",\"output\":");
    JsonBuf_String(&b, output ? output : "");
    JsonBuf_Puts(&b, ",\"is_error\":");
    JsonBuf_Bool(&b, is_error);
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

static void AppendMessageText(PicoApp *app, PicoAgent *agent, int idx, const char *s, size_t n)
{
    if (idx < 0 || idx >= agent->message_count || !s || n == 0)
    {
        return;
    }
    PicoMessage *m = &agent->messages[idx];
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

static void ReparseMessage(PicoApp *app, PicoAgent *agent, int idx)
{
    if (idx < 0 || idx >= agent->message_count)
    {
        return;
    }
    PicoMessage *m = &agent->messages[idx];
    MdDocument_Free(&m->doc);
    size_t len = m->source ? strlen(m->source) : 0;
    m->doc = MdDocument_ParseEx(m->source ? m->source : "", len, MD_PARSE_DEFAULT);
}

static void SetMessageText(PicoApp *app, PicoAgent *agent, int idx, const char *text)
{
    if (idx < 0 || idx >= agent->message_count)
    {
        return;
    }
    PicoMessage *m = &agent->messages[idx];
    free(m->source);
    m->source = Dup(text ? text : "");
    ReparseMessage(app, agent, idx);
}

static void PopLastMessage(PicoApp *app, PicoAgent *agent)
{
    if (agent->message_count <= 0)
    {
        return;
    }
    int i = --agent->message_count;
    free(agent->messages[i].source);
    for (int t = 0; t < agent->messages[i].trace_count; t++)
    {
        free(agent->messages[i].trace[t].text);
        free(agent->messages[i].trace[t].tool_name);
        free(agent->messages[i].trace[t].tool_args);
        free(agent->messages[i].trace[t].tool_output);
        MdDocument_Free(&agent->messages[i].trace[t].doc);
    }
    free(agent->messages[i].trace);
    MdDocument_Free(&agent->messages[i].doc);
    memset(&agent->messages[i], 0, sizeof(agent->messages[i]));
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

static bool MessageSourceEmpty(const PicoAgent *agent, int idx)
{
    if (idx < 0 || idx >= agent->message_count)
    {
        return true;
    }
    return Blank(agent->messages[idx].source);
}

static bool MessageEmpty(const PicoAgent *agent, int idx)
{
    if (idx < 0 || idx >= agent->message_count)
    {
        return true;
    }
    if (!Blank(agent->messages[idx].source))
    {
        return false;
    }
    for (int t = 0; t < agent->messages[idx].trace_count; t++)
    {
        if (!Blank(agent->messages[idx].trace[t].text) || !Blank(agent->messages[idx].trace[t].tool_name))
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

static void TraceAppendThink(PicoApp *app, PicoAgent *agent, int idx, const char *s, size_t n)
{
    if (idx < 0 || idx >= agent->message_count || !s || n == 0)
    {
        return;
    }
    PicoMessage *m = &agent->messages[idx];
    PicoTraceLine *line = NULL;
    if (m->trace_count > 0 && !m->trace[m->trace_count - 1].is_tool &&
        m->trace[m->trace_count - 1].think_steps == 0)
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

static void ReparseThinkSummary(PicoTraceLine *line)
{
    MdDocument_Free(&line->doc);
    const char *text = line->text ? line->text : "";
    size_t tlen = strlen(text);
    if (line->think_steps > 1)
    {
        char prefix[32];
        int plen = snprintf(prefix, sizeof(prefix), "%dx ", line->think_steps);
        if (plen < 0)
        {
            line->doc = MdDocument_ParseEx(text, tlen, MD_PARSE_DEFAULT);
            return;
        }
        size_t n = (size_t)plen + tlen;
        char *src = (char *)malloc(n + 1);
        if (!src)
        {
            line->doc = MdDocument_ParseEx(text, tlen, MD_PARSE_DEFAULT);
            return;
        }
        memcpy(src, prefix, (size_t)plen);
        memcpy(src + plen, text, tlen + 1);
        line->doc = MdDocument_ParseEx(src, n, MD_PARSE_DEFAULT);
        free(src);
        return;
    }
    line->doc = MdDocument_ParseEx(text, tlen, MD_PARSE_DEFAULT);
}

static void TraceSetThinkSummary(PicoApp *app, PicoAgent *agent, int idx, const char *s, size_t n, int steps)
{
    if (idx < 0 || idx >= agent->message_count || !s || n == 0)
    {
        return;
    }
    PicoMessage *m = &agent->messages[idx];
    PicoTraceLine *line = NULL;
    if (m->trace_count > 0 && !m->trace[m->trace_count - 1].is_tool &&
        m->trace[m->trace_count - 1].think_steps > 0)
    {
        line = &m->trace[m->trace_count - 1];
    }
    else
    {
        line = TracePush(m, false);
        if (line)
        {
            line->think_steps = 1;
        }
    }
    if (!line)
    {
        return;
    }
    char *next = (char *)realloc(line->text, n + 1);
    if (!next)
    {
        return;
    }
    memcpy(next, s, n);
    next[n] = '\0';
    line->text = next;
    if (steps > 0)
    {
        line->think_steps = steps;
    }
    ReparseThinkSummary(line);
}

static void TraceAddTool(PicoApp *app, PicoAgent *agent, int idx, const char *name, const char *args_json)
{
    if (idx < 0 || idx >= agent->message_count)
    {
        return;
    }
    PicoAgent_AddToolCall(app, agent, name, args_json);
}

static void TraceSetLastToolOutput(PicoApp *app, PicoAgent *agent, int idx, const char *output, bool is_error)
{
    if (idx < 0 || idx >= agent->message_count)
    {
        return;
    }
    PicoMessage *m = &agent->messages[idx];
    for (int t = m->trace_count - 1; t >= 0; t--)
    {
        if (m->trace[t].is_tool)
        {
            free(m->trace[t].tool_output);
            m->trace[t].tool_output = Dup(output ? output : "");
            m->trace[t].tool_error = is_error;
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

static void PushFunctionOutput(PicoAgentRt *rt, const char *call_id, const char *name, const char *output,
                               bool is_error)
{
    PushInput(rt, BuildToolResult(call_id, name, output, is_error));
}

static void AbortRemainingCalls(PicoApp *app, PicoAgent *agent, PicoAgentRt *rt)
{
    for (int i = rt->pending_next; i < rt->pending_count; i++)
    {
        PushFunctionOutput(rt, rt->pending[i].call_id, rt->pending[i].name, "(interrupted)", true);
        PicoSession_LogToolResult(app, agent, rt->pending[i].call_id, rt->pending[i].name,
                                  "(interrupted)", true, NULL);
    }
    ClearPending(rt);
}

static void GoIdle(PicoApp *app, PicoAgent *agent)
{
    PicoAgentRt *rt = agent->runtime;
    pthread_mutex_lock(&rt->mu);
    bool may_release_tools = !rt->busy && !rt->retired;
    pthread_mutex_unlock(&rt->mu);
    if (may_release_tools)
    {
        ClearOfferedTools(rt);
    }
    agent->state = PICO_AGENT_IDLE;
    rt->stream_msg = -1;
    rt->stream_dirty = false;
    rt->compacting = false;
    rt->compact_no_tools = false;
    agent->activity[0] = '\0';
}

static void EndTurnIdle(PicoApp *app, PicoAgent *agent)
{
    GoIdle(app, agent);
    pico_run_hooks(app, PICO_HOOK_ON_TURN_END, agent->id);
}

static void ApplyCancel(PicoApp *app, PicoAgent *agent)
{
    PicoAgentRt *rt = agent->runtime;
    if (rt->compacting)
    {
        GoIdle(app, agent);
        pico_run_hooks(app, PICO_HOOK_ON_CANCEL, agent->id);
        return;
    }
    FinishAssistantHistory(app, agent);
    if (rt->stream_msg >= 0 && !MessageSourceEmpty(agent, rt->stream_msg))
    {
        PicoSession_LogAssistant(app, agent, agent->messages[rt->stream_msg].source);
    }
    if (rt->stream_msg >= 0 && MessageEmpty(agent, rt->stream_msg))
    {
        PopLastMessage(app, agent);
    }
    bool open_tool = rt->pending_next < rt->pending_count;
    AbortRemainingCalls(app, agent, rt);
    if (open_tool && rt->stream_msg >= 0)
    {
        TraceSetLastToolOutput(app, agent, rt->stream_msg, "(interrupted)", true);
    }
    GoIdle(app, agent);
    pico_run_hooks(app, PICO_HOOK_ON_CANCEL, agent->id);
}

static int CompactThreshold(const PicoAgent *agent)
{
    if (!agent->compact_enabled || agent->context_limit <= 0)
    {
        return 0;
    }
    int t = (int)((double)agent->context_limit * agent->compact_ratio);
    return t > 0 ? t : 0;
}

static void ApplyCompaction(PicoApp *app, PicoAgent *agent, const char *summary)
{
    int before = agent->tokens_used;
    PicoAgent_ClearInput(agent);
    JsonBuf b;
    JsonBuf_Init(&b);
    JsonBuf_Puts(&b, "Briefing:\n");
    JsonBuf_Puts(&b, summary ? summary : "");
    PicoAgent_PushHistoryUser(agent, b.data ? b.data : "Briefing:\n");
    JsonBuf_Free(&b);
    PicoAgent_RotateCacheKey(agent);
    PicoSession_LogCompaction(app, agent, summary, before);
    agent->tokens_used = 0;
    agent->tokens_cached = 0;
    free(agent->compact_summary);
    agent->compact_summary = NULL;
    pico_run_hooks(app, PICO_HOOK_AFTER_COMPACT, agent->id);
}

static void StartCompact(PicoApp *app, PicoAgent *agent)
{
    PicoAgentRt *rt = agent->runtime;
    rt->stream_msg = -1;
    rt->stream_dirty = false;
    agent->state = PICO_AGENT_COMPACT_WAIT;
    SetActivity(app, agent, "Compacting…");
    free(agent->compact_summary);
    agent->compact_summary = NULL;
    pico_run_hooks(app, PICO_HOOK_ON_COMPACT, agent->id);
    if (agent->compact_summary && agent->compact_summary[0])
    {
        ApplyCompaction(app, agent, agent->compact_summary);
        EndTurnIdle(app, agent);
        return;
    }
    rt->compacting = true;
    rt->compact_no_tools = false;
    if (!QueueLlm(app, agent, true, true))
    {
        SetErrorState(app, agent, "Failed to start compaction");
    }
}

static void FinishTurn(PicoApp *app, PicoAgent *agent)
{
    PicoAgentRt *rt = agent->runtime;
    if (rt->stream_msg >= 0 && MessageEmpty(agent, rt->stream_msg))
    {
        PopLastMessage(app, agent);
    }
    if (CompactThreshold(agent) > 0 && agent->tokens_used >= CompactThreshold(agent))
    {
        StartCompact(app, agent);
        return;
    }
    EndTurnIdle(app, agent);
}

static void SetErrorState(PicoApp *app, PicoAgent *agent, const char *msg)
{
    PicoAgentRt *rt = agent->runtime;
    free(agent->error);
    agent->error = Dup(msg ? msg : "agent error");
    agent->state = PICO_AGENT_ERROR;
    if (rt->stream_msg >= 0 && MessageEmpty(agent, rt->stream_msg))
    {
        SetMessageText(app, agent, rt->stream_msg, agent->error);
    }
    rt->stream_msg = -1;
    rt->stream_dirty = false;
    ClearPending(rt);
    ClearOfferedTools(rt);
    rt->compacting = false;
    rt->compact_no_tools = false;
    agent->activity[0] = '\0';
    pico_run_hooks(app, PICO_HOOK_ON_ERROR, agent->id);
}

static void StartLlm(PicoApp *app, PicoAgent *agent);

static void StartNextTool(PicoApp *app, PicoAgent *agent)
{
    PicoAgentRt *rt = agent->runtime;
    if (rt->pending_next >= rt->pending_count)
    {
        ClearPending(rt);
        StartLlm(app, agent);
        return;
    }
    PicoPendingCall *call = &rt->pending[rt->pending_next];
    agent->state = PICO_AGENT_TOOL_WAIT;
    SetActivity(app, agent, call->name && call->name[0] ? call->name : "tool");
    PicoTool *tool = FindOfferedTool(rt, call->name);
    if (!tool)
    {
        JsonBuf b;
        JsonBuf_Init(&b);
        JsonBuf_Puts(&b, "tool was not offered for this request: ");
        JsonBuf_Puts(&b, call->name ? call->name : "");
        char *error = JsonBuf_Steal(&b);
        PushFunctionOutput(rt, call->call_id, call->name, error ? error : "unoffered tool", true);
        PicoSession_LogToolResult(app, agent, call->call_id, call->name,
                                  error ? error : "unoffered tool", true, NULL);
        if (rt->stream_msg >= 0)
        {
            TraceAddTool(app, agent, rt->stream_msg, call->name ? call->name : "tool",
                         call->arguments ? call->arguments : "{}");
            TraceSetLastToolOutput(app, agent, rt->stream_msg,
                                   error ? error : "unoffered tool", true);
        }
        free(error);
        rt->pending_next++;
        StartNextTool(app, agent);
        return;
    }
    if (!QueueTool(app, agent, call->name, call->arguments, call->call_id, tool->run))
    {
        SetErrorState(app, agent, "Failed to start tool");
    }
}

static void StartLlm(PicoApp *app, PicoAgent *agent)
{
    PicoAgentRt *rt = agent->runtime;
    int last = agent->message_count - 1;
    if (last >= 0 && agent->messages[last].role == PICO_ROLE_ASSISTANT && MessageSourceEmpty(agent, last))
    {
        rt->stream_msg = last;
    }
    else
    {
        PicoAgent_AddMessage(app, agent, PICO_ROLE_ASSISTANT, "");
        rt->stream_msg = agent->message_count - 1;
    }
    rt->stream_dirty = false;
    agent->state = PICO_AGENT_LLM_WAIT;
    SetActivity(app, agent, "Thinking…");
    free(agent->error);
    agent->error = NULL;
    if (!QueueLlm(app, agent, false, true))
    {
        SetErrorState(app, agent, "Failed to start model request");
    }
}

static void FinishAssistantHistory(PicoApp *app, PicoAgent *agent)
{
    PicoAgentRt *rt = agent->runtime;
    if (rt->stream_msg >= 0 && !MessageSourceEmpty(agent, rt->stream_msg))
    {
        PushInput(rt, BuildAssistantItem(agent->messages[rt->stream_msg].source));
    }
}

static void IngestResult(PicoApp *app, PicoAgent *agent, const char *payload)
{
    PicoAgentRt *rt = agent->runtime;
    if (!payload)
    {
        FinishAssistantHistory(app, agent);
        return;
    }
    JsonDoc doc;
    if (JsonParse(&doc, payload, strlen(payload)) != 0)
    {
        FinishAssistantHistory(app, agent);
        return;
    }

    char *assistant = JsonObjStr(&doc, 0, "assistant_text");
    if (rt->stream_msg >= 0 && MessageSourceEmpty(agent, rt->stream_msg) && assistant && assistant[0])
    {
        SetMessageText(app, agent, rt->stream_msg, assistant);
    }

    char *think = JsonObjStr(&doc, 0, "think_text");
    if (think && think[0] && rt->stream_msg >= 0 && !HasThinkTrace(&agent->messages[rt->stream_msg]))
    {
        TraceAppendThink(app, agent, rt->stream_msg, think, strlen(think));
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

    FinishAssistantHistory(app, agent);

    int calls = JsonObjGet(&doc, 0, "calls");
    int n = JsonIsArray(&doc, calls) ? JsonArrayLen(&doc, calls) : 0;
    for (int i = 0; i < n; i++)
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

static void OnLlmDone(PicoApp *app, PicoAgent *agent, PicoAgentEv *ev)
{
    PicoAgentRt *rt = agent->runtime;
    int normalized_cached = 0;
    if (PicoUsage_Apply(agent, ev->tokens, ev->cached, &normalized_cached))
    {
        PicoSession_LogUsage(app, agent, ev->tokens, normalized_cached);
    }
    if (rt->compacting)
    {
        if (ResultCallCount(ev->payload) > 0 && !rt->compact_no_tools)
        {
            rt->compact_no_tools = true;
            if (!QueueLlm(app, agent, true, false))
            {
                SetErrorState(app, agent, "Failed to start compaction");
            }
            return;
        }
        char *text = ResultStr(ev->payload, "assistant_text");
        if ((!text || !text[0]) && rt->stream_msg >= 0 && !MessageSourceEmpty(agent, rt->stream_msg))
        {
            free(text);
            text = Dup(agent->messages[rt->stream_msg].source);
        }
        if (text && text[0])
        {
            ApplyCompaction(app, agent, text);
            free(text);
            EndTurnIdle(app, agent);
            return;
        }
        free(text);
        rt->compacting = false;
        SetErrorState(app, agent, "Compaction failed");
        return;
    }
    IngestResult(app, agent, ev->payload);
    if (rt->stream_msg >= 0 && !MessageSourceEmpty(agent, rt->stream_msg))
    {
        PicoSession_LogAssistant(app, agent, agent->messages[rt->stream_msg].source);
    }
    for (int i = 0; i < rt->pending_count; i++)
    {
        PicoSession_LogToolCall(app, agent, rt->pending[i].call_id, rt->pending[i].name, rt->pending[i].arguments);
    }
    if (rt->pending_count > 0)
    {
        rt->pending_next = 0;
        StartNextTool(app, agent);
        return;
    }
    FinishTurn(app, agent);
}

static int CountToolTrace(const PicoMessage *m)
{
    int n = 0;
    if (!m)
    {
        return 0;
    }
    for (int t = 0; t < m->trace_count; t++)
    {
        if (m->trace[t].is_tool)
        {
            n++;
        }
    }
    return n;
}

static void OnToolStart(PicoApp *app, PicoAgent *agent, PicoAgentEv *ev)
{
    PicoAgentRt *rt = agent->runtime;
    const char *name = ev->text ? ev->text : "tool";
    const char *args = ev->payload ? ev->payload : "{}";
    if (rt->pending_next < rt->pending_count)
    {
        PicoPendingCall *call = &rt->pending[rt->pending_next];
        if (ev->payload)
        {
            free(call->arguments);
            call->arguments = Dup(ev->payload);
        }
    }
    char *line = FormatToolLine(name, args);
    SetActivity(app, agent, line);
    free(line);
    if (rt->stream_msg >= 0)
    {
        TraceAddTool(app, agent, rt->stream_msg, name, args);
    }
}

static void OnToolDone(PicoApp *app, PicoAgent *agent, PicoAgentEv *ev, bool failed)
{
    PicoAgentRt *rt = agent->runtime;
    PicoPendingCall *call = NULL;
    if (rt->pending_next < rt->pending_count)
    {
        call = &rt->pending[rt->pending_next];
    }
    if (call && ev->tool_args)
    {
        free(call->arguments);
        call->arguments = Dup(ev->tool_args);
    }
    const char *call_id = ev->payload;
    if (call && call->call_id && !call_id)
    {
        call_id = call->call_id;
    }
    const char *name = call && call->name ? call->name : "";
    bool cancel;
    pthread_mutex_lock(&rt->mu);
    cancel = rt->cancel;
    pthread_mutex_unlock(&rt->mu);

    bool is_error = failed || ev->is_error || cancel;
    const char *details = ev->tool_details;
    char *apply_error = NULL;
    PicoTool *tool = FindOfferedTool(rt, name);
    if (!is_error && ev->executed && details && tool && tool->apply &&
        !tool->apply(app, agent->id, details, false))
    {
        is_error = true;
        details = NULL;
        apply_error = Dup("tool result details could not be applied");
    }

    const char *raw = apply_error ? apply_error : (ev->text ? ev->text : (is_error ? "tool failed" : ""));
    char *output = NULL;
    if (!cancel)
    {
        output = RunToolAfterHooks(app, agent->id, name, call_id,
                                   call ? call->arguments : NULL, raw, details,
                                   ev->executed, is_error);
    }
    const char *use = output ? output : raw;
    PushFunctionOutput(rt, call_id, name, use, is_error);
    PicoSession_LogToolResult(app, agent, call_id, name, use, is_error, details);
    if (rt->stream_msg >= 0)
    {
        if (CountToolTrace(&agent->messages[rt->stream_msg]) <= rt->pending_next)
        {
            TraceAddTool(app, agent, rt->stream_msg, name[0] ? name : "tool",
                         call && call->arguments ? call->arguments : "{}");
        }
        TraceSetLastToolOutput(app, agent, rt->stream_msg, use, is_error);
    }
    rt->pending_next++;
    free(output);
    free(apply_error);
    if (cancel)
    {
        AbortRemainingCalls(app, agent, rt);
        GoIdle(app, agent);
        pico_run_hooks(app, PICO_HOOK_ON_CANCEL, agent->id);
        return;
    }
    StartNextTool(app, agent);
}

static bool LiveBusyState(PicoAgentState s)
{
    return s == PICO_AGENT_LLM_WAIT || s == PICO_AGENT_TOOL_WAIT || s == PICO_AGENT_COMPACT_WAIT;
}

bool PicoAgent_IsBusy(const PicoAgent *agent)
{
    return agent && LiveBusyState(agent->state);
}

bool PicoAgent_CancelRequested(const PicoAgent *agent)
{
    PicoAgentRt *rt = agent ? agent->runtime : NULL;
    if (!rt)
    {
        return false;
    }
    pthread_mutex_lock(&rt->mu);
    bool c = rt->cancel;
    pthread_mutex_unlock(&rt->mu);
    return c;
}

bool PicoAgent_AskUiOpen(const PicoAgent *agent)
{
    PicoAgentRt *rt = agent ? agent->runtime : NULL;
    return rt && rt->snap_id != 0;
}

static void PublishAskSnapshot(PicoAgentRt *rt)
{
    pthread_mutex_lock(&rt->mu);
    bool waiting = rt->ask_waiting && !rt->cancel && !rt->stop;
    uint64_t live_id = waiting ? rt->ask_id : 0;
    char *live_copy = NULL;
    if (waiting && rt->ask_request && (rt->snap_retired || rt->snap_id != live_id))
    {
        live_copy = Dup(rt->ask_request);
    }
    pthread_mutex_unlock(&rt->mu);

    if (!rt->snap_retired && rt->snap_id == live_id && live_id != 0)
    {
        return;
    }
    free(rt->snap_request);
    rt->snap_request = NULL;
    rt->snap_id = 0;
    rt->snap_retired = false;
    if (live_id != 0)
    {
        rt->snap_id = live_id;
        rt->snap_request = live_copy;
        live_copy = NULL;
    }
    free(live_copy);
}

bool PicoAgent_BlocksReload(const PicoAgent *agent)
{
    PicoAgentRt *rt = agent ? agent->runtime : NULL;
    if (!rt)
    {
        return false;
    }
    pthread_mutex_lock(&rt->mu);
    bool blocked = PicoAgent_IsBusy(agent) || rt->busy || rt->work != PICO_WORK_IDLE ||
                   rt->event_count > 0 || rt->ask_waiting || rt->pending_count > 0 ||
                   rt->offered_tool_count > 0 || rt->stream != NULL || rt->think != NULL ||
                   rt->summary != NULL;
    pthread_mutex_unlock(&rt->mu);
    return blocked;
}

void PicoAgent_PrepareReload(PicoAgent *agent)
{
    PicoAgentRt *rt = agent ? agent->runtime : NULL;
    if (!rt || PicoAgent_BlocksReload(agent))
    {
        return;
    }
    pthread_mutex_lock(&rt->mu);
    memset(rt->tool_before_hooks, 0, sizeof(rt->tool_before_hooks));
    rt->tool_before_hook_count = 0;
    pthread_mutex_unlock(&rt->mu);
}

bool PicoAgent_RevalidateToolPolicy(const PicoApp *app, PicoAgent *agent)
{
    if (!agent)
    {
        return false;
    }
    bool valid = true;
    for (int i = 0; app && agent->allowed_tools && i < agent->allowed_tool_count; i++)
    {
        bool found = false;
        for (int t = 0; t < app->tool_count; t++)
        {
            if (app->tools[t].name && agent->allowed_tools[i] &&
                strcmp(app->tools[t].name, agent->allowed_tools[i]) == 0)
            {
                found = true;
                break;
            }
        }
        if (!found)
        {
            valid = false;
            break;
        }
    }
    agent->tool_policy_valid = valid;
    return valid;
}

static void KillToolChild(pid_t pid)
{
    if (pid <= 0)
    {
        return;
    }
    if (kill(-pid, SIGKILL) != 0)
    {
        kill(pid, SIGKILL);
    }
}

static void FreeRt(PicoAgentRt *rt)
{
    if (!rt)
    {
        return;
    }
    for (int i = 0; i < rt->event_count; i++)
    {
        free(rt->events[i].text);
        free(rt->events[i].payload);
        free(rt->events[i].tool_args);
        free(rt->events[i].tool_details);
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
    free(rt->summary);
    free(rt->work_model);
    free(rt->work_base_url);
    free(rt->work_effort);
    free(rt->work_instructions);
    free(rt->work_cache_key);
    for (int i = 0; i < rt->work_input_count; i++)
    {
        free(rt->work_input[i]);
    }
    free(rt->work_input);
    ClearOfferedTools(rt);
    free(rt->work_tool_name);
    free(rt->work_tool_args);
    free(rt->work_call_id);
    free(rt->instructions);
    free(rt->turn_provider);
    free(rt->ask_request);
    free(rt->ask_answer);
    free(rt->snap_request);
    pthread_mutex_destroy(&rt->mu);
    pthread_cond_destroy(&rt->cv);
    free(rt);
}

static void RefreshWorkerContext(PicoAgentRt *rt, const PicoApp *app, const PicoAgent *agent)
{
    if (!rt || !app || !agent)
    {
        return;
    }
    PicoAgentContext *ctx = &rt->context;
    ctx->runtime = rt;
    ctx->manager = app->agents;
    ctx->agent_id = agent->id;
    ctx->runtime_generation = agent->runtime_generation;
    snprintf(ctx->workspace, sizeof(ctx->workspace), "%s", app->workspace);
    snprintf(ctx->session_id, sizeof(ctx->session_id), "%s", agent->session_id);
    snprintf(ctx->profile, sizeof(ctx->profile), "%s", agent->profile);
    snprintf(ctx->purpose, sizeof(ctx->purpose), "%s", agent->purpose);
    ctx->safe_mode = app->safe_mode;
    ctx->auth_store = app->auth_store;
    memcpy(rt->tool_before_hooks, app->tool_before_hooks, sizeof(rt->tool_before_hooks));
    rt->tool_before_hook_count = app->tool_before_hook_count;
}

static PicoAgentRt *CreateRt(PicoApp *app, PicoAgent *agent)
{
    PicoAgentRt *rt = (PicoAgentRt *)calloc(1, sizeof(PicoAgentRt));
    if (!rt)
    {
        return NULL;
    }
    RefreshWorkerContext(rt, app, agent);
    rt->stream_msg = -1;
    pthread_mutex_init(&rt->mu, NULL);
    pthread_cond_init(&rt->cv, NULL);
    if (pthread_create(&rt->thread, NULL, WorkerMain, rt) == 0)
    {
        rt->started = true;
    }
    return rt;
}

/* True if the thread has exited (or never started) and rt can be freed. */
static bool StopRt(PicoAgentRt *rt, const struct timespec *deadline)
{
    pthread_mutex_lock(&rt->mu);
    rt->stop = true;
    rt->cancel = true;
    pid_t child = rt->tool_child;
    pthread_cond_broadcast(&rt->cv);
    if (deadline && rt->busy)
    {
        while (rt->busy)
        {
            if (pthread_cond_timedwait(&rt->cv, &rt->mu, deadline) == ETIMEDOUT)
            {
                break;
            }
        }
    }
    bool done = !rt->busy;
    pthread_mutex_unlock(&rt->mu);
    KillToolChild(child);
    return done;
}

void PicoAgent_ReapRetired(PicoAgentManager *manager)
{
    if (!manager)
    {
        return;
    }
    PicoAgentRt **pp = &manager->retired_runtimes;
    while (*pp)
    {
        PicoAgentRt *z = *pp;
        pthread_mutex_lock(&z->mu);
        bool done = !z->busy;
        pthread_mutex_unlock(&z->mu);
        if (!done)
        {
            pp = &z->zombie_next;
            continue;
        }
        *pp = z->zombie_next;
        manager->retired_count--;
        if (z->started)
        {
            pthread_join(z->thread, NULL);
        }
        FreeRt(z);
    }
}

bool PicoAgent_RetiredReferences(const PicoAgentManager *manager, PicoAgentId id)
{
    for (const PicoAgentRt *z = manager ? manager->retired_runtimes : NULL; z; z = z->zombie_next)
    {
        if (z->context.agent_id == id)
        {
            return true;
        }
    }
    return false;
}

/* Share one shutdown deadline across every retired runtime. */
bool PicoAgent_ShutdownRetired(PicoAgentManager *manager, const struct timespec *deadline)
{
    bool all_done = true;
    PicoAgentRt *z = manager ? manager->retired_runtimes : NULL;
    if (manager)
    {
        manager->retired_runtimes = NULL;
        manager->retired_count = 0;
    }
    while (z)
    {
        PicoAgentRt *next = z->zombie_next;
        z->zombie_next = NULL;
        if (StopRt(z, deadline))
        {
            if (z->started)
            {
                pthread_join(z->thread, NULL);
            }
            FreeRt(z);
        }
        else
        {
            if (z->started)
            {
                pthread_detach(z->thread);
            }
            all_done = false;
        }
        z = next;
    }
    return all_done;
}

void PicoAgent_Compact(PicoApp *app, PicoAgent *agent)
{
    if (!app || !agent || !agent->runtime || PicoAgent_IsBusy(agent) ||
        !PicoAgentManager_AcceptsNewWork(app->agents) || !agent->tool_policy_valid)
    {
        return;
    }
    StartCompact(app, agent);
}

void PicoAgent_RebindHost(PicoApp *app, PicoAgent *agent, PicoAgentManager *manager)
{
    if (!app || !agent || !agent->runtime || PicoAgent_BlocksReload(agent))
    {
        return;
    }
    agent->manager = manager;
    RefreshWorkerContext(agent->runtime, app, agent);
}

PicoAgent *PicoAgent_Create(PicoApp *app)
{
    static PicoAgentId next_id;
    PicoAgent *agent = (PicoAgent *)calloc(1, sizeof(PicoAgent));
    if (!agent)
    {
        return NULL;
    }
    agent->manager = app ? app->agents : NULL;
    agent->id = ++next_id;
    agent->runtime_generation = 1;
    agent->kind = PICO_AGENT_NORMAL;
    agent->state = PICO_AGENT_IDLE;
    agent->persistence = PICO_SESSION_EPHEMERAL;
    agent->tool_policy_valid = true;
    PicoSettings_InitAgent(app, agent);
    agent->runtime = CreateRt(app, agent);
    if (!agent->runtime)
    {
        free(agent);
        return NULL;
    }
    Pico_RandomHex(agent->runtime->cache_key, sizeof(agent->runtime->cache_key));
    return agent;
}

bool PicoAgent_DestroyBefore(PicoAgent *agent, const struct timespec *deadline)
{
    if (!agent)
    {
        return true;
    }
    PicoAgentRt *rt = agent->runtime;
    if (rt)
    {
        bool done = StopRt(rt, deadline);
        /* A worker stuck in a callback keeps the execution host alive. */
        if (!done)
        {
            if (rt->started)
            {
                pthread_detach(rt->thread);
            }
            agent->runtime = NULL;
            return false;
        }
        if (rt->started)
        {
            pthread_join(rt->thread, NULL);
        }
        FreeRt(rt);
        agent->runtime = NULL;
    }
    PicoAgent_ClearMessages(agent);
    free(agent->messages);
    free(agent->error);
    free(agent->compact_summary);
    for (int i = 0; i < agent->allowed_tool_count; i++)
    {
        free(agent->allowed_tools[i]);
    }
    free(agent->allowed_tools);
    free(agent);
    return true;
}

bool PicoAgent_Destroy(PicoAgent *agent)
{
    struct timespec deadline;
    clock_gettime(CLOCK_REALTIME, &deadline);
    deadline.tv_sec += 1;
    return PicoAgent_DestroyBefore(agent, &deadline);
}

void PicoAgent_StartTurn(PicoApp *app, PicoAgent *agent, const char *user_text)
{
    if (!app || !agent || !agent->runtime || !user_text || !user_text[0])
    {
        return;
    }
    if (PicoAgent_IsBusy(agent) || !PicoAgentManager_AcceptsNewWork(app->agents))
    {
        return;
    }
    if (!PicoAgent_RevalidateToolPolicy(app, agent))
    {
        pico_status_warn(app, "This agent's restricted tool policy references a tool that is not currently registered.");
        return;
    }
    PicoAgent_DismissError(agent);
    free(agent->runtime->instructions);
    agent->runtime->instructions = PicoSettings_LoadSystemPrompt(app);
    if (agent->kind == PICO_AGENT_SUBAGENT)
    {
        JsonBuf instructions;
        JsonBuf_Init(&instructions);
        JsonBuf_Puts(&instructions, agent->runtime->instructions ? agent->runtime->instructions : "");
        JsonBuf_Puts(&instructions, "\n\n---\nSubagent profile: ");
        JsonBuf_Puts(&instructions, agent->profile);
        JsonBuf_Puts(&instructions, "\nPurpose:\n");
        JsonBuf_Puts(&instructions, agent->purpose);
        JsonBuf_Puts(&instructions, "\n---");
        free(agent->runtime->instructions);
        agent->runtime->instructions = JsonBuf_Steal(&instructions);
    }
    PushInput(agent->runtime, BuildUserItem(user_text));
    StartLlm(app, agent);
}

void PicoAgent_Cancel(PicoAgent *agent)
{
    PicoAgentRt *rt = agent ? agent->runtime : NULL;
    if (!rt || !PicoAgent_IsBusy(agent))
    {
        return;
    }
    PicoAgentManager_CancelChildDelegation(agent->manager, agent->id);
    pthread_mutex_lock(&rt->mu);
    rt->cancel = true;
    pthread_cond_broadcast(&rt->cv);
    pthread_mutex_unlock(&rt->mu);
    PicoAgentManager_CancelDelegations(agent->manager, agent->id,
                                       agent->runtime_generation);
    rt->snap_retired = true;
}

void PicoAgent_ForceCancel(PicoApp *app, PicoAgent *agent)
{
    PicoAgentRt *old = agent ? agent->runtime : NULL;
    if (!old || !PicoAgent_IsBusy(agent))
    {
        return;
    }

    PicoAgentManager *manager = agent->manager;
    PicoAgent_ReapRetired(manager);
    if (!manager || manager->retired_count >= PICO_MAX_RETIRED_RUNTIMES)
    {
        PicoAgent_Cancel(agent);
        return;
    }

    PicoAgentManager_CancelChildDelegation(manager, agent->id);
    uint64_t next_generation = agent->runtime_generation + 1;
    PicoAgentRt *rt = CreateRt(app, agent);
    if (!rt)
    {
        PicoAgent_Cancel(agent);
        return;
    }
    agent->runtime_generation = next_generation;
    rt->context.runtime_generation = next_generation;
    PicoAgentManager_CancelDelegations(manager, agent->id,
                                       old->context.runtime_generation);

    pthread_mutex_lock(&old->mu);
    old->cancel = true;
    old->stop = true;
    old->retired = true;
    pid_t child = old->tool_child;
    old->tool_child = 0;
    pthread_cond_broadcast(&old->cv);
    pthread_mutex_unlock(&old->mu);
    KillToolChild(child);

    ApplyCancel(app, agent);

    rt->input = old->input;
    rt->input_count = old->input_count;
    rt->input_cap = old->input_cap;
    old->input = NULL;
    old->input_count = 0;
    old->input_cap = 0;
    memcpy(rt->cache_key, old->cache_key, sizeof(rt->cache_key));
    rt->instructions = old->instructions;
    old->instructions = NULL;

    old->zombie_next = manager->retired_runtimes;
    manager->retired_runtimes = old;
    manager->retired_count++;
    agent->runtime = rt;
}

void pico_tool_set_child(PicoAgentContext *ctx, pid_t pid)
{
    if (t_worker_context != PICO_WORKER_TOOL || !AgentContextActive(ctx))
    {
        return;
    }
    PicoAgentRt *rt = ctx->runtime;
    pthread_mutex_lock(&rt->mu);
    if (!rt->retired && !rt->stop && !rt->cancel)
    {
        rt->tool_child = pid;
    }
    pthread_mutex_unlock(&rt->mu);
}

static uint64_t NextAskId(void)
{
    pthread_mutex_lock(&g_ask_id_mu);
    g_ask_next_id++;
    if (g_ask_next_id == 0)
    {
        g_ask_next_id++;
    }
    uint64_t id = g_ask_next_id;
    pthread_mutex_unlock(&g_ask_id_mu);
    return id;
}

static int InvalidAskResult(char **answer_json)
{
    char *answer = Dup("{\"error\":\"invalid ask payload; fix it and try again\"}");
    if (!answer)
    {
        return PICO_ASK_FAIL;
    }
    if (answer_json)
    {
        *answer_json = answer;
    }
    else
    {
        free(answer);
    }
    return PICO_ASK_OK;
}

static bool AskRequestInvalid(const char *request_json)
{
    JsonDoc doc;
    if (JsonParse(&doc, request_json, strlen(request_json)) != 0)
    {
        return true;
    }
    bool builtin_confirm = JsonEq(&doc, JsonObjGet(&doc, 0, "type"), "confirm") &&
                           !JsonEq(&doc, JsonObjGet(&doc, 0, "ui"), "custom");
    char *message = builtin_confirm ? JsonObjStr(&doc, 0, "message") : NULL;
    bool invalid = builtin_confirm && (!message || !message[0]);
    free(message);
    JsonFree(&doc);
    return invalid;
}

static int AskFail(char **answer_json)
{
    if (answer_json)
    {
        *answer_json = NULL;
    }
    return PICO_ASK_FAIL;
}

static int AskCancel(char **answer_json)
{
    if (answer_json)
    {
        *answer_json = NULL;
    }
    return PICO_ASK_CANCEL;
}

int pico_tool_ask(PicoAgentContext *ctx, const char *request_json, char **answer_json)
{
    if (answer_json)
    {
        *answer_json = NULL;
    }
    if (!ctx || !request_json)
    {
        return PICO_ASK_FAIL;
    }
    if (strlen(request_json) > PICO_TOOL_ASK_MAX_REQUEST)
    {
        return PICO_ASK_FAIL;
    }
    if (t_worker_context != PICO_WORKER_TOOL || !AgentContextActive(ctx))
    {
        return PICO_ASK_FAIL;
    }
    bool invalid = AskRequestInvalid(request_json);
    PicoAgentRt *rt = ctx->runtime;
    pthread_mutex_lock(&rt->mu);
    if (rt->ask_waiting)
    {
        pthread_mutex_unlock(&rt->mu);
        return AskFail(answer_json);
    }
    if (rt->cancel || rt->stop)
    {
        pthread_mutex_unlock(&rt->mu);
        return AskCancel(answer_json);
    }
    if (invalid)
    {
        pthread_mutex_unlock(&rt->mu);
        return InvalidAskResult(answer_json);
    }
    char *req = Dup(request_json);
    if (!req)
    {
        pthread_mutex_unlock(&rt->mu);
        return AskFail(answer_json);
    }
    rt->ask_id = NextAskId();
    rt->ask_request = req;
    free(rt->ask_answer);
    rt->ask_answer = NULL;
    rt->ask_waiting = true;
    rt->ask_done = false;
    while (!rt->ask_done && !rt->cancel && !rt->stop)
    {
        pthread_cond_wait(&rt->cv, &rt->mu);
    }
    int rc;
    if (rt->stop || rt->cancel)
    {
        free(rt->ask_answer);
        rt->ask_answer = NULL;
        rc = PICO_ASK_CANCEL;
    }
    else
    {
        if (answer_json)
        {
            *answer_json = rt->ask_answer ? rt->ask_answer : Dup("");
        }
        else
        {
            free(rt->ask_answer);
        }
        rt->ask_answer = NULL;
        rc = PICO_ASK_OK;
    }
    free(rt->ask_request);
    rt->ask_request = NULL;
    rt->ask_waiting = false;
    rt->ask_done = false;
    rt->ask_id = 0;
    pthread_mutex_unlock(&rt->mu);
    if (rc == PICO_ASK_CANCEL)
    {
        return AskCancel(answer_json);
    }
    return rc;
}

bool PicoAgent_PendingAsk(const PicoAgent *agent, PicoToolAsk *out)
{
    PicoAgentRt *rt = agent ? agent->runtime : NULL;
    if (!rt || !out || rt->snap_id == 0 || !rt->snap_request || rt->snap_retired)
    {
        return false;
    }
    out->id = rt->snap_id;
    out->agent_id = agent->id;
    out->profile = agent->profile;
    out->purpose = agent->purpose;
    out->request_json = rt->snap_request;
    return true;
}

bool PicoAgent_AnswerAsk(PicoAgent *agent, uint64_t id, const char *answer_json)
{
    if (!agent || id == 0)
    {
        return false;
    }
    const char *src = answer_json ? answer_json : "";
    if (strlen(src) > PICO_TOOL_ASK_MAX_ANSWER)
    {
        return false;
    }
    PicoAgentRt *rt = agent->runtime;
    if (!rt)
    {
        return false;
    }
    pthread_mutex_lock(&rt->mu);
    if (!rt->ask_waiting || rt->ask_done || rt->ask_id != id || rt->cancel || rt->stop)
    {
        pthread_mutex_unlock(&rt->mu);
        return false;
    }
    char *ans = Dup(src);
    if (!ans)
    {
        pthread_mutex_unlock(&rt->mu);
        return false;
    }
    rt->ask_answer = ans;
    rt->ask_done = true;
    pthread_cond_broadcast(&rt->cv);
    pthread_mutex_unlock(&rt->mu);
    if (rt->snap_id == id)
    {
        rt->snap_retired = true;
    }
    return true;
}

void PicoAgent_DismissError(PicoAgent *agent)
{
    if (!agent)
    {
        return;
    }
    if (agent->state == PICO_AGENT_ERROR)
    {
        agent->state = PICO_AGENT_IDLE;
    }
    free(agent->error);
    agent->error = NULL;
}

void PicoAgent_Pump(PicoApp *app, PicoAgent *agent)
{
    PicoAgentRt *rt = agent ? agent->runtime : NULL;
    if (!rt)
    {
        return;
    }
    PublishAskSnapshot(rt);

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
    char *summary = rt->summary;
    size_t summary_len = rt->summary_len;
    int summary_steps = rt->summary_steps;
    rt->summary = NULL;
    rt->summary_len = 0;
    rt->summary_cap = 0;
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
        AppendMessageText(app, agent, rt->stream_msg, stream, stream_len);
        rt->stream_dirty = true;
    }
    free(stream);

    if (think && think_len && rt->stream_msg >= 0)
    {
        TraceAppendThink(app, agent, rt->stream_msg, think, think_len);
    }
    free(think);

    if (summary && summary_len && rt->stream_msg >= 0)
    {
        TraceSetThinkSummary(app, agent, rt->stream_msg, summary, summary_len, summary_steps);
    }
    free(summary);

    if (status[0])
    {
        char line[192];
        snprintf(line, sizeof(line), "Calling `%s`…", status);
        SetActivity(app, agent, line);
    }

    if (rt->stream_dirty)
    {
        ReparseMessage(app, agent, rt->stream_msg);
        rt->stream_dirty = false;
    }

    for (int i = 0; i < event_count; i++)
    {
        PicoAgentEv *ev = &events[i];
        switch (ev->type)
        {
        case PICO_AEV_LLM_DONE:
            OnLlmDone(app, agent, ev);
            break;
        case PICO_AEV_LLM_FAIL:
            SetErrorState(app, agent, ev->text ? ev->text : "LLM request failed");
            break;
        case PICO_AEV_LLM_CANCEL:
            ApplyCancel(app, agent);
            break;
        case PICO_AEV_TOOL_START:
            OnToolStart(app, agent, ev);
            break;
        case PICO_AEV_TOOL_DONE:
            OnToolDone(app, agent, ev, false);
            break;
        case PICO_AEV_TOOL_FAIL:
            OnToolDone(app, agent, ev, true);
            break;
        }
        free(ev->text);
        free(ev->payload);
        free(ev->tool_args);
        free(ev->tool_details);
    }
    free(events);
}

const char *PicoAgent_CacheKey(const PicoAgent *agent)
{
    return agent && agent->runtime ? agent->runtime->cache_key : "";
}

void PicoAgent_SetCacheKey(PicoAgent *agent, const char *key)
{
    if (agent && agent->runtime && key)
    {
        snprintf(agent->runtime->cache_key, sizeof(agent->runtime->cache_key), "%s", key);
    }
}

void PicoAgent_RotateCacheKey(PicoAgent *agent)
{
    if (agent && agent->runtime)
    {
        Pico_RandomHex(agent->runtime->cache_key, sizeof(agent->runtime->cache_key));
    }
}

void PicoAgent_ClearInput(PicoAgent *agent)
{
    PicoAgentRt *rt = agent ? agent->runtime : NULL;
    if (!rt) return;
    for (int i = 0; i < rt->input_count; i++)
    {
        free(rt->input[i].json);
        rt->input[i].json = NULL;
    }
    rt->input_count = 0;
}

void PicoAgent_PushHistoryUser(PicoAgent *agent, const char *text)
{
    if (agent && agent->runtime) PushInput(agent->runtime, BuildUserItem(text));
}

void PicoAgent_PushHistoryAssistant(PicoAgent *agent, const char *text)
{
    if (agent && agent->runtime) PushInput(agent->runtime, BuildAssistantItem(text));
}

void PicoAgent_PushHistoryFunctionCall(PicoAgent *agent, const char *call_id, const char *name, const char *args)
{
    if (agent && agent->runtime) PushInput(agent->runtime, BuildToolCall(call_id, name, args));
}

void PicoAgent_PushHistoryFunctionOutput(PicoAgent *agent, const char *call_id, const char *name,
                                         const char *output, bool is_error)
{
    if (agent && agent->runtime) PushFunctionOutput(agent->runtime, call_id, name, output, is_error);
}

char *PicoAgent_BuildInstructions(PicoApp *app, PicoAgent *agent)
{
    (void)agent;
    if (!app) return JsonDup("");
    char *base = PicoSettings_LoadSystemPrompt(app);
    char *instr = NULL;
    PicoTool *tools = NULL;
    int tool_count = 0;
    RunLlmHooks(app, agent, false, true, base, &instr, &tools, &tool_count);
    free(base);
    free(tools);
    return instr ? instr : JsonDup("");
}

static bool AgentContextActive(const PicoAgentContext *ctx)
{
    if (!ctx || t_agent_context != ctx || t_worker_rt != ctx->runtime || !ctx->runtime)
    {
        return false;
    }
    pthread_mutex_lock(&ctx->runtime->mu);
    bool active = !ctx->runtime->retired;
    pthread_mutex_unlock(&ctx->runtime->mu);
    return active;
}

PicoAgentId pico_agent_context_id(const PicoAgentContext *ctx)
{
    return AgentContextActive(ctx) ? ctx->agent_id : 0;
}

uint64_t pico_agent_context_generation(const PicoAgentContext *ctx)
{
    return AgentContextActive(ctx) ? ctx->runtime_generation : 0;
}

const char *pico_agent_context_workspace(const PicoAgentContext *ctx)
{
    return AgentContextActive(ctx) ? ctx->workspace : "";
}

const char *pico_agent_context_session_id(const PicoAgentContext *ctx)
{
    return AgentContextActive(ctx) ? ctx->session_id : "";
}

const char *pico_agent_context_profile(const PicoAgentContext *ctx)
{
    return AgentContextActive(ctx) ? ctx->profile : "";
}

const char *pico_agent_context_purpose(const PicoAgentContext *ctx)
{
    return AgentContextActive(ctx) ? ctx->purpose : "";
}

bool pico_agent_context_safe_mode(const PicoAgentContext *ctx)
{
    return AgentContextActive(ctx) && ctx->safe_mode;
}

bool pico_agent_context_cancelled(const PicoAgentContext *ctx)
{
    if (!AgentContextActive(ctx))
    {
        return true;
    }
    return WorkerIsCancelled(ctx->runtime);
}

PicoAgentManager *PicoAgentContext_Manager(const PicoAgentContext *ctx)
{
    return AgentContextActive(ctx) ? ctx->manager : NULL;
}

struct PicoAuthStore *PicoAgentContext_AuthStore(const PicoAgentContext *ctx)
{
    if (!ctx || t_agent_context != ctx || t_worker_rt != ctx->runtime || !ctx->runtime)
    {
        return NULL;
    }
    return ctx->auth_store;
}

bool PicoAgentContext_LockIfLive(const PicoAgentContext *ctx)
{
    if (!ctx || t_agent_context != ctx || t_worker_rt != ctx->runtime || !ctx->runtime)
    {
        return false;
    }
    pthread_mutex_lock(&ctx->runtime->mu);
    if (ctx->runtime->retired || ctx->runtime->cancel || ctx->runtime->stop)
    {
        pthread_mutex_unlock(&ctx->runtime->mu);
        return false;
    }
    return true;
}

void PicoAgentContext_UnlockLive(const PicoAgentContext *ctx)
{
    if (ctx && ctx->runtime)
    {
        pthread_mutex_unlock(&ctx->runtime->mu);
    }
}

PicoAgentId pico_agent_id(const PicoAgent *agent)
{
    return agent ? agent->id : 0;
}

bool pico_agent_info_snapshot(const PicoAgent *agent, PicoAgentInfo *out)
{
    if (!agent || !out)
    {
        return false;
    }
    PicoAgent_CopyInfo(agent, out);
    return true;
}

void PicoAgent_CopyInfo(const PicoAgent *agent, PicoAgentInfo *out)
{
    if (!out) return;
    memset(out, 0, sizeof(*out));
    if (!agent) return;
    out->id = agent->id; out->parent_id = agent->parent_id; out->kind = agent->kind;
    out->state = agent->state; out->depth = agent->depth;
    snprintf(out->session_id, sizeof(out->session_id), "%s", agent->session_id);
    snprintf(out->profile, sizeof(out->profile), "%s", agent->profile);
    snprintf(out->purpose, sizeof(out->purpose), "%s", agent->purpose);
    snprintf(out->model, sizeof(out->model), "%s", agent->model);
    snprintf(out->effort, sizeof(out->effort), "%s", agent->effort);
    snprintf(out->activity, sizeof(out->activity), "%s", agent->activity);
    out->persistence = agent->persistence;
    out->busy = PicoAgent_IsBusy(agent);
    out->cancelling = PicoAgent_CancelRequested(agent);
    out->resumable = agent->persistence == PICO_SESSION_DURABLE && agent->session_id[0];
}
