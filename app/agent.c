#define _POSIX_C_SOURCE 200809L

#include "agent.h"
#include "workspace_internal.h"
#include "canonical.h"
#include "json.h"
#include "path.h"
#include "session.h"
#include "settings.h"
#include "subagent_config.h"
#include "usage.h"
#include "host_internal.h"
#include "trace_group.h"

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

#define PICO_MAX_PENDING_CALLS PICO_MAX_PARALLEL_TOOLS

typedef struct PicoInputItem {
    char *json;
} PicoInputItem;

typedef struct PicoPendingCall {
    char *call_id;
    char *name;
    char *arguments;
    char *item_id;
    PicoToolCallProgress progress;
    bool completed;
    bool parallel; /* eligibility snapshotted before any batch callbacks run */
} PicoPendingCall;

/* A tool call whose name is known while its arguments are still streaming.
 * Worker side is mutex-protected; the row mirror lives on the main thread. */
typedef struct PicoProvStream {
    int call_index; /* provider wire index */
    char *call_id;  /* NULL until the provider sends it */
    char *name;     /* NULL until known */
    size_t args_bytes;
    bool dirty;
} PicoProvStream;

typedef struct PicoProvRow {
    int call_index;
    char *call_id;
    char *name;
    size_t args_bytes;
    int msg_idx;   /* -1 until the trace row exists */
    int trace_idx;
    bool reconciled;
} PicoProvRow;

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
    int call_index; /* tool events only */
    int tokens;
    int cached;
    bool executed;
    bool is_error;
} PicoAgentEv;

typedef enum PicoWorkKind {
    PICO_WORK_IDLE = 0,
    PICO_WORK_LLM,
} PicoWorkKind;

struct PicoAgentContext {
    PicoAgentRt *runtime;
    struct PicoToolJob *job; /* NULL for provider callbacks */
    PicoWorkspace *workspace_owner;
    PicoAgentId agent_id;
    uint64_t runtime_generation;
    PicoWorkspaceId workspace_id;
    uint64_t registration_generation;
    char workspace[4096];
    char session_id[40];
    char profile[65];
    char purpose[1025];
    const char *tool_call_id;
    bool safe_mode;
    struct PicoAuthStore *auth_store;
};

/* One callback context, ask, and process group per invocation. Jobs remain
 * owned by the runtime until their thread is joined on the main thread. */
typedef struct PicoToolJob {
    PicoAgentContext context;
    pthread_t thread;
    bool started;
    int call_index;
    char *name;
    char *args;
    char *call_id;
    PicoToolFn run;
    void *state;
    pid_t child;
    uint64_t ask_id;
    char *ask_request;
    char *ask_answer;
    bool ask_waiting;
    bool ask_done;
    uint64_t snap_id;
    char *snap_request;
    bool snap_retired;
} PicoToolJob;

struct PicoAgentRt {
    /* Heap-owned worker services and callback-scoped public context. */
    PicoAgentContext context;
    PicoRegistrationGeneration *registration;
    PicoToolBeforeEntry tool_before_hooks[PICO_MAX_TOOL_HOOKS];
    int tool_before_hook_count;
    pthread_t thread;
    pthread_mutex_t mu;
    pthread_cond_t cv;
    bool stop;
    bool started;
    bool busy;
    bool cancel;
    bool retired;
    PicoToolJob *jobs[PICO_MAX_PENDING_CALLS];
    int active_tools; /* worker count; protected by mu */
    int running_tools; /* unfinished main-thread completions */
    int max_parallel_tools;
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
    bool work_vision;
    PicoTool *work_tools;
    int work_tool_count;
    void *work_stream_state;

    char *stream;
    char provider_status[256];
    bool provider_status_dirty;
    size_t stream_len;
    size_t stream_cap;
    char *think;
    size_t think_len;
    size_t think_cap;
    char *turn_think;
    size_t turn_think_len;
    size_t turn_think_cap;
    char *summary;
    size_t summary_len;
    size_t summary_cap;
    char **summary_parts;
    int summary_part_count;
    int summary_part_cap;
    int summary_steps;
    bool summary_new_step;

    PicoProvStream *prov; /* worker-side, under mu */
    int prov_count;
    int prov_cap;
    bool prov_dirty;
    PicoProvRow *prov_rows; /* main thread only */
    int prov_row_count;
    int prov_row_cap;

    PicoAgentEv *events;
    int event_count;
    int event_cap;

    PicoInputItem *input;
    int input_count;
    int input_cap;
    char cache_key[33];
    char *instructions;
    bool compacting;
    bool compact_no_tools;

    PicoPendingCall pending[PICO_MAX_PENDING_CALLS];
    int pending_count;
    PicoTool *offered_tools;
    int offered_tool_count;

    int stream_msg;
    bool stream_dirty;
    double action_t0;
};

static __thread PicoAgentRt *t_worker_rt;
static __thread PicoAgentContext *t_agent_context;

typedef enum PicoWorkerContext {
    PICO_WORKER_NONE = 0,
    PICO_WORKER_PROVIDER,
    PICO_WORKER_TOOL,
} PicoWorkerContext;
static __thread PicoWorkerContext t_worker_context;

static void SetErrorState(PicoHost *app, PicoAgent *agent, const char *msg);
static void SweepProvisionalRows(PicoAgent *agent, PicoAgentRt *rt);
static void ClearProvStream(PicoAgentRt *rt);
static void FinishAssistantHistory(PicoHost *app, PicoAgent *agent, const char *thinking,
                                   const char *signature);
static void RefreshWorkerContext(PicoAgentRt *rt, const PicoHost *app, const PicoAgent *agent);
static char *BuildUserItem(const char *text, const char *parts_json);
static void *WorkerMain(void *arg);
static bool AgentContextActive(const PicoAgentContext *ctx);
static void *ToolWorkerMain(void *arg);
static void JoinToolJob(PicoAgentRt *rt, int index);
static void KillToolChildren(PicoAgentRt *rt);

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
        free(rt->pending[i].item_id);
        memset(&rt->pending[i], 0, sizeof(rt->pending[i]));
    }
    rt->pending_count = 0;
    rt->running_tools = 0;
}

/* Completing a work item updates the active worker count in the same critical
 * section as its terminal event. The main thread separately tracks completions
 * still to apply, and joins each job before freeing it or starting a new batch. */
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
                if (t_agent_context && t_agent_context->job)
                {
                    rt->active_tools--;
                }
                rt->busy = rt->active_tools != 0;
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
    ev->call_index = t_agent_context && t_agent_context->job ? t_agent_context->job->call_index : -1;
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
        if (t_agent_context && t_agent_context->job)
        {
            rt->active_tools--;
        }
        rt->busy = rt->active_tools != 0;
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

static void ClearSummaryParts(PicoAgentRt *rt)
{
    if (!rt)
    {
        return;
    }
    if (rt->summary_parts)
    {
        for (int i = 0; i < rt->summary_part_count; i++)
        {
            free(rt->summary_parts[i]);
        }
        free(rt->summary_parts);
    }
    rt->summary_parts = NULL;
    rt->summary_part_count = 0;
    rt->summary_part_cap = 0;
}

static void SummaryPartSet(PicoAgentRt *rt, int index, const char *s, size_t n)
{
    if (!rt || index < 0)
    {
        return;
    }
    if (index >= rt->summary_part_cap)
    {
        int cap = rt->summary_part_cap == 0 ? 4 : rt->summary_part_cap;
        while (cap <= index)
        {
            cap *= 2;
        }
        char **next = (char **)realloc(rt->summary_parts, (size_t)cap * sizeof(char *));
        if (!next)
        {
            return;
        }
        for (int i = rt->summary_part_cap; i < cap; i++)
        {
            next[i] = NULL;
        }
        rt->summary_parts = next;
        rt->summary_part_cap = cap;
    }
    if (index >= rt->summary_part_count)
    {
        rt->summary_part_count = index + 1;
    }
    char *copy = (char *)malloc(n + 1);
    if (!copy)
    {
        return;
    }
    if (s && n)
    {
        memcpy(copy, s, n);
    }
    copy[n] = '\0';
    free(rt->summary_parts[index]);
    rt->summary_parts[index] = copy;
}

static void SetActivity(PicoHost *app, PicoAgent *agent, const char *msg)
{
    snprintf(agent->activity, sizeof(agent->activity), "%s", msg ? msg : "");
}

static char *EncodeResult(const PicoLlmResult *r)
{
    JsonBuf b;
    JsonBuf_Init(&b);
    JsonBuf_Puts(&b, "{\"items\":[");
    int n = r ? r->item_count : 0;
    for (int i = 0; i < n; i++)
    {
        if (i)
        {
            JsonBuf_Putc(&b, ',');
        }
        char *json = pico_canonical_item_json(&r->items[i]);
        JsonBuf_Puts(&b, json ? json : "{}");
        free(json);
    }
    JsonBuf_Puts(&b, "]}");
    return JsonBuf_Steal(&b);
}

static int ResultItems(const char *payload, JsonDoc *doc)
{
    if (!payload || !doc)
    {
        return -1;
    }
    memset(doc, 0, sizeof(*doc));
    if (JsonParse(doc, payload, strlen(payload)) != 0)
    {
        return -1;
    }
    int items = JsonObjGet(doc, 0, "items");
    return JsonIsArray(doc, items) ? items : -1;
}

static int ResultCallCount(const char *payload)
{
    JsonDoc doc;
    int items = ResultItems(payload, &doc);
    if (items < 0)
    {
        return 0;
    }
    int n = JsonArrayLen(&doc, items);
    int count = 0;
    for (int i = 0; i < n; i++)
    {
        int item = JsonArrayAt(&doc, items, i);
        if (JsonEq(&doc, JsonObjGet(&doc, item, "type"), "tool_call"))
        {
            count++;
        }
    }
    JsonFree(&doc);
    return count;
}

static char *ResultAssistantText(const char *payload)
{
    JsonDoc doc;
    int items = ResultItems(payload, &doc);
    if (items < 0)
    {
        return NULL;
    }
    JsonBuf b;
    JsonBuf_Init(&b);
    JsonBuf refusal;
    JsonBuf_Init(&refusal);
    int n = JsonArrayLen(&doc, items);
    for (int i = 0; i < n; i++)
    {
        int item = JsonArrayAt(&doc, items, i);
        if (!JsonEq(&doc, JsonObjGet(&doc, item, "type"), "assistant"))
        {
            continue;
        }
        PicoLlmPart *parts = NULL;
        int part_n = 0;
        if (!pico_canonical_parse_parts(&doc, item, &parts, &part_n))
        {
            continue;
        }
        char *text = pico_canonical_compact_text(parts, part_n);
        if (text && text[0])
        {
            JsonBuf_Puts(&b, text);
        }
        else
        {
            for (int p = 0; p < part_n; p++)
            {
                if (parts[p].kind == PICO_LLM_PART_REFUSAL && parts[p].text)
                {
                    JsonBuf_Puts(&refusal, parts[p].text);
                }
            }
        }
        free(text);
        pico_canonical_free_parts(parts, part_n);
    }
    JsonFree(&doc);
    if (b.len)
    {
        JsonBuf_Free(&refusal);
        return JsonBuf_Steal(&b);
    }
    JsonBuf_Free(&b);
    return JsonBuf_Steal(&refusal);
}

static void ProvStreamDirty(PicoProvStream *entry)
{
    entry->dirty = true;
}

static PicoProvStream *ProvStreamFor(PicoAgentRt *rt, int call_index)
{
    /* rt->mu held */
    for (int i = 0; i < rt->prov_count; i++)
    {
        if (rt->prov[i].call_index == call_index)
        {
            return &rt->prov[i];
        }
    }
    if (rt->prov_count >= PICO_MAX_PENDING_CALLS)
    {
        return NULL;
    }
    if (rt->prov_count >= rt->prov_cap)
    {
        int cap = rt->prov_cap ? rt->prov_cap * 2 : 4;
        PicoProvStream *next =
            (PicoProvStream *)realloc(rt->prov, (size_t)cap * sizeof(PicoProvStream));
        if (!next)
        {
            return NULL;
        }
        rt->prov = next;
        rt->prov_cap = cap;
    }
    PicoProvStream *entry = &rt->prov[rt->prov_count++];
    memset(entry, 0, sizeof(*entry));
    entry->call_index = call_index;
    return entry;
}

static void DeltaCb(void *user, const PicoLlmDelta *delta)
{
    PicoAgentRt *rt = (PicoAgentRt *)user;
    if (!delta)
    {
        return;
    }
    PicoLlmDeltaKind kind = delta->kind;
    const char *s = delta->text;
    size_t n = delta->len;
    pthread_mutex_lock(&rt->mu);
    if (kind == PICO_LLM_DELTA_STATUS)
    {
        size_t len = s ? (n < sizeof(rt->provider_status) - 1 ? n : sizeof(rt->provider_status) - 1) : 0;
        if (len) memcpy(rt->provider_status, s, len);
        rt->provider_status[len] = '\0';
        rt->provider_status_dirty = true;
        pthread_mutex_unlock(&rt->mu);
        return;
    }
    if (rt->provider_status[0])
    {
        rt->provider_status[0] = '\0';
        rt->provider_status_dirty = true;
    }
    if (kind == PICO_LLM_DELTA_TOOL_CALL_BEGIN)
    {
        PicoProvStream *entry = ProvStreamFor(rt, delta->call_index);
        if (entry)
        {
            if (delta->call_id && delta->call_id[0] &&
                (!entry->call_id || strcmp(entry->call_id, delta->call_id) != 0))
            {
                free(entry->call_id);
                entry->call_id = Dup(delta->call_id);
            }
            if (delta->name && delta->name[0] &&
                (!entry->name || strcmp(entry->name, delta->name) != 0))
            {
                free(entry->name);
                entry->name = Dup(delta->name);
            }
            ProvStreamDirty(entry);
            rt->prov_dirty = true;
        }
        pthread_mutex_unlock(&rt->mu);
        return;
    }
    if (kind == PICO_LLM_DELTA_TOOL_CALL_ARGS)
    {
        PicoProvStream *entry = ProvStreamFor(rt, delta->call_index);
        if (entry)
        {
            entry->args_bytes += n;
            ProvStreamDirty(entry);
            rt->prov_dirty = true;
        }
        pthread_mutex_unlock(&rt->mu);
        return;
    }
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
            if (rt->summary_steps > 0)
            {
                SummaryPartSet(rt, rt->summary_steps - 1, s, n);
            }
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
        BufAppend(&rt->turn_think, &rt->turn_think_len, &rt->turn_think_cap, s, n);
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

/* Scheduling has already used the original delegation identity. A before hook
 * may change the briefing, but cannot redirect that call to another profile or
 * session, even if another hook would later restore the original identity. */
static bool SameDelegationIdentity(const char *before, const char *after)
{
    JsonDoc a, b;
    if (!before || !after || JsonParse(&a, before, strlen(before)) != 0)
    {
        return false;
    }
    if (JsonParse(&b, after, strlen(after)) != 0)
    {
        JsonFree(&a);
        return false;
    }
    bool same = JsonIsObject(&a, 0) && JsonIsObject(&b, 0);
    const char *keys[] = {"profile", "session_id"};
    for (int i = 0; same && i < 2; i++)
    {
        int x = JsonObjGet(&a, 0, keys[i]);
        int y = JsonObjGet(&b, 0, keys[i]);
        if (x < 0 || y < 0)
        {
            same = x < 0 && y < 0;
            continue;
        }
        int xs = JsonTokStart(&a, x), ys = JsonTokStart(&b, y);
        bool xquoted = xs > 0 && before[xs - 1] == '"';
        bool yquoted = ys > 0 && after[ys - 1] == '"';
        char *xvalue = JsonStrDup(&a, x);
        char *yvalue = JsonStrDup(&b, y);
        same = xquoted == yquoted && xvalue && yvalue && strcmp(xvalue, yvalue) == 0;
        free(xvalue);
        free(yvalue);
    }
    JsonFree(&a);
    JsonFree(&b);
    return same;
}

static char *RunToolBeforeHooks(PicoToolJob *job, const char *name, const char *call_id, char **args_inout,
                                bool *denied)
{
    PicoAgentRt *rt = job->context.runtime;
    *denied = false;
    char *args = *args_inout;
    if (!args)
    {
        args = Dup("{}");
        *args_inout = args;
    }
    for (int i = 0; i < rt->tool_before_hook_count; i++)
    {
        if (!rt->tool_before_hooks[i].fn)
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
        rt->tool_before_hooks[i].fn(&job->context, &ev, rt->tool_before_hooks[i].state);
        if (ev.args_json_out)
        {
            if (strcmp(name, "subagent") == 0 && !SameDelegationIdentity(args, ev.args_json_out))
            {
                if (args != ev.args_json_out)
                {
                    free(ev.args_json_out);
                }
                free(ev.result);
                *denied = true;
                return Dup("subagent: before-tool hooks cannot change profile or session_id");
            }
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

static char *RunToolAfterHooks(PicoAgentRt *rt, const char *name, const char *call_id,
                               const char *args, const char *output, const char *details_json,
                               bool executed, bool is_error)
{
    char *cur = Dup(output ? output : "");
    PicoWorkspace *workspace = rt ? rt->context.workspace_owner : NULL;
    const PicoRegistrationGeneration *registration = rt ? rt->registration : NULL;
    if (!workspace || !registration)
    {
        return cur;
    }
    for (int i = 0; i < registration->tool_after_hook_count; i++)
    {
        if (WorkerIsCancelled(rt))
        {
            break;
        }
        if (!registration->tool_after_hooks[i].fn)
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
        registration->tool_after_hooks[i].fn(workspace, rt->context.agent_id, &ev,
                                             registration->tool_after_hooks[i].state);
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

static void RunLlmHooks(PicoAgentRt *rt, PicoAgent *agent, bool compact, bool include_tools,
                        const char *base, char **instructions, PicoTool **tools, int *tool_count)
{
    char *instr = Dup(base);
    PicoTool eligible[PICO_MAX_TOOLS];
    int ntools = 0;
    PicoWorkspace *workspace = rt ? rt->context.workspace_owner : NULL;
    const PicoRegistrationGeneration *registration = rt ? rt->registration : NULL;
    for (int i = 0; registration && i < registration->tool_count && ntools < PICO_MAX_TOOLS; i++)
    {
        if (AgentAllowsTool(agent, registration->tools[i].name))
        {
            eligible[ntools++] = registration->tools[i];
        }
    }
    bool exclude[PICO_MAX_TOOLS];
    memset(exclude, 0, sizeof(exclude));
    /* Filtering pass: collect exclusions so every hook sees the final catalog. */
    for (int i = 0; registration && i < registration->llm_hook_count; i++)
    {
        if (!registration->llm_hooks[i].fn)
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
        registration->llm_hooks[i].fn(workspace, agent ? agent->id : 0, &ev,
                                       registration->llm_hooks[i].state);
        free(ev.extra_instructions);
    }
    /* Instructions pass: extras go under one heading; later hooks see the section. */
    bool extras = false;
    for (int i = 0; registration && i < registration->llm_hook_count; i++)
    {
        if (!registration->llm_hooks[i].fn)
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
        registration->llm_hooks[i].fn(workspace, agent ? agent->id : 0, &ev,
                                       registration->llm_hooks[i].state);
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

static char *ValidateLlmResult(const PicoLlmResult *result)
{
    if (!result || result->item_count < 0 || (result->item_count > 0 && !result->items))
    {
        return Dup("provider returned a malformed result item array");
    }
    int call_count = 0;
    for (int i = 0; i < result->item_count; i++)
    {
        const PicoLlmItem *item = &result->items[i];
        if (item->kind == PICO_LLM_ITEM_ASSISTANT)
        {
            if (item->part_count < 0 || (item->part_count > 0 && !item->parts))
            {
                return Dup("provider returned a malformed assistant parts array");
            }
            for (int p = 0; p < item->part_count; p++)
            {
                const PicoLlmPart *part = &item->parts[p];
                if (part->kind == PICO_LLM_PART_TEXT || part->kind == PICO_LLM_PART_REFUSAL)
                {
                    if (!part->text)
                    {
                        return Dup("provider returned a malformed text part");
                    }
                }
                else if (part->kind == PICO_LLM_PART_IMAGE || part->kind == PICO_LLM_PART_AUDIO)
                {
                    if ((!part->path || !part->path[0]) && (!part->url || !part->url[0]))
                    {
                        return Dup("provider returned a malformed media part");
                    }
                }
                else
                {
                    return Dup("provider returned an unsupported result part kind");
                }
            }
            continue;
        }
        if (item->kind != PICO_LLM_ITEM_TOOL_CALL)
        {
            return Dup("provider returned an unsupported result item kind");
        }
        call_count++;
        if (call_count > PICO_MAX_PENDING_CALLS)
        {
            return Dup("provider returned too many or an invalid number of tool calls");
        }
        if (!item->call_id || !item->call_id[0])
        {
            return Dup("provider returned a tool call with an empty call id");
        }
        if (!item->name || !item->name[0] || !item->arguments)
        {
            return Dup("provider returned a malformed tool call");
        }
        for (int j = 0; j < i; j++)
        {
            const PicoLlmItem *prev = &result->items[j];
            if (prev->kind == PICO_LLM_ITEM_TOOL_CALL && prev->call_id &&
                strcmp(prev->call_id, item->call_id) == 0)
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
        bool vision = rt->work_vision;
        PicoTool *work_tools = rt->work_tools;
        int work_tool_count = rt->work_tool_count;
        void *stream_state = rt->work_stream_state;
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
        rt->work_vision = false;
        rt->work_tools = NULL;
        rt->work_tool_count = 0;
        rt->work_stream_state = NULL;
        pthread_mutex_unlock(&rt->mu);

        if (kind == PICO_WORK_LLM)
        {
            pthread_mutex_lock(&rt->mu);
            rt->provider_status[0] = '\0';
            rt->provider_status_dirty = false;
            free(rt->summary);
            rt->summary = NULL;
            rt->summary_len = 0;
            rt->summary_cap = 0;
            ClearSummaryParts(rt);
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
            turn.vision = vision;
            turn.input_json = (const char *const *)input;
            turn.input_count = input_count;
            turn.tools = work_tools;
            turn.tool_count = include_tools ? work_tool_count : 0;
            PicoLlmResult result;
            memset(&result, 0, sizeof(result));
            t_worker_context = PICO_WORKER_PROVIDER;
            t_agent_context = &rt->context;
            int rc = stream_fn ? stream_fn(&rt->context, &turn, CancelCb, DeltaCb, rt, &result,
                                           stream_state)
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
                char *call_error = ValidateLlmResult(&result);
                if (call_error)
                {
                    PostEvent(rt, PICO_AEV_LLM_FAIL, call_error, NULL, 0, 0);
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
    }
    return NULL;
}

static void *ToolWorkerMain(void *arg)
{
    PicoToolJob *job = arg;
    PicoAgentRt *rt = job->context.runtime;
    t_worker_rt = rt;
    t_worker_context = PICO_WORKER_TOOL;
    t_agent_context = &job->context;
    bool denied = false;
    char *denial = RunToolBeforeHooks(job, job->name, job->call_id, &job->args, &denied);
    PicoToolResult result = {0};
    bool executed = false;
    if (WorkerIsCancelled(rt))
    {
        result.output = Dup("(interrupted)");
        result.is_error = true;
    }
    else if (denied)
    {
        result.output = denial ? denial : Dup("User denied this tool.");
        denial = NULL;
        result.is_error = true;
    }
    else
    {
        PostEventEx(rt, PICO_AEV_TOOL_START, Dup(job->name), Dup(job->args),
                    NULL, NULL, 0, 0, false, false, false);
        job->run(&job->context, job->args, &result, job->state);
        executed = true;
        bool valid = false;
        result.details_json = ValidateToolDetails(result.details_json, &valid);
        if (!valid)
        {
            free(result.output);
            result.output = Dup("tool returned invalid details");
            result.is_error = true;
        }
    }
    free(denial);
    PostEventEx(rt, PICO_AEV_TOOL_DONE, result.output ? result.output : Dup(""), Dup(job->call_id),
                Dup(job->args), result.details_json, 0, 0, true, executed, result.is_error);
    t_agent_context = NULL;
    t_worker_context = PICO_WORKER_NONE;
    t_worker_rt = NULL;
    return NULL;
}

static void RunContextHooks(PicoAgentRt *rt, PicoAgent *agent, bool compact,
                            const PicoTool *tools, int tool_count,
                            char ***input_inout, int *count_inout)
{
    char **input = *input_inout;
    int base_count = *count_inout;
    char *extras[PICO_MAX_CONTEXT_HOOKS];
    int extra_count = 0;
    memset(extras, 0, sizeof(extras));
    PicoWorkspace *workspace = rt ? rt->context.workspace_owner : NULL;
    const PicoRegistrationGeneration *registration = rt ? rt->registration : NULL;
    if (!workspace || !registration)
    {
        return;
    }

    for (int i = 0; i < registration->context_hook_count; i++)
    {
        if (!registration->context_hooks[i].fn)
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
        registration->context_hooks[i].fn(workspace, agent ? agent->id : 0, &ev,
                                           registration->context_hooks[i].state);
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
    int appended = 0;
    for (int i = 0; i < extra_count; i++)
    {
        char *text = pico_canonical_context_text(extras[i]);
        free(extras[i]);
        if (text)
        {
            input[base_count + appended++] = text;
        }
    }
    *input_inout = input;
    *count_inout = base_count + appended;
}

static bool InputHasContextItem(char **input, int count)
{
    for (int i = 0; i < count; i++)
    {
        if (!input[i] || !input[i][0])
        {
            continue;
        }
        JsonDoc doc;
        memset(&doc, 0, sizeof(doc));
        if (JsonParse(&doc, input[i], strlen(input[i])) != 0)
        {
            continue;
        }
        bool match = JsonEq(&doc, JsonObjGet(&doc, 0, "type"), "context");
        JsonFree(&doc);
        if (match)
        {
            return true;
        }
    }
    return false;
}

static bool QueueLlm(PicoHost *app, PicoAgent *agent, bool compact, bool include_tools)
{
    PicoAgentRt *rt = agent->runtime;
    RefreshWorkerContext(rt, app, agent);
    PicoModel *m = PicoSettings_ActiveModel(agent);
    if (!m || !m->provider[0])
    {
        SetErrorState(app, agent, "Active model has no provider. Set `provider` on the model in settings.json.");
        return false;
    }
    const PicoProvider *p = NULL;
    for (int i = 0; rt->registration && i < rt->registration->provider_count; i++)
    {
        if (rt->registration->providers[i].name &&
            strcmp(rt->registration->providers[i].name, m->provider) == 0)
        {
            p = &rt->registration->providers[i];
            break;
        }
    }
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
            if (!input[i])
            {
                for (int j = 0; j < i; j++)
                {
                    free(input[j]);
                }
                free(input);
                SetErrorState(app, agent, "out of memory");
                return false;
            }
        }
    }

    char *instructions = NULL;
    PicoTool *tools = NULL;
    int tool_count = 0;
    RunLlmHooks(rt, agent, compact, include_tools, rt->instructions, &instructions, &tools, &tool_count);
    RunContextHooks(rt, agent, compact, tools, tool_count, &input, &input_count);

    if (InputHasContextItem(input, input_count) && !p->map_context)
    {
        char buf[256];
        snprintf(buf, sizeof(buf), "Provider `%s` does not map context input items.",
                 p->name ? p->name : "?");
        for (int i = 0; i < input_count; i++)
        {
            free(input[i]);
        }
        free(input);
        free(instructions);
        free(tools);
        SetErrorState(app, agent, buf);
        return false;
    }

    if (!m->vision)
    {
        for (int i = 0; i < input_count; i++)
        {
            if (pico_canonical_json_has_media(input[i]))
            {
                for (int j = 0; j < input_count; j++)
                {
                    free(input[j]);
                }
                free(input);
                free(instructions);
                free(tools);
                SetErrorState(app, agent, "model does not accept images");
                return false;
            }
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
        free(instructions);
        free(tools);
        return false;
    }
    free(rt->turn_think);
    rt->turn_think = NULL;
    rt->turn_think_len = 0;
    rt->turn_think_cap = 0;
    rt->work = PICO_WORK_LLM;
    rt->work_stream = p->stream;
    rt->work_stream_state = p->state;
    rt->work_model = Dup(m->id);
    rt->work_base_url = Dup(m->base_url);
    rt->work_effort = Dup(PicoSettings_ActiveEffort(agent));
    rt->work_instructions = instructions;
    rt->work_cache_key = Dup(rt->cache_key);
    rt->work_compact = compact;
    rt->work_include_tools = include_tools;
    rt->work_vision = m->vision;
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

static bool QueueTool(PicoAgent *agent, int index, const PicoTool *tool)
{
    PicoAgentRt *rt = agent->runtime;
    PicoPendingCall *call = &rt->pending[index];
    PicoToolJob *job = calloc(1, sizeof(*job));
    if (!job)
    {
        return false;
    }
    job->context = rt->context;
    job->context.job = job;
    job->call_index = index;
    job->name = Dup(call->name);
    job->args = Dup(call->arguments ? call->arguments : "{}");
    job->call_id = Dup(call->call_id);
    job->context.tool_call_id = job->call_id;
    job->run = tool->run;
    job->state = tool->state;
    rt->jobs[index] = job;
    if (!job->name || !job->args || !job->call_id)
    {
        JoinToolJob(rt, index);
        return false;
    }
    pthread_mutex_lock(&rt->mu);
    if (rt->stop || rt->cancel)
    {
        pthread_mutex_unlock(&rt->mu);
        JoinToolJob(rt, index);
        return false;
    }
    rt->active_tools++;
    rt->busy = true;
    if (pthread_create(&job->thread, NULL, ToolWorkerMain, job) != 0)
    {
        rt->active_tools--;
        rt->busy = rt->active_tools != 0;
        pthread_mutex_unlock(&rt->mu);
        JoinToolJob(rt, index);
        return false;
    }
    job->started = true;
    pthread_mutex_unlock(&rt->mu);
    call->progress = PICO_TOOL_CALL_RUNNING;
    rt->running_tools++;
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

static char *BuildUserItem(const char *text, const char *parts_json)
{
    if (parts_json && parts_json[0] == '[')
    {
        JsonBuf b;
        JsonBuf_Init(&b);
        JsonBuf_Puts(&b, "{\"type\":\"user\",\"parts\":");
        JsonBuf_Puts(&b, parts_json);
        JsonBuf_Putc(&b, '}');
        return JsonBuf_Steal(&b);
    }
    return pico_canonical_user_text(text);
}

static char *BuildAssistantItem(const char *text, const char *thinking, const char *signature)
{
    PicoLlmPart part;
    memset(&part, 0, sizeof(part));
    int n = 0;
    if (text && text[0])
    {
        part.kind = PICO_LLM_PART_TEXT;
        part.text = (char *)text;
        n = 1;
    }
    return pico_canonical_assistant_json(n ? &part : NULL, n, thinking, signature);
}

static char *BuildAssistantItemFromParts(const char *parts_json, const char *thinking,
                                         const char *signature)
{
    if (parts_json && parts_json[0] == '[')
    {
        JsonBuf b;
        JsonBuf_Init(&b);
        JsonBuf_Puts(&b, "{\"type\":\"assistant\",\"parts\":");
        JsonBuf_Puts(&b, parts_json);
        if (thinking && thinking[0])
        {
            JsonBuf_Puts(&b, ",\"thinking\":");
            JsonBuf_String(&b, thinking);
        }
        if (signature && signature[0])
        {
            JsonBuf_Puts(&b, ",\"thinking_signature\":");
            JsonBuf_String(&b, signature);
        }
        JsonBuf_Putc(&b, '}');
        return JsonBuf_Steal(&b);
    }
    return BuildAssistantItem(NULL, thinking, signature);
}

static bool Blank(const char *s);
static int FreezeTrailingThinkMs(PicoMessage *m);

static void AppendMessageText(PicoHost *app, PicoAgent *agent, int idx, const char *s, size_t n)
{
    if (idx < 0 || idx >= agent->message_count || !s || n == 0)
    {
        return;
    }
    PicoMessage *m = &agent->messages[idx];
    size_t old = m->source ? strlen(m->source) : 0;
    if (old == 0)
    {
        FreezeTrailingThinkMs(m);
    }
    char *next = (char *)realloc(m->source, old + n + 1);
    if (!next)
    {
        return;
    }
    memcpy(next + old, s, n);
    next[old + n] = '\0';
    m->source = next;
}

static void ReparseMessage(PicoHost *app, PicoAgent *agent, int idx)
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

static void SetMessageText(PicoHost *app, PicoAgent *agent, int idx, const char *text)
{
    if (idx < 0 || idx >= agent->message_count)
    {
        return;
    }
    PicoMessage *m = &agent->messages[idx];
    if (!Blank(text))
    {
        FreezeTrailingThinkMs(m);
    }
    free(m->source);
    m->source = Dup(text ? text : "");
    ReparseMessage(app, agent, idx);
}

static void PopLastMessage(PicoHost *app, PicoAgent *agent)
{
    if (agent->message_count <= 0)
    {
        return;
    }
    int i = --agent->message_count;
    free(agent->messages[i].source);
    for (int t = 0; t < agent->messages[i].trace_count; t++)
    {
        PicoTraceLine_Release(&agent->messages[i].trace[t]);
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

static int TrailingThinkIndex(const PicoMessage *m)
{
    /* Receiving rows are presentation only, not a boundary between reasoning
     * rounds. Completed tool calls still terminate the preceding round. */
    for (int i = m ? m->trace_count - 1 : -1; i >= 0; i--)
    {
        const PicoTraceLine *line = &m->trace[i];
        if (line->is_tool && line->tool_streaming)
        {
            continue;
        }
        return line->is_tool ? -1 : i;
    }
    return -1;
}

static PicoTraceLine *TrailingThinkLine(PicoMessage *m)
{
    int index = TrailingThinkIndex(m);
    return index >= 0 ? &m->trace[index] : NULL;
}

static char *TrailingThinkPartsJson(PicoMessage *m)
{
    PicoTraceLine *line = TrailingThinkLine(m);
    if (!line || line->think_part_count <= 0 || !line->think_parts)
    {
        return NULL;
    }
    JsonBuf b;
    JsonBuf_Init(&b);
    JsonBuf_Putc(&b, '[');
    for (int i = 0; i < line->think_part_count; i++)
    {
        if (i)
        {
            JsonBuf_Putc(&b, ',');
        }
        JsonBuf_String(&b, line->think_parts[i] ? line->think_parts[i] : "");
    }
    JsonBuf_Putc(&b, ']');
    return JsonBuf_Steal(&b);
}

static bool HasTrailingThinkTrace(const PicoMessage *m)
{
    int index = TrailingThinkIndex(m);
    return index >= 0 && !Blank(m->trace[index].text);
}

static double ThinkNow(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1000000000.0;
}

static void StampActionT0(PicoAgentRt *rt)
{
    if (!rt)
    {
        return;
    }
    rt->action_t0 = ThinkNow();
    if (rt->action_t0 <= 0.0)
    {
        rt->action_t0 = 0.000000001;
    }
}

static int ElapsedMsSince(double t0)
{
    if (t0 <= 0.0)
    {
        return 0;
    }
    double elapsed = ThinkNow() - t0;
    if (elapsed < 0.0)
    {
        elapsed = 0.0;
    }
    int ms = (int)(elapsed * 1000.0 + 0.5);
    if (ms < 1)
    {
        ms = 1;
    }
    return ms;
}

void PicoTraceLine_Release(PicoTraceLine *line)
{
    if (!line)
    {
        return;
    }
    free(line->text);
    free(line->tool_name);
    free(line->tool_call_id);
    free(line->tool_args);
    free(line->tool_args_json);
    free(line->tool_output);
    if (line->think_parts)
    {
        for (int i = 0; i < line->think_part_count; i++)
        {
            free(line->think_parts[i]);
        }
        free(line->think_parts);
    }
    MdDocument_Free(&line->doc);
    memset(line, 0, sizeof(*line));
}

static void ThinkLineStart(const PicoAgent *agent, PicoTraceLine *line)
{
    if (!line || line->think_ms > 0 || line->think_t0 > 0.0)
    {
        return;
    }
    PicoAgentRt *rt = agent ? agent->runtime : NULL;
    if (rt && rt->action_t0 > 0.0)
    {
        line->think_t0 = rt->action_t0;
    }
    else
    {
        line->think_t0 = ThinkNow();
    }
    if (line->think_t0 <= 0.0)
    {
        line->think_t0 = 0.000000001;
    }
}

void PicoTraceLine_FreezeThink(PicoTraceLine *line)
{
    if (!line || line->think_ms > 0 || line->think_t0 <= 0.0)
    {
        return;
    }
    line->think_ms = ElapsedMsSince(line->think_t0);
}

static int FreezeTrailingThinkMs(PicoMessage *m)
{
    PicoTraceLine *line = TrailingThinkLine(m);
    if (!line)
    {
        return 0;
    }
    PicoTraceLine_FreezeThink(line);
    return line->think_ms;
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

static void TraceAppendThink(PicoHost *app, PicoAgent *agent, int idx, const char *s, size_t n)
{
    if (idx < 0 || idx >= agent->message_count || !s || n == 0)
    {
        return;
    }
    PicoMessage *m = &agent->messages[idx];
    PicoTraceLine *line = TrailingThinkLine(m);
    if (!line || line->think_steps != 0)
    {
        line = TracePush(m, false);
    }
    if (!line)
    {
        return;
    }
    ThinkLineStart(agent, line);
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
    const char *text = NULL;
    if (line->think_part_count > 0 && line->think_parts)
    {
        text = line->think_parts[line->think_part_count - 1];
    }
    if (!text)
    {
        text = line->text ? line->text : "";
    }
    line->doc = MdDocument_ParseEx(text, strlen(text), MD_PARSE_DEFAULT);
}

static bool ThinkPartSet(PicoTraceLine *line, int index, const char *s, size_t n)
{
    if (!line || index < 0 || !s)
    {
        return false;
    }
    if (index >= line->think_part_count)
    {
        char **next = (char **)realloc(line->think_parts, (size_t)(index + 1) * sizeof(char *));
        if (!next)
        {
            return false;
        }
        for (int i = line->think_part_count; i <= index; i++)
        {
            next[i] = NULL;
        }
        line->think_parts = next;
        line->think_part_count = index + 1;
    }
    char *copy = (char *)malloc(n + 1);
    if (!copy)
    {
        return false;
    }
    memcpy(copy, s, n);
    copy[n] = '\0';
    free(line->think_parts[index]);
    line->think_parts[index] = copy;
    return true;
}

static void TraceSetThinkSummary(PicoHost *app, PicoAgent *agent, int idx, const char *s, size_t n, int steps)
{
    if (idx < 0 || idx >= agent->message_count || !s || n == 0)
    {
        return;
    }
    PicoMessage *m = &agent->messages[idx];
    PicoTraceLine *line = TrailingThinkLine(m);
    if (!line || line->think_steps <= 0)
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
    ThinkLineStart(agent, line);
    int index = (steps > 0 ? steps : line->think_steps) - 1;
    if (index < 0)
    {
        index = 0;
    }
    if (!ThinkPartSet(line, index, s, n))
    {
        return;
    }
    line->think_steps = line->think_part_count;
    char *latest = line->think_parts[line->think_part_count - 1];
    size_t latest_n = latest ? strlen(latest) : 0;
    char *next = (char *)realloc(line->text, latest_n + 1);
    if (!next)
    {
        ReparseThinkSummary(line);
        return;
    }
    if (latest)
    {
        memcpy(next, latest, latest_n + 1);
    }
    else
    {
        next[0] = '\0';
    }
    line->text = next;
    ReparseThinkSummary(line);
}

static void TraceAddTool(PicoHost *app, PicoAgent *agent, int idx, const char *call_id,
                         const char *name, const char *args_json)
{
    if (idx < 0 || idx >= agent->message_count)
    {
        return;
    }
    PicoAgent_AddToolCallWithId(app, agent, call_id, name, args_json);
}

static bool TraceHasToolCall(const PicoAgent *agent, int idx, const char *call_id)
{
    if (!agent || idx < 0 || idx >= agent->message_count || !call_id || !call_id[0])
    {
        return false;
    }
    const PicoMessage *message = &agent->messages[idx];
    for (int t = 0; t < message->trace_count; t++)
    {
        if (message->trace[t].is_tool && message->trace[t].tool_call_id &&
            strcmp(message->trace[t].tool_call_id, call_id) == 0)
        {
            return true;
        }
    }
    return false;
}

static void TraceSetLastToolOutput(PicoHost *app, PicoAgent *agent, int idx, const char *output, bool is_error)
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
            pico_trace_line_stamp_tool_done(&m->trace[t]);
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
            if (name && (strcmp(name, "sh") == 0 || strcmp(name, "run_background") == 0))
            {
                detail = JsonObjStr(&doc, 0, "description");
            }
            if (!detail || !detail[0])
            {
                free(detail);
                detail = JsonObjStr(&doc, 0, "command");
            }
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
    PushInput(rt, pico_canonical_tool_result_json(call_id, name, output, is_error));
}

static void AbortRemainingCalls(PicoHost *app, PicoAgent *agent, PicoAgentRt *rt)
{
    for (int i = 0; i < rt->pending_count; i++)
    {
        if (rt->pending[i].completed) continue;
        PushFunctionOutput(rt, rt->pending[i].call_id, rt->pending[i].name, "(interrupted)", true);
        PicoSession_LogToolResult(app, agent, rt->pending[i].call_id, rt->pending[i].name,
                                  "(interrupted)", true, NULL);
        PicoAgent_SetToolOutputByCallId(agent, rt->pending[i].call_id,
                                        "(interrupted)", true);
    }
    ClearPending(rt);
}

static void GoIdle(PicoHost *app, PicoAgent *agent)
{
    PicoAgentRt *rt = agent->runtime;
    pthread_mutex_lock(&rt->mu);
    bool may_release_tools = !rt->busy && !rt->retired;
    free(rt->turn_think);
    rt->turn_think = NULL;
    rt->turn_think_len = 0;
    rt->turn_think_cap = 0;
    pthread_mutex_unlock(&rt->mu);
    if (may_release_tools)
    {
        ClearOfferedTools(rt);
    }
    agent->state = PICO_AGENT_IDLE;
    if (rt->stream_msg >= 0 && rt->stream_msg < agent->message_count)
    {
        FreezeTrailingThinkMs(&agent->messages[rt->stream_msg]);
    }
    rt->stream_msg = -1;
    rt->stream_dirty = false;
    rt->compacting = false;
    rt->compact_no_tools = false;
    agent->activity[0] = '\0';
}

static void EndTurnIdle(PicoHost *app, PicoAgent *agent)
{
    GoIdle(app, agent);
    if (pico_agent_active(app) != agent->id)
    {
        PicoSession_SetUnseenComplete(app, agent, true);
    }
    pico_run_hooks(app, PICO_HOOK_ON_TURN_END, agent->id);
}

static void SavePartialAssistant(PicoHost *app, PicoAgent *agent)
{
    PicoAgentRt *rt = agent->runtime;
    pthread_mutex_lock(&rt->mu);
    char *thinking = Dup(rt->turn_think);
    pthread_mutex_unlock(&rt->mu);
    FinishAssistantHistory(app, agent, thinking, NULL);
    char *thinking_parts = rt->stream_msg >= 0 && rt->stream_msg < agent->message_count
                               ? TrailingThinkPartsJson(&agent->messages[rt->stream_msg])
                               : NULL;
    if (rt->stream_msg >= 0 &&
        (!MessageSourceEmpty(agent, rt->stream_msg) || (thinking && thinking[0]) || thinking_parts))
    {
        int think_ms = FreezeTrailingThinkMs(&agent->messages[rt->stream_msg]);
        PicoSession_LogAssistant(app, agent, rt->stream_msg,
                                 MessageSourceEmpty(agent, rt->stream_msg)
                                     ? ""
                                     : agent->messages[rt->stream_msg].source,
                                 thinking, NULL, NULL, thinking_parts, think_ms);
    }
    free(thinking_parts);
    free(thinking);
}

static void ApplyCancel(PicoHost *app, PicoAgent *agent)
{
    PicoAgentRt *rt = agent->runtime;
    if (rt->compacting)
    {
        GoIdle(app, agent);
        pico_run_hooks(app, PICO_HOOK_ON_CANCEL, agent->id);
        return;
    }
    /* The completed assistant/tool-call result is already canonical and persisted
     * once tool execution begins. Only provider cancellation has partial output. */
    if (agent->state == PICO_AGENT_LLM_WAIT)
    {
        SavePartialAssistant(app, agent);
    }
    SweepProvisionalRows(agent, rt);
    if (rt->stream_msg >= 0 && MessageEmpty(agent, rt->stream_msg))
    {
        PopLastMessage(app, agent);
    }
    /* Completed calls may be anywhere in a parallel batch; never rewrite them. */
    AbortRemainingCalls(app, agent, rt);
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

static void ApplyCompaction(PicoHost *app, PicoAgent *agent, const char *summary)
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

static void StartCompact(PicoHost *app, PicoAgent *agent)
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

static void FinishTurn(PicoHost *app, PicoAgent *agent)
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

static void SetErrorState(PicoHost *app, PicoAgent *agent, const char *msg)
{
    PicoAgentRt *rt = agent->runtime;
    free(agent->error);
    agent->error = Dup(msg ? msg : "agent error");
    agent->state = PICO_AGENT_ERROR;
    SweepProvisionalRows(agent, rt);
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

static void StartLlm(PicoHost *app, PicoAgent *agent);

static bool CallAllowsParallel(PicoAgentRt *rt, const PicoPendingCall *call)
{
    const PicoTool *tool = FindOfferedTool(rt, call->name);
    if (!tool || tool->execution != PICO_TOOL_PARALLEL)
    {
        return false;
    }
    if (strcmp(call->name, "subagent") != 0)
    {
        return true;
    }

    /* Unknown/malformed profiles fail closed to a barrier. Reload cannot replace
     * this workspace's profile snapshot while the accepted turn is draining. */
    JsonDoc doc;
    if (!call->arguments || JsonParse(&doc, call->arguments, strlen(call->arguments)) != 0)
    {
        return false;
    }
    char *name = JsonIsObject(&doc, 0) ? JsonObjStr(&doc, 0, "profile") : NULL;
    const PicoSubagentProfileInfo *profile = name
        ? PicoSubagentConfig_Find(rt->context.workspace_owner, name) : NULL;
    bool parallel = profile && profile->parallel_safe;
    free(name);
    JsonFree(&doc);
    return parallel;
}

static void StartNextTool(PicoHost *app, PicoAgent *agent)
{
    PicoAgentRt *rt = agent->runtime;
    if (PicoAgent_CancelRequested(agent))
    {
        return;
    }
    for (int i = 0; i < rt->pending_count; i++)
    {
        PicoPendingCall *call = &rt->pending[i];
        if (call->completed)
        {
            continue;
        }
        PicoTool *tool = FindOfferedTool(rt, call->name);
        bool parallel = call->parallel;
        if (call->progress == PICO_TOOL_CALL_RUNNING)
        {
            if (!parallel)
            {
                return;
            }
            continue;
        }
        if (rt->running_tools >= rt->max_parallel_tools || (!parallel && rt->running_tools))
        {
            return;
        }
        agent->state = PICO_AGENT_TOOL_WAIT;
        if (!tool)
        {
            JsonBuf b;
            JsonBuf_Init(&b);
            JsonBuf_Puts(&b, "tool was not offered for this request: ");
            JsonBuf_Puts(&b, call->name);
            char *error = JsonBuf_Steal(&b);
            if (!error) error = Dup("unoffered tool");
            PushFunctionOutput(rt, call->call_id, call->name, error, true);
            PicoSession_LogToolResult(app, agent, call->call_id, call->name, error, true, NULL);
            PicoAgent_SetToolOutputByCallId(agent, call->call_id, error, true);
            free(error);
            call->completed = true;
            continue;
        }
        SetActivity(app, agent, call->name);
        StampActionT0(rt);
        if (!QueueTool(agent, i, tool))
        {
            /* A dispatch failure is a call failure, not a reason to abandon siblings. */
            const char *error = "Failed to start tool";
            PushFunctionOutput(rt, call->call_id, call->name, error, true);
            PicoSession_LogToolResult(app, agent, call->call_id, call->name, error, true, NULL);
            PicoAgent_SetToolOutputByCallId(agent, call->call_id, error, true);
            call->completed = true;
            continue;
        }
        if (!parallel)
        {
            return;
        }
    }
    if (!rt->running_tools)
    {
        ClearPending(rt);
        StartLlm(app, agent);
    }
}

static void StartLlm(PicoHost *app, PicoAgent *agent)
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
    ClearProvStream(rt);
    agent->state = PICO_AGENT_LLM_WAIT;
    StampActionT0(rt);
    SetActivity(app, agent, "Thinking…");
    free(agent->error);
    agent->error = NULL;
    if (!QueueLlm(app, agent, false, true) && agent->state != PICO_AGENT_ERROR)
    {
        SetErrorState(app, agent, "Failed to start model request");
    }
}

static void FinishAssistantHistory(PicoHost *app, PicoAgent *agent, const char *thinking,
                                   const char *signature)
{
    PicoAgentRt *rt = agent->runtime;
    const char *text = (rt->stream_msg >= 0 && !MessageSourceEmpty(agent, rt->stream_msg))
                           ? agent->messages[rt->stream_msg].source
                           : "";
    if ((text && text[0]) || (thinking && thinking[0]) || (signature && signature[0]))
    {
        PushInput(rt, BuildAssistantItem(text, thinking, signature));
    }
}

static bool PersistMediaParts(PicoHost *app, PicoAgent *agent, PicoLlmPart *parts, int n)
{
    char dir[4096];
    const char *sid = agent->session_id[0] ? agent->session_id : "tmp";
    if (!PicoPath_Format(dir, sizeof(dir), "%s/.pico/media/%s", PicoAgent_WorkspacePath(agent), sid))
    {
        return false;
    }
    for (int i = 0; i < n; i++)
    {
        PicoLlmPart *p = &parts[i];
        if ((p->kind != PICO_LLM_PART_IMAGE && p->kind != PICO_LLM_PART_AUDIO) ||
            (p->path && p->path[0]))
        {
            continue;
        }
        if (p->url && strncmp(p->url, "data:", 5) == 0)
        {
            char *path = pico_canonical_persist_data_url(dir, p->url);
            if (!path)
            {
                return false;
            }
            free(p->path);
            p->path = path;
            free(p->url);
            p->url = NULL;
        }
    }
    return true;
}

static void ClearProvStream(PicoAgentRt *rt)
{
    pthread_mutex_lock(&rt->mu);
    for (int i = 0; i < rt->prov_count; i++)
    {
        free(rt->prov[i].call_id);
        free(rt->prov[i].name);
    }
    rt->prov_count = 0;
    rt->prov_dirty = false;
    pthread_mutex_unlock(&rt->mu);
}

static PicoProvRow *ProvRowFor(PicoAgentRt *rt, int call_index)
{
    for (int i = 0; i < rt->prov_row_count; i++)
    {
        if (rt->prov_rows[i].call_index == call_index)
        {
            return &rt->prov_rows[i];
        }
    }
    if (rt->prov_row_count >= rt->prov_row_cap)
    {
        int cap = rt->prov_row_cap ? rt->prov_row_cap * 2 : 4;
        PicoProvRow *next = (PicoProvRow *)realloc(rt->prov_rows, (size_t)cap * sizeof(PicoProvRow));
        if (!next)
        {
            return NULL;
        }
        rt->prov_rows = next;
        rt->prov_row_cap = cap;
    }
    PicoProvRow *row = &rt->prov_rows[rt->prov_row_count++];
    memset(row, 0, sizeof(*row));
    row->call_index = call_index;
    row->msg_idx = -1;
    row->trace_idx = -1;
    return row;
}

/* Main thread, from the pump: create or refresh the provisional trace row for
 * a tool call whose arguments are still streaming. */
static void ProvRowSync(PicoHost *app, PicoAgent *agent, PicoAgentRt *rt,
                        const PicoProvStream *update)
{
    PicoProvRow *row = ProvRowFor(rt, update->call_index);
    if (!row)
    {
        return;
    }
    if (update->name && update->name[0] && (!row->name || strcmp(row->name, update->name) != 0))
    {
        free(row->name);
        row->name = Dup(update->name);
    }
    if (update->call_id && update->call_id[0] &&
        (!row->call_id || strcmp(row->call_id, update->call_id) != 0))
    {
        free(row->call_id);
        row->call_id = Dup(update->call_id);
    }
    row->args_bytes = update->args_bytes;
    if (row->msg_idx >= 0)
    {
        if (row->msg_idx < agent->message_count)
        {
            PicoMessage *m = &agent->messages[row->msg_idx];
            if (row->trace_idx < m->trace_count && m->trace[row->trace_idx].tool_streaming)
            {
                PicoTraceLine *line = &m->trace[row->trace_idx];
                line->tool_stream_bytes = row->args_bytes;
                if (row->call_id && !line->tool_call_id)
                {
                    line->tool_call_id = Dup(row->call_id);
                }
                if (row->name && line->tool_name && strcmp(line->tool_name, row->name) != 0)
                {
                    free(line->tool_name);
                    line->tool_name = Dup(row->name);
                }
            }
        }
        return;
    }
    if (!row->name || !row->name[0] || rt->stream_msg < 0)
    {
        return; /* a row needs a name; arguments may stream before BEGIN lands */
    }
    PicoAgent_AddToolCallWithId(app, agent, row->call_id, row->name, NULL);
    if (agent->message_count <= 0)
    {
        return;
    }
    PicoMessage *m = &agent->messages[agent->message_count - 1];
    if (m->trace_count <= 0)
    {
        return;
    }
    PicoTraceLine *line = &m->trace[m->trace_count - 1];
    line->tool_streaming = true;
    line->tool_stream_bytes = row->args_bytes;
    row->msg_idx = agent->message_count - 1;
    row->trace_idx = m->trace_count - 1;
}

/* Fill a provisional row with the completed call instead of appending a new
 * row. Matches by call id first, then by first-seen order. */
static bool TraceReconcileProvisional(PicoAgent *agent, const char *call_id, const char *name,
                                      const char *args_json)
{
    PicoAgentRt *rt = agent->runtime;
    PicoProvRow *row = NULL;
    if (call_id && call_id[0])
    {
        for (int i = 0; i < rt->prov_row_count; i++)
        {
            PicoProvRow *r = &rt->prov_rows[i];
            if (!r->reconciled && r->msg_idx >= 0 && r->call_id &&
                strcmp(r->call_id, call_id) == 0)
            {
                row = r;
                break;
            }
        }
    }
    if (!row)
    {
        for (int i = 0; i < rt->prov_row_count; i++)
        {
            PicoProvRow *r = &rt->prov_rows[i];
            if (!r->reconciled && r->msg_idx >= 0)
            {
                row = r;
                break;
            }
        }
    }
    if (!row || row->msg_idx < 0 || row->msg_idx >= agent->message_count)
    {
        return false;
    }
    PicoMessage *m = &agent->messages[row->msg_idx];
    if (row->trace_idx >= m->trace_count || !m->trace[row->trace_idx].is_tool ||
        !m->trace[row->trace_idx].tool_streaming)
    {
        return false;
    }
    PicoTraceLine *line = &m->trace[row->trace_idx];
    free(line->tool_call_id);
    line->tool_call_id = Dup(call_id);
    if (name && name[0] && (!line->tool_name || strcmp(line->tool_name, name) != 0))
    {
        free(line->tool_name);
        line->tool_name = Dup(name);
    }
    free(line->tool_args);
    line->tool_args = PicoAgent_FormatToolArgs(line->tool_name, args_json);
    free(line->tool_args_json);
    line->tool_args_json = Dup(args_json ? args_json : "");
    line->tool_streaming = false;
    line->tool_stream_bytes = 0;
    row->reconciled = true;
    return true;
}

static void TraceRemoveLine(PicoMessage *m, int trace_idx)
{
    PicoTraceLine_Release(&m->trace[trace_idx]);
    if (trace_idx + 1 < m->trace_count)
    {
        memmove(&m->trace[trace_idx], &m->trace[trace_idx + 1],
                (size_t)(m->trace_count - trace_idx - 1) * sizeof(PicoTraceLine));
    }
    m->trace_count--;
}

/* Drop provisional rows the completed result never claimed. An interrupted
 * call is not persisted to the session log, so the transcript must not show
 * it either. */
static void SweepProvisionalRows(PicoAgent *agent, PicoAgentRt *rt)
{
    for (int i = 0; i < rt->prov_row_count; i++)
    {
        PicoProvRow *row = &rt->prov_rows[i];
        if (row->reconciled || row->msg_idx < 0 || row->msg_idx >= agent->message_count)
        {
            continue;
        }
        PicoMessage *m = &agent->messages[row->msg_idx];
        if (row->trace_idx < m->trace_count && m->trace[row->trace_idx].is_tool &&
            m->trace[row->trace_idx].tool_streaming)
        {
            TraceRemoveLine(m, row->trace_idx);
            /* Rows can receive names (and acquire trace indices) in a different
             * order from their first argument fragments. Keep every remaining
             * index valid rather than relying on provider arrival order. */
            for (int j = i + 1; j < rt->prov_row_count; j++)
            {
                PicoProvRow *next = &rt->prov_rows[j];
                if (next->msg_idx == row->msg_idx && next->trace_idx > row->trace_idx)
                {
                    next->trace_idx--;
                }
            }
        }
    }
    for (int i = 0; i < rt->prov_row_count; i++)
    {
        free(rt->prov_rows[i].call_id);
        free(rt->prov_rows[i].name);
    }
    rt->prov_row_count = 0;
    ClearProvStream(rt);
}

static bool IngestResult(PicoHost *app, PicoAgent *agent, const char *payload)
{
    PicoAgentRt *rt = agent->runtime;
    if (!payload)
    {
        FinishAssistantHistory(app, agent, NULL, NULL);
        return true;
    }
    JsonDoc doc;
    int items = ResultItems(payload, &doc);
    if (items < 0)
    {
        JsonFree(&doc);
        FinishAssistantHistory(app, agent, NULL, NULL);
        return true;
    }

    int n = JsonArrayLen(&doc, items);
    JsonBuf staged;
    JsonBuf_Init(&staged);
    JsonBuf_Puts(&staged, "{\"items\":[");
    bool stage_ok = true;
    for (int i = 0; i < n && stage_ok; i++)
    {
        int obj = JsonArrayAt(&doc, items, i);
        char *type = JsonObjStr(&doc, obj, "type");
        char *json = NULL;
        if (type && strcmp(type, "assistant") == 0)
        {
            PicoLlmPart *parts = NULL;
            int part_n = 0;
            char *think = JsonObjStr(&doc, obj, "thinking");
            char *sig = JsonObjStr(&doc, obj, "thinking_signature");
            stage_ok = pico_canonical_parse_parts(&doc, obj, &parts, &part_n) &&
                       PersistMediaParts(app, agent, parts, part_n);
            if (stage_ok)
            {
                json = pico_canonical_assistant_json(parts, part_n, think, sig);
                stage_ok = json != NULL;
            }
            pico_canonical_free_parts(parts, part_n);
            free(think);
            free(sig);
        }
        else if (type && strcmp(type, "tool_call") == 0)
        {
            char *call_id = JsonObjStr(&doc, obj, "call_id");
            char *name = JsonObjStr(&doc, obj, "name");
            char *arguments = JsonObjStr(&doc, obj, "arguments");
            char *item_id = JsonObjStr(&doc, obj, "item_id");
            json = pico_canonical_tool_call_json(call_id, name, arguments, item_id);
            stage_ok = json != NULL;
            free(call_id);
            free(name);
            free(arguments);
            free(item_id);
        }
        else
        {
            stage_ok = false;
        }
        if (stage_ok)
        {
            if (i)
            {
                JsonBuf_Putc(&staged, ',');
            }
            JsonBuf_Puts(&staged, json);
        }
        free(json);
        free(type);
    }
    JsonBuf_Puts(&staged, "]}");
    JsonFree(&doc);
    char *staged_json = JsonBuf_Steal(&staged);
    if (!stage_ok || ResultItems(staged_json, &doc) < 0)
    {
        if (doc.toks)
        {
            JsonFree(&doc);
        }
        free(staged_json);
        SetErrorState(app, agent, "failed to persist provider media");
        return false;
    }
    items = JsonObjGet(&doc, 0, "items");

    JsonBuf visible;
    JsonBuf_Init(&visible);
    for (int i = 0; i < n; i++)
    {
        int obj = JsonArrayAt(&doc, items, i);
        char *type = JsonObjStr(&doc, obj, "type");
        if (type && strcmp(type, "assistant") == 0)
        {
            PicoLlmPart *parts = NULL;
            int part_n = 0;
            pico_canonical_parse_parts(&doc, obj, &parts, &part_n);
            char *display = pico_canonical_display(parts, part_n);
            char *think = JsonObjStr(&doc, obj, "thinking");
            char *sig = JsonObjStr(&doc, obj, "thinking_signature");
            if (display && display[0])
            {
                JsonBuf_Puts(&visible, display);
            }
            if (think && think[0] && rt->stream_msg >= 0 &&
                !HasTrailingThinkTrace(&agent->messages[rt->stream_msg]))
            {
                TraceAppendThink(app, agent, rt->stream_msg, think, strlen(think));
            }
            PushInput(rt, pico_canonical_assistant_json(parts, part_n, think, sig));
            char *thinking_parts_json =
                rt->stream_msg >= 0 ? TrailingThinkPartsJson(&agent->messages[rt->stream_msg]) : NULL;
            if ((display && display[0]) || (think && think[0]) || (sig && sig[0]) ||
                pico_canonical_parts_need_log(parts, part_n) || thinking_parts_json)
            {
                char *parts_json = pico_canonical_parts_need_log(parts, part_n)
                                       ? pico_canonical_parts_json(parts, part_n)
                                       : NULL;
                int think_ms = rt->stream_msg >= 0
                                   ? FreezeTrailingThinkMs(&agent->messages[rt->stream_msg])
                                   : 0;
                PicoSession_LogAssistant(app, agent, rt->stream_msg,
                                         display ? display : "", think, sig, parts_json,
                                         thinking_parts_json, think_ms);
                free(parts_json);
            }
            free(thinking_parts_json);
            pico_canonical_free_parts(parts, part_n);
            free(display);
            free(think);
            free(sig);
        }
        else if (type && strcmp(type, "tool_call") == 0)
        {
            if (rt->pending_count < PICO_MAX_PENDING_CALLS)
            {
                PicoPendingCall *call = &rt->pending[rt->pending_count++];
                call->call_id = JsonObjStr(&doc, obj, "call_id");
                call->name = JsonObjStr(&doc, obj, "name");
                call->arguments = JsonObjStr(&doc, obj, "arguments");
                call->item_id = JsonObjStr(&doc, obj, "item_id");
                call->parallel = CallAllowsParallel(rt, call);
                PushInput(rt, pico_canonical_tool_call_json(call->call_id, call->name, call->arguments,
                                                            call->item_id));
                if (rt->stream_msg >= 0 &&
                    !TraceReconcileProvisional(agent, call->call_id, call->name, call->arguments))
                {
                    TraceAddTool(app, agent, rt->stream_msg, call->call_id,
                                 call->name, call->arguments);
                }
                PicoSession_LogToolCall(app, agent, rt->stream_msg, call->call_id, call->name,
                                        call->arguments, call->item_id);
            }
        }
        free(type);
    }
    if (rt->stream_msg >= 0 && visible.len)
    {
        SetMessageText(app, agent, rt->stream_msg, visible.data);
    }
    JsonBuf_Free(&visible);
    JsonFree(&doc);
    free(staged_json);
    return true;
}

static void OnLlmDone(PicoHost *app, PicoAgent *agent, PicoAgentEv *ev)
{
    PicoAgentRt *rt = agent->runtime;
    if (rt->compacting)
    {
        SweepProvisionalRows(agent, rt);
        int normalized_cached = 0;
        if (PicoUsage_Apply(agent, ev->tokens, ev->cached, &normalized_cached))
        {
            PicoSession_LogUsage(app, agent, ev->tokens, normalized_cached);
        }
        if (ResultCallCount(ev->payload) > 0 && !rt->compact_no_tools)
        {
            rt->compact_no_tools = true;
            if (!QueueLlm(app, agent, true, false))
            {
                SetErrorState(app, agent, "Failed to start compaction");
            }
            return;
        }
        char *text = ResultAssistantText(ev->payload);
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
    if (!IngestResult(app, agent, ev->payload))
    {
        return;
    }
    SweepProvisionalRows(agent, rt);
    int normalized_cached = 0;
    if (PicoUsage_Apply(agent, ev->tokens, ev->cached, &normalized_cached))
    {
        PicoSession_LogUsage(app, agent, ev->tokens, normalized_cached);
    }
    if (rt->pending_count > 0)
    {
        rt->running_tools = 0;
        StartNextTool(app, agent);
        return;
    }
    FinishTurn(app, agent);
}

static void OnToolStart(PicoHost *app, PicoAgent *agent, PicoAgentEv *ev)
{
    PicoAgentRt *rt = agent->runtime;
    const char *name = ev->text ? ev->text : "tool";
    const char *args = ev->payload ? ev->payload : "{}";
    if (ev->call_index >= 0 && ev->call_index < rt->pending_count)
    {
        PicoPendingCall *call = &rt->pending[ev->call_index];
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
        PicoPendingCall *call = ev->call_index >= 0 && ev->call_index < rt->pending_count
                                    ? &rt->pending[ev->call_index] : NULL;
        if (call && TraceHasToolCall(agent, rt->stream_msg, call->call_id))
        {
            PicoAgent_SetToolArgsByCallId(agent, call->call_id, args);
        }
        else
        {
            TraceAddTool(app, agent, rt->stream_msg,
                         call ? call->call_id : NULL, name, args);
        }
    }
}

static void OnToolDone(PicoHost *app, PicoAgent *agent, PicoAgentEv *ev, bool failed)
{
    PicoAgentRt *rt = agent->runtime;
    PicoPendingCall *call = NULL;
    if (ev->call_index >= 0 && ev->call_index < rt->pending_count)
    {
        call = &rt->pending[ev->call_index];
    }
    if (!call || call->completed)
    {
        return;
    }
    bool ended_ask = rt->jobs[ev->call_index] && rt->jobs[ev->call_index]->snap_id;
    JoinToolJob(rt, ev->call_index);
    if (ended_ask)
    {
        pico_run_hooks(app, PICO_HOOK_ON_ASK_END, agent->id);
        if (agent->runtime != rt) return;
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
        !tool->apply(rt->context.workspace_owner, agent->id, details, false, tool->state))
    {
        is_error = true;
        details = NULL;
        apply_error = Dup("tool result details could not be applied");
    }

    if (agent->runtime != rt)
    {
        free(apply_error);
        return;
    }
    cancel = WorkerIsCancelled(rt);
    is_error = is_error || cancel;
    const char *raw = apply_error ? apply_error : (ev->text ? ev->text : (is_error ? "tool failed" : ""));
    char *output = NULL;
    if (!cancel)
    {
        output = RunToolAfterHooks(rt, name, call_id, call ? call->arguments : NULL, raw, details,
                                   ev->executed, is_error);
    }
    if (agent->runtime != rt)
    {
        free(output);
        free(apply_error);
        return;
    }
    cancel = WorkerIsCancelled(rt);
    is_error = is_error || cancel;
    const char *use = output ? output : raw;
    PushFunctionOutput(rt, call_id, name, use, is_error);
    PicoSession_LogToolResult(app, agent, call_id, name, use, is_error, details);
    if (rt->stream_msg >= 0)
    {
        if (!TraceHasToolCall(agent, rt->stream_msg, call_id))
        {
            TraceAddTool(app, agent, rt->stream_msg, call ? call->call_id : call_id,
                         name[0] ? name : "tool",
                         call && call->arguments ? call->arguments : "{}");
        }
        PicoAgent_SetToolOutputByCallId(agent, call_id, use, is_error);
    }
    call->completed = true;
    call->progress = PICO_TOOL_CALL_IDLE;
    rt->running_tools--;
    free(output);
    free(apply_error);
    if (cancel)
    {
        if (rt->running_tools)
        {
            return;
        }
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

PicoToolCallProgress PicoAgent_ToolCallProgress(const PicoAgent *agent, const char *call_id)
{
    PicoAgentRt *rt = agent ? agent->runtime : NULL;
    if (!rt || !call_id || !call_id[0] || agent->state != PICO_AGENT_TOOL_WAIT)
    {
        return PICO_TOOL_CALL_IDLE;
    }
    for (int i = 0; i < rt->pending_count; i++)
    {
        const char *id = rt->pending[i].call_id;
        if (!id || strcmp(id, call_id) != 0)
        {
            continue;
        }
        return rt->pending[i].completed ? PICO_TOOL_CALL_IDLE :
               rt->pending[i].progress == PICO_TOOL_CALL_RUNNING ? PICO_TOOL_CALL_RUNNING : PICO_TOOL_CALL_QUEUED;
    }
    return PICO_TOOL_CALL_IDLE;
}

int PicoAgent_LiveActionMs(const PicoAgent *agent)
{
    PicoAgentRt *rt = agent ? agent->runtime : NULL;
    if (!rt || rt->action_t0 <= 0.0)
    {
        return 0;
    }
    if (agent->state != PICO_AGENT_LLM_WAIT && agent->state != PICO_AGENT_TOOL_WAIT)
    {
        return 0;
    }
    return ElapsedMsSince(rt->action_t0);
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
    for (int i = 0; rt && i < PICO_MAX_PENDING_CALLS; i++)
        if (rt->jobs[i] && rt->jobs[i]->snap_id) return true;
    return false;
}

static void PublishJobAskSnapshot(PicoHost *app, PicoAgent *agent, PicoToolJob *job)
{
    PicoAgentRt *rt = agent ? agent->runtime : NULL;
    uint64_t previous_id;
    uint64_t now_id;
    uint64_t publish_id;
    char *live_copy = NULL;
    if (!rt)
    {
        return;
    }
    previous_id = job->snap_id;
    pthread_mutex_lock(&rt->mu);
    bool waiting = job->ask_waiting && !rt->cancel && !rt->stop;
    uint64_t live_id = waiting ? job->ask_id : 0;
    /* An answered/cancelled snapshot stays unpublished until a new ask id appears. */
    publish_id = (job->snap_retired && live_id == job->snap_id) ? 0 : live_id;
    if (publish_id != 0 && job->ask_request && (job->snap_retired || job->snap_id != publish_id))
    {
        live_copy = Dup(job->ask_request);
    }
    pthread_mutex_unlock(&rt->mu);

    if (!job->snap_retired && job->snap_id == publish_id && publish_id != 0)
    {
        return;
    }
    free(job->snap_request);
    job->snap_request = NULL;
    job->snap_id = 0;
    job->snap_retired = false;
    if (publish_id != 0)
    {
        job->snap_id = publish_id;
        job->snap_request = live_copy;
        live_copy = NULL;
    }
    free(live_copy);
    now_id = job->snap_id;
    if (previous_id != 0 && previous_id != now_id)
    {
        pico_run_hooks(app, PICO_HOOK_ON_ASK_END, agent->id);
    }
    if (agent->runtime == rt && now_id != 0 && now_id != previous_id)
    {
        pico_run_hooks(app, PICO_HOOK_ON_ASK, agent->id);
    }
}

static void PublishAskSnapshot(PicoHost *app, PicoAgent *agent)
{
    PicoAgentRt *rt = agent->runtime;
    for (int i = 0; i < PICO_MAX_PENDING_CALLS && agent->runtime == rt; i++)
        if (rt->jobs[i]) PublishJobAskSnapshot(app, agent, rt->jobs[i]);
}

bool PicoAgent_BlocksReload(const PicoAgent *agent)
{
    if (!agent)
    {
        return false;
    }
    if (PicoAgent_IsBusy(agent))
    {
        return true;
    }
    PicoAgentRt *rt = agent->runtime;
    if (!rt)
    {
        return false;
    }
    pthread_mutex_lock(&rt->mu);
    bool blocked = rt->busy || rt->work != PICO_WORK_IDLE ||
                   rt->event_count > 0 || rt->pending_count > 0 ||
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

bool PicoAgent_RevalidateToolPolicy(const PicoHost *app, PicoAgent *agent)
{
    if (!agent)
    {
        return false;
    }
    const PicoWorkspace *ws = PicoAgent_Workspace(agent);
    if (!ws)
    {
        ws = PicoHost_SelectedWorkspaceConst(app);
    }
    bool valid = true;
    for (int i = 0; ws && agent->allowed_tools && i < agent->allowed_tool_count; i++)
    {
        bool found = false;
        for (int t = 0; t < ws->tool_count; t++)
        {
            if (ws->tools[t].name && agent->allowed_tools[i] &&
                strcmp(ws->tools[t].name, agent->allowed_tools[i]) == 0)
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

static void KillToolChildren(PicoAgentRt *rt)
{
    pthread_mutex_lock(&rt->mu);
    for (int i = 0; i < PICO_MAX_PENDING_CALLS; i++)
    {
        PicoToolJob *job = rt->jobs[i];
        if (job)
        {
            KillToolChild(job->child);
            job->child = 0;
        }
    }
    pthread_mutex_unlock(&rt->mu);
}

static void JoinToolJob(PicoAgentRt *rt, int index)
{
    PicoToolJob *job = rt->jobs[index];
    if (!job)
    {
        return;
    }
    if (job->started)
    {
        pthread_join(job->thread, NULL);
    }
    free(job->name);
    free(job->args);
    free(job->call_id);
    free(job->ask_request);
    free(job->ask_answer);
    free(job->snap_request);
    free(job);
    rt->jobs[index] = NULL;
}

static void FreeRt(PicoAgentRt *rt)
{
    if (!rt)
    {
        return;
    }
    for (int i = 0; i < PICO_MAX_PENDING_CALLS; i++)
    {
        JoinToolJob(rt, i);
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
    free(rt->turn_think);
    free(rt->summary);
    for (int i = 0; i < rt->prov_count; i++)
    {
        free(rt->prov[i].call_id);
        free(rt->prov[i].name);
    }
    free(rt->prov);
    for (int i = 0; i < rt->prov_row_count; i++)
    {
        free(rt->prov_rows[i].call_id);
        free(rt->prov_rows[i].name);
    }
    free(rt->prov_rows);
    ClearSummaryParts(rt);
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
    free(rt->instructions);
    PicoWorkspace_RegistrationRelease(rt->registration);
    rt->registration = NULL;
    pthread_mutex_destroy(&rt->mu);
    pthread_cond_destroy(&rt->cv);
    free(rt);
}

static void RefreshWorkerContext(PicoAgentRt *rt, const PicoHost *app, const PicoAgent *agent)
{
    if (!rt || !app || !agent)
    {
        return;
    }
    PicoAgentContext *ctx = &rt->context;
    PicoWorkspace *workspace = PicoAgent_Workspace(agent);
    ctx->runtime = rt;
    ctx->workspace_owner = workspace;
    ctx->agent_id = agent->id;
    ctx->runtime_generation = agent->runtime_generation;
    ctx->workspace_id = workspace ? workspace->id : 0;
    PicoRegistrationGeneration *active = PicoWorkspace_RegistrationActive(workspace);
    if (rt->registration != active)
    {
        bool quiescent;
        PicoRegistrationGeneration *old;
        pthread_mutex_lock(&rt->mu);
        quiescent = !rt->busy && rt->work == PICO_WORK_IDLE && rt->event_count == 0 &&
                    rt->pending_count == 0;
        if (quiescent)
        {
            if (active)
            {
                PicoWorkspace_RegistrationRetain(active);
            }
            old = rt->registration;
            rt->registration = active;
        }
        else
        {
            old = NULL;
        }
        pthread_mutex_unlock(&rt->mu);
        PicoWorkspace_RegistrationRelease(old);
    }
    ctx->registration_generation = rt->registration ? rt->registration->id
                                                    : (workspace ? workspace->registration_generation : 0);
    snprintf(ctx->workspace, sizeof(ctx->workspace), "%s", PicoWorkspace_Path(workspace));
    snprintf(ctx->session_id, sizeof(ctx->session_id), "%s", agent->session_id);
    snprintf(ctx->profile, sizeof(ctx->profile), "%s", agent->profile);
    snprintf(ctx->purpose, sizeof(ctx->purpose), "%s", agent->purpose);
    ctx->safe_mode = app->safe_mode;
    ctx->auth_store = app->auth_store;
    if (rt->registration)
    {
        memcpy(rt->tool_before_hooks, rt->registration->tool_before_hooks,
               sizeof(rt->tool_before_hooks));
        rt->tool_before_hook_count = rt->registration->tool_before_hook_count;
    }
    else
    {
        rt->tool_before_hook_count = 0;
    }
}

static PicoAgentRt *CreateRt(PicoHost *app, PicoAgent *agent)
{
    PicoAgentRt *rt = (PicoAgentRt *)calloc(1, sizeof(PicoAgentRt));
    if (!rt)
    {
        return NULL;
    }
    rt->stream_msg = -1;
    rt->max_parallel_tools = PICO_DEFAULT_PARALLEL_TOOLS;
    if (pthread_mutex_init(&rt->mu, NULL) != 0)
    {
        free(rt);
        return NULL;
    }
    if (pthread_cond_init(&rt->cv, NULL) != 0)
    {
        pthread_mutex_destroy(&rt->mu);
        free(rt);
        return NULL;
    }
    rt->event_cap = 2 * PICO_MAX_PENDING_CALLS + 1;
    rt->events = calloc((size_t)rt->event_cap, sizeof(*rt->events));
    if (!rt->events)
    {
        FreeRt(rt);
        return NULL;
    }
    RefreshWorkerContext(rt, app, agent);
    if (pthread_create(&rt->thread, NULL, WorkerMain, rt) != 0)
    {
        /* Without a worker, queued work would set busy forever and block
           destruction; fail creation instead of returning a dead agent. */
        FreeRt(rt);
        return NULL;
    }
    rt->started = true;
    return rt;
}

/* True if the thread has exited (or never started) and rt can be freed. */
static bool StopRt(PicoAgentRt *rt, const struct timespec *deadline)
{
    pthread_mutex_lock(&rt->mu);
    rt->stop = true;
    rt->cancel = true;
    pthread_cond_broadcast(&rt->cv);
    pthread_mutex_unlock(&rt->mu);
    KillToolChildren(rt);
    pthread_mutex_lock(&rt->mu);
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
    return done;
}

void PicoAgent_ReapRetired(PicoWorkspace *workspace)
{
    if (!workspace)
    {
        return;
    }
    PicoAgentRt **pp = &workspace->retired_runtimes;
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
        workspace->retired_count--;
        if (z->started)
        {
            pthread_join(z->thread, NULL);
        }
        FreeRt(z);
    }
}

bool PicoAgent_RetiredReferences(const PicoWorkspace *workspace, PicoAgentId id)
{
    for (const PicoAgentRt *z = workspace ? workspace->retired_runtimes : NULL; z; z = z->zombie_next)
    {
        if (z->context.agent_id == id)
        {
            return true;
        }
    }
    return false;
}

static void DetachRt(PicoAgentRt *rt)
{
    if (rt->started) pthread_detach(rt->thread);
    for (int i = 0; i < PICO_MAX_PENDING_CALLS; i++)
        if (rt->jobs[i] && rt->jobs[i]->started) pthread_detach(rt->jobs[i]->thread);
}

/* Share one shutdown deadline across every retired runtime. */
bool PicoAgent_ShutdownRetired(PicoWorkspace *workspace, const struct timespec *deadline)
{
    bool all_done = true;
    PicoAgentRt *z = workspace ? workspace->retired_runtimes : NULL;
    if (workspace)
    {
        workspace->retired_runtimes = NULL;
        workspace->retired_count = 0;
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
            DetachRt(z);
            all_done = false;
        }
        z = next;
    }
    return all_done;
}

void PicoAgent_Compact(PicoHost *app, PicoAgent *agent)
{
    if (!app || !agent || !agent->runtime || PicoAgent_IsBusy(agent) ||
        !PicoWorkspace_AcceptsNewWork(PicoAgent_Workspace(agent)) || !agent->tool_policy_valid)
    {
        return;
    }
    StartCompact(app, agent);
}

void PicoAgent_RefreshRegistration(PicoHost *app, PicoAgent *agent)
{
    if (agent && agent->runtime)
    {
        RefreshWorkerContext(agent->runtime, app, agent);
    }
}

PicoRegistrationGeneration *PicoAgent_Registration(PicoAgent *agent)
{
    return agent && agent->runtime ? agent->runtime->registration : NULL;
}

PicoAgent *PicoAgent_Create(PicoHost *app, PicoWorkspace *workspace)
{
    PicoAgent *agent = (PicoAgent *)calloc(1, sizeof(PicoAgent));
    if (!agent)
    {
        return NULL;
    }
    agent->workspace = workspace;
    agent->id = app ? app->next_agent_id++ : 1;
    agent->runtime_generation = 1;
    agent->kind = PICO_AGENT_MAIN;
    agent->state = PICO_AGENT_IDLE;
    agent->persistence = PICO_SESSION_EPHEMERAL;
    agent->tool_policy_valid = true;
    PicoSettings_InitAgent(agent);
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
            DetachRt(rt);
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

void PicoAgent_StartTurnParts(PicoHost *app, PicoAgent *agent, const char *user_text,
                              const char *parts_json)
{
    bool has_parts = parts_json && parts_json[0] == '[';
    if (!app || !agent || !agent->runtime || ((!user_text || !user_text[0]) && !has_parts))
    {
        return;
    }
    if (PicoAgent_IsBusy(agent) || !PicoWorkspace_AcceptsNewWork(agent->workspace))
    {
        return;
    }
    if (!PicoAgent_RevalidateToolPolicy(app, agent))
    {
        pico_status_warn(app, "This agent's restricted tool policy references a tool that is not currently registered.");
        return;
    }
    PicoAgent_DismissError(agent);
    int parallel = agent->max_parallel_tools_override ? agent->max_parallel_tools_override
                                                      : agent->workspace->settings.max_parallel_tools;
    agent->runtime->max_parallel_tools = parallel >= 1 && parallel <= PICO_MAX_PARALLEL_TOOLS
                                             ? parallel : PICO_DEFAULT_PARALLEL_TOOLS;
    free(agent->runtime->instructions);
    agent->runtime->instructions = PicoSettings_LoadSystemPrompt(PicoAgent_Workspace(agent));
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
    PushInput(agent->runtime, BuildUserItem(user_text, parts_json));
    StartLlm(app, agent);
}

void PicoAgent_StartTurn(PicoHost *app, PicoAgent *agent, const char *user_text)
{
    PicoAgent_StartTurnParts(app, agent, user_text, NULL);
}

void PicoAgent_Cancel(PicoAgent *agent)
{
    PicoAgentRt *rt = agent ? agent->runtime : NULL;
    if (!rt || !PicoAgent_IsBusy(agent))
    {
        return;
    }
    PicoWorkspace_CancelChildDelegation(agent->workspace, agent->id);
    pthread_mutex_lock(&rt->mu);
    rt->cancel = true;
    pthread_cond_broadcast(&rt->cv);
    pthread_mutex_unlock(&rt->mu);
    PicoWorkspace_CancelDelegations(agent->workspace, agent->id,
                                    agent->runtime_generation);
    for (int i = 0; i < PICO_MAX_PENDING_CALLS; i++)
        if (rt->jobs[i]) rt->jobs[i]->snap_retired = true;
}

void PicoAgent_ForceCancel(PicoHost *app, PicoAgent *agent)
{
    PicoAgentRt *old = agent ? agent->runtime : NULL;
    if (!old || !PicoAgent_IsBusy(agent))
    {
        return;
    }

    PicoWorkspace *workspace = agent->workspace;
    PicoAgent_ReapRetired(workspace);
    if (!workspace || workspace->retired_count >= PICO_MAX_RETIRED_RUNTIMES)
    {
        PicoAgent_Cancel(agent);
        return;
    }

    PicoWorkspace_CancelChildDelegation(workspace, agent->id);
    uint64_t next_generation = agent->runtime_generation + 1;
    PicoAgentRt *rt = CreateRt(app, agent);
    if (!rt)
    {
        PicoAgent_Cancel(agent);
        return;
    }
    pthread_mutex_lock(&workspace->ui_post_mu);
    agent->runtime_generation = next_generation;
    rt->context.runtime_generation = next_generation;
    pthread_mutex_unlock(&workspace->ui_post_mu);
    PicoWorkspace_CancelDelegations(workspace, agent->id,
                                    old->context.runtime_generation);

    pthread_mutex_lock(&old->mu);
    old->cancel = true;
    old->stop = true;
    old->retired = true;
    pthread_cond_broadcast(&old->cv);
    pthread_mutex_unlock(&old->mu);
    KillToolChildren(old);

    int asks_open = 0;
    for (int i = 0; i < PICO_MAX_PENDING_CALLS; i++)
        if (old->jobs[i] && old->jobs[i]->snap_id) asks_open++;
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

    old->zombie_next = workspace->retired_runtimes;
    workspace->retired_runtimes = old;
    workspace->retired_count++;
    agent->runtime = rt;
    while (asks_open-- > 0)
    {
        pico_run_hooks(app, PICO_HOOK_ON_ASK_END, agent->id);
    }
}

void pico_tool_set_child(PicoAgentContext *ctx, pid_t pid)
{
    if (t_worker_context != PICO_WORKER_TOOL || !ctx || t_agent_context != ctx ||
        t_worker_rt != ctx->runtime || !ctx->job)
        return;
    PicoAgentRt *rt = ctx->runtime;
    pthread_mutex_lock(&rt->mu);
    bool cancelled = rt->retired || rt->stop || rt->cancel;
    ctx->job->child = cancelled ? 0 : pid;
    pthread_mutex_unlock(&rt->mu);
    /* Cover cancellation racing with spawn/bind; never orphan a late child. */
    if (cancelled) KillToolChild(pid);
}

static uint64_t NextAskId(PicoAgentRt *rt)
{
    PicoHost *host = NULL;
    if (rt && rt->context.workspace_owner)
    {
        host = rt->context.workspace_owner->host;
    }
    return PicoHost_AllocAskId(host);
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

void pico_ui_post(PicoAgentContext *ctx, const char *name, PicoUiPostKind kind,
                  const char *text, size_t n)
{
    PicoWorkspace *workspace;
    if (t_worker_context != PICO_WORKER_TOOL || !AgentContextActive(ctx))
    {
        return;
    }
    if (!name || !name[0] || strlen(name) >= PICO_UI_MODAL_NAME)
    {
        return;
    }
    if (kind != PICO_UI_POST_TEXT && kind != PICO_UI_POST_STATUS)
    {
        return;
    }
    workspace = ctx->workspace_owner;
    if (!workspace)
    {
        return;
    }
    PicoWorkspace_UiPost(workspace, name, kind, ctx->agent_id, ctx->runtime_generation, text, n);
}

bool pico_agent_ui_latest(const PicoHost *app, PicoAgentId agent_id, const char *name, PicoUiPost *out)
{
    if (out)
    {
        memset(out, 0, sizeof(*out));
    }
    if (!app || !name || !name[0] || agent_id == 0)
    {
        return false;
    }
    PicoAgent *agent = PicoHost_FindAgent((PicoHost *)app, agent_id);
    if (!agent || !agent->workspace)
    {
        return false;
    }
    return PicoWorkspace_UiLatest(agent->workspace, agent_id, name, out);
}

bool pico_ui_latest(const PicoHost *app, const char *name, PicoUiPost *out)
{
    if (out)
    {
        memset(out, 0, sizeof(*out));
    }
    if (!app || !name || !name[0] || app->selected_agent_id == 0)
    {
        return false;
    }
    return pico_agent_ui_latest(app, app->selected_agent_id, name, out);
}

void pico_agent_ui_clear(PicoHost *app, PicoAgentId agent_id, const char *name)
{
    if (!app || !name || !name[0] || agent_id == 0)
    {
        return;
    }
    PicoAgent *agent = PicoHost_FindAgent(app, agent_id);
    if (!agent || !agent->workspace)
    {
        return;
    }
    PicoWorkspace_UiClear(agent->workspace, agent_id, name);
}

void pico_ui_clear(PicoHost *app, const char *name)
{
    if (!app || !name || !name[0] || app->selected_agent_id == 0)
    {
        return;
    }
    pico_agent_ui_clear(app, app->selected_agent_id, name);
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
    PicoToolJob *job = ctx->job;
    pthread_mutex_lock(&rt->mu);
    if (job->ask_waiting)
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
    job->ask_id = NextAskId(rt);
    job->ask_request = req;
    free(job->ask_answer);
    job->ask_answer = NULL;
    job->ask_waiting = true;
    job->ask_done = false;
    while (!job->ask_done && !rt->cancel && !rt->stop)
    {
        pthread_cond_wait(&rt->cv, &rt->mu);
    }
    int rc;
    if (rt->stop || rt->cancel)
    {
        free(job->ask_answer);
        job->ask_answer = NULL;
        rc = PICO_ASK_CANCEL;
    }
    else
    {
        if (answer_json)
        {
            *answer_json = job->ask_answer ? job->ask_answer : Dup("");
        }
        else
        {
            free(job->ask_answer);
        }
        job->ask_answer = NULL;
        rc = PICO_ASK_OK;
    }
    free(job->ask_request);
    job->ask_request = NULL;
    job->ask_waiting = false;
    job->ask_done = false;
    job->ask_id = 0;
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
    PicoToolJob *oldest = NULL;
    if (!rt || !out)
    {
        return false;
    }
    for (int i = 0; i < PICO_MAX_PENDING_CALLS; i++)
    {
        PicoToolJob *job = rt->jobs[i];
        if (job && job->snap_id && job->snap_request && !job->snap_retired &&
            (!oldest || job->snap_id < oldest->snap_id))
        {
            oldest = job;
        }
    }
    if (!oldest)
    {
        return false;
    }
    out->id = oldest->snap_id;
    out->agent_id = agent->id;
    out->profile = agent->profile;
    out->purpose = agent->purpose;
    out->request_json = oldest->snap_request;
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
    PicoToolJob *job = NULL;
    for (int i = 0; i < PICO_MAX_PENDING_CALLS; i++)
    {
        if (rt->jobs[i] && rt->jobs[i]->ask_id == id)
        {
            job = rt->jobs[i];
            break;
        }
    }
    if (!job || !job->ask_waiting || job->ask_done || job->ask_id != id || rt->cancel || rt->stop)
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
    job->ask_answer = ans;
    job->ask_done = true;
    pthread_cond_broadcast(&rt->cv);
    pthread_mutex_unlock(&rt->mu);
    if (job->snap_id == id)
    {
        job->snap_retired = true;
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

void PicoAgent_PumpBounded(PicoHost *app, PicoAgent *agent, int *budget)
{
    PicoAgentRt *rt = agent ? agent->runtime : NULL;
    if (agent && agent->workspace)
    {
        PicoWorkspace_PumpUiPosts(agent->workspace);
    }
    if (!rt)
    {
        return;
    }
    PublishAskSnapshot(app, agent);
    if (agent->runtime != rt) return;

    pthread_mutex_lock(&rt->mu);
    char provider_status[256];
    memcpy(provider_status, rt->provider_status, sizeof(provider_status));
    bool status_dirty = rt->provider_status_dirty;
    rt->provider_status_dirty = false;
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
    char **summary_parts = NULL;
    int summary_part_count = rt->summary_part_count;
    if (summary && summary_len && summary_part_count > 0 && rt->summary_parts)
    {
        summary_parts = (char **)calloc((size_t)summary_part_count, sizeof(char *));
        if (summary_parts)
        {
            for (int i = 0; i < summary_part_count; i++)
            {
                summary_parts[i] = rt->summary_parts[i] ? Dup(rt->summary_parts[i]) : NULL;
            }
        }
    }
    rt->summary = NULL;
    rt->summary_len = 0;
    rt->summary_cap = 0;

    PicoProvStream *prov_updates = NULL;
    int prov_update_count = 0;
    if (rt->prov_dirty)
    {
        prov_updates = (PicoProvStream *)calloc((size_t)rt->prov_count, sizeof(PicoProvStream));
        if (prov_updates)
        {
            for (int i = 0; i < rt->prov_count; i++)
            {
                PicoProvStream *e = &rt->prov[i];
                if (!e->dirty)
                {
                    continue;
                }
                prov_updates[prov_update_count].call_index = e->call_index;
                prov_updates[prov_update_count].call_id = e->call_id ? Dup(e->call_id) : NULL;
                prov_updates[prov_update_count].name = e->name ? Dup(e->name) : NULL;
                prov_updates[prov_update_count].args_bytes = e->args_bytes;
                prov_update_count++;
                e->dirty = false;
            }
            rt->prov_dirty = false;
        }
    }

    int max_to_drain = rt->event_count;
    if (budget && *budget >= 0 && max_to_drain > *budget)
    {
        max_to_drain = *budget;
    }
    PicoAgentEv *events = NULL;
    int event_count = max_to_drain;
    if (event_count > 0)
    {
        events = (PicoAgentEv *)malloc((size_t)event_count * sizeof(PicoAgentEv));
        if (events)
        {
            memcpy(events, rt->events, (size_t)event_count * sizeof(PicoAgentEv));
            if (rt->event_count > event_count)
            {
                memmove(rt->events, rt->events + event_count, (size_t)(rt->event_count - event_count) * sizeof(PicoAgentEv));
            }
            rt->event_count -= event_count;
            if (budget)
            {
                *budget -= event_count;
            }
        }
        else
        {
            event_count = 0;
        }
    }
    pthread_mutex_unlock(&rt->mu);

    if (status_dirty && agent->state == PICO_AGENT_LLM_WAIT)
        SetActivity(app, agent, provider_status[0] ? provider_status : "Thinking…");

    if (stream && stream_len && rt->stream_msg >= 0)
    {
        AppendMessageText(app, agent, rt->stream_msg, stream, stream_len);
        rt->stream_dirty = true;
        SetActivity(app, agent, "Writing…");
    }
    free(stream);

    if (think && think_len && rt->stream_msg >= 0)
    {
        TraceAppendThink(app, agent, rt->stream_msg, think, think_len);
    }
    free(think);

    if (summary && summary_len && rt->stream_msg >= 0)
    {
        if (summary_parts && summary_part_count > 0)
        {
            for (int i = 0; i < summary_part_count; i++)
            {
                if (summary_parts[i] && summary_parts[i][0])
                {
                    TraceSetThinkSummary(app, agent, rt->stream_msg, summary_parts[i],
                                         strlen(summary_parts[i]), i + 1);
                }
            }
        }
        else
        {
            TraceSetThinkSummary(app, agent, rt->stream_msg, summary, summary_len, summary_steps);
        }
    }
    free(summary);
    if (summary_parts)
    {
        for (int i = 0; i < summary_part_count; i++)
        {
            free(summary_parts[i]);
        }
        free(summary_parts);
    }

    if (prov_update_count > 0 && agent->state == PICO_AGENT_LLM_WAIT)
    {
        for (int i = 0; i < prov_update_count; i++)
        {
            ProvRowSync(app, agent, rt, &prov_updates[i]);
        }
    }
    for (int i = 0; i < prov_update_count; i++)
    {
        free(prov_updates[i].call_id);
        free(prov_updates[i].name);
    }
    free(prov_updates);

    if (rt->stream_dirty)
    {
        ReparseMessage(app, agent, rt->stream_msg);
        rt->stream_dirty = false;
    }

    for (int i = 0; i < event_count; i++)
    {
        PicoAgentEv *ev = &events[i];
        /* A main-thread callback may force-cancel while this drain is in progress. */
        if (agent->runtime == rt) switch (ev->type)
        {
        case PICO_AEV_LLM_DONE:
            OnLlmDone(app, agent, ev);
            break;
        case PICO_AEV_LLM_FAIL:
            if (!rt->compacting)
            {
                SavePartialAssistant(app, agent);
            }
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

void PicoAgent_Pump(PicoHost *app, PicoAgent *agent)
{
    PicoAgent_PumpBounded(app, agent, NULL);
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
    if (agent && agent->runtime) PushInput(agent->runtime, BuildUserItem(text, NULL));
}

void PicoAgent_PushHistoryUserParts(PicoAgent *agent, const char *text, const char *parts_json)
{
    if (agent && agent->runtime) PushInput(agent->runtime, BuildUserItem(text, parts_json));
}

void PicoAgent_PushHistoryAssistant(PicoAgent *agent, const char *text, const char *thinking,
                                    const char *signature)
{
    if (agent && agent->runtime) PushInput(agent->runtime, BuildAssistantItem(text, thinking, signature));
}

void PicoAgent_PushHistoryAssistantParts(PicoAgent *agent, const char *text, const char *thinking,
                                         const char *signature, const char *parts_json)
{
    if (!agent || !agent->runtime)
    {
        return;
    }
    if (parts_json && parts_json[0] == '[')
    {
        PushInput(agent->runtime, BuildAssistantItemFromParts(parts_json, thinking, signature));
        return;
    }
    PushInput(agent->runtime, BuildAssistantItem(text, thinking, signature));
}

void PicoAgent_PushHistoryFunctionCall(PicoAgent *agent, const char *call_id, const char *name, const char *args,
                                       const char *item_id)
{
    if (agent && agent->runtime)
    {
        PushInput(agent->runtime, pico_canonical_tool_call_json(call_id, name, args, item_id));
    }
}

void PicoAgent_PushHistoryFunctionOutput(PicoAgent *agent, const char *call_id, const char *name,
                                         const char *output, bool is_error)
{
    if (agent && agent->runtime) PushFunctionOutput(agent->runtime, call_id, name, output, is_error);
}

void PicoAgent_AppendThink(PicoHost *app, PicoAgent *agent, const char *text, int think_ms)
{
    if (!agent || !text || !text[0] || agent->message_count <= 0)
    {
        return;
    }
    int idx = agent->message_count - 1;
    if (agent->messages[idx].role != PICO_ROLE_ASSISTANT ||
        HasTrailingThinkTrace(&agent->messages[idx]))
    {
        return;
    }
    TraceAppendThink(app, agent, idx, text, strlen(text));
    PicoTraceLine *line = TrailingThinkLine(&agent->messages[idx]);
    if (!line)
    {
        return;
    }
    line->think_t0 = 0.0;
    if (think_ms > 0 && line->think_ms == 0)
    {
        line->think_ms = think_ms;
    }
}

void PicoAgent_AppendThinkSummary(PicoHost *app, PicoAgent *agent, const char *text,
                                  int step, int think_ms)
{
    if (!agent || !text || !text[0] || step <= 0 || agent->message_count <= 0)
    {
        return;
    }
    int idx = agent->message_count - 1;
    if (agent->messages[idx].role != PICO_ROLE_ASSISTANT)
    {
        return;
    }
    TraceSetThinkSummary(app, agent, idx, text, strlen(text), step);
    PicoTraceLine *line = TrailingThinkLine(&agent->messages[idx]);
    if (!line)
    {
        return;
    }
    line->think_t0 = 0.0;
    if (think_ms > 0 && line->think_ms == 0)
    {
        line->think_ms = think_ms;
    }
}

char *PicoAgent_BuildInstructionsSpans(PicoHost *app, PicoAgent *agent, PicoPromptSpan *spans,
                                       int *span_count)
{
    if (span_count)
    {
        *span_count = 0;
    }
    if (!app)
    {
        return JsonDup("");
    }
    PicoPromptSpan base_spans[PICO_PROMPT_SPAN_MAX];
    int base_count = 0;
    if (agent && agent->runtime)
    {
        RefreshWorkerContext(agent->runtime, app, agent);
    }
    char *base = PicoSettings_LoadSystemPromptSpans(PicoAgent_Workspace(agent), base_spans, &base_count);
    size_t base_len = base ? strlen(base) : 0;
    char *instr = NULL;
    PicoTool *tools = NULL;
    int tool_count = 0;
    RunLlmHooks(agent ? agent->runtime : NULL, agent, false, true, base, &instr, &tools,
                 &tool_count);
    free(base);
    free(tools);
    if (!instr)
    {
        instr = JsonDup("");
    }
    if (spans && span_count)
    {
        int n = 0;
        for (int i = 0; i < base_count && n < PICO_PROMPT_SPAN_MAX; i++)
        {
            spans[n++] = base_spans[i];
        }
        size_t instr_len = strlen(instr);
        if (instr_len > base_len && n < PICO_PROMPT_SPAN_MAX)
        {
            size_t start = base_len ? base_len + 2 : 0;
            if (start < instr_len)
            {
                spans[n].source = PICO_PROMPT_SOURCE_LLM_HOOK;
                spans[n].start = start;
                spans[n].length = instr_len - start;
                n++;
            }
        }
        *span_count = n;
    }
    return instr;
}

char *PicoAgent_BuildInstructions(PicoHost *app, PicoAgent *agent)
{
    return PicoAgent_BuildInstructionsSpans(app, agent, NULL, NULL);
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

uint64_t pico_agent_context_registration_generation(const PicoAgentContext *ctx)
{
    return AgentContextActive(ctx) ? ctx->registration_generation : 0;
}

PicoWorkspaceId pico_agent_context_workspace_id(const PicoAgentContext *ctx)
{
    return AgentContextActive(ctx) ? ctx->workspace_id : 0;
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

PicoWorkspace *PicoAgentContext_Workspace(const PicoAgentContext *ctx)
{
    return AgentContextActive(ctx) ? ctx->workspace_owner : NULL;
}

const char *PicoAgentContext_ToolCallId(const PicoAgentContext *ctx)
{
    return AgentContextActive(ctx) && ctx->tool_call_id ? ctx->tool_call_id : "";
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
