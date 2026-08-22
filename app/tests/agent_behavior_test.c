#define _POSIX_C_SOURCE 200809L

#include "agent.h"
#include "agent_manager.h"
#include "json.h"
#include "pico/plugin.h"
#include "settings.h"
#include "usage.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

typedef enum TestMode {
    TEST_SINGLE = 0,
    TEST_SEQUENTIAL,
    TEST_INVALID,
    TEST_BLOCK,
    TEST_PROVIDER_BLOCK,
    TEST_CATALOG_BLOCK,
    TEST_DUPLICATE_CALLS,
    TEST_EMPTY_CALL_ID,
    TEST_TOO_MANY_CALLS,
    TEST_CONCURRENT_REVERSE,
} TestMode;

typedef struct TestState {
    pthread_mutex_t mu;
    pthread_cond_t cv;
    TestMode mode;
    int provider_tool_limit;
    int provider_tools_issued;
    int tool_invocations;
    int ask_rc[4][2];
    char *ask_answer[4][2];
    bool block_entered;
    int block_entered_count;
    bool block_release;
    bool block_done;
    bool block_saw_live_state;
    int provider_entered_count;
    PicoAgentId first_provider_id;
    char issue_tool_name[64];
    char issue_tool_args[2048];
    char *last_instructions;
    int last_tool_count;
    char last_tools[512];
    char *last_input;
    char *tool_seen_args;
    char *after_seen_args;
    char *after_seen_output;
    char *after_seen_details;
    bool after_executed;
    bool after_is_error;
    int apply_calls;
    bool context_saw_base_tail;
    int followup_hook_calls;
    bool provider_fail;
    int provider_tokens;
    int provider_cached_tokens;
    int usage_log_count;
    bool emit_think_summaries;
    int life_turn_end;
    int life_cancel;
    int life_error;
    int life_after_compact;
    PicoAgentId tool_ctx_id;
    PicoAgentId after_agent_id;
    PicoAgentId apply_agent_id;
    PicoAgentId llm_agent_id;
    PicoAgentId context_agent_id;
    PicoAgentId hook_agent_id;
} TestState;

static TestState g_test = {
    .mu = PTHREAD_MUTEX_INITIALIZER,
    .cv = PTHREAD_COND_INITIALIZER,
};
static int g_plugin_shutdowns;
static int g_auth_frees;
static char g_config_dir[4096] = "/tmp/pico-agent-behavior";

static void ResetTest(TestMode mode, int tool_limit)
{
    pthread_mutex_lock(&g_test.mu);
    for (int i = 0; i < 4; i++)
    {
        for (int j = 0; j < 2; j++)
        {
            free(g_test.ask_answer[i][j]);
            g_test.ask_answer[i][j] = NULL;
            g_test.ask_rc[i][j] = -1;
        }
    }
    g_test.mode = mode;
    g_test.provider_tool_limit = tool_limit;
    g_test.provider_tools_issued = 0;
    g_test.tool_invocations = 0;
    g_test.block_entered = false;
    g_test.block_entered_count = 0;
    g_test.block_release = false;
    g_test.block_done = false;
    g_test.block_saw_live_state = false;
    g_test.provider_entered_count = 0;
    g_test.first_provider_id = 0;
    snprintf(g_test.issue_tool_name, sizeof(g_test.issue_tool_name), "ask_test");
    snprintf(g_test.issue_tool_args, sizeof(g_test.issue_tool_args), "{}");
    free(g_test.last_instructions);
    g_test.last_instructions = NULL;
    g_test.last_tool_count = 0;
    g_test.last_tools[0] = '\0';
    free(g_test.last_input);
    g_test.last_input = NULL;
    free(g_test.tool_seen_args);
    g_test.tool_seen_args = NULL;
    free(g_test.after_seen_args);
    g_test.after_seen_args = NULL;
    free(g_test.after_seen_output);
    g_test.after_seen_output = NULL;
    free(g_test.after_seen_details);
    g_test.after_seen_details = NULL;
    g_test.after_executed = false;
    g_test.after_is_error = false;
    g_test.apply_calls = 0;
    g_test.context_saw_base_tail = false;
    g_test.followup_hook_calls = 0;
    g_test.provider_fail = false;
    g_test.provider_tokens = 0;
    g_test.provider_cached_tokens = 0;
    g_test.usage_log_count = 0;
    g_test.emit_think_summaries = false;
    g_test.life_turn_end = 0;
    g_test.life_cancel = 0;
    g_test.life_error = 0;
    g_test.life_after_compact = 0;
    g_test.tool_ctx_id = 0;
    g_test.after_agent_id = 0;
    g_test.apply_agent_id = 0;
    g_test.llm_agent_id = 0;
    g_test.context_agent_id = 0;
    g_test.hook_agent_id = 0;
    pthread_mutex_unlock(&g_test.mu);
    g_plugin_shutdowns = 0;
    g_auth_frees = 0;
}

static int Fail(const char *test, const char *message)
{
    fprintf(stderr, "%s: %s\n", test, message);
    return 1;
}

static void SleepOneMs(void)
{
    struct timespec delay = {.tv_nsec = 1000000L};
    nanosleep(&delay, NULL);
}

static void SnapshotTurn(const PicoLlmTurn *turn)
{
    free(g_test.last_instructions);
    g_test.last_instructions = JsonDup(turn && turn->instructions ? turn->instructions : "");
    g_test.last_tool_count = turn ? turn->tool_count : 0;
    g_test.last_tools[0] = '\0';
    if (turn && turn->tools)
    {
        size_t n = 0;
        for (int i = 0; i < turn->tool_count; i++)
        {
            const char *nm = turn->tools[i].name ? turn->tools[i].name : "";
            int wrote = snprintf(g_test.last_tools + n, sizeof(g_test.last_tools) - n, "%s%s", i ? "," : "", nm);
            if (wrote < 0 || n + (size_t)wrote + 1 >= sizeof(g_test.last_tools))
            {
                break;
            }
            n += (size_t)wrote;
        }
    }
    free(g_test.last_input);
    JsonBuf b;
    JsonBuf_Init(&b);
    if (turn && turn->input_json)
    {
        for (int i = 0; i < turn->input_count; i++)
        {
            JsonBuf_Puts(&b, turn->input_json[i] ? turn->input_json[i] : "");
            JsonBuf_Puts(&b, "\n");
        }
    }
    g_test.last_input = JsonBuf_Steal(&b);
}

static int FakeProvider(PicoAgentContext *ctx, const PicoLlmTurn *turn, PicoLlmCancelFn cancel,
                        PicoLlmDeltaFn on_delta, void *user, PicoLlmResult *out)
{
    (void)ctx;

    pthread_mutex_lock(&g_test.mu);
    SnapshotTurn(turn);
    TestMode mode = g_test.mode;
    bool fail = g_test.provider_fail;
    int tokens = g_test.provider_tokens;
    int cached_tokens = g_test.provider_cached_tokens;
    bool emit_summaries = g_test.emit_think_summaries;
    bool issue_tool = !fail && g_test.provider_tools_issued < g_test.provider_tool_limit;
    int call_number = ++g_test.provider_tools_issued;
    char tool_name[64];
    char tool_args[2048];
    snprintf(tool_name, sizeof(tool_name), "%s", g_test.issue_tool_name);
    snprintf(tool_args, sizeof(tool_args), "%s", g_test.issue_tool_args);
    pthread_mutex_unlock(&g_test.mu);

    if (fail)
    {
        out->error = JsonDup("provider failed");
        return PICO_LLM_FAIL;
    }

    out->input_tokens = tokens;
    out->cached_tokens = cached_tokens;
    if (mode == TEST_PROVIDER_BLOCK)
    {
        while (!cancel(user))
        {
            SleepOneMs();
        }
        return PICO_LLM_CANCEL;
    }
    if (mode == TEST_CONCURRENT_REVERSE)
    {
        pthread_mutex_lock(&g_test.mu);
        int entered = ++g_test.provider_entered_count;
        if (entered == 1)
        {
            g_test.first_provider_id = pico_agent_context_id(ctx);
        }
        pthread_cond_broadcast(&g_test.cv);
        while (entered == 1 && !g_test.block_release)
        {
            pthread_cond_wait(&g_test.cv, &g_test.mu);
        }
        pthread_mutex_unlock(&g_test.mu);
        out->assistant_text = JsonDup(entered == 1 ? "first finished" : "second finished");
        return PICO_LLM_OK;
    }
    if (mode == TEST_CATALOG_BLOCK)
    {
        pthread_mutex_lock(&g_test.mu);
        g_test.block_entered = true;
        pthread_cond_broadcast(&g_test.cv);
        while (!g_test.block_release)
        {
            pthread_cond_wait(&g_test.cv, &g_test.mu);
        }
        pthread_mutex_unlock(&g_test.mu);
    }

    if (emit_summaries && on_delta)
    {
        if (issue_tool)
        {
            on_delta(user, PICO_LLM_DELTA_THINKING_SUMMARY, "", 0);
            on_delta(user, PICO_LLM_DELTA_THINKING_SUMMARY, "**first**", 9);
            on_delta(user, PICO_LLM_DELTA_THINKING_SUMMARY, "", 0);
            on_delta(user, PICO_LLM_DELTA_THINKING_SUMMARY, "**second**", 10);
        }
        else
        {
            on_delta(user, PICO_LLM_DELTA_THINKING_SUMMARY, "", 0);
            on_delta(user, PICO_LLM_DELTA_THINKING_SUMMARY, "**third**", 9);
        }
    }

    if (!issue_tool)
    {
        out->assistant_text = JsonDup("done");
        return PICO_LLM_OK;
    }

    int emitted = mode == TEST_TOO_MANY_CALLS ? 17 :
                  (mode == TEST_DUPLICATE_CALLS ? 2 : 1);
    out->calls = (PicoLlmToolCall *)calloc((size_t)emitted, sizeof(PicoLlmToolCall));
    if (!out->calls)
    {
        out->error = JsonDup("allocation failed");
        return PICO_LLM_FAIL;
    }
    for (int i = 0; i < emitted; i++)
    {
        char call_id[32];
        snprintf(call_id, sizeof(call_id), "call-%d-%d", call_number, i);
        if (mode == TEST_EMPTY_CALL_ID && i == 0)
        {
            call_id[0] = '\0';
        }
        else if (mode == TEST_DUPLICATE_CALLS && i == 1)
        {
            snprintf(call_id, sizeof(call_id), "call-%d-0", call_number);
        }
        out->calls[i].call_id = JsonDup(call_id);
        out->calls[i].name = JsonDup(tool_name);
        out->calls[i].arguments = JsonDup(tool_args);
    }
    out->call_count = emitted;
    return PICO_LLM_OK;
}

static void StoreAskResult(int invocation, int step, int rc, char *answer)
{
    pthread_mutex_lock(&g_test.mu);
    g_test.ask_rc[invocation][step] = rc;
    g_test.ask_answer[invocation][step] = answer;
    pthread_cond_broadcast(&g_test.cv);
    pthread_mutex_unlock(&g_test.mu);
}

static void AskTool(PicoAgentContext *ctx, const char *args_json, PicoToolResult *out)
{
    (void)args_json;
    if (out)
    {
        memset(out, 0, sizeof(*out));
    }

    pthread_mutex_lock(&g_test.mu);
    int invocation = g_test.tool_invocations++;
    TestMode mode = g_test.mode;
    if (mode == TEST_BLOCK)
    {
        g_test.block_entered = true;
        g_test.block_entered_count++;
        pthread_cond_broadcast(&g_test.cv);
        while (!g_test.block_release)
        {
            pthread_cond_wait(&g_test.cv, &g_test.mu);
        }
        g_test.block_saw_live_state = pico_agent_context_id(ctx) != 0 &&
                                      pico_agent_context_generation(ctx) != 0;
        g_test.block_done = true;
        pthread_cond_broadcast(&g_test.cv);
        pthread_mutex_unlock(&g_test.mu);
        if (out)
        {
            out->output = JsonDup("unblocked");
        }
        return;
    }
    pthread_mutex_unlock(&g_test.mu);

    if (mode == TEST_INVALID)
    {
        char *answer = NULL;
        int rc = pico_tool_ask(ctx, "{not-json", &answer);
        StoreAskResult(invocation, 0, rc, answer);
        answer = NULL;
        rc = pico_tool_ask(ctx, "{\"type\":\"confirm\"}", &answer);
        StoreAskResult(invocation, 1, rc, answer);
    }
    else
    {
        char request[128];
        snprintf(request, sizeof(request),
                 "{\"type\":\"confirm\",\"message\":\"request-%d-first\"}", invocation);
        char *answer = NULL;
        int rc = pico_tool_ask(ctx, request, &answer);
        StoreAskResult(invocation, 0, rc, answer);

        if (mode == TEST_SEQUENTIAL && rc == PICO_ASK_OK)
        {
            snprintf(request, sizeof(request),
                     "{\"type\":\"confirm\",\"message\":\"request-%d-second\"}", invocation);
            answer = NULL;
            rc = pico_tool_ask(ctx, request, &answer);
            StoreAskResult(invocation, 1, rc, answer);
        }
    }

    if (out)
    {
        out->output = JsonDup("tool complete");
    }
}

static void EchoTool(PicoAgentContext *ctx, const char *args_json, PicoToolResult *out)
{
    (void)ctx;
    pthread_mutex_lock(&g_test.mu);
    g_test.tool_invocations++;
    g_test.tool_ctx_id = pico_agent_context_id(ctx);
    free(g_test.tool_seen_args);
    g_test.tool_seen_args = JsonDup(args_json ? args_json : "");
    pthread_mutex_unlock(&g_test.mu);
    if (out)
    {
        out->output = JsonDup("echo-out");
    }
}

static void DetailsTool(PicoAgentContext *ctx, const char *args_json, PicoToolResult *out)
{
    (void)ctx;
    (void)args_json;
    if (!out)
    {
        return;
    }
    out->output = JsonDup("details-out");
    out->details_json = JsonDup("{\"value\":7}");
}

static void InvalidDetailsTool(PicoAgentContext *ctx, const char *args_json, PicoToolResult *out)
{
    (void)ctx;
    (void)args_json;
    if (!out)
    {
        return;
    }
    out->output = JsonDup("bad-details-out");
    out->details_json = JsonDup("{\"value\":\"\xC3(\"}");
}

static bool ApplyDetails(PicoApp *app, PicoAgentId agent_id, const char *details_json, bool replay)
{
    (void)app;
    (void)agent_id;
    (void)replay;
    if (!details_json || !strstr(details_json, "\"value\":7"))
    {
        return false;
    }
    pthread_mutex_lock(&g_test.mu);
    g_test.apply_calls++;
    g_test.apply_agent_id = agent_id;
    pthread_mutex_unlock(&g_test.mu);
    return true;
}

static void DenyBefore(PicoAgentContext *ctx, PicoToolEvent *ev)
{
    (void)ctx;
    ev->deny = true;
}

static void AskBefore(PicoAgentContext *ctx, PicoToolEvent *ev)
{
    (void)ev;
    char *answer = NULL;
    int rc = pico_tool_ask(ctx, "{\"type\":\"confirm\",\"message\":\"gate\"}", &answer);
    pthread_mutex_lock(&g_test.mu);
    g_test.ask_rc[0][0] = rc;
    free(g_test.ask_answer[0][0]);
    g_test.ask_answer[0][0] = answer;
    pthread_mutex_unlock(&g_test.mu);
}

static void RewriteArgsBefore(PicoAgentContext *ctx, PicoToolEvent *ev)
{
    (void)ctx;
    ev->args_json_out = JsonDup("{\"rewritten\":true}");
}

static void BlockingBefore(PicoAgentContext *ctx, PicoToolEvent *ev)
{
    (void)ctx;
    (void)ev;
    pthread_mutex_lock(&g_test.mu);
    g_test.block_entered = true;
    pthread_cond_broadcast(&g_test.cv);
    while (!g_test.block_release)
    {
        pthread_cond_wait(&g_test.cv, &g_test.mu);
    }
    g_test.block_saw_live_state = pico_agent_context_id(ctx) != 0;
    g_test.block_done = true;
    pthread_cond_broadcast(&g_test.cv);
    pthread_mutex_unlock(&g_test.mu);
}

static void CountBefore(PicoAgentContext *ctx, PicoToolEvent *ev)
{
    (void)ctx;
    (void)ev;
    pthread_mutex_lock(&g_test.mu);
    g_test.followup_hook_calls++;
    pthread_mutex_unlock(&g_test.mu);
}

static void RewriteOutAfter(PicoApp *app, PicoAgentId agent_id, PicoToolEvent *ev)
{
    (void)app;
    (void)agent_id;
    ev->result = JsonDup("rewritten-output");
}

static void CaptureAfter(PicoApp *app, PicoAgentId agent_id, PicoToolEvent *ev)
{
    (void)app;
    (void)agent_id;
    pthread_mutex_lock(&g_test.mu);
    free(g_test.after_seen_args);
    g_test.after_seen_args = JsonDup(ev->args_json ? ev->args_json : "");
    free(g_test.after_seen_output);
    g_test.after_seen_output = JsonDup(ev->output ? ev->output : "");
    free(g_test.after_seen_details);
    g_test.after_seen_details = JsonDup(ev->details_json ? ev->details_json : "");
    g_test.after_executed = ev->executed;
    g_test.after_is_error = ev->is_error;
    g_test.after_agent_id = agent_id;
    pthread_mutex_unlock(&g_test.mu);
}

static void LifeTurnEnd(PicoApp *app, const PicoHookEvent *event)
{
    (void)app;
    (void)event;
    g_test.life_turn_end++;
    g_test.hook_agent_id = event ? event->agent_id : 0;
}

static void LifeCancel(PicoApp *app, const PicoHookEvent *event)
{
    (void)app;
    (void)event;
    g_test.life_cancel++;
}

static void LifeError(PicoApp *app, const PicoHookEvent *event)
{
    (void)app;
    (void)event;
    g_test.life_error++;
}

static void LifeAfterCompact(PicoApp *app, const PicoHookEvent *event)
{
    (void)app;
    (void)event;
    g_test.life_after_compact++;
}

static void ExtraInstructions(PicoApp *app, PicoAgentId agent_id, PicoLlmEvent *ev)
{
    (void)app;
    g_test.llm_agent_id = agent_id;
    ev->extra_instructions = JsonDup("injected-line");
}

static void ExtraWhenTools(PicoApp *app, PicoAgentId agent_id, PicoLlmEvent *ev)
{
    (void)app;
    (void)agent_id;
    if (!ev->include_tools || ev->tool_count == 0)
    {
        return;
    }
    ev->extra_instructions = JsonDup("tool-notes");
}

static void ExcludeAskTest(PicoApp *app, PicoAgentId agent_id, PicoLlmEvent *ev)
{
    (void)app;
    (void)agent_id;
    if (!ev->exclude)
    {
        return;
    }
    for (int i = 0; i < ev->tool_count; i++)
    {
        if (ev->tools[i].name && strcmp(ev->tools[i].name, "ask_test") == 0)
        {
            ev->exclude[i] = true;
        }
    }
}

static void ExcludeAskUser(PicoApp *app, PicoAgentId agent_id, PicoLlmEvent *ev)
{
    (void)app;
    (void)agent_id;
    for (int i = 0; ev && ev->exclude && i < ev->tool_count; i++)
    {
        if (ev->tools[i].name && strcmp(ev->tools[i].name, "ask_user") == 0)
        {
            ev->exclude[i] = true;
        }
    }
}

static void AddEphemeralContext(PicoApp *app, PicoAgentId agent_id, PicoContextEvent *ev)
{
    (void)app;
    g_test.context_agent_id = agent_id;
    ev->extra_context = JsonDup("ephemeral-context");
}

static void InspectBaseContext(PicoApp *app, PicoAgentId agent_id, PicoContextEvent *ev)
{
    (void)app;
    (void)agent_id;
    bool saw_user_tail = false;
    if (ev->history_count > 0)
    {
        const char *last = ev->history_json[ev->history_count - 1];
        saw_user_tail = last && strstr(last, "\"type\":\"user\"") != NULL &&
                        strstr(last, "ephemeral-context") == NULL;
    }
    pthread_mutex_lock(&g_test.mu);
    g_test.context_saw_base_tail = saw_user_tail;
    pthread_mutex_unlock(&g_test.mu);
}

/* Minimal host implementations used by the real agent worker. */
void PicoSettings_Load(PicoApp *app)
{
    (void)app;
}

char *PicoSettings_LoadSystemPrompt(const PicoApp *app)
{
    (void)app;
    return JsonDup("");
}

PicoModel *PicoSettings_ActiveModel(PicoApp *app, const PicoAgent *agent)
{
    (void)agent;
    return app && app->model_count > 0 ? &app->models[0] : NULL;
}

const PicoModel *PicoSettings_ActiveModelConst(const PicoApp *app, const PicoAgent *agent)
{
    (void)agent;
    return app && app->model_count > 0 ? &app->models[0] : NULL;
}

const PicoModel *PicoSettings_FindModelConst(const PicoApp *app, const char *id)
{
    if (!app || !id) return NULL;
    for (int i = 0; i < app->model_count; i++)
    {
        if (strcmp(app->models[i].id, id) == 0) return &app->models[i];
    }
    return NULL;
}

bool PicoSettings_EffortAllowed(const PicoModel *model, const char *effort)
{
    if (!model || !effort) return false;
    if (strcmp(effort, "none") == 0 && model->effort_count == 0) return true;
    for (int i = 0; i < model->effort_count; i++)
    {
        if (strcmp(model->effort[i], effort) == 0) return true;
    }
    return false;
}

void PicoSettings_SyncAgent(const PicoApp *app, PicoAgent *agent)
{
    (void)app;
    (void)agent;
}

const char *PicoSettings_ActiveEffort(const PicoAgent *agent)
{
    (void)agent;
    return "none";
}


void PicoSettings_InitAgent(const PicoApp *app, PicoAgent *agent)
{
    if (!agent) return;
    snprintf(agent->model, sizeof(agent->model), "test-model");
    snprintf(agent->effort, sizeof(agent->effort), "none");
    agent->context_limit = 1000;
    agent->compact_enabled = app ? app->settings.compact_enabled : false;
    agent->compact_ratio = app ? app->settings.compact_ratio : 0.9;
}
void Pico_ConfigDir(char *out, size_t cap)
{
    snprintf(out, cap, "%s", g_config_dir);
}

void Pico_MkdirP(const char *path)
{
    char copy[4096];
    snprintf(copy, sizeof(copy), "%s", path ? path : "");
    for (char *p = copy + 1; *p; p++)
    {
        if (*p == '/')
        {
            *p = '\0';
            mkdir(copy, 0700);
            *p = '/';
        }
    }
    mkdir(copy, 0700);
}

void Pico_RandomHex(char *out, size_t cap)
{
    if (cap > 0)
    {
        snprintf(out, cap, "0123456789abcdef");
    }
}

MdDocument MdDocument_ParseEx(const char *src, size_t length, int flags)
{
    (void)src;
    (void)length;
    (void)flags;
    return (MdDocument){0};
}

void MdDocument_Free(MdDocument *doc)
{
    if (doc)
    {
        memset(doc, 0, sizeof(*doc));
    }
}

void PicoSession_LogUsage(PicoApp *app, PicoAgent *agent, int input_tokens, int cached_tokens)
{
    (void)app;
    (void)agent;
    (void)input_tokens;
    (void)cached_tokens;
    g_test.usage_log_count++;
}

void PicoSession_LogAssistant(PicoApp *app, PicoAgent *agent, const char *content)
{
    (void)app;
    (void)agent;
    (void)content;
}

void PicoSession_LogToolCall(PicoApp *app, PicoAgent *agent, const char *call_id, const char *name, const char *args)
{
    (void)app;
    (void)agent;
    (void)call_id;
    (void)name;
    (void)args;
}

void PicoSession_LogToolResult(PicoApp *app, PicoAgent *agent, const char *call_id, const char *name, const char *output,
                               bool is_error, const char *details_json)
{
    (void)app;
    (void)agent;
    (void)call_id;
    (void)name;
    (void)output;
    (void)is_error;
    (void)details_json;
}

void PicoSession_LogCompaction(PicoApp *app, PicoAgent *agent, const char *summary, int tokens_before)
{
    (void)app;
    (void)agent;
    (void)summary;
    (void)tokens_before;
}

void PicoPlugins_Load(PicoApp *app)
{
    (void)app;
}

void PicoPlugins_Shutdown(PicoApp *app)
{
    (void)app;
    g_plugin_shutdowns++;
}

void PicoAuth_Load(PicoApp *app)
{
    (void)app;
}

void PicoSession_Start(PicoApp *app, PicoAgent *agent, PicoSessionStart start, const char *session_file)
{
    (void)app; (void)agent; (void)start; (void)session_file;
}

int PicoSession_Resolve(const PicoApp *app, const char *id, bool allow_prefix,
                        char *path, size_t path_cap)
{
    (void)app; (void)id; (void)allow_prefix; (void)path; (void)path_cap;
    return -1;
}

int PicoSession_Replay(PicoApp *app, PicoAgent *agent, const char *path, bool append_interrupted)
{
    (void)app; (void)agent; (void)path; (void)append_interrupted;
    return -1;
}

void PicoSession_ReplayToolDetails(PicoApp *app, PicoAgent *agent)
{
    (void)app; (void)agent;
}

void PicoSession_AppendInterrupted(PicoApp *app, PicoAgent *agent)
{
    (void)app; (void)agent;
}

void PicoSession_Reset(PicoApp *app, PicoAgent *agent)
{
    (void)app; (void)agent;
}

void PicoAuth_Free(PicoApp *app)
{
    (void)app;
    g_auth_frees++;
}

void PicoChatSel_Clear(PicoApp *app)
{
    if (app)
    {
        app->chat_sel.msg = -1;
    }
}

static void InitApp(PicoApp *app)
{
    memset(app, 0, sizeof(*app));
    app->settings.compact_enabled = false;
    app->models = (PicoModel *)calloc(1, sizeof(PicoModel));
    app->model_count = app->models ? 1 : 0;
    if (app->models)
    {
        snprintf(app->models[0].id, sizeof(app->models[0].id), "test-model");
        snprintf(app->models[0].provider, sizeof(app->models[0].provider), "test");
    }
    pico_add_provider(app, &(PicoProvider){.name = "test", .stream = FakeProvider});
    pico_add_tool(app, "ask_test", "test", "{}", AskTool, NULL);
    app->agents = PicoAgentManager_Create(app);
    PicoAgentCreateOptions options = {
        .kind = PICO_AGENT_NORMAL,
        .session_start = PICO_SESSION_NONE,
        .select = true,
    };
    PicoAgentId id = 0;
    (void)pico_agent_create(app, &options, &id);
}

static bool WaitForPending(PicoApp *app, uint64_t different_from, PicoToolAsk *out)
{
    for (int i = 0; i < 3000; i++)
    {
        PicoAgent_Pump(app, PicoApp_ActiveAgent(app));
        PicoToolAsk ask;
        if (pico_tool_pending_ask(app, &ask) && ask.id != different_from)
        {
            *out = ask;
            return true;
        }
        SleepOneMs();
    }
    return false;
}

static bool WaitForIdle(PicoApp *app)
{
    for (int i = 0; i < 3000; i++)
    {
        PicoAgent_Pump(app, PicoApp_ActiveAgent(app));
        if (!PicoAgent_IsBusy(PicoApp_ActiveAgent(app)))
        {
            return true;
        }
        SleepOneMs();
    }
    return false;
}

static PicoTraceLine *LastToolTrace(PicoApp *app)
{
    if (!app || PicoApp_ActiveAgent(app)->message_count <= 0)
    {
        return NULL;
    }
    PicoMessage *m = &PicoApp_ActiveAgent(app)->messages[PicoApp_ActiveAgent(app)->message_count - 1];
    for (int t = m->trace_count - 1; t >= 0; t--)
    {
        if (m->trace[t].is_tool)
        {
            return &m->trace[t];
        }
    }
    return NULL;
}

static bool WaitForBlockCount(PicoApp *app, int count)
{
    for (int i = 0; i < 3000; i++)
    {
        PicoAgent_Pump(app, PicoApp_ActiveAgent(app));
        pthread_mutex_lock(&g_test.mu);
        bool entered = g_test.block_entered_count >= count;
        pthread_mutex_unlock(&g_test.mu);
        if (entered) return true;
        SleepOneMs();
    }
    return false;
}

static bool WaitForBlock(PicoApp *app)
{
    for (int i = 0; i < 3000; i++)
    {
        PicoAgent_Pump(app, PicoApp_ActiveAgent(app));
        pthread_mutex_lock(&g_test.mu);
        bool entered = g_test.block_entered;
        pthread_mutex_unlock(&g_test.mu);
        if (entered)
        {
            return true;
        }
        SleepOneMs();
    }
    return false;
}

static int TestSequential(void)
{
    const char *name = "sequential ask delivery";
    ResetTest(TEST_SEQUENTIAL, 1);
    PicoApp app;
    InitApp(&app);
    PicoAgent_StartTurn(&app, PicoApp_ActiveAgent(&app), "start");

    PicoToolAsk first;
    if (!WaitForPending(&app, 0, &first) || !strstr(first.request_json, "first"))
    {
        return Fail(name, "first request was not published");
    }
    if (!pico_tool_answer(&app, first.id, "{\"step\":1}"))
    {
        return Fail(name, "first answer was rejected");
    }

    PicoToolAsk second;
    if (!WaitForPending(&app, first.id, &second) || !strstr(second.request_json, "second"))
    {
        return Fail(name, "second request was not published");
    }
    if (!pico_tool_answer(&app, second.id, "{\"step\":2}"))
    {
        return Fail(name, "second answer was rejected");
    }
    if (!WaitForIdle(&app))
    {
        return Fail(name, "agent did not return idle");
    }

    pthread_mutex_lock(&g_test.mu);
    bool ok = g_test.ask_rc[0][0] == PICO_ASK_OK && g_test.ask_rc[0][1] == PICO_ASK_OK &&
              g_test.ask_answer[0][0] && strcmp(g_test.ask_answer[0][0], "{\"step\":1}") == 0 &&
              g_test.ask_answer[0][1] && strcmp(g_test.ask_answer[0][1], "{\"step\":2}") == 0;
    pthread_mutex_unlock(&g_test.mu);
    PicoApp_Free(&app);
    return ok ? 0 : Fail(name, "tool did not receive both answers");
}

static int TestCancellation(void)
{
    const char *name = "ask cancellation";
    ResetTest(TEST_SINGLE, 1);
    PicoApp app;
    InitApp(&app);
    PicoAgent_StartTurn(&app, PicoApp_ActiveAgent(&app), "start");

    PicoToolAsk ask;
    if (!WaitForPending(&app, 0, &ask))
    {
        return Fail(name, "request was not published");
    }
    PicoAgent_Cancel(PicoApp_ActiveAgent(&app));
    if (!WaitForIdle(&app))
    {
        return Fail(name, "agent did not cancel");
    }
    pthread_mutex_lock(&g_test.mu);
    bool ok = g_test.ask_rc[0][0] == PICO_ASK_CANCEL && !g_test.ask_answer[0][0];
    pthread_mutex_unlock(&g_test.mu);
    PicoApp_Free(&app);
    return ok ? 0 : Fail(name, "tool did not receive cancellation");
}

static int TestStaleId(void)
{
    const char *name = "stale ask id";
    ResetTest(TEST_SINGLE, 1);
    PicoApp app;
    InitApp(&app);
    PicoAgent_StartTurn(&app, PicoApp_ActiveAgent(&app), "first turn");

    PicoToolAsk old_ask;
    if (!WaitForPending(&app, 0, &old_ask))
    {
        return Fail(name, "first request was not published");
    }
    pthread_mutex_lock(&g_test.mu);
    g_test.provider_tool_limit = 2;
    pthread_mutex_unlock(&g_test.mu);
    PicoAgent_ForceCancel(&app, PicoApp_ActiveAgent(&app));
    PicoAgent_StartTurn(&app, PicoApp_ActiveAgent(&app), "second turn");

    PicoToolAsk new_ask;
    if (!WaitForPending(&app, old_ask.id, &new_ask))
    {
        return Fail(name, "replacement request was not published");
    }
    if (pico_tool_answer(&app, old_ask.id, "{\"stale\":true}"))
    {
        return Fail(name, "stale id answered the replacement request");
    }
    PicoToolAsk still_pending;
    if (!pico_tool_pending_ask(&app, &still_pending) || still_pending.id != new_ask.id)
    {
        return Fail(name, "replacement request was retired by stale answer");
    }
    if (!pico_tool_answer(&app, new_ask.id, "{\"ok\":true}"))
    {
        return Fail(name, "replacement answer was rejected");
    }
    if (!WaitForIdle(&app))
    {
        return Fail(name, "replacement turn did not finish");
    }
    PicoApp_Free(&app);
    return 0;
}

static int TestToolSchemaValidation(void)
{
    const char *name = "tool schema validation";
    PicoApp app;
    memset(&app, 0, sizeof(app));
    bool null_schema = pico_add_tool(&app, "null_schema", "null", NULL, EchoTool, NULL);
    bool empty_schema = pico_add_tool(&app, "empty_schema", "empty", "", EchoTool, NULL);
    bool valid = pico_add_tool(&app, "valid", "valid", "{\"type\":\"object\"}", EchoTool, NULL);
    bool rejected =
        !pico_add_tool(&app, "extra", "invalid", "{\"type\":\"object\"}}", EchoTool, NULL) &&
        !pico_add_tool(&app, "literal", "invalid", "{\"type\":falsee}", EchoTool, NULL) &&
        !pico_add_tool(&app, "number", "invalid", "{\"minimum\":01}", EchoTool, NULL) &&
        !pico_add_tool(&app, "comma", "invalid", "{\"type\":\"object\",}", EchoTool, NULL) &&
        !pico_add_tool(&app, "array", "invalid", "[]", EchoTool, NULL) &&
        !pico_add_tool(&app, "multiple", "invalid", "{} {}", EchoTool, NULL) &&
        !pico_add_tool(&app, "utf8", "invalid", "{\"x\":\"\xC3\x28\"}", EchoTool, NULL);

    PicoApp todo_app;
    memset(&todo_app, 0, sizeof(todo_app));
    PicoExt todo = pico_ext_todo();
    todo.init(&todo_app);
    bool todo_registered = todo_app.tool_count == 1 && todo_app.tools[0].params_json;
    if (todo.shutdown) todo.shutdown(&todo_app);
    bool ok = null_schema && empty_schema && valid && rejected && app.tool_count == 3 && todo_registered;
    free(app.status_warn);
    free(todo_app.status_warn);
    return ok ? 0 : Fail(name, "invalid JSON Schema was accepted or builtin todo_update was unavailable");
}

static bool WarnMentions(const PicoApp *app, const char *tool, const char *reason)
{
    return app->status_warn && strstr(app->status_warn, tool) && strstr(app->status_warn, reason);
}

static int TestToolRegistrationFailureWarns(void)
{
    const char *name = "tool registration failure warns";
    PicoApp app;
    memset(&app, 0, sizeof(app));
    if (!pico_add_tool(&app, "ok", "ok", "{}", EchoTool, NULL) || app.status_warn)
    {
        free(app.status_warn);
        return Fail(name, "successful registration warned or failed");
    }
    if (pico_add_tool(&app, "ok", "dup", "{}", EchoTool, NULL) ||
        !WarnMentions(&app, "\"ok\"", "already registered"))
    {
        free(app.status_warn);
        return Fail(name, "duplicate name did not name the tool and reason");
    }
    free(app.status_warn);
    app.status_warn = NULL;
    if (pico_add_tool(&app, "bad_schema", "invalid", "[]", EchoTool, NULL) ||
        !WarnMentions(&app, "\"bad_schema\"", "JSON object"))
    {
        free(app.status_warn);
        return Fail(name, "invalid schema did not name the tool and reason");
    }
    free(app.status_warn);
    app.status_warn = NULL;
    if (pico_add_tool(&app, "no_run", "missing", "{}", NULL, NULL) ||
        !WarnMentions(&app, "\"no_run\"", "missing run function"))
    {
        free(app.status_warn);
        return Fail(name, "missing run did not name the tool and reason");
    }
    free(app.status_warn);
    app.status_warn = NULL;
    char extra[PICO_MAX_TOOLS][8];
    while (app.tool_count < PICO_MAX_TOOLS)
    {
        int i = app.tool_count;
        snprintf(extra[i], sizeof(extra[i]), "x%d", i);
        if (!pico_add_tool(&app, extra[i], "pad", "{}", EchoTool, NULL))
        {
            free(app.status_warn);
            return Fail(name, "padding tools to the limit failed");
        }
    }
    if (pico_add_tool(&app, "overflow", "full", "{}", EchoTool, NULL) ||
        !WarnMentions(&app, "\"overflow\"", "tool limit reached"))
    {
        free(app.status_warn);
        return Fail(name, "tool limit did not name the tool and reason");
    }
    free(app.status_warn);
    return 0;
}

static int TestProductionInit(void)
{
    const char *name = "production app initialization";
    ResetTest(TEST_SINGLE, 0);
    PicoApp app;
    PicoApp_Init(&app, NULL, "/workspace", false, PICO_SESSION_NONE, NULL);
    bool ok = PicoApp_ActiveAgent(&app) && PicoApp_ActiveAgent(&app)->state == PICO_AGENT_IDLE && app.composer.text;
    PicoApp_Free(&app);
    return ok ? 0 : Fail(name, "PicoApp_Init did not create a usable agent before session/plugin startup");
}

static int TestInvalidPayload(void)
{
    const char *name = "invalid ask payload";
    ResetTest(TEST_INVALID, 1);
    PicoApp app;
    InitApp(&app);
    PicoAgent_StartTurn(&app, PicoApp_ActiveAgent(&app), "start");
    if (!WaitForIdle(&app))
    {
        return Fail(name, "agent did not finish invalid request");
    }
    PicoToolAsk ask;
    if (pico_tool_pending_ask(&app, &ask))
    {
        return Fail(name, "invalid request was published to the UI");
    }
    pthread_mutex_lock(&g_test.mu);
    const char *expected = "{\"error\":\"invalid ask payload; fix it and try again\"}";
    bool ok = g_test.ask_rc[0][0] == PICO_ASK_OK && g_test.ask_answer[0][0] &&
              strcmp(g_test.ask_answer[0][0], expected) == 0 &&
              g_test.ask_rc[0][1] == PICO_ASK_OK && g_test.ask_answer[0][1] &&
              strcmp(g_test.ask_answer[0][1], expected) == 0;
    pthread_mutex_unlock(&g_test.mu);
    PicoApp_Free(&app);
    return ok ? 0 : Fail(name, "tool did not receive the validation error");
}

static int TestShutdownTimeout(void)
{
    const char *name = "shutdown timeout preservation";
    ResetTest(TEST_BLOCK, 1);
    PicoApp *app = (PicoApp *)calloc(1, sizeof(PicoApp));
    if (!app)
    {
        return Fail(name, "allocation failed");
    }
    InitApp(app);
    PicoAgent_StartTurn(app, PicoApp_ActiveAgent(app), "start");
    if (!WaitForBlock(app))
    {
        return Fail(name, "blocking tool did not start");
    }

    PicoModel *models = app->models;
    PicoApp_Free(app);
    if (g_plugin_shutdowns != 0 || g_auth_frees != 0 || app->models != models ||
        app->tool_count != 1 || !app->tools[0].run)
    {
        return Fail(name, "PicoApp_Free tore down worker-visible state");
    }

    pthread_mutex_lock(&g_test.mu);
    g_test.block_release = true;
    pthread_cond_broadcast(&g_test.cv);
    while (!g_test.block_done)
    {
        pthread_cond_wait(&g_test.cv, &g_test.mu);
    }
    bool saw_live_state = g_test.block_saw_live_state;
    pthread_mutex_unlock(&g_test.mu);
    return saw_live_state ? 0 : Fail(name, "detached worker observed torn-down state");
}

static int TestRetiredRuntimeCap(void)
{
    const char *name = "retired runtime cap";
    ResetTest(TEST_BLOCK, PICO_MAX_RETIRED_RUNTIMES + 1);
    PicoApp app;
    InitApp(&app);

    bool ok = true;
    int failed_at = -1;
    const char *failed_why = NULL;
    for (int i = 0; i <= PICO_MAX_RETIRED_RUNTIMES; i++)
    {
        PicoAgent_StartTurn(&app, PicoApp_ActiveAgent(&app), "block");
        if (!WaitForBlockCount(&app, i + 1))
        {
            ok = false;
            failed_at = i;
            failed_why = "worker did not reach blocking tool";
            break;
        }
        PicoAgentRt *before = PicoApp_ActiveAgent(&app)->runtime;
        PicoAgent_ForceCancel(&app, PicoApp_ActiveAgent(&app));
        if ((i < PICO_MAX_RETIRED_RUNTIMES && PicoApp_ActiveAgent(&app)->runtime == before) ||
            (i == PICO_MAX_RETIRED_RUNTIMES && PicoApp_ActiveAgent(&app)->runtime != before))
        {
            ok = false;
            failed_at = i;
            failed_why = "runtime replacement decision was wrong";
            break;
        }
    }

    pthread_mutex_lock(&g_test.mu);
    g_test.block_release = true;
    pthread_cond_broadcast(&g_test.cv);
    pthread_mutex_unlock(&g_test.mu);
    if (!WaitForIdle(&app)) ok = false;
    PicoApp_Free(&app);
    if (!ok && failed_why)
    {
        fprintf(stderr, "%s: iteration %d: %s\n", name, failed_at, failed_why);
    }
    return ok ? 0 : Fail(name, "force cancellation did not fall back to cooperative cancellation at the cap");
}

static int TestBeforeForceCancel(void)
{
    const char *name = "force cancel stops remaining before hooks";
    ResetTest(TEST_SINGLE, 1);
    PicoApp app;
    InitApp(&app);
    pico_add_tool_before_hook(&app, BlockingBefore);
    pico_add_tool_before_hook(&app, CountBefore);
    PicoAgent_StartTurn(&app, PicoApp_ActiveAgent(&app), "start");
    if (!WaitForBlock(&app))
    {
        return Fail(name, "blocking before hook did not start");
    }

    PicoAgent_ForceCancel(&app, PicoApp_ActiveAgent(&app));
    pthread_mutex_lock(&g_test.mu);
    g_test.block_release = true;
    pthread_cond_broadcast(&g_test.cv);
    while (!g_test.block_done)
    {
        pthread_cond_wait(&g_test.cv, &g_test.mu);
    }
    pthread_mutex_unlock(&g_test.mu);

    bool reaped = false;
    for (int i = 0; i < 3000; i++)
    {
        PicoAgent_Pump(&app, PicoApp_ActiveAgent(&app));
        if (!PicoAgent_BlocksReload(PicoApp_ActiveAgent(&app)))
        {
            reaped = true;
            break;
        }
        SleepOneMs();
    }
    pthread_mutex_lock(&g_test.mu);
    bool skipped = g_test.followup_hook_calls == 0;
    bool stale_rejected = !g_test.block_saw_live_state;
    pthread_mutex_unlock(&g_test.mu);
    PicoApp_Free(&app);
    if (!reaped)
    {
        return Fail(name, "abandoned before hook was not reaped");
    }
    return skipped && stale_rejected ? 0 :
           Fail(name, "cancelled runtime used a stale context or executed a later before hook");
}

static int TestBeforeDeny(void)
{
    const char *name = "before hook deny skips tool";
    ResetTest(TEST_SINGLE, 1);
    PicoApp app;
    InitApp(&app);
    pico_add_tool_before_hook(&app, DenyBefore);
    PicoAgent_StartTurn(&app, PicoApp_ActiveAgent(&app), "start");
    if (!WaitForIdle(&app))
    {
        return Fail(name, "agent did not return idle");
    }
    pthread_mutex_lock(&g_test.mu);
    bool ok = g_test.tool_invocations == 0 && g_test.provider_tools_issued == 2;
    pthread_mutex_unlock(&g_test.mu);
    PicoApp_Free(&app);
    return ok ? 0 : Fail(name, "denied tool ran or turn did not continue");
}

static int TestBeforeAskCancel(void)
{
    const char *name = "cancel during before ask skips tool";
    ResetTest(TEST_SINGLE, 1);
    PicoApp app;
    InitApp(&app);
    pico_add_tool_before_hook(&app, AskBefore);
    PicoAgent_StartTurn(&app, PicoApp_ActiveAgent(&app), "start");
    PicoToolAsk ask;
    if (!WaitForPending(&app, 0, &ask))
    {
        return Fail(name, "before-hook request was not published");
    }
    PicoAgent_Cancel(PicoApp_ActiveAgent(&app));
    if (!WaitForIdle(&app))
    {
        return Fail(name, "agent did not cancel");
    }
    pthread_mutex_lock(&g_test.mu);
    bool ok = g_test.tool_invocations == 0 && g_test.ask_rc[0][0] == PICO_ASK_CANCEL;
    pthread_mutex_unlock(&g_test.mu);
    PicoApp_Free(&app);
    return ok ? 0 : Fail(name, "tool ran after cancelled before-ask");
}

static int TestBeforeRewriteArgs(void)
{
    const char *name = "before hook rewrites tool args";
    ResetTest(TEST_SINGLE, 1);
    PicoApp app;
    InitApp(&app);
    pico_add_tool(&app, "echo_test", "echo", "{}", EchoTool, NULL);
    snprintf(g_test.issue_tool_name, sizeof(g_test.issue_tool_name), "echo_test");
    pico_add_tool_before_hook(&app, RewriteArgsBefore);
    PicoAgent_StartTurn(&app, PicoApp_ActiveAgent(&app), "start");
    if (!WaitForIdle(&app))
    {
        return Fail(name, "agent did not return idle");
    }
    pthread_mutex_lock(&g_test.mu);
    bool ok = g_test.tool_invocations == 1 &&
              g_test.tool_ctx_id == pico_agent_id(PicoApp_ActiveAgent(&app)) && g_test.tool_seen_args &&
              strcmp(g_test.tool_seen_args, "{\"rewritten\":true}") == 0;
    pthread_mutex_unlock(&g_test.mu);
    PicoApp_Free(&app);
    return ok ? 0 : Fail(name, "tool did not see rewritten args");
}

static int TestAfterRewriteOutput(void)
{
    const char *name = "after hook rewrites tool output";
    ResetTest(TEST_SINGLE, 1);
    PicoApp app;
    InitApp(&app);
    pico_add_tool(&app, "echo_test", "echo", "{}", EchoTool, NULL);
    snprintf(g_test.issue_tool_name, sizeof(g_test.issue_tool_name), "echo_test");
    pico_add_tool_after_hook(&app, RewriteOutAfter);
    pico_add_tool_after_hook(&app, CaptureAfter);
    PicoAgent_StartTurn(&app, PicoApp_ActiveAgent(&app), "start");
    if (!WaitForIdle(&app))
    {
        return Fail(name, "agent did not return idle");
    }
    pthread_mutex_lock(&g_test.mu);
    bool ok = g_test.last_input && strstr(g_test.last_input, "rewritten-output") != NULL &&
              g_test.after_seen_output && strcmp(g_test.after_seen_output, "rewritten-output") == 0;
    pthread_mutex_unlock(&g_test.mu);
    PicoApp_Free(&app);
    return ok ? 0 : Fail(name, "rewritten output was not chained to the model and later hooks");
}

static int TestRewriteThenDenyArgs(void)
{
    const char *name = "denied call preserves rewritten args";
    ResetTest(TEST_SINGLE, 1);
    PicoApp app;
    InitApp(&app);
    pico_add_tool_before_hook(&app, RewriteArgsBefore);
    pico_add_tool_before_hook(&app, DenyBefore);
    pico_add_tool_after_hook(&app, CaptureAfter);
    PicoAgent_StartTurn(&app, PicoApp_ActiveAgent(&app), "start");
    if (!WaitForIdle(&app))
    {
        return Fail(name, "agent did not return idle");
    }
    pthread_mutex_lock(&g_test.mu);
    bool ok = g_test.tool_invocations == 0 && g_test.after_seen_args &&
              strcmp(g_test.after_seen_args, "{\"rewritten\":true}") == 0 &&
              g_test.after_seen_output && strcmp(g_test.after_seen_output, "User denied this tool.") == 0;
    pthread_mutex_unlock(&g_test.mu);
    PicoApp_Free(&app);
    return ok ? 0 : Fail(name, "after hook did not receive the denied call's current values");
}

static int TestStructuredToolDetails(void)
{
    const char *name = "structured tool details apply and reach hooks";
    ResetTest(TEST_SINGLE, 1);
    PicoApp app;
    InitApp(&app);
    if (!pico_add_tool(&app, "details_test", "details", "{}", DetailsTool, ApplyDetails) ||
        pico_add_tool(&app, "details_test", "duplicate", "{}", DetailsTool, ApplyDetails))
    {
        PicoApp_Free(&app);
        return Fail(name, "tool name uniqueness contract failed");
    }
    snprintf(g_test.issue_tool_name, sizeof(g_test.issue_tool_name), "details_test");
    pico_add_tool_after_hook(&app, CaptureAfter);
    PicoAgent_StartTurn(&app, PicoApp_ActiveAgent(&app), "start");
    if (!WaitForIdle(&app))
    {
        PicoApp_Free(&app);
        return Fail(name, "agent did not return idle");
    }
    pthread_mutex_lock(&g_test.mu);
    bool ok = g_test.apply_calls == 1 && g_test.apply_agent_id == pico_agent_id(PicoApp_ActiveAgent(&app)) &&
              g_test.after_agent_id == pico_agent_id(PicoApp_ActiveAgent(&app)) &&
              g_test.after_executed && !g_test.after_is_error &&
              g_test.after_seen_details && strcmp(g_test.after_seen_details, "{\"value\":7}") == 0 &&
              g_test.last_input && strstr(g_test.last_input, "\"name\":\"details_test\"") &&
              strstr(g_test.last_input, "\"is_error\":false");
    pthread_mutex_unlock(&g_test.mu);
    PicoApp_Free(&app);
    return ok ? 0 : Fail(name, "details were not applied or represented in the result input");
}

static int TestInvalidDetailsFailClosed(void)
{
    const char *name = "invalid structured details fail closed";
    ResetTest(TEST_SINGLE, 1);
    PicoApp app;
    InitApp(&app);
    pico_add_tool(&app, "invalid_details", "details", "{}", InvalidDetailsTool, ApplyDetails);
    snprintf(g_test.issue_tool_name, sizeof(g_test.issue_tool_name), "invalid_details");
    pico_add_tool_after_hook(&app, CaptureAfter);
    PicoAgent_StartTurn(&app, PicoApp_ActiveAgent(&app), "start");
    if (!WaitForIdle(&app))
    {
        PicoApp_Free(&app);
        return Fail(name, "agent did not return idle");
    }
    pthread_mutex_lock(&g_test.mu);
    bool ok = g_test.apply_calls == 0 && g_test.after_executed && g_test.after_is_error &&
              g_test.after_seen_details && g_test.after_seen_details[0] == '\0';
    pthread_mutex_unlock(&g_test.mu);
    PicoApp_Free(&app);
    return ok ? 0 : Fail(name, "invalid details were applied or exposed as successful");
}

static int TestDeniedDetailsDoNotApply(void)
{
    const char *name = "denied structured tool does not apply";
    ResetTest(TEST_SINGLE, 1);
    PicoApp app;
    InitApp(&app);
    pico_add_tool(&app, "details_test", "details", "{}", DetailsTool, ApplyDetails);
    snprintf(g_test.issue_tool_name, sizeof(g_test.issue_tool_name), "details_test");
    pico_add_tool_before_hook(&app, DenyBefore);
    pico_add_tool_after_hook(&app, CaptureAfter);
    PicoAgent_StartTurn(&app, PicoApp_ActiveAgent(&app), "start");
    if (!WaitForIdle(&app))
    {
        PicoApp_Free(&app);
        return Fail(name, "agent did not return idle");
    }
    pthread_mutex_lock(&g_test.mu);
    bool ok = g_test.apply_calls == 0 && !g_test.after_executed && g_test.after_is_error;
    pthread_mutex_unlock(&g_test.mu);
    PicoApp_Free(&app);
    return ok ? 0 : Fail(name, "denied result applied details or looked successful");
}

static int CountOccurrences(const char *text, const char *needle)
{
    int count = 0;
    size_t n = strlen(needle);
    for (const char *p = text; p && (p = strstr(p, needle)); p += n)
    {
        count++;
    }
    return count;
}

static int TestRequestOnlyContext(void)
{
    const char *name = "context hook appends request-only base-isolated context";
    ResetTest(TEST_SINGLE, 0);
    PicoApp app;
    InitApp(&app);
    pico_add_context_hook(&app, AddEphemeralContext);
    pico_add_context_hook(&app, InspectBaseContext);
    PicoAgent_StartTurn(&app, PicoApp_ActiveAgent(&app), "first");
    if (!WaitForIdle(&app))
    {
        PicoApp_Free(&app);
        return Fail(name, "first turn did not finish");
    }
    PicoAgent_StartTurn(&app, PicoApp_ActiveAgent(&app), "second");
    if (!WaitForIdle(&app))
    {
        PicoApp_Free(&app);
        return Fail(name, "second turn did not finish");
    }
    pthread_mutex_lock(&g_test.mu);
    bool ok = g_test.context_saw_base_tail &&
              g_test.context_agent_id == pico_agent_id(PicoApp_ActiveAgent(&app)) && g_test.last_input &&
              CountOccurrences(g_test.last_input, "ephemeral-context") == 1 &&
              (!g_test.last_instructions || strstr(g_test.last_instructions, "ephemeral-context") == NULL);
    pthread_mutex_unlock(&g_test.mu);
    PicoApp_Free(&app);
    return ok ? 0 : Fail(name, "context persisted, entered instructions, or changed the next hook's base history");
}

static int TestLlmExtraInstructions(void)
{
    const char *name = "llm hook appends instructions";
    ResetTest(TEST_SINGLE, 0);
    PicoApp app;
    InitApp(&app);
    pico_add_llm_hook(&app, ExtraInstructions);
    PicoAgent_StartTurn(&app, PicoApp_ActiveAgent(&app), "start");
    if (!WaitForIdle(&app))
    {
        return Fail(name, "agent did not return idle");
    }
    pthread_mutex_lock(&g_test.mu);
    bool ok = g_test.llm_agent_id == pico_agent_id(PicoApp_ActiveAgent(&app)) &&
              g_test.last_instructions && strstr(g_test.last_instructions, "injected-line") != NULL;
    pthread_mutex_unlock(&g_test.mu);
    PicoApp_Free(&app);
    return ok ? 0 : Fail(name, "provider did not receive extra instructions");
}

static int TestBuildInstructionsMatchTurn(void)
{
    const char *name = "previewed instructions match the next turn";
    ResetTest(TEST_SINGLE, 0);
    PicoApp app;
    InitApp(&app);
    pico_add_llm_hook(&app, ExtraWhenTools);
    char *preview = PicoAgent_BuildInstructions(&app, PicoApp_ActiveAgent(&app));
    PicoAgent_StartTurn(&app, PicoApp_ActiveAgent(&app), "start");
    if (!WaitForIdle(&app))
    {
        free(preview);
        PicoApp_Free(&app);
        return Fail(name, "agent did not return idle");
    }
    pthread_mutex_lock(&g_test.mu);
    bool ok = preview && strstr(preview, "tool-notes") != NULL && g_test.last_instructions &&
              strcmp(preview, g_test.last_instructions) == 0;
    pthread_mutex_unlock(&g_test.mu);
    free(preview);
    PicoApp_Free(&app);
    return ok ? 0 : Fail(name, "preview did not match the instructions sent to the provider");
}

static int TestAgentPolicyPrecedesLlmHooks(void)
{
    const char *name = "agent policy precedes llm hooks";
    ResetTest(TEST_SINGLE, 0);
    PicoApp app;
    InitApp(&app);
    PicoApp_ActiveAgent(&app)->allowed_tools = (char **)calloc(1, sizeof(char *));
    PicoApp_ActiveAgent(&app)->allowed_tool_count = 0;
    pico_add_llm_hook(&app, ExtraWhenTools);
    PicoAgent_StartTurn(&app, PicoApp_ActiveAgent(&app), "start");
    if (!WaitForIdle(&app))
    {
        PicoApp_Free(&app);
        return Fail(name, "agent did not return idle");
    }
    pthread_mutex_lock(&g_test.mu);
    bool ok = g_test.last_tool_count == 0 && g_test.last_instructions &&
              strstr(g_test.last_instructions, "tool-notes") == NULL;
    pthread_mutex_unlock(&g_test.mu);
    PicoApp_Free(&app);
    return ok ? 0 : Fail(name, "LLM hook or provider saw a policy-hidden tool");
}

static int TestLlmExcludeTool(void)
{
    const char *name = "llm hook excludes a tool";
    ResetTest(TEST_SINGLE, 0);
    PicoApp app;
    InitApp(&app);
    pico_add_llm_hook(&app, ExcludeAskTest);
    PicoAgent_StartTurn(&app, PicoApp_ActiveAgent(&app), "start");
    if (!WaitForIdle(&app))
    {
        return Fail(name, "agent did not return idle");
    }
    pthread_mutex_lock(&g_test.mu);
    bool ok = g_test.last_tool_count == 0 && strstr(g_test.last_tools, "ask_test") == NULL;
    pthread_mutex_unlock(&g_test.mu);
    PicoApp_Free(&app);
    return ok ? 0 : Fail(name, "excluded tool was still in the catalog");
}

static int TestHiddenToolCallIsControlled(void)
{
    const char *name = "hidden tool call is controlled";
    ResetTest(TEST_SINGLE, 1);
    PicoApp app;
    InitApp(&app);
    pico_add_llm_hook(&app, ExcludeAskTest);
    pico_add_tool_before_hook(&app, CountBefore);
    pico_add_tool_after_hook(&app, CaptureAfter);
    PicoAgent_StartTurn(&app, PicoApp_ActiveAgent(&app), "start");
    if (!WaitForIdle(&app))
    {
        PicoApp_Free(&app);
        return Fail(name, "agent did not finish the recovery round");
    }
    pthread_mutex_lock(&g_test.mu);
    bool ok = g_test.tool_invocations == 0 && g_test.followup_hook_calls == 0 &&
              !g_test.after_seen_output && g_test.last_input &&
              strstr(g_test.last_input, "tool was not offered for this request") != NULL;
    pthread_mutex_unlock(&g_test.mu);
    PicoApp_Free(&app);
    return ok ? 0 : Fail(name, "hidden call executed a hook/tool or was not returned as a tool error");
}

static int TestOfferedCatalogSnapshot(void)
{
    const char *name = "offered tool snapshot survives registry mutation";
    ResetTest(TEST_CATALOG_BLOCK, 1);
    PicoApp app;
    InitApp(&app);
    pico_add_tool(&app, "echo_test", "echo", "{}", EchoTool, NULL);
    snprintf(g_test.issue_tool_name, sizeof(g_test.issue_tool_name), "echo_test");
    PicoAgent_StartTurn(&app, PicoApp_ActiveAgent(&app), "start");
    if (!WaitForBlock(&app))
    {
        PicoApp_Free(&app);
        return Fail(name, "provider gate did not open");
    }
    for (int i = 0; i < app.tool_count; i++)
    {
        if (app.tools[i].name && strcmp(app.tools[i].name, "echo_test") == 0)
        {
            app.tools[i].run = NULL;
        }
    }
    pthread_mutex_lock(&g_test.mu);
    g_test.block_release = true;
    pthread_cond_broadcast(&g_test.cv);
    pthread_mutex_unlock(&g_test.mu);
    if (!WaitForIdle(&app))
    {
        PicoApp_Free(&app);
        return Fail(name, "agent did not finish");
    }
    pthread_mutex_lock(&g_test.mu);
    bool ok = g_test.tool_invocations == 1;
    pthread_mutex_unlock(&g_test.mu);
    PicoApp_Free(&app);
    return ok ? 0 : Fail(name, "tool resolution used the mutated global registry");
}

static int TestMalformedCallsFail(TestMode mode, const char *expected)
{
    ResetTest(mode, 1);
    PicoApp app;
    InitApp(&app);
    PicoAgent_StartTurn(&app, PicoApp_ActiveAgent(&app), "start");
    if (!WaitForIdle(&app))
    {
        PicoApp_Free(&app);
        return 1;
    }
    pthread_mutex_lock(&g_test.mu);
    bool no_tool = g_test.tool_invocations == 0;
    pthread_mutex_unlock(&g_test.mu);
    bool ok = no_tool && PicoApp_ActiveAgent(&app)->state == PICO_AGENT_ERROR && PicoApp_ActiveAgent(&app)->error &&
              strstr(PicoApp_ActiveAgent(&app)->error, expected) != NULL;
    PicoApp_Free(&app);
    return ok ? 0 : 1;
}

static int TestMalformedToolCalls(void)
{
    const char *name = "malformed tool calls fail the provider round";
    if (TestMalformedCallsFail(TEST_DUPLICATE_CALLS, "duplicate tool call ids") ||
        TestMalformedCallsFail(TEST_EMPTY_CALL_ID, "empty call id") ||
        TestMalformedCallsFail(TEST_TOO_MANY_CALLS, "invalid number of tool calls"))
    {
        return Fail(name, "a malformed, duplicate, or oversized call array did not fail explicitly");
    }
    return 0;
}

static void AddLifeHooks(PicoApp *app)
{
    pico_add_hook(app, PICO_HOOK_ON_TURN_END, LifeTurnEnd);
    pico_add_hook(app, PICO_HOOK_ON_CANCEL, LifeCancel);
    pico_add_hook(app, PICO_HOOK_ON_ERROR, LifeError);
    pico_add_hook(app, PICO_HOOK_AFTER_COMPACT, LifeAfterCompact);
}

static int TestTurnEnd(void)
{
    const char *name = "turn end notification";
    ResetTest(TEST_SINGLE, 0);
    PicoApp app;
    InitApp(&app);
    AddLifeHooks(&app);
    PicoAgent_StartTurn(&app, PicoApp_ActiveAgent(&app), "start");
    if (!WaitForIdle(&app))
    {
        return Fail(name, "agent did not return idle");
    }
    bool ok = g_test.life_turn_end == 1 && g_test.hook_agent_id == pico_agent_id(PicoApp_ActiveAgent(&app)) &&
              g_test.life_cancel == 0 && g_test.life_error == 0 &&
              PicoApp_ActiveAgent(&app)->state == PICO_AGENT_IDLE;
    PicoApp_Free(&app);
    return ok ? 0 : Fail(name, "ON_TURN_END did not fire once");
}

static int TestCancelNotification(void)
{
    const char *name = "cancel notification";
    ResetTest(TEST_SINGLE, 1);
    PicoApp app;
    InitApp(&app);
    AddLifeHooks(&app);
    PicoAgent_StartTurn(&app, PicoApp_ActiveAgent(&app), "start");
    PicoToolAsk ask;
    if (!WaitForPending(&app, 0, &ask))
    {
        return Fail(name, "request was not published");
    }
    PicoAgent_Cancel(PicoApp_ActiveAgent(&app));
    if (!WaitForIdle(&app))
    {
        return Fail(name, "agent did not cancel");
    }
    bool ok = g_test.life_cancel == 1 && g_test.life_turn_end == 0 && g_test.life_error == 0;
    PicoApp_Free(&app);
    return ok ? 0 : Fail(name, "ON_CANCEL did not fire (or TURN_END did)");
}

static int TestErrorNotification(void)
{
    const char *name = "error notification";
    ResetTest(TEST_SINGLE, 0);
    g_test.provider_fail = true;
    g_test.provider_tokens = 100;
    g_test.provider_cached_tokens = 80;
    PicoApp app;
    InitApp(&app);
    AddLifeHooks(&app);
    PicoAgent_StartTurn(&app, PicoApp_ActiveAgent(&app), "start");
    if (!WaitForIdle(&app))
    {
        return Fail(name, "agent did not leave the busy state");
    }
    bool ok = g_test.life_error == 1 && g_test.life_turn_end == 0 && g_test.life_cancel == 0 &&
              PicoApp_ActiveAgent(&app)->state == PICO_AGENT_ERROR && PicoApp_ActiveAgent(&app)->error && PicoApp_ActiveAgent(&app)->session_input_tokens == 0 &&
              PicoApp_ActiveAgent(&app)->session_cached_tokens == 0 && g_test.usage_log_count == 0;
    PicoApp_Free(&app);
    return ok ? 0 : Fail(name, "failed request changed usage or did not fire ON_ERROR");
}

static int TestCancelledProviderUsage(void)
{
    const char *name = "cancelled provider usage";
    ResetTest(TEST_PROVIDER_BLOCK, 0);
    g_test.provider_tokens = 100;
    g_test.provider_cached_tokens = 80;
    PicoApp app;
    InitApp(&app);
    PicoAgent_StartTurn(&app, PicoApp_ActiveAgent(&app), "start");
    PicoAgent_Cancel(PicoApp_ActiveAgent(&app));
    if (!WaitForIdle(&app))
    {
        PicoApp_Free(&app);
        return Fail(name, "cancelled provider did not return idle");
    }
    bool ok = PicoApp_ActiveAgent(&app)->session_input_tokens == 0 && PicoApp_ActiveAgent(&app)->session_cached_tokens == 0 &&
              g_test.usage_log_count == 0;
    PicoApp_Free(&app);
    return ok ? 0 : Fail(name, "cancelled provider result contributed usage");
}

static int TestSessionUsageAccumulation(void)
{
    const char *name = "session usage accumulates successful requests";
    ResetTest(TEST_SINGLE, 1);
    g_test.provider_tokens = 100;
    g_test.provider_cached_tokens = 25;
    PicoApp app;
    InitApp(&app);
    pico_add_tool(&app, "echo_test", "echo", "{}", EchoTool, NULL);
    snprintf(g_test.issue_tool_name, sizeof(g_test.issue_tool_name), "echo_test");
    PicoAgent_StartTurn(&app, PicoApp_ActiveAgent(&app), "first");
    if (!WaitForIdle(&app))
    {
        PicoApp_Free(&app);
        return Fail(name, "tool turn did not finish");
    }
    g_test.provider_tokens = 200;
    g_test.provider_cached_tokens = 150;
    PicoAgent_StartTurn(&app, PicoApp_ActiveAgent(&app), "second");
    if (!WaitForIdle(&app))
    {
        PicoApp_Free(&app);
        return Fail(name, "second turn did not finish");
    }
    bool ok = PicoApp_ActiveAgent(&app)->tokens_used == 200 && PicoApp_ActiveAgent(&app)->tokens_cached == 150 && PicoApp_ActiveAgent(&app)->session_input_tokens == 400 &&
              PicoApp_ActiveAgent(&app)->session_cached_tokens == 200 && g_test.usage_log_count == 3;
    PicoApp_Free(&app);
    return ok ? 0 : Fail(name, "latest or cumulative usage did not include every successful request");
}

static int TestUsageNormalizationAndSaturation(void)
{
    const char *name = "usage normalization and saturation";
    PicoAgent agent;
    memset(&agent, 0, sizeof(agent));
    int cached = -1;
    int percent = -1;
    bool ignored = !PicoUsage_Apply(&agent, 0, 10, &cached) && agent.session_input_tokens == 0 &&
                   !PicoUsage_SessionPercent(&agent, &percent);
    bool negative = PicoUsage_Apply(&agent, 10, -5, &cached) && cached == 0 && agent.tokens_cached == 0;
    agent.tokens_used = 0;
    agent.session_input_tokens = 400;
    agent.session_cached_tokens = 200;
    bool cumulative_rate = PicoUsage_SessionPercent(&agent, &percent) && percent == 50 && agent.tokens_used == 0;
    agent.session_input_tokens = UINT64_MAX - 5;
    agent.session_cached_tokens = UINT64_MAX - 5;
    bool saturated = PicoUsage_Apply(&agent, 10, 20, &cached) && cached == 10 && agent.tokens_cached == 10 &&
                     agent.session_input_tokens == UINT64_MAX && agent.session_cached_tokens == UINT64_MAX &&
                     PicoUsage_SessionPercent(&agent, &percent) && percent == 100;
    return ignored && negative && cumulative_rate && saturated
               ? 0
               : Fail(name, "usage or cumulative percentage was not normalized or saturated");
}

static int TestAfterCompact(void)
{
    const char *name = "after compact then turn end";
    ResetTest(TEST_SINGLE, 0);
    g_test.provider_tokens = 100;
    g_test.provider_cached_tokens = 40;
    PicoApp app;
    InitApp(&app);
    app.settings.compact_enabled = true;
    app.settings.compact_ratio = 0.5;
    PicoApp_ActiveAgent(&app)->compact_enabled = true;
    PicoApp_ActiveAgent(&app)->compact_ratio = 0.5;
    PicoApp_ActiveAgent(&app)->context_limit = 100;
    AddLifeHooks(&app);
    PicoAgent_StartTurn(&app, PicoApp_ActiveAgent(&app), "start");
    if (!WaitForIdle(&app))
    {
        return Fail(name, "agent did not return idle");
    }
    bool ok = g_test.life_after_compact == 1 && g_test.life_turn_end == 1 && g_test.life_cancel == 0 &&
              g_test.life_error == 0 && PicoApp_ActiveAgent(&app)->tokens_used == 0 && PicoApp_ActiveAgent(&app)->tokens_cached == 0 &&
              PicoApp_ActiveAgent(&app)->session_input_tokens == 200 && PicoApp_ActiveAgent(&app)->session_cached_tokens == 80 &&
              g_test.usage_log_count == 2;
    PicoApp_Free(&app);
    return ok ? 0 : Fail(name, "compaction did not retain cumulative usage or fire lifecycle hooks");
}

static int TestToolTraceError(void)
{
    const char *name = "tool trace error flag";
    ResetTest(TEST_SINGLE, 1);
    PicoApp app;
    InitApp(&app);
    pico_add_tool(&app, "echo_test", "echo", "{}", EchoTool, NULL);
    snprintf(g_test.issue_tool_name, sizeof(g_test.issue_tool_name), "echo_test");
    PicoAgent_StartTurn(&app, PicoApp_ActiveAgent(&app), "start");
    if (!WaitForIdle(&app))
    {
        return Fail(name, "successful tool did not finish");
    }
    PicoTraceLine *ok_line = LastToolTrace(&app);
    bool ok = ok_line && ok_line->tool_output && !ok_line->tool_error;
    PicoApp_Free(&app);
    if (!ok)
    {
        return Fail(name, "successful tool was marked as error");
    }

    ResetTest(TEST_SINGLE, 1);
    InitApp(&app);
    snprintf(g_test.issue_tool_name, sizeof(g_test.issue_tool_name), "missing_tool");
    PicoAgent_StartTurn(&app, PicoApp_ActiveAgent(&app), "start");
    if (!WaitForIdle(&app))
    {
        return Fail(name, "unknown tool did not finish");
    }
    PicoTraceLine *err_line = LastToolTrace(&app);
    ok = err_line && err_line->tool_output && err_line->tool_error;
    PicoApp_Free(&app);
    return ok ? 0 : Fail(name, "unknown tool was not marked as error");
}

static int TestTodoAgentIsolation(void)
{
    const char *name = "todo state is keyed by agent id";
    PicoApp app;
    memset(&app, 0, sizeof(app));
    PicoExt ext = pico_ext_todo();
    ext.init(&app);
    const char *first = "{\"version\":1,\"todos\":[{\"id\":\"a\",\"text\":\"first-agent\",\"status\":\"pending\"}]}";
    const char *second = "{\"version\":1,\"todos\":[{\"id\":\"b\",\"text\":\"second-agent\",\"status\":\"in_progress\"}]}";
    bool ok = app.tool_count == 1 && app.tools[0].apply &&
              app.tools[0].apply(&app, 11, first, true) &&
              app.tools[0].apply(&app, 22, second, true);

    PicoContextEvent one = {.tools = app.tools, .tool_count = app.tool_count};
    PicoContextEvent two = {.tools = app.tools, .tool_count = app.tool_count};
    app.context_hooks[0](&app, 11, &one);
    app.context_hooks[0](&app, 22, &two);
    PicoContextEvent hidden = {0};
    app.context_hooks[0](&app, 22, &hidden);
    ok = ok && !hidden.extra_context && one.extra_context && strstr(one.extra_context, "first-agent") &&
         !strstr(one.extra_context, "second-agent") && two.extra_context &&
         strstr(two.extra_context, "second-agent") && !strstr(two.extra_context, "first-agent");
    free(one.extra_context);
    free(two.extra_context);

    PicoHookEvent reset = {.hook = PICO_HOOK_ON_SESSION_RESET, .agent_id = 11};
    for (int i = 0; i < app.hook_count; i++)
    {
        if (app.hooks[i].hook == reset.hook)
        {
            app.hooks[i].fn(&app, &reset);
        }
    }
    one = (PicoContextEvent){.tools = app.tools, .tool_count = app.tool_count};
    two = (PicoContextEvent){.tools = app.tools, .tool_count = app.tool_count};
    app.context_hooks[0](&app, 11, &one);
    app.context_hooks[0](&app, 22, &two);
    ok = ok && !one.extra_context && two.extra_context && strstr(two.extra_context, "second-agent");
    free(one.extra_context);
    free(two.extra_context);
    ext.shutdown(&app);
    pico_clear_registrations(&app);
    return ok ? 0 : Fail(name, "apply, context, or reset crossed agent boundaries");
}

static bool AskUserRegistered(const PicoApp *app)
{
    return app && app->tool_count == 1 && app->tools[0].name &&
           strcmp(app->tools[0].name, "ask_user") == 0 && app->tools[0].run &&
           app->context_hook_count == 1 && app->view_count[PICO_SLOT_OVERLAY] == 1 &&
           app->hook_count == 1;
}

static int TestAskUserHiddenOmitsGuidance(void)
{
    const char *name = "hidden ask_user omits guidance";
    ResetTest(TEST_SINGLE, 0);
    PicoApp app;
    InitApp(&app);
    PicoExt ext = pico_ext_ask_user();
    ext.init(&app);
    pico_add_llm_hook(&app, ExcludeAskUser);
    PicoAgent_StartTurn(&app, PicoApp_ActiveAgent(&app), "start");
    if (!WaitForIdle(&app))
    {
        ext.shutdown(&app);
        PicoApp_Free(&app);
        return Fail(name, "agent did not finish");
    }
    pthread_mutex_lock(&g_test.mu);
    bool ok = g_test.last_input && strstr(g_test.last_input, "always use ask_user") == NULL;
    pthread_mutex_unlock(&g_test.mu);
    ext.shutdown(&app);
    PicoApp_Free(&app);
    return ok ? 0 : Fail(name, "guidance remained after ask_user was excluded");
}

static int TestAskUserRegistrationReload(void)
{
    const char *name = "ask_user registration reload";
    PicoApp app;
    memset(&app, 0, sizeof(app));
    PicoExt ext = pico_ext_ask_user();
    ext.init(&app);
    if (!AskUserRegistered(&app))
    {
        ext.shutdown(&app);
        return Fail(name, "builtin did not register its tool, instruction hook, and overlay");
    }
    pico_clear_registrations(&app);
    ext.init(&app);
    bool ok = AskUserRegistered(&app);
    ext.shutdown(&app);
    pico_clear_registrations(&app);
    return ok ? 0 : Fail(name, "builtin did not re-register cleanly after registrations were cleared");
}

static void ConfigureAskUserCall(void)
{
    pthread_mutex_lock(&g_test.mu);
    snprintf(g_test.issue_tool_name, sizeof(g_test.issue_tool_name), "ask_user");
    snprintf(g_test.issue_tool_args, sizeof(g_test.issue_tool_args),
             "{\"questions\":[{\"id\":\"target\",\"question\":\"Which?\",\"kind\":\"select\","
             "\"options\":[\"CLI\",\"GUI\"]},{\"id\":\"notes\",\"question\":\"Notes?\","
             "\"kind\":\"text\"}]}");
    pthread_mutex_unlock(&g_test.mu);
}

static int TestAskUserToolSuccess(void)
{
    const char *name = "ask_user tool success";
    const char *expected_request =
        "{\"type\":\"questionnaire\",\"ui\":\"custom\",\"questions\":[{\"id\":\"target\","
        "\"question\":\"Which?\",\"kind\":\"select\",\"options\":[\"CLI\",\"GUI\"]},"
        "{\"id\":\"notes\",\"question\":\"Notes?\",\"kind\":\"text\"}]}";
    const char *answer =
        "{\"answers\":[{\"id\":\"target\",\"answer\":\"GUI\"},{\"id\":\"notes\","
        "\"answer\":\"Keep it fast.\"}]}";

    ResetTest(TEST_SINGLE, 1);
    PicoApp app;
    InitApp(&app);
    PicoExt ext = pico_ext_ask_user();
    ext.init(&app);
    ConfigureAskUserCall();
    PicoAgent_StartTurn(&app, PicoApp_ActiveAgent(&app), "start");

    PicoToolAsk ask;
    if (!WaitForPending(&app, 0, &ask) || strcmp(ask.request_json, expected_request) != 0)
    {
        PicoAgent_Cancel(PicoApp_ActiveAgent(&app));
        WaitForIdle(&app);
        ext.shutdown(&app);
        PicoApp_Free(&app);
        return Fail(name, "questionnaire request was not published unchanged");
    }
    if (!pico_tool_answer(&app, ask.id, answer) || !WaitForIdle(&app))
    {
        ext.shutdown(&app);
        PicoApp_Free(&app);
        return Fail(name, "answer was not delivered to the tool");
    }

    PicoTraceLine *line = LastToolTrace(&app);
    bool ok = line && line->tool_output && strcmp(line->tool_output, answer) == 0 && !line->tool_error &&
              g_test.last_input && strstr(g_test.last_input, "always use ask_user");
    ext.shutdown(&app);
    PicoApp_Free(&app);
    return ok ? 0 : Fail(name, "tool result or clarification instruction was not preserved");
}

static int TestAskUserToolCancellation(void)
{
    const char *name = "ask_user tool cancellation";
    ResetTest(TEST_SINGLE, 1);
    PicoApp app;
    InitApp(&app);
    PicoExt ext = pico_ext_ask_user();
    ext.init(&app);
    ConfigureAskUserCall();
    PicoAgent_StartTurn(&app, PicoApp_ActiveAgent(&app), "start");

    PicoToolAsk ask;
    if (!WaitForPending(&app, 0, &ask))
    {
        ext.shutdown(&app);
        PicoApp_Free(&app);
        return Fail(name, "questionnaire request was not published");
    }
    PicoAgent_Cancel(PicoApp_ActiveAgent(&app));
    if (!WaitForIdle(&app))
    {
        ext.shutdown(&app);
        PicoApp_Free(&app);
        return Fail(name, "cancelled questionnaire did not return idle");
    }
    PicoTraceLine *line = LastToolTrace(&app);
    bool ok = line && line->tool_error && line->tool_output &&
              strcmp(line->tool_output, "{\"error\":\"questionnaire cancelled\"}") == 0;
    ext.shutdown(&app);
    PicoApp_Free(&app);
    return ok ? 0 : Fail(name, "cancellation did not produce the expected tool error");
}

static int TestResumedToolCallArgs(void)
{
    const char *name = "resumed tool args match live format";
    PicoApp app;
    InitApp(&app);
    PicoApp_AddToolCall(&app, "sh", "{\"command\":\"cat examples/extra_instructions.c\"}");
    PicoTraceLine *line = LastToolTrace(&app);
    bool ok = line && line->tool_args && strcmp(line->tool_args, "command: cat examples/extra_instructions.c") == 0;
    PicoApp_Free(&app);
    return ok ? 0 : Fail(name, "resumed tool call kept raw JSON arguments");
}

#include "agent_manager_test.c"

static int TestThinkSummaryCoalesce(void)
{
    const char *name = "think summaries coalesce until a tool";
    ResetTest(TEST_SINGLE, 1);
    g_test.emit_think_summaries = true;
    PicoApp app;
    InitApp(&app);
    pico_add_tool(&app, "echo_test", "echo", "{}", EchoTool, NULL);
    snprintf(g_test.issue_tool_name, sizeof(g_test.issue_tool_name), "echo_test");
    PicoAgent_StartTurn(&app, PicoApp_ActiveAgent(&app), "start");
    if (!WaitForIdle(&app))
    {
        PicoApp_Free(&app);
        return Fail(name, "turn did not finish");
    }
    PicoMessage *msg = NULL;
    for (int i = PicoApp_ActiveAgent(&app)->message_count - 1; i >= 0; i--)
    {
        if (PicoApp_ActiveAgent(&app)->messages[i].role == PICO_ROLE_ASSISTANT)
        {
            msg = &PicoApp_ActiveAgent(&app)->messages[i];
            break;
        }
    }
    bool ok = msg && msg->trace_count == 3 && !msg->trace[0].is_tool && msg->trace[0].think_steps == 2 &&
              msg->trace[0].text && strcmp(msg->trace[0].text, "**second**") == 0 && msg->trace[1].is_tool &&
              !msg->trace[2].is_tool && msg->trace[2].think_steps == 1 && msg->trace[2].text &&
              strcmp(msg->trace[2].text, "**third**") == 0;
    PicoApp_Free(&app);
    return ok ? 0 : Fail(name, "consecutive summaries did not replace with Nx, or tool did not reset");
}

int main(void)
{
    int failed = 0;
    failed |= TestSequential();
    failed |= TestCancellation();
    failed |= TestStaleId();
    failed |= TestToolSchemaValidation();
    failed |= TestToolRegistrationFailureWarns();
    failed |= TestProductionInit();
    failed |= TestInvalidPayload();
    failed |= TestShutdownTimeout();
    failed |= TestRetiredRuntimeCap();
    failed |= TestBeforeForceCancel();
    failed |= TestBeforeDeny();
    failed |= TestBeforeAskCancel();
    failed |= TestBeforeRewriteArgs();
    failed |= TestAfterRewriteOutput();
    failed |= TestRewriteThenDenyArgs();
    failed |= TestStructuredToolDetails();
    failed |= TestInvalidDetailsFailClosed();
    failed |= TestDeniedDetailsDoNotApply();
    failed |= TestRequestOnlyContext();
    failed |= TestLlmExtraInstructions();
    failed |= TestBuildInstructionsMatchTurn();
    failed |= TestAgentPolicyPrecedesLlmHooks();
    failed |= TestLlmExcludeTool();
    failed |= TestHiddenToolCallIsControlled();
    failed |= TestOfferedCatalogSnapshot();
    failed |= TestMalformedToolCalls();
    failed |= TestTurnEnd();
    failed |= TestCancelNotification();
    failed |= TestErrorNotification();
    failed |= TestCancelledProviderUsage();
    failed |= TestSessionUsageAccumulation();
    failed |= TestUsageNormalizationAndSaturation();
    failed |= TestAfterCompact();
    failed |= TestToolTraceError();
    failed |= TestTodoAgentIsolation();
    failed |= TestAskUserHiddenOmitsGuidance();
    failed |= TestAskUserRegistrationReload();
    failed |= TestAskUserToolSuccess();
    failed |= TestAskUserToolCancellation();
    failed |= TestResumedToolCallArgs();
    failed |= TestManagerProfileRegistry();
    failed |= TestManagerConcurrencyAndIsolation();
    failed |= TestThinkSummaryCoalesce();
    return failed ? 1 : 0;
}
