#define _POSIX_C_SOURCE 200809L

#include "agent.h"
#include "json.h"
#include "settings.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

typedef enum TestMode {
    TEST_SINGLE = 0,
    TEST_SEQUENTIAL,
    TEST_INVALID,
    TEST_BLOCK,
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
    bool block_release;
    bool block_done;
    bool block_saw_live_state;
    char issue_tool_name[64];
    char *last_instructions;
    int last_tool_count;
    char last_tools[512];
    char *last_input;
    char *tool_seen_args;
    char *after_seen_args;
    char *after_seen_output;
    int followup_hook_calls;
    bool provider_fail;
    int provider_tokens;
    int life_turn_end;
    int life_cancel;
    int life_error;
    int life_after_compact;
} TestState;

static TestState g_test = {
    .mu = PTHREAD_MUTEX_INITIALIZER,
    .cv = PTHREAD_COND_INITIALIZER,
};
static int g_plugin_shutdowns;
static int g_auth_frees;

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
    g_test.block_release = false;
    g_test.block_done = false;
    g_test.block_saw_live_state = false;
    snprintf(g_test.issue_tool_name, sizeof(g_test.issue_tool_name), "ask_test");
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
    g_test.followup_hook_calls = 0;
    g_test.provider_fail = false;
    g_test.provider_tokens = 0;
    g_test.life_turn_end = 0;
    g_test.life_cancel = 0;
    g_test.life_error = 0;
    g_test.life_after_compact = 0;
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

static int FakeProvider(PicoApp *app, const PicoLlmTurn *turn, PicoLlmCancelFn cancel,
                        PicoLlmDeltaFn on_delta, void *user, PicoLlmResult *out)
{
    (void)app;
    (void)cancel;
    (void)on_delta;
    (void)user;

    pthread_mutex_lock(&g_test.mu);
    SnapshotTurn(turn);
    bool fail = g_test.provider_fail;
    int tokens = g_test.provider_tokens;
    bool issue_tool = !fail && g_test.provider_tools_issued < g_test.provider_tool_limit;
    int call_number = ++g_test.provider_tools_issued;
    char tool_name[64];
    snprintf(tool_name, sizeof(tool_name), "%s", g_test.issue_tool_name);
    pthread_mutex_unlock(&g_test.mu);

    if (fail)
    {
        out->error = JsonDup("provider failed");
        return PICO_LLM_FAIL;
    }

    out->input_tokens = tokens;

    if (!issue_tool)
    {
        out->assistant_text = JsonDup("done");
        return PICO_LLM_OK;
    }

    out->calls = (PicoLlmToolCall *)calloc(1, sizeof(PicoLlmToolCall));
    if (!out->calls)
    {
        out->error = JsonDup("allocation failed");
        return PICO_LLM_FAIL;
    }
    char call_id[32];
    snprintf(call_id, sizeof(call_id), "call-%d", call_number);
    out->calls[0].call_id = JsonDup(call_id);
    out->calls[0].name = JsonDup(tool_name);
    out->calls[0].arguments = JsonDup("{}");
    out->call_count = 1;
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

static void AskTool(PicoApp *app, const char *args_json, char **out)
{
    (void)args_json;
    if (out)
    {
        *out = NULL;
    }

    pthread_mutex_lock(&g_test.mu);
    int invocation = g_test.tool_invocations++;
    TestMode mode = g_test.mode;
    if (mode == TEST_BLOCK)
    {
        g_test.block_entered = true;
        pthread_cond_broadcast(&g_test.cv);
        while (!g_test.block_release)
        {
            pthread_cond_wait(&g_test.cv, &g_test.mu);
        }
        g_test.block_saw_live_state = app->tool_count > 0 && app->tools[0].name &&
                                      strcmp(app->tools[0].name, "ask_test") == 0 &&
                                      app->models && app->model_count == 1;
        g_test.block_done = true;
        pthread_cond_broadcast(&g_test.cv);
        pthread_mutex_unlock(&g_test.mu);
        if (out)
        {
            *out = JsonDup("unblocked");
        }
        return;
    }
    pthread_mutex_unlock(&g_test.mu);

    if (mode == TEST_INVALID)
    {
        char *answer = NULL;
        int rc = pico_tool_ask(app, "{not-json", &answer);
        StoreAskResult(invocation, 0, rc, answer);
        answer = NULL;
        rc = pico_tool_ask(app, "{\"type\":\"confirm\"}", &answer);
        StoreAskResult(invocation, 1, rc, answer);
    }
    else
    {
        char request[128];
        snprintf(request, sizeof(request),
                 "{\"type\":\"confirm\",\"message\":\"request-%d-first\"}", invocation);
        char *answer = NULL;
        int rc = pico_tool_ask(app, request, &answer);
        StoreAskResult(invocation, 0, rc, answer);

        if (mode == TEST_SEQUENTIAL && rc == PICO_ASK_OK)
        {
            snprintf(request, sizeof(request),
                     "{\"type\":\"confirm\",\"message\":\"request-%d-second\"}", invocation);
            answer = NULL;
            rc = pico_tool_ask(app, request, &answer);
            StoreAskResult(invocation, 1, rc, answer);
        }
    }

    if (out)
    {
        *out = JsonDup("tool complete");
    }
}

static void EchoTool(PicoApp *app, const char *args_json, char **out)
{
    (void)app;
    pthread_mutex_lock(&g_test.mu);
    g_test.tool_invocations++;
    free(g_test.tool_seen_args);
    g_test.tool_seen_args = JsonDup(args_json ? args_json : "");
    pthread_mutex_unlock(&g_test.mu);
    if (out)
    {
        *out = JsonDup("echo-out");
    }
}

static void DenyBefore(PicoApp *app, PicoToolEvent *ev)
{
    (void)app;
    ev->deny = true;
}

static void AskBefore(PicoApp *app, PicoToolEvent *ev)
{
    (void)ev;
    char *answer = NULL;
    int rc = pico_tool_ask(app, "{\"type\":\"confirm\",\"message\":\"gate\"}", &answer);
    pthread_mutex_lock(&g_test.mu);
    g_test.ask_rc[0][0] = rc;
    free(g_test.ask_answer[0][0]);
    g_test.ask_answer[0][0] = answer;
    pthread_mutex_unlock(&g_test.mu);
}

static void RewriteArgsBefore(PicoApp *app, PicoToolEvent *ev)
{
    (void)app;
    ev->args_json_out = JsonDup("{\"rewritten\":true}");
}

static void BlockingBefore(PicoApp *app, PicoToolEvent *ev)
{
    (void)app;
    (void)ev;
    pthread_mutex_lock(&g_test.mu);
    g_test.block_entered = true;
    pthread_cond_broadcast(&g_test.cv);
    while (!g_test.block_release)
    {
        pthread_cond_wait(&g_test.cv, &g_test.mu);
    }
    g_test.block_done = true;
    pthread_cond_broadcast(&g_test.cv);
    pthread_mutex_unlock(&g_test.mu);
}

static void CountBefore(PicoApp *app, PicoToolEvent *ev)
{
    (void)app;
    (void)ev;
    pthread_mutex_lock(&g_test.mu);
    g_test.followup_hook_calls++;
    pthread_mutex_unlock(&g_test.mu);
}

static void RewriteOutAfter(PicoApp *app, PicoToolEvent *ev)
{
    (void)app;
    ev->result = JsonDup("rewritten-output");
}

static void CaptureAfter(PicoApp *app, PicoToolEvent *ev)
{
    (void)app;
    pthread_mutex_lock(&g_test.mu);
    free(g_test.after_seen_args);
    g_test.after_seen_args = JsonDup(ev->args_json ? ev->args_json : "");
    free(g_test.after_seen_output);
    g_test.after_seen_output = JsonDup(ev->output ? ev->output : "");
    pthread_mutex_unlock(&g_test.mu);
}

static void LifeTurnEnd(PicoApp *app)
{
    (void)app;
    g_test.life_turn_end++;
}

static void LifeCancel(PicoApp *app)
{
    (void)app;
    g_test.life_cancel++;
}

static void LifeError(PicoApp *app)
{
    (void)app;
    g_test.life_error++;
}

static void LifeAfterCompact(PicoApp *app)
{
    (void)app;
    g_test.life_after_compact++;
}

static void CompactBrief(PicoApp *app)
{
    app->compact_summary = JsonDup("brief");
}

static void ExtraInstructions(PicoApp *app, PicoLlmEvent *ev)
{
    (void)app;
    ev->extra_instructions = JsonDup("injected-line");
}

static void ExcludeAskTest(PicoApp *app, PicoLlmEvent *ev)
{
    (void)app;
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

PicoModel *PicoSettings_ActiveModel(PicoApp *app)
{
    return app && app->model_count > 0 ? &app->models[0] : NULL;
}

const char *PicoSettings_ActiveEffort(const PicoApp *app)
{
    (void)app;
    return "none";
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

void PicoSession_LogAssistant(PicoApp *app, const char *content, int input_tokens, int cached_tokens)
{
    (void)app;
    (void)content;
    (void)input_tokens;
    (void)cached_tokens;
}

void PicoSession_LogToolCall(PicoApp *app, const char *call_id, const char *name, const char *args)
{
    (void)app;
    (void)call_id;
    (void)name;
    (void)args;
}

void PicoSession_LogToolResult(PicoApp *app, const char *call_id, const char *output, bool is_error)
{
    (void)app;
    (void)call_id;
    (void)output;
    (void)is_error;
}

void PicoSession_LogCompaction(PicoApp *app, const char *summary, int tokens_before)
{
    (void)app;
    (void)summary;
    (void)tokens_before;
}

void PicoPlugins_Shutdown(PicoApp *app)
{
    (void)app;
    g_plugin_shutdowns++;
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
    app->agent_state = PICO_AGENT_IDLE;
    app->settings.compact_enabled = false;
    app->models = (PicoModel *)calloc(1, sizeof(PicoModel));
    app->model_count = app->models ? 1 : 0;
    if (app->models)
    {
        snprintf(app->models[0].id, sizeof(app->models[0].id), "test-model");
        snprintf(app->models[0].provider, sizeof(app->models[0].provider), "test");
    }
    pico_add_provider(app, &(PicoProvider){.name = "test", .stream = FakeProvider});
    pico_add_tool(app, "ask_test", "test", "{}", AskTool);
    PicoAgent_Init(app);
}

static bool WaitForPending(PicoApp *app, uint64_t different_from, PicoToolAsk *out)
{
    for (int i = 0; i < 3000; i++)
    {
        PicoAgent_Pump(app);
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
        PicoAgent_Pump(app);
        if (!PicoAgent_IsBusy(app))
        {
            return true;
        }
        SleepOneMs();
    }
    return false;
}

static bool WaitForBlock(PicoApp *app)
{
    for (int i = 0; i < 3000; i++)
    {
        PicoAgent_Pump(app);
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
    PicoAgent_StartTurn(&app, "start");

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
    PicoAgent_StartTurn(&app, "start");

    PicoToolAsk ask;
    if (!WaitForPending(&app, 0, &ask))
    {
        return Fail(name, "request was not published");
    }
    PicoAgent_Cancel(&app);
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
    PicoAgent_StartTurn(&app, "first turn");

    PicoToolAsk old_ask;
    if (!WaitForPending(&app, 0, &old_ask))
    {
        return Fail(name, "first request was not published");
    }
    pthread_mutex_lock(&g_test.mu);
    g_test.provider_tool_limit = 2;
    pthread_mutex_unlock(&g_test.mu);
    PicoAgent_ForceCancel(&app);
    PicoAgent_StartTurn(&app, "second turn");

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

static int TestInvalidPayload(void)
{
    const char *name = "invalid ask payload";
    ResetTest(TEST_INVALID, 1);
    PicoApp app;
    InitApp(&app);
    PicoAgent_StartTurn(&app, "start");
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
    PicoAgent_StartTurn(app, "start");
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

static int TestBeforeForceCancel(void)
{
    const char *name = "force cancel stops remaining before hooks";
    ResetTest(TEST_SINGLE, 1);
    PicoApp app;
    InitApp(&app);
    pico_add_tool_hook(&app, PICO_TOOL_BEFORE, BlockingBefore);
    pico_add_tool_hook(&app, PICO_TOOL_BEFORE, CountBefore);
    PicoAgent_StartTurn(&app, "start");
    if (!WaitForBlock(&app))
    {
        return Fail(name, "blocking before hook did not start");
    }

    PicoAgent_ForceCancel(&app);
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
        PicoAgent_Pump(&app);
        if (!PicoAgent_BlocksReload(&app))
        {
            reaped = true;
            break;
        }
        SleepOneMs();
    }
    pthread_mutex_lock(&g_test.mu);
    bool skipped = g_test.followup_hook_calls == 0;
    pthread_mutex_unlock(&g_test.mu);
    PicoApp_Free(&app);
    if (!reaped)
    {
        return Fail(name, "abandoned before hook was not reaped");
    }
    return skipped ? 0 : Fail(name, "cancelled runtime executed a later before hook");
}

static int TestBeforeDeny(void)
{
    const char *name = "before hook deny skips tool";
    ResetTest(TEST_SINGLE, 1);
    PicoApp app;
    InitApp(&app);
    pico_add_tool_hook(&app, PICO_TOOL_BEFORE, DenyBefore);
    PicoAgent_StartTurn(&app, "start");
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
    pico_add_tool_hook(&app, PICO_TOOL_BEFORE, AskBefore);
    PicoAgent_StartTurn(&app, "start");
    PicoToolAsk ask;
    if (!WaitForPending(&app, 0, &ask))
    {
        return Fail(name, "before-hook request was not published");
    }
    PicoAgent_Cancel(&app);
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
    pico_add_tool(&app, "echo_test", "echo", "{}", EchoTool);
    snprintf(g_test.issue_tool_name, sizeof(g_test.issue_tool_name), "echo_test");
    pico_add_tool_hook(&app, PICO_TOOL_BEFORE, RewriteArgsBefore);
    PicoAgent_StartTurn(&app, "start");
    if (!WaitForIdle(&app))
    {
        return Fail(name, "agent did not return idle");
    }
    pthread_mutex_lock(&g_test.mu);
    bool ok = g_test.tool_invocations == 1 && g_test.tool_seen_args &&
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
    pico_add_tool(&app, "echo_test", "echo", "{}", EchoTool);
    snprintf(g_test.issue_tool_name, sizeof(g_test.issue_tool_name), "echo_test");
    pico_add_tool_hook(&app, PICO_TOOL_AFTER, RewriteOutAfter);
    pico_add_tool_hook(&app, PICO_TOOL_AFTER, CaptureAfter);
    PicoAgent_StartTurn(&app, "start");
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
    pico_add_tool_hook(&app, PICO_TOOL_BEFORE, RewriteArgsBefore);
    pico_add_tool_hook(&app, PICO_TOOL_BEFORE, DenyBefore);
    pico_add_tool_hook(&app, PICO_TOOL_AFTER, CaptureAfter);
    PicoAgent_StartTurn(&app, "start");
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

static int TestLlmExtraInstructions(void)
{
    const char *name = "llm hook appends instructions";
    ResetTest(TEST_SINGLE, 0);
    PicoApp app;
    InitApp(&app);
    pico_add_llm_hook(&app, ExtraInstructions);
    PicoAgent_StartTurn(&app, "start");
    if (!WaitForIdle(&app))
    {
        return Fail(name, "agent did not return idle");
    }
    pthread_mutex_lock(&g_test.mu);
    bool ok = g_test.last_instructions && strstr(g_test.last_instructions, "injected-line") != NULL;
    pthread_mutex_unlock(&g_test.mu);
    PicoApp_Free(&app);
    return ok ? 0 : Fail(name, "provider did not receive extra instructions");
}

static int TestLlmExcludeTool(void)
{
    const char *name = "llm hook excludes a tool";
    ResetTest(TEST_SINGLE, 0);
    PicoApp app;
    InitApp(&app);
    pico_add_llm_hook(&app, ExcludeAskTest);
    PicoAgent_StartTurn(&app, "start");
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
    PicoAgent_StartTurn(&app, "start");
    if (!WaitForIdle(&app))
    {
        return Fail(name, "agent did not return idle");
    }
    bool ok = g_test.life_turn_end == 1 && g_test.life_cancel == 0 && g_test.life_error == 0 &&
              app.agent_state == PICO_AGENT_IDLE;
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
    PicoAgent_StartTurn(&app, "start");
    PicoToolAsk ask;
    if (!WaitForPending(&app, 0, &ask))
    {
        return Fail(name, "request was not published");
    }
    PicoAgent_Cancel(&app);
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
    PicoApp app;
    InitApp(&app);
    AddLifeHooks(&app);
    PicoAgent_StartTurn(&app, "start");
    if (!WaitForIdle(&app))
    {
        return Fail(name, "agent did not leave the busy state");
    }
    bool ok = g_test.life_error == 1 && g_test.life_turn_end == 0 && g_test.life_cancel == 0 &&
              app.agent_state == PICO_AGENT_ERROR && app.agent_error;
    PicoApp_Free(&app);
    return ok ? 0 : Fail(name, "ON_ERROR did not fire");
}

static int TestAfterCompact(void)
{
    const char *name = "after compact then turn end";
    ResetTest(TEST_SINGLE, 0);
    g_test.provider_tokens = 100;
    PicoApp app;
    InitApp(&app);
    app.settings.compact_enabled = true;
    app.settings.compact_ratio = 0.5;
    app.tokens_limit = 100;
    AddLifeHooks(&app);
    pico_add_hook(&app, PICO_HOOK_ON_COMPACT, CompactBrief);
    PicoAgent_StartTurn(&app, "start");
    if (!WaitForIdle(&app))
    {
        return Fail(name, "agent did not return idle");
    }
    bool ok = g_test.life_after_compact == 1 && g_test.life_turn_end == 1 && g_test.life_cancel == 0 &&
              g_test.life_error == 0;
    PicoApp_Free(&app);
    return ok ? 0 : Fail(name, "AFTER_COMPACT/ON_TURN_END did not fire after briefing");
}

int main(void)
{
    int failed = 0;
    failed |= TestSequential();
    failed |= TestCancellation();
    failed |= TestStaleId();
    failed |= TestInvalidPayload();
    failed |= TestShutdownTimeout();
    failed |= TestBeforeForceCancel();
    failed |= TestBeforeDeny();
    failed |= TestBeforeAskCancel();
    failed |= TestBeforeRewriteArgs();
    failed |= TestAfterRewriteOutput();
    failed |= TestRewriteThenDenyArgs();
    failed |= TestLlmExtraInstructions();
    failed |= TestLlmExcludeTool();
    failed |= TestTurnEnd();
    failed |= TestCancelNotification();
    failed |= TestErrorNotification();
    failed |= TestAfterCompact();
    return failed ? 1 : 0;
}
