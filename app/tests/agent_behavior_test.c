#define _POSIX_C_SOURCE 200809L

#include "agent.h"
#include "workspace_internal.h"
#include "json.h"
#include "path.h"
#include "pico/plugin.h"
#include "session.h"
#include "settings.h"
#include "subagent_config.h"
#include "usage.h"
#include "host_internal.h"

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
    TEST_BATCH_TOOLS,
    TEST_PROVIDER_BLOCK,
    TEST_PROVIDER_THINK_BLOCK,
    TEST_SIGNATURE_CONTINUATION,
    TEST_CATALOG_BLOCK,
    TEST_DUPLICATE_CALLS,
    TEST_EMPTY_CALL_ID,
    TEST_TOO_MANY_CALLS,
    TEST_CONCURRENT_REVERSE,
    TEST_DELEGATION,
    TEST_DELEGATION_CHILD_BLOCK,
    TEST_DELEGATION_CHILD_ASK,
    TEST_DELEGATION_CHILD_EMPTY,
    TEST_ITEM_ORDER,
    TEST_ITEM_ORDER_REVERSE,
    TEST_MULTI_ASSISTANT_ITEMS,
    TEST_INTERLEAVED_TRACE_ITEMS,
    TEST_MALFORMED_RESULT,
    TEST_MEDIA_PERSIST_FAIL,
    TEST_UI_POST,
    TEST_UI_POST_BLOCK,
    TEST_UI_POST_CAP,
    TEST_UI_POST_LIMIT,
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
    uint64_t block_registration_generation;
    int provider_entered_count;
    PicoAgentId first_provider_id;
    char issue_tool_name[64];
    char issue_tool_args[2048];
    char *last_instructions;
    int last_tool_count;
    char last_tools[512];
    char *last_input;
    char *child_instructions;
    char *child_input;
    char child_tools[512];
    char child_model[128];
    char child_effort[PICO_EFFORT_LEN];
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
    bool logged_thinking_parts;
    char logged_thinking[256];
    char session_item_order[64];
    int session_message_groups[64];
    int session_message_group_count;
    int life_turn_end;
    int life_cancel;
    int life_error;
    int life_after_compact;
    int life_ask;
    int life_ask_end;
    PicoAgentId tool_ctx_id;
    PicoAgentId after_agent_id;
    PicoAgentId apply_agent_id;
    PicoAgentId llm_agent_id;
    PicoAgentId context_agent_id;
    PicoAgentId hook_agent_id;
    bool context_workspace_matches;
    char context_workspace[4096];
} TestState;

static TestState g_test = {
    .mu = PTHREAD_MUTEX_INITIALIZER,
    .cv = PTHREAD_COND_INITIALIZER,
};
typedef struct FakeSessionState {
    bool enabled;
    bool resolve_ok;
    char id[40];
    char path[4096];
    char profile[65];
    char purpose[1025];
    char header_model[128];
    char replayed_model[128];
    char cache_key[65];
    int replay_count;
    int log_user_count;
    int append_interrupted_count;
} FakeSessionState;

static int g_plugin_shutdowns;
static int g_plugin_reloads;
static int g_auth_frees;
static int g_random_hex_calls;
static FakeSessionState g_fake_session;
static char g_config_dir[4096] = "/tmp/pico-agent-behavior";
static PicoWorkspace *g_expected_context_workspace;

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
    g_test.block_registration_generation = 0;
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
    free(g_test.child_instructions);
    g_test.child_instructions = NULL;
    free(g_test.child_input);
    g_test.child_input = NULL;
    g_test.child_tools[0] = '\0';
    g_test.child_model[0] = '\0';
    g_test.child_effort[0] = '\0';
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
    g_test.logged_thinking_parts = false;
    g_test.logged_thinking[0] = '\0';
    g_test.session_item_order[0] = '\0';
    g_test.session_message_group_count = 0;
    g_test.life_turn_end = 0;
    g_test.life_cancel = 0;
    g_test.life_error = 0;
    g_test.life_after_compact = 0;
    g_test.life_ask = 0;
    g_test.life_ask_end = 0;
    g_test.tool_ctx_id = 0;
    g_test.after_agent_id = 0;
    g_test.apply_agent_id = 0;
    g_test.llm_agent_id = 0;
    g_test.context_agent_id = 0;
    g_test.hook_agent_id = 0;
    g_test.context_workspace_matches = false;
    g_test.context_workspace[0] = '\0';
    g_expected_context_workspace = NULL;
    pthread_mutex_unlock(&g_test.mu);
    g_plugin_shutdowns = 0;
    g_auth_frees = 0;
    g_random_hex_calls = 0;
    memset(&g_fake_session, 0, sizeof(g_fake_session));
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
                        PicoLlmDeltaFn on_delta, void *user, PicoLlmResult *out, void *state)
{
    (void)state;
    pthread_mutex_lock(&g_test.mu);
    SnapshotTurn(turn);
    snprintf(g_test.context_workspace, sizeof(g_test.context_workspace), "%s",
             pico_agent_context_workspace(ctx));
    g_test.context_workspace_matches = !g_expected_context_workspace ||
                                     PicoAgentContext_Workspace(ctx) == g_expected_context_workspace;
    bool child_turn = pico_agent_context_profile(ctx)[0] != '\0';
    if (child_turn)
    {
        free(g_test.child_instructions);
        g_test.child_instructions = JsonDup(g_test.last_instructions ? g_test.last_instructions : "");
        free(g_test.child_input);
        g_test.child_input = JsonDup(g_test.last_input ? g_test.last_input : "");
        snprintf(g_test.child_tools, sizeof(g_test.child_tools), "%s", g_test.last_tools);
        snprintf(g_test.child_model, sizeof(g_test.child_model), "%s", turn->model ? turn->model : "");
        snprintf(g_test.child_effort, sizeof(g_test.child_effort), "%s", turn->effort ? turn->effort : "");
    }
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

    if (mode == TEST_DELEGATION_CHILD_BLOCK && child_turn)
    {
        while (!cancel(user))
        {
            SleepOneMs();
        }
        return PICO_LLM_CANCEL;
    }
    if (mode == TEST_DELEGATION_CHILD_ASK && child_turn)
    {
        snprintf(tool_name, sizeof(tool_name), "ask_test");
        snprintf(tool_args, sizeof(tool_args), "{}");
    }
    if (fail)
    {
        out->error = JsonDup("provider failed");
        return PICO_LLM_FAIL;
    }
    if (mode == TEST_MALFORMED_RESULT)
    {
        out->item_count = 1;
        out->items = NULL;
        return PICO_LLM_OK;
    }
    if (mode == TEST_MEDIA_PERSIST_FAIL)
    {
        pico_llm_result_add_text(out, "must not commit");
        PicoLlmItem *item = pico_llm_result_add_item(out, PICO_LLM_ITEM_ASSISTANT);
        if (item)
        {
            pico_llm_item_add_part(item, PICO_LLM_PART_IMAGE, NULL, NULL,
                                   "data:image/png;base64,not-valid", "image/png");
        }
        return PICO_LLM_OK;
    }
    if (mode == TEST_MULTI_ASSISTANT_ITEMS)
    {
        pico_llm_result_add_text(out, "hello");
        pico_llm_result_add_text(out, "world");
        return PICO_LLM_OK;
    }
    if (mode == TEST_INTERLEAVED_TRACE_ITEMS)
    {
        if (!issue_tool)
        {
            pico_llm_result_add_text(out, "done");
            return PICO_LLM_OK;
        }
        PicoLlmItem *first = pico_llm_result_add_item(out, PICO_LLM_ITEM_ASSISTANT);
        if (first)
        {
            first->thinking = JsonDup("think-before");
        }
        pico_llm_result_add_tool_call(out, "call-order", "missing_tool", "{}", NULL);
        PicoLlmItem *second = pico_llm_result_add_item(out, PICO_LLM_ITEM_ASSISTANT);
        if (second)
        {
            second->thinking = JsonDup("think-after");
        }
        return PICO_LLM_OK;
    }
    if (mode == TEST_ITEM_ORDER || mode == TEST_ITEM_ORDER_REVERSE)
    {
        out->input_tokens = tokens;
        out->cached_tokens = cached_tokens;
        if (!issue_tool)
        {
            pico_llm_result_add_text(out, "done");
            return PICO_LLM_OK;
        }
        if (mode == TEST_ITEM_ORDER)
        {
            pico_llm_result_add_text(out, "hello");
            pico_llm_result_add_tool_call(out, "call-order", "missing_tool", "{}", NULL);
        }
        else
        {
            pico_llm_result_add_tool_call(out, "call-order", "missing_tool", "{}", NULL);
            pico_llm_result_add_text(out, "hello");
        }
        return PICO_LLM_OK;
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
    if (mode == TEST_PROVIDER_THINK_BLOCK)
    {
        if (on_delta)
        {
            on_delta(user, PICO_LLM_DELTA_THINKING, "partial-think", 13);
        }
        pthread_mutex_lock(&g_test.mu);
        g_test.block_entered = true;
        pthread_cond_broadcast(&g_test.cv);
        pthread_mutex_unlock(&g_test.mu);
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
        pico_llm_result_add_text(out, entered == 1 ? "first finished" : "second finished");
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

    if (!issue_tool && mode == TEST_DELEGATION_CHILD_EMPTY && child_turn)
    {
        return PICO_LLM_OK;
    }
    if (!issue_tool)
    {
        pico_llm_result_add_text(out, "done");
        return PICO_LLM_OK;
    }

    if (emit_summaries)
    {
        (void)pico_llm_result_add_item(out, PICO_LLM_ITEM_ASSISTANT);
    }
    if (mode == TEST_SIGNATURE_CONTINUATION)
    {
        PicoLlmItem *item = pico_llm_result_add_item(out, PICO_LLM_ITEM_ASSISTANT);
        if (item)
        {
            item->thinking_signature = JsonDup(
                "{\"type\":\"reasoning\",\"id\":\"rs-only\",\"encrypted_content\":\"blob\"}");
        }
    }

    int emitted = mode == TEST_TOO_MANY_CALLS ? 17 :
                  (mode == TEST_DUPLICATE_CALLS || mode == TEST_BATCH_TOOLS ? 2 : 1);
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
        pico_llm_result_add_tool_call(out, call_id, tool_name, tool_args, NULL);
    }
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

static void AskTool(PicoAgentContext *ctx, const char *args_json, PicoToolResult *out, void *state)
{
    (void)state;
    (void)args_json;
    if (out)
    {
        memset(out, 0, sizeof(*out));
    }

    pthread_mutex_lock(&g_test.mu);
    int invocation = g_test.tool_invocations++;
    TestMode mode = g_test.mode;
    if (mode == TEST_BLOCK || mode == TEST_BATCH_TOOLS)
    {
        g_test.block_entered = true;
        g_test.block_entered_count++;
        g_test.block_registration_generation = pico_agent_context_registration_generation(ctx);
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

static void EchoTool(PicoAgentContext *ctx, const char *args_json, PicoToolResult *out, void *state)
{
    (void)state;
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

static void LateDelegateTool(PicoAgentContext *ctx, const char *args_json, PicoToolResult *out, void *state)
{
    (void)state;
    (void)args_json;
    if (out)
    {
        memset(out, 0, sizeof(*out));
    }
    pthread_mutex_lock(&g_test.mu);
    g_test.block_entered = true;
    pthread_cond_broadcast(&g_test.cv);
    while (!g_test.block_release)
    {
        pthread_cond_wait(&g_test.cv, &g_test.mu);
    }
    pthread_mutex_unlock(&g_test.mu);

    bool is_error = false;
    char *result = PicoWorkspace_Delegate(ctx, "exploration", "late task", NULL,
                                           &is_error);
    if (out)
    {
        out->output = result;
        out->is_error = is_error;
    }
    else
    {
        free(result);
    }
}

static void UiPostTool(PicoAgentContext *ctx, const char *args_json, PicoToolResult *out, void *state)
{
    (void)state;
    TestMode mode;
    (void)args_json;
    if (out)
    {
        memset(out, 0, sizeof(*out));
    }

    pthread_mutex_lock(&g_test.mu);
    mode = g_test.mode;
    g_test.tool_invocations++;
    pthread_mutex_unlock(&g_test.mu);

    if (mode == TEST_UI_POST_CAP)
    {
        char *buf = (char *)malloc(PICO_UI_POST_TEXT_MAX + 8);
        if (buf)
        {
            memset(buf, 'a', PICO_UI_POST_TEXT_MAX + 4);
            pico_ui_post(ctx, "cap", PICO_UI_POST_TEXT, buf, PICO_UI_POST_TEXT_MAX + 4);
            pico_ui_post(ctx, "cap", PICO_UI_POST_TEXT, "b", 1);
            free(buf);
        }
    }
    else if (mode == TEST_UI_POST_BLOCK)
    {
        pico_ui_post(ctx, "stream", PICO_UI_POST_TEXT, "live", 4);
        pthread_mutex_lock(&g_test.mu);
        g_test.block_entered = true;
        pthread_cond_broadcast(&g_test.cv);
        while (!g_test.block_release)
        {
            pthread_cond_wait(&g_test.cv, &g_test.mu);
        }
        pthread_mutex_unlock(&g_test.mu);
        pico_ui_post(ctx, "stream", PICO_UI_POST_TEXT, "dead", 4);
    }
    else if (mode == TEST_UI_POST_LIMIT)
    {
        int i;
        for (i = 0; i < PICO_MAX_UI_POSTS + 1; i++)
        {
            char name[8];
            snprintf(name, sizeof(name), "b%d", i);
            pico_ui_post(ctx, name, PICO_UI_POST_STATUS, "x", 1);
        }
    }
    else
    {
        char status[PICO_UI_POST_STATUS_MAX];
        memset(status, 's', sizeof(status));
        pico_ui_post(ctx, "stream", PICO_UI_POST_STATUS, "searching", 9);
        pico_ui_post(ctx, "stream", PICO_UI_POST_TEXT, "ab", 2);
        pico_ui_post(ctx, "stream", PICO_UI_POST_TEXT, "cd", 2);
        pico_ui_post(ctx, "stream", PICO_UI_POST_STATUS, status, sizeof(status));
        pico_ui_post(ctx, "other", PICO_UI_POST_TEXT, "zz", 2);
    }

    pthread_mutex_lock(&g_test.mu);
    g_test.block_done = true;
    pthread_cond_broadcast(&g_test.cv);
    pthread_mutex_unlock(&g_test.mu);
    if (out)
    {
        out->output = JsonDup("posted");
    }
}

static void DetailsTool(PicoAgentContext *ctx, const char *args_json, PicoToolResult *out, void *state)
{
    (void)state;
    (void)ctx;
    (void)args_json;
    if (!out)
    {
        return;
    }
    out->output = JsonDup("details-out");
    out->details_json = JsonDup("{\"value\":7}");
}

static void InvalidDetailsTool(PicoAgentContext *ctx, const char *args_json, PicoToolResult *out, void *state)
{
    (void)state;
    (void)ctx;
    (void)args_json;
    if (!out)
    {
        return;
    }
    out->output = JsonDup("bad-details-out");
    out->details_json = JsonDup("{\"value\":\"\xC3(\"}");
}

static bool ApplyDetails(PicoWorkspace *workspace, PicoAgentId agent_id, const char *details_json, bool replay, void *state)
{
    (void)workspace;
    (void)state;
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

static void DenyBefore(PicoAgentContext *ctx, PicoToolEvent *ev, void *state)
{
    (void)state;
    (void)ctx;
    ev->deny = true;
}

static void AskBefore(PicoAgentContext *ctx, PicoToolEvent *ev, void *state)
{
    (void)state;
    (void)ev;
    char *answer = NULL;
    int rc = pico_tool_ask(ctx, "{\"type\":\"confirm\",\"message\":\"gate\"}", &answer);
    pthread_mutex_lock(&g_test.mu);
    g_test.ask_rc[0][0] = rc;
    free(g_test.ask_answer[0][0]);
    g_test.ask_answer[0][0] = answer;
    pthread_mutex_unlock(&g_test.mu);
}

static void RewriteArgsBefore(PicoAgentContext *ctx, PicoToolEvent *ev, void *state)
{
    (void)state;
    (void)ctx;
    ev->args_json_out = JsonDup("{\"rewritten\":true}");
}

static void BlockingBefore(PicoAgentContext *ctx, PicoToolEvent *ev, void *state)
{
    (void)state;
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

static void CountBefore(PicoAgentContext *ctx, PicoToolEvent *ev, void *state)
{
    (void)state;
    (void)ctx;
    (void)ev;
    pthread_mutex_lock(&g_test.mu);
    g_test.followup_hook_calls++;
    pthread_mutex_unlock(&g_test.mu);
}

static void RewriteOutAfter(PicoWorkspace *workspace, PicoAgentId agent_id, PicoToolEvent *event, void *state)
{
    (void)workspace;
    (void)state;
    (void)agent_id;
    event->result = JsonDup("rewritten-output");
}

static void CaptureAfter(PicoWorkspace *workspace, PicoAgentId agent_id, PicoToolEvent *event, void *state)
{
    (void)workspace;
    (void)state;
    (void)agent_id;
    pthread_mutex_lock(&g_test.mu);
    free(g_test.after_seen_args);
    g_test.after_seen_args = JsonDup(event->args_json ? event->args_json : "");
    free(g_test.after_seen_output);
    g_test.after_seen_output = JsonDup(event->output ? event->output : "");
    free(g_test.after_seen_details);
    g_test.after_seen_details = JsonDup(event->details_json ? event->details_json : "");
    g_test.after_executed = event->executed;
    g_test.after_is_error = event->is_error;
    g_test.after_agent_id = agent_id;
    pthread_mutex_unlock(&g_test.mu);
}

static void LifeTurnEnd(PicoWorkspace *workspace, const PicoHookEvent *event, void *state)
{
    (void)workspace;
    (void)state;
    g_test.life_turn_end++;
    g_test.hook_agent_id = event ? event->agent_id : 0;
}

static void LifeCancel(PicoWorkspace *workspace, const PicoHookEvent *event, void *state)
{
    (void)workspace;
    (void)state;
    (void)event;
    g_test.life_cancel++;
}

static void LifeError(PicoWorkspace *workspace, const PicoHookEvent *event, void *state)
{
    (void)workspace;
    (void)state;
    (void)event;
    g_test.life_error++;
}

static void LifeAsk(PicoWorkspace *workspace, const PicoHookEvent *event, void *state)
{
    (void)workspace;
    (void)state;
    g_test.life_ask++;
    g_test.hook_agent_id = event ? event->agent_id : 0;
}

static void LifeAskEnd(PicoWorkspace *workspace, const PicoHookEvent *event, void *state)
{
    (void)workspace;
    (void)state;
    g_test.life_ask_end++;
    g_test.hook_agent_id = event ? event->agent_id : 0;
}

static void LifeAfterCompact(PicoWorkspace *workspace, const PicoHookEvent *event, void *state)
{
    (void)workspace;
    (void)state;
    (void)event;
    g_test.life_after_compact++;
}

static void ExtraInstructions(PicoWorkspace *workspace, PicoAgentId agent_id, PicoLlmEvent *event, void *state)
{
    (void)workspace;
    (void)state;
    g_test.llm_agent_id = agent_id;
    event->extra_instructions = JsonDup("injected-line");
}

static void ExtraWhenTools(PicoWorkspace *workspace, PicoAgentId agent_id, PicoLlmEvent *event, void *state)
{
    (void)workspace;
    (void)state;
    (void)agent_id;
    if (!event->include_tools || event->tool_count == 0)
    {
        return;
    }
    event->extra_instructions = JsonDup("tool-notes");
}

static void ExcludeAskTest(PicoWorkspace *workspace, PicoAgentId agent_id, PicoLlmEvent *event, void *state)
{
    (void)workspace;
    (void)state;
    (void)agent_id;
    if (!event->exclude)
    {
        return;
    }
    for (int i = 0; i < event->tool_count; i++)
    {
        if (event->tools[i].name && strcmp(event->tools[i].name, "ask_test") == 0)
        {
            event->exclude[i] = true;
        }
    }
}

static void ExcludeAskUser(PicoWorkspace *workspace, PicoAgentId agent_id, PicoLlmEvent *event, void *state)
{
    (void)workspace;
    (void)state;
    (void)agent_id;
    for (int i = 0; event && event->exclude && i < event->tool_count; i++)
    {
        if (event->tools[i].name && strcmp(event->tools[i].name, "ask_user") == 0)
        {
            event->exclude[i] = true;
        }
    }
}

static void AddEphemeralContext(PicoWorkspace *workspace, PicoAgentId agent_id, PicoContextEvent *event, void *state)
{
    (void)workspace;
    (void)state;
    g_test.context_agent_id = agent_id;
    event->extra_context = JsonDup("ephemeral-context");
}

static void InspectBaseContext(PicoWorkspace *workspace, PicoAgentId agent_id, PicoContextEvent *event, void *state)
{
    (void)workspace;
    (void)state;
    (void)agent_id;
    bool saw_user_tail = false;
    if (event->history_count > 0)
    {
        const char *last = event->history_json[event->history_count - 1];
        saw_user_tail = last && strstr(last, "\"type\":\"user\"") != NULL &&
                        strstr(last, "ephemeral-context") == NULL;
    }
    pthread_mutex_lock(&g_test.mu);
    g_test.context_saw_base_tail = saw_user_tail;
    pthread_mutex_unlock(&g_test.mu);
}

/* Minimal host implementations used by the real agent worker. */
void *PicoPlugins_HostState(const PicoHost *host, const char *name)
{
    (void)host;
    (void)name;
    return NULL;
}

void *PicoPlugins_WorkspaceState(const PicoWorkspace *workspace, const char *name)
{
    (void)workspace;
    (void)name;
    return NULL;
}

void PicoChat_SubagentToolRow(PicoWorkspace *workspace, PicoToolRowEvent *event, void *state)
{
    (void)workspace;
    (void)event;
    (void)state;
}

void PicoHostPreferences_Load(PicoHost *app)
{
    (void)app;
}

bool PicoWorkspaceSettings_Load(PicoWorkspace *workspace)
{
    if (!workspace)
    {
        return false;
    }
    if (workspace->model_count == 0)
    {
        workspace->models = (PicoModel *)calloc(1, sizeof(PicoModel));
        workspace->model_count = workspace->models ? 1 : 0;
        if (workspace->models)
        {
            snprintf(workspace->models[0].id, sizeof(workspace->models[0].id), "test-model");
            snprintf(workspace->models[0].provider, sizeof(workspace->models[0].provider), "test");
        }
    }
    return workspace->model_count > 0;
}

void PicoSettings_ReconcileIdleAgent(PicoAgent *agent)
{
    (void)agent;
}

char *PicoSettings_LoadSystemPrompt(const PicoWorkspace *workspace)
{
    (void)workspace;
    return JsonDup("");
}

char *PicoSettings_LoadSystemPromptSpans(const PicoWorkspace *workspace, PicoPromptSpan *spans, int *span_count)
{
    char *text = PicoSettings_LoadSystemPrompt(workspace);
    if (span_count)
    {
        *span_count = 0;
    }
    if (spans && span_count && text && text[0])
    {
        spans[0].source = PICO_PROMPT_SOURCE_BASE;
        spans[0].start = 0;
        spans[0].length = strlen(text);
        *span_count = 1;
    }
    return text;
}

PicoModel *PicoSettings_FindModel(PicoWorkspace *workspace, const char *id)
{
    if (!workspace || !id) return NULL;
    for (int i = 0; i < workspace->model_count; i++)
    {
        if (strcmp(workspace->models[i].id, id) == 0) return &workspace->models[i];
    }
    return NULL;
}

const PicoModel *PicoSettings_FindModelConst(const PicoWorkspace *workspace, const char *id)
{
    return PicoSettings_FindModel((PicoWorkspace *)workspace, id);
}

PicoModel *PicoSettings_ActiveModel(const PicoAgent *agent)
{
    return (agent && agent->workspace && agent->workspace->model_count > 0) ? &agent->workspace->models[0] : NULL;
}

const PicoModel *PicoSettings_ActiveModelConst(const PicoAgent *agent)
{
    return (agent && agent->workspace && agent->workspace->model_count > 0) ? &agent->workspace->models[0] : NULL;
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

void PicoSettings_SyncAgent(PicoAgent *agent)
{
    (void)agent;
}

const char *PicoSettings_ActiveEffort(const PicoAgent *agent)
{
    (void)agent;
    return "none";
}

void PicoSettings_InitAgent(PicoAgent *agent)
{
    if (!agent) return;
    snprintf(agent->model, sizeof(agent->model), "test-model");
    snprintf(agent->effort, sizeof(agent->effort), "none");
    agent->context_limit = 1000;
    agent->compact_enabled = (agent->workspace) ? agent->workspace->settings.compact_enabled : false;
    agent->compact_ratio = (agent->workspace) ? agent->workspace->settings.compact_ratio : 0.9;
}
bool Pico_ConfigDir(char *out, size_t cap)
{
    return PicoPath_Format(out, cap, "%s", g_config_dir);
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
    g_random_hex_calls++;
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

PicoSessionWriteResult PicoSession_LogUsage(PicoHost *app, PicoAgent *agent,
                                            int input_tokens, int cached_tokens)
{
    (void)app;
    (void)agent;
    (void)input_tokens;
    (void)cached_tokens;
    g_test.usage_log_count++;
    return PICO_SESSION_WRITE_OK;
}

PicoSessionWriteResult PicoSession_LogAssistant(PicoHost *app, PicoAgent *agent,
                                                int message_group, const char *content,
                                                const char *thinking, const char *thinking_signature,
                                                const char *parts_json,
                                                const char *thinking_parts_json,
                                                int thinking_ms)
{
    (void)app;
    (void)agent;
    (void)content;
    (void)thinking_signature;
    (void)parts_json;
    if (thinking_parts_json && strstr(thinking_parts_json, "**first**") &&
        strstr(thinking_parts_json, "**second**"))
    {
        g_test.logged_thinking_parts = true;
    }
    (void)thinking_ms;
    snprintf(g_test.logged_thinking, sizeof(g_test.logged_thinking), "%s", thinking ? thinking : "");
    strncat(g_test.session_item_order, "A",
            sizeof(g_test.session_item_order) - strlen(g_test.session_item_order) - 1);
    if (g_test.session_message_group_count < 64)
    {
        g_test.session_message_groups[g_test.session_message_group_count++] = message_group;
    }
    return PICO_SESSION_WRITE_OK;
}

PicoSessionWriteResult PicoSession_LogUser(PicoHost *app, PicoAgent *agent, const char *content,
                                           const char *display, const char *parts_json)
{
    (void)app;
    (void)agent;
    (void)content;
    (void)display;
    (void)parts_json;
    if (g_fake_session.enabled)
    {
        g_fake_session.log_user_count++;
    }
    return PICO_SESSION_WRITE_OK;
}

PicoSessionWriteResult PicoSession_LogToolCall(PicoHost *app, PicoAgent *agent,
                                               int message_group, const char *call_id,
                                               const char *name, const char *args,
                                               const char *item_id)
{
    (void)app;
    (void)agent;
    (void)call_id;
    (void)name;
    (void)args;
    (void)item_id;
    strncat(g_test.session_item_order, "T",
            sizeof(g_test.session_item_order) - strlen(g_test.session_item_order) - 1);
    if (g_test.session_message_group_count < 64)
    {
        g_test.session_message_groups[g_test.session_message_group_count++] = message_group;
    }
    return PICO_SESSION_WRITE_OK;
}

PicoSessionWriteResult PicoSession_LogToolResult(PicoHost *app, PicoAgent *agent, const char *call_id, const char *name, const char *output,
                               bool is_error, const char *details_json)
{
    (void)app;
    (void)agent;
    (void)call_id;
    (void)name;
    (void)output;
    (void)is_error;
    (void)details_json;
    return PICO_SESSION_WRITE_OK;
}

PicoSessionWriteResult PicoSession_LogCompaction(PicoHost *app, PicoAgent *agent, const char *summary, int tokens_before)
{
    (void)app;
    (void)agent;
    (void)summary;
    (void)tokens_before;
    return PICO_SESSION_WRITE_OK;
}

static int g_log_title_calls;
static char g_logged_title[PICO_COMPLETE_LABEL_MAX + 1];

PicoSessionWriteResult PicoSession_LogTitle(PicoHost *app, PicoAgent *agent, const char *title)
{
    (void)app;
    (void)agent;
    const char *next = title ? title : "";
    if (strcmp(g_logged_title, next) != 0)
    {
        g_log_title_calls++;
        snprintf(g_logged_title, sizeof(g_logged_title), "%s", next);
    }
    return PICO_SESSION_WRITE_OK;
}

PicoSessionWriteResult PicoSession_LogUnseenComplete(PicoHost *app, PicoAgent *agent, bool complete)
{
    (void)app;
    if (agent)
    {
        agent->unseen_complete = complete;
    }
    return PICO_SESSION_WRITE_OK;
}

void PicoSession_SetUnseenComplete(PicoHost *app, PicoAgent *agent, bool complete)
{
    (void)app;
    if (agent)
    {
        agent->unseen_complete = complete;
    }
}

void PicoSessionPersist_Init(PicoHost *host)
{
    (void)host;
}

void PicoSessionPersist_Shutdown(PicoHost *host)
{
    (void)host;
}

void PicoSessionPersist_Pump(PicoHost *host)
{
    (void)host;
}

bool PicoCatalog_DrainOrderPersistBefore(PicoHost *host, const struct timespec *deadline)
{
    (void)host;
    (void)deadline;
    return true;
}

void PicoSession_DrainPersist(PicoHost *app, PicoAgent *agent)
{
    (void)app;
    (void)agent;
}

bool PicoSession_DrainPersistBefore(PicoHost *app, PicoAgent *agent, const struct timespec *deadline)
{
    (void)app;
    (void)agent;
    (void)deadline;
    return true;
}

void PicoPlugins_Load(PicoHost *app)
{
    if (app)
    {
        PicoWorkspace *ws = PicoHost_PrimaryWorkspace(app);
        if (ws && ws->provider_count == 0)
        {
            PicoHost_BeginRegistration(app, PICO_REG_WORKSPACE, ws);
            pico_add_provider(ws, &(PicoProvider){.name = "test", .stream = FakeProvider, .map_context = true});
            pico_add_tool(ws, "ask_test", "test", "{}", AskTool, NULL);
            PicoHost_PublishRegistration(app, NULL);
        }
    }
    if (app && app->reload_queued)
    {
        g_plugin_reloads++;
    }
}

bool PicoPlugins_ReloadHost(PicoHost *app)
{
    (void)app;
    g_plugin_reloads++;
    return true;
}

void PicoPlugins_Reload(PicoHost *app)
{
    PicoPlugins_ReloadHost(app);
}

void PicoPlugins_Shutdown(PicoHost *app)
{
    (void)app;
    g_plugin_shutdowns++;
}

void PicoPlugins_LoadWorkspaceSources(PicoHost *host, PicoWorkspace *workspace)
{
    (void)host;
    (void)workspace;
}

void PicoPlugins_InitWorkspace(PicoHost *host, PicoWorkspace *workspace)
{
    if (!host || !workspace || workspace->provider_count != 0)
    {
        return;
    }
    PicoHost_BeginRegistration(host, PICO_REG_WORKSPACE, workspace);
    pico_add_provider(workspace, &(PicoProvider){.name = "test", .stream = FakeProvider, .map_context = true});
    pico_add_tool(workspace, "ask_test", "test", "{}", AskTool, NULL);
    PicoHost_PublishRegistration(host, NULL);
}

void PicoOverlay_Notify(PicoHost *app, const char *message)
{
    (void)app;
    (void)message;
}

void PicoAuth_Load(PicoHost *app)
{
    (void)app;
}

void PicoSession_Start(PicoHost *app, PicoAgent *agent, PicoSessionStart start, const char *session_file)
{
    (void)app; (void)agent; (void)start; (void)session_file;
}

int PicoCatalog_Ensure(const char *workspace_path)
{
    (void)workspace_path;
    return 0;
}

int PicoSession_Resolve(const PicoWorkspace *workspace, const char *id, bool allow_prefix,
                        char *path, size_t path_cap)
{
    (void)workspace;
    (void)allow_prefix;
    if (!g_fake_session.enabled || !g_fake_session.resolve_ok || !id ||
        strcmp(id, g_fake_session.id) != 0)
    {
        return -1;
    }
    snprintf(path, path_cap, "%s", g_fake_session.path);
    return 0;
}

int PicoSession_LoadTranscript(const PicoWorkspace *workspace, const char *id,
                               PicoMessage **out, int *out_count)
{
    (void)workspace;
    if (out)
    {
        *out = NULL;
    }
    if (out_count)
    {
        *out_count = 0;
    }
    if (!g_fake_session.enabled || !g_fake_session.resolve_ok || !id ||
        strcmp(id, g_fake_session.id) != 0)
    {
        return -1;
    }
    PicoMessage *messages = (PicoMessage *)calloc(2, sizeof(PicoMessage));
    if (!messages)
    {
        return -1;
    }
    messages[0].role = PICO_ROLE_USER;
    messages[0].source = JsonDup("previous delegated context");
    messages[1].role = PICO_ROLE_ASSISTANT;
    messages[1].source = JsonDup("previous answer");
    if (out)
    {
        *out = messages;
    }
    if (out_count)
    {
        *out_count = 2;
    }
    return 0;
}

int PicoSession_Replay(PicoHost *app, PicoAgent *agent, const char *path, bool append_interrupted)
{
    (void)append_interrupted;
    if (!g_fake_session.enabled || !g_fake_session.resolve_ok || !agent || !path ||
        strcmp(path, g_fake_session.path) != 0)
    {
        return -1;
    }
    agent->persistence = PICO_SESSION_DURABLE;
    snprintf(agent->session_id, sizeof(agent->session_id), "%s", g_fake_session.id);
    snprintf(agent->session_path, sizeof(agent->session_path), "%s", g_fake_session.path);
    agent->kind = PICO_AGENT_SUBAGENT;
    snprintf(agent->profile, sizeof(agent->profile), "%s", g_fake_session.profile);
    snprintf(agent->purpose, sizeof(agent->purpose), "%s", g_fake_session.purpose);
    snprintf(agent->model, sizeof(agent->model), "%s", g_fake_session.replayed_model);
    PicoAgent_AddMessage(app, agent, PICO_ROLE_USER, "previous delegated context");
    PicoAgent_AddMessage(app, agent, PICO_ROLE_ASSISTANT, "previous answer");
    PicoAgent_PushHistoryUser(agent, "previous delegated context");
    PicoAgent_PushHistoryAssistant(agent, "previous answer", NULL, NULL);
    PicoAgent_SetCacheKey(agent, g_fake_session.cache_key);
    g_fake_session.replay_count++;
    return 0;
}

int PicoSession_ReadHeader(const char *path, PicoSessionHeader *out)
{
    if (!g_fake_session.enabled || !g_fake_session.resolve_ok || !path || !out ||
        strcmp(path, g_fake_session.path) != 0)
    {
        return -1;
    }
    memset(out, 0, sizeof(*out));
    out->version = 3;
    out->kind = PICO_AGENT_SUBAGENT;
    snprintf(out->id, sizeof(out->id), "%s", g_fake_session.id);
    snprintf(out->profile, sizeof(out->profile), "%s", g_fake_session.profile);
    snprintf(out->initial_purpose, sizeof(out->initial_purpose), "%s", g_fake_session.purpose);
    snprintf(out->model, sizeof(out->model), "%s", g_fake_session.header_model);
    return 0;
}

void PicoSession_ReplayToolDetails(PicoHost *app, PicoAgent *agent)
{
    (void)app; (void)agent;
}

void PicoSession_AppendInterrupted(PicoHost *app, PicoAgent *agent)
{
    (void)app;
    (void)agent;
    if (g_fake_session.enabled)
    {
        g_fake_session.append_interrupted_count++;
    }
}

void PicoSession_Reset(PicoHost *app, PicoAgent *agent)
{
    (void)app; (void)agent;
}

void PicoAuth_Free(PicoHost *app)
{
    (void)app;
    g_auth_frees++;
}

void PicoChatSel_Clear(PicoHost *app)
{
    if (app)
    {
        app->chat_sel.msg = -1;
    }
}

bool PicoChat_InspectIsOpen(void)
{
    return false;
}

void PicoChat_InspectClose(void)
{
}

static bool g_composer_has_attachment;

bool PicoComposer_HasAttachments(const PicoHost *app)
{
    (void)app;
    return g_composer_has_attachment;
}

bool PicoComposer_PointerOverAttachments(void)
{
    return false;
}

bool PicoComposer_PointerOverAttachmentRemove(void)
{
    return false;
}

bool PicoComposer_ApplyAttachments(PicoHost *app)
{
    if (!g_composer_has_attachment)
    {
        return true;
    }
    free(app->agent_parts);
    app->agent_parts = JsonDup(
        "[{\"type\":\"text\",\"text\":\"\"},{\"type\":\"image\",\"path\":\"/tmp/pasted.png\"}]");
    return app->agent_parts != NULL;
}

void PicoComposer_ReleaseAttachments(void)
{
    g_composer_has_attachment = false;
}

void PicoComposer_DiscardAttachments(void)
{
    g_composer_has_attachment = false;
}

char *pico_composer_display_message(const char *text)
{
    (void)text;
    return g_composer_has_attachment ? JsonDup("![image](/tmp/pasted.png)") : NULL;
}

bool PicoComposer_PreviewOpen(void)
{
    return false;
}

static PicoWorkspace *TestWs(PicoHost *host)
{
    PicoWorkspace *ws = PicoHost_PrimaryWorkspace(host);
    if (ws)
    {
        return ws;
    }
    ws = (PicoWorkspace *)calloc(1, sizeof(*ws));
    if (!ws)
    {
        return NULL;
    }
    ws->host = host;
    ws->id = host->next_workspace_id ? host->next_workspace_id++ : 1;
    if (host->next_workspace_id == 0)
    {
        host->next_workspace_id = 2;
    }
    snprintf(ws->path, sizeof(ws->path), ".");
    ws->state = PICO_WORKSPACE_OPEN;
    host->workspaces[0] = ws;
    host->workspace_count = 1;
    return ws;
}


static PicoAgent *TestAgent(PicoHost *app)
{
    return PicoHost_FindAgent(app, pico_agent_active(app));
}

static void InitApp(PicoHost *app)
{
    PicoWorkspace *workspace;
    g_composer_has_attachment = false;
    memset(app, 0, sizeof(*app));
    app->next_workspace_id = 1;
    app->next_agent_id = 1;
    pthread_mutex_init(&app->settings_mu, NULL);
    pthread_mutex_init(&app->ask_id_mu, NULL);
    app->ask_id_mu_ready = true;
    workspace = (PicoWorkspace *)calloc(1, sizeof(PicoWorkspace));
    workspace->host = app;
    workspace->id = app->next_workspace_id++;
    snprintf(workspace->path, sizeof(workspace->path), ".");
    workspace->state = PICO_WORKSPACE_OPEN;
    pthread_mutex_init(&workspace->settings_mu, NULL);
    pthread_mutex_init(&workspace->delegation_mu, NULL);
    pthread_mutex_init(&workspace->lifecycle_mu, NULL);
    pthread_mutex_init(&workspace->ui_post_mu, NULL);
    workspace->accepting_work = true;
    workspace->settings.compact_enabled = false;
    workspace->models = (PicoModel *)calloc(1, sizeof(PicoModel));
    workspace->model_count = workspace->models ? 1 : 0;
    if (workspace->models)
    {
        snprintf(workspace->models[0].id, sizeof(workspace->models[0].id), "test-model");
        snprintf(workspace->models[0].provider, sizeof(workspace->models[0].provider), "test");
    }
    app->workspaces[0] = workspace;
    app->workspace_count = 1;
    PicoHost_BeginRegistration(app, PICO_REG_WORKSPACE, workspace);
    pico_add_provider(workspace, &(PicoProvider){.name = "test", .stream = FakeProvider, .map_context = true});
    pico_add_tool(workspace, "ask_test", "test", "{}", AskTool, NULL);
    PicoHost_PublishRegistration(app, NULL);
    PicoAgentCreateOptions options = {
        .kind = PICO_AGENT_MAIN,
        .session_start = PICO_SESSION_NONE,
        .select = true,
    };
    PicoAgentId id = 0;
    (void)pico_main_agent_create(app, workspace->id, &options, &id);
}

static void InitExt(PicoHost *app, PicoWorkspace *ws, PicoExt ext, void **host_state, void **ws_state)
{
    if (ext.host_init)
    {
        PicoHost_BeginRegistration(app, PICO_REG_HOST, NULL);
        ext.host_init(app, host_state);
        PicoHost_PublishRegistration(app, host_state ? *host_state : NULL);
    }
    if (ext.workspace_init && ws)
    {
        PicoHost_BeginRegistration(app, PICO_REG_WORKSPACE, ws);
        ext.workspace_init(ws, ws_state);
        PicoHost_PublishRegistration(app, ws_state ? *ws_state : NULL);
    }
}

static void TestAddToolBeforeHook(PicoHost *app, PicoToolBeforeFn fn)
{
    PicoWorkspace *ws = PicoHost_PrimaryWorkspace(app);
    PicoHost_BeginRegistration(app, PICO_REG_WORKSPACE, ws);
    pico_add_tool_before_hook(ws, fn);
    PicoHost_PublishRegistration(app, NULL);
}

static void TestAddToolAfterHook(PicoHost *app, PicoToolAfterFn fn)
{
    PicoWorkspace *ws = PicoHost_PrimaryWorkspace(app);
    PicoHost_BeginRegistration(app, PICO_REG_WORKSPACE, ws);
    pico_add_tool_after_hook(ws, fn);
    PicoHost_PublishRegistration(app, NULL);
}

static void TestAddLlmHook(PicoHost *app, PicoLlmHookFn fn)
{
    PicoWorkspace *ws = PicoHost_PrimaryWorkspace(app);
    PicoHost_BeginRegistration(app, PICO_REG_WORKSPACE, ws);
    pico_add_llm_hook(ws, fn);
    PicoHost_PublishRegistration(app, NULL);
}

static void TestAddContextHook(PicoHost *app, PicoContextHookFn fn)
{
    PicoWorkspace *ws = PicoHost_PrimaryWorkspace(app);
    PicoHost_BeginRegistration(app, PICO_REG_WORKSPACE, ws);
    pico_add_context_hook(ws, fn);
    PicoHost_PublishRegistration(app, NULL);
}

static void TestAddHook(PicoHost *app, PicoHook hook, PicoWorkspaceHookFn fn)
{
    PicoWorkspace *ws = PicoHost_PrimaryWorkspace(app);
    PicoHost_BeginRegistration(app, PICO_REG_WORKSPACE, ws);
    pico_workspace_add_hook(ws, hook, fn);
    PicoHost_PublishRegistration(app, NULL);
}

static bool TestAddTool(PicoHost *app, const char *name, const char *description, const char *params_json, PicoToolFn run, PicoToolApplyFn apply)
{
    PicoWorkspace *ws = PicoHost_PrimaryWorkspace(app);
    PicoHost_BeginRegistration(app, PICO_REG_WORKSPACE, ws);
    bool ok = pico_add_tool(ws, name, description, params_json, run, apply);
    PicoHost_PublishRegistration(app, NULL);
    return ok;
}

static void TestAddProvider(PicoHost *app, const PicoProvider *provider)
{
    PicoWorkspace *ws = PicoHost_PrimaryWorkspace(app);
    PicoHost_BeginRegistration(app, PICO_REG_WORKSPACE, ws);
    pico_add_provider(ws, provider);
    PicoHost_PublishRegistration(app, NULL);
}

static bool WaitForPending(PicoHost *app, uint64_t different_from, PicoToolAsk *out)
{
    for (int i = 0; i < 3000; i++)
    {
        PicoAgent_Pump(app, TestAgent(app));
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

static bool WaitForIdle(PicoHost *app)
{
    for (int i = 0; i < 3000; i++)
    {
        PicoAgent_Pump(app, TestAgent(app));
        if (!PicoAgent_IsBusy(TestAgent(app)))
        {
            return true;
        }
        SleepOneMs();
    }
    return false;
}

static PicoTraceLine *LastToolTrace(PicoHost *app)
{
    if (!app || TestAgent(app)->message_count <= 0)
    {
        return NULL;
    }
    PicoMessage *m = &TestAgent(app)->messages[TestAgent(app)->message_count - 1];
    for (int t = m->trace_count - 1; t >= 0; t--)
    {
        if (m->trace[t].is_tool)
        {
            return &m->trace[t];
        }
    }
    return NULL;
}

static PicoTraceLine *ToolTraceByCallId(PicoHost *app, const char *call_id)
{
    PicoAgent *agent = TestAgent(app);
    if (!agent || !call_id)
    {
        return NULL;
    }
    for (int m = 0; m < agent->message_count; m++)
    {
        PicoMessage *msg = &agent->messages[m];
        for (int t = 0; t < msg->trace_count; t++)
        {
            PicoTraceLine *line = &msg->trace[t];
            if (line->is_tool && line->tool_call_id && strcmp(line->tool_call_id, call_id) == 0)
            {
                return line;
            }
        }
    }
    return NULL;
}

static bool WaitForBlockCount(PicoHost *app, int count)
{
    for (int i = 0; i < 3000; i++)
    {
        PicoAgent_Pump(app, TestAgent(app));
        pthread_mutex_lock(&g_test.mu);
        bool entered = g_test.block_entered_count >= count;
        pthread_mutex_unlock(&g_test.mu);
        if (entered) return true;
        SleepOneMs();
    }
    return false;
}

static bool WaitForBlock(PicoHost *app)
{
    for (int i = 0; i < 3000; i++)
    {
        PicoAgent_Pump(app, TestAgent(app));
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
    PicoHost app;
    InitApp(&app);
    PicoAgent_StartTurn(&app, TestAgent(&app), "start");

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
    PicoHost_Shutdown(&app);
    return ok ? 0 : Fail(name, "tool did not receive both answers");
}

static int TestCancellation(void)
{
    const char *name = "ask cancellation";
    ResetTest(TEST_SINGLE, 1);
    PicoHost app;
    InitApp(&app);
    PicoAgent_StartTurn(&app, TestAgent(&app), "start");

    PicoToolAsk ask;
    if (!WaitForPending(&app, 0, &ask))
    {
        return Fail(name, "request was not published");
    }
    PicoAgent_Cancel(TestAgent(&app));
    if (!WaitForIdle(&app))
    {
        return Fail(name, "agent did not cancel");
    }
    pthread_mutex_lock(&g_test.mu);
    bool ok = g_test.ask_rc[0][0] == PICO_ASK_CANCEL && !g_test.ask_answer[0][0];
    pthread_mutex_unlock(&g_test.mu);
    PicoHost_Shutdown(&app);
    return ok ? 0 : Fail(name, "tool did not receive cancellation");
}

static int TestStaleId(void)
{
    const char *name = "stale ask id";
    ResetTest(TEST_SINGLE, 1);
    PicoHost app;
    InitApp(&app);
    PicoAgent_StartTurn(&app, TestAgent(&app), "first turn");

    PicoToolAsk old_ask;
    if (!WaitForPending(&app, 0, &old_ask))
    {
        return Fail(name, "first request was not published");
    }
    pthread_mutex_lock(&g_test.mu);
    g_test.provider_tool_limit = 2;
    pthread_mutex_unlock(&g_test.mu);
    PicoAgent_ForceCancel(&app, TestAgent(&app));
    PicoAgent_StartTurn(&app, TestAgent(&app), "second turn");

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
    PicoHost_Shutdown(&app);
    return 0;
}

static int TestToolSchemaValidation(void)
{
    const char *name = "tool schema validation";
    PicoHost app;
    memset(&app, 0, sizeof(app));
    (void)TestWs(&app);
    PicoWorkspace *ws = PicoHost_PrimaryWorkspace(&app);
    PicoHost_BeginRegistration(&app, PICO_REG_WORKSPACE, ws);
    bool null_schema = pico_add_tool(ws, "null_schema", "null", NULL, EchoTool, NULL);
    bool empty_schema = pico_add_tool(ws, "empty_schema", "empty", "", EchoTool, NULL);
    bool valid = pico_add_tool(ws, "valid", "valid", "{\"type\":\"object\"}", EchoTool, NULL);
    bool rejected =
        !pico_add_tool(ws, "extra", "invalid", "{\"type\":\"object\"}}", EchoTool, NULL) &&
        !pico_add_tool(ws, "literal", "invalid", "{\"type\":falsee}", EchoTool, NULL) &&
        !pico_add_tool(ws, "number", "invalid", "{\"minimum\":01}", EchoTool, NULL) &&
        !pico_add_tool(ws, "comma", "invalid", "{\"type\":\"object\",}", EchoTool, NULL) &&
        !pico_add_tool(ws, "array", "invalid", "[]", EchoTool, NULL) &&
        !pico_add_tool(ws, "multiple", "invalid", "{} {}", EchoTool, NULL) &&
        !pico_add_tool(ws, "utf8", "invalid", "{\"x\":\"\xC3\x28\"}", EchoTool, NULL);
    PicoHost_PublishRegistration(&app, NULL);

    PicoHost todo_app;
    memset(&todo_app, 0, sizeof(todo_app));
    (void)TestWs(&todo_app);
    PicoExt todo = pico_ext_todo();
    PicoWorkspace *todo_ws = PicoHost_PrimaryWorkspace(&todo_app);
    InitExt(&todo_app, todo_ws, todo, NULL, NULL);
    bool todo_registered = todo_ws->tool_count == 1 && todo_ws->tools[0].params_json;
    if (todo.workspace_shutdown) todo.workspace_shutdown(todo_ws, NULL);
    bool ok = null_schema && empty_schema && valid && rejected && ws->tool_count == 3 && todo_registered;
    free(app.status_warn);
    free(todo_app.status_warn);
    return ok ? 0 : Fail(name, "invalid JSON Schema was accepted or builtin todo_update was unavailable");
}

static bool WarnMentions(const PicoHost *app, const char *tool, const char *reason)
{
    return app->status_warn && strstr(app->status_warn, tool) && strstr(app->status_warn, reason);
}

static int TestToolRegistrationFailureWarns(void)
{
    const char *name = "tool registration failure warns";
    PicoHost app;
    memset(&app, 0, sizeof(app));
    (void)TestWs(&app);
    PicoWorkspace *ws = PicoHost_PrimaryWorkspace(&app);
    PicoHost_BeginRegistration(&app, PICO_REG_WORKSPACE, ws);
    if (!pico_add_tool(ws, "ok", "ok", "{}", EchoTool, NULL) || app.status_warn)
    {
        free(app.status_warn);
        return Fail(name, "successful registration warned or failed");
    }
    if (pico_add_tool(ws, "ok", "dup", "{}", EchoTool, NULL) ||
        !WarnMentions(&app, "\"ok\"", "already registered"))
    {
        free(app.status_warn);
        return Fail(name, "duplicate name did not name the tool and reason");
    }
    free(app.status_warn);
    app.status_warn = NULL;
    if (pico_add_tool(ws, "bad_schema", "invalid", "[]", EchoTool, NULL) ||
        !WarnMentions(&app, "\"bad_schema\"", "JSON object"))
    {
        free(app.status_warn);
        return Fail(name, "invalid schema did not name the tool and reason");
    }
    free(app.status_warn);
    app.status_warn = NULL;
    if (pico_add_tool(ws, "no_run", "missing", "{}", NULL, NULL) ||
        !WarnMentions(&app, "\"no_run\"", "missing run function"))
    {
        free(app.status_warn);
        return Fail(name, "missing run did not name the tool and reason");
    }
    free(app.status_warn);
    app.status_warn = NULL;
    char extra[PICO_MAX_TOOLS][16];
    while (ws->tool_count + app.staging.ws_tool_count < PICO_MAX_TOOLS)
    {
        int i = ws->tool_count + app.staging.ws_tool_count;
        snprintf(extra[i], sizeof(extra[i]), "x%d", i);
        if (!pico_add_tool(ws, extra[i], "pad", "{}", EchoTool, NULL))
        {
            free(app.status_warn);
            return Fail(name, "padding tools to the limit failed");
        }
    }
    if (pico_add_tool(ws, "overflow", "full", "{}", EchoTool, NULL) ||
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
    PicoHost app;
    PicoHost_Start(&app, NULL, ".", false, PICO_SESSION_NONE, NULL);
    bool ok = TestAgent(&app) && TestAgent(&app)->state == PICO_AGENT_IDLE && app.composer.text;
    PicoHost_Shutdown(&app);
    return ok ? 0 : Fail(name, "PicoHost_Start did not create a usable agent before session/plugin startup");
}

static int TestReloadQuiescence(void)
{
    const char *name = "reload quiescence";
    ResetTest(TEST_BLOCK, 1);
    g_plugin_reloads = 0;
    PicoHost app;
    InitApp(&app);
    PicoWorkspace *ws = PicoHost_PrimaryWorkspace(&app);
    PicoAgent_StartTurn(&app, TestAgent(&app), "start");
    if (!WaitForBlock(&app))
    {
        return Fail(name, "blocking tool did not start");
    }

    PicoHost_RequestReload(&app);
    if (!app.reload_queued || !ws || ws->state != PICO_WORKSPACE_RELOADING ||
        PicoWorkspace_AcceptsNewWork(ws) || g_plugin_reloads != 0)
    {
        return Fail(name, "reload did not queue behind live extension work");
    }
    pthread_mutex_lock(&g_test.mu);
    g_test.block_release = true;
    pthread_cond_broadcast(&g_test.cv);
    pthread_mutex_unlock(&g_test.mu);
    if (!WaitForIdle(&app))
    {
        return Fail(name, "agent did not drain for reload");
    }

    int issued = g_test.provider_tools_issued;
    PicoAgent_StartTurn(&app, TestAgent(&app), "must wait");
    if (PicoAgent_IsBusy(TestAgent(&app)) || g_test.provider_tools_issued != issued)
    {
        return Fail(name, "a new turn started while reload was queued");
    }
    pico_host_pump(&app);
    bool ok = !app.reload_queued && ws->state == PICO_WORKSPACE_OPEN && PicoWorkspace_AcceptsNewWork(ws) &&
              g_plugin_reloads == 1;
    PicoHost_Shutdown(&app);
    return ok ? 0 : Fail(name, "reload did not run exactly once after quiescence");
}

static int TestDeferredWorkspaceChange(void)
{
    const char *name = "cd opens or selects without replacing";
    char old_template[] = "/tmp/pico-workspace-old-XXXXXX";
    char new_template[] = "/tmp/pico-workspace-new-XXXXXX";
    char *old_dir = mkdtemp(old_template);
    char *new_dir = mkdtemp(new_template);
    PicoWorkspace *old_ws;
    PicoWorkspace *new_ws;
    PicoWorkspaceId old_ws_id;
    PicoAgent *old_agent;
    int i;
    if (!old_dir || !new_dir)
    {
        return Fail(name, "could not create workspaces");
    }

    ResetTest(TEST_BLOCK, 1);
    g_plugin_reloads = 0;
    PicoHost app;
    InitApp(&app);
    snprintf(PicoHost_PrimaryWorkspace(&app)->path, sizeof(PicoHost_PrimaryWorkspace(&app)->path), "%s", old_dir);
    PicoAgentId old_id = pico_agent_active(&app);
    old_ws = PicoHost_PrimaryWorkspace(&app);
    old_ws_id = old_ws->id;
    PicoAgent_StartTurn(&app, TestAgent(&app), "start");
    if (!WaitForBlock(&app) || !PicoHost_ChangeWorkspace(&app, old_ws, new_dir))
    {
        PicoHost_Shutdown(&app);
        rmdir(old_dir);
        rmdir(new_dir);
        return Fail(name, "workspace request was not accepted while busy");
    }
    old_agent = PicoHost_FindAgent(&app, old_id);
    new_ws = PicoHost_SelectedWorkspace(&app);
    if (!old_agent || !old_ws || !new_ws || old_ws == new_ws || pico_workspace_count(&app) != 2 ||
        old_ws->id != old_ws_id || strcmp(old_ws->path, old_dir) != 0 ||
        old_ws->state != PICO_WORKSPACE_OPEN || !PicoWorkspace_AcceptsNewWork(old_ws) ||
        !PicoAgent_IsBusy(old_agent) || pico_agent_active(&app) == old_id)
    {
        PicoHost_Shutdown(&app);
        rmdir(old_dir);
        rmdir(new_dir);
        return Fail(name, "cd destroyed or paused the old workspace");
    }

    pthread_mutex_lock(&g_test.mu);
    g_test.block_release = true;
    pthread_cond_broadcast(&g_test.cv);
    pthread_mutex_unlock(&g_test.mu);
    for (i = 0; i < 3000 && PicoAgent_IsBusy(old_agent); i++)
    {
        pico_host_pump(&app);
        SleepOneMs();
    }
    if (PicoAgent_IsBusy(old_agent) || pico_agent_count(&app) != 2 ||
        PicoHost_FindAgent(&app, old_id) != old_agent)
    {
        PicoHost_Shutdown(&app);
        rmdir(old_dir);
        rmdir(new_dir);
        return Fail(name, "old workspace work did not continue after selection moved");
    }
    if (!PicoHost_ChangeWorkspace(&app, new_ws, old_dir) || pico_workspace_count(&app) != 2 ||
        PicoHost_SelectedWorkspace(&app) != old_ws || pico_agent_active(&app) != old_id)
    {
        PicoHost_Shutdown(&app);
        rmdir(old_dir);
        rmdir(new_dir);
        return Fail(name, "returning to an open path did not reuse its workspace");
    }
    PicoHost_Shutdown(&app);
    rmdir(old_dir);
    rmdir(new_dir);
    return 0;
}

static PicoAgentId g_submit_retarget_id;

static void SelectOtherOnSubmit(PicoWorkspace *workspace, const PicoHookEvent *event, void *state)
{
    PicoHost *host = workspace ? workspace->host : NULL;
    (void)event;
    (void)state;
    pico_agent_select(host, g_submit_retarget_id);
}

static int TestSubmitHookCannotRetarget(void)
{
    const char *name = "submit hook cannot retarget selected agent";
    ResetTest(TEST_SINGLE, 0);
    PicoHost app;
    InitApp(&app);
    PicoAgentId first_id = pico_agent_active(&app);
    PicoAgentCreateOptions options = {
        .kind = PICO_AGENT_MAIN,
        .session_start = PICO_SESSION_NONE,
        .select = false,
    };
    PicoAgentId second_id = 0;
    if (pico_main_agent_create(&app, PicoHost_PrimaryWorkspace(&app)->id, &options, &second_id) != PICO_OK)
    {
        PicoHost_Shutdown(&app);
        return Fail(name, "could not create a second main agent");
    }
    g_submit_retarget_id = second_id;
    TestAddHook(&app, PICO_HOOK_BEFORE_SUBMIT, SelectOtherOnSubmit);
    app.composer.text = JsonDup("keep this on the first agent");
    app.composer.length = (int)strlen(app.composer.text);
    app.composer.capacity = app.composer.length + 1;
    app.composer.cursor = app.composer.length;
    app.composer.sel_anchor = app.composer.length;

    PicoHost_Submit(&app);
    PicoAgent *first = PicoHost_FindAgent(&app, first_id);
    for (int i = 0; i < 3000 && first && PicoAgent_IsBusy(first); i++)
    {
        PicoAgent_Pump(&app, first);
        SleepOneMs();
    }
    if (first && PicoAgent_IsBusy(first))
    {
        PicoHost_Shutdown(&app);
        return Fail(name, "snapshotted submit did not finish");
    }
    bool ok = pico_agent_active(&app) == second_id &&
              pico_agent_message_count(&app, first_id) > 0 &&
              pico_agent_message_count(&app, second_id) == 0;
    PicoHost_Shutdown(&app);
    return ok ? 0 : Fail(name, "BEFORE_SUBMIT selection change retargeted the turn");
}

static int TestInvalidRestrictedPolicyPreservesSubmit(void)
{
    const char *name = "invalid restricted policy preserves submit";
    ResetTest(TEST_SINGLE, 0);
    PicoHost app;
    InitApp(&app);
    const char *tools[] = {"ask_test"};
    PicoAgentCreateOptions options = {
        .kind = PICO_AGENT_MAIN,
        .tools = tools,
        .tool_count = 1,
        .session_start = PICO_SESSION_NONE,
        .select = true,
    };
    PicoAgentId restricted = 0;
    if (pico_main_agent_create(&app, PicoHost_PrimaryWorkspace(&app)->id, &options, &restricted) != PICO_OK)
    {
        PicoHost_Shutdown(&app);
        return Fail(name, "could not create restricted agent");
    }
    PicoWorkspace *ws = PicoHost_PrimaryWorkspace(&app);
    if (ws) ws->tool_count = 0; /* simulate the allowed tool disappearing on reload */
    app.composer.text = JsonDup("keep this draft");
    app.composer.length = (int)strlen(app.composer.text);
    app.composer.capacity = app.composer.length + 1;
    app.composer.cursor = app.composer.length;
    app.composer.sel_anchor = app.composer.length;

    PicoHost_Submit(&app);
    bool ok = pico_agent_active(&app) == restricted &&
              pico_agent_message_count(&app, restricted) == 0 &&
              !PicoAgent_IsBusy(TestAgent(&app)) &&
              app.composer.length == (int)strlen("keep this draft") &&
              strcmp(app.composer.text, "keep this draft") == 0 &&
              app.status_warn && strstr(app.status_warn, "restricted tool policy");
    PicoHost_Shutdown(&app);
    return ok ? 0 : Fail(name, "invalid policy persisted a stranded turn or cleared the draft");
}

static int TestImageOnlySubmitStartsTurn(void)
{
    const char *name = "image-only submit starts turn";
    ResetTest(TEST_SINGLE, 0);
    PicoHost app;
    InitApp(&app);
    PicoHost_PrimaryWorkspace(&app)->models[0].vision = true;
    app.composer.text = JsonDup("");
    app.composer.capacity = 1;
    g_composer_has_attachment = true;

    PicoHost_Submit(&app);
    if (!WaitForIdle(&app))
    {
        PicoHost_Shutdown(&app);
        return Fail(name, "image-only turn did not finish");
    }
    pthread_mutex_lock(&g_test.mu);
    bool sent = g_test.last_input && strstr(g_test.last_input, "pasted.png") &&
                strstr(g_test.last_input, "\"type\":\"image\"");
    pthread_mutex_unlock(&g_test.mu);
    bool ok = sent && !g_composer_has_attachment && app.composer.length == 0;
    PicoHost_Shutdown(&app);
    return ok ? 0 : Fail(name, "image-only input was not sent as canonical media");
}

static int TestNonVisionSubmitPreservesDraft(void)
{
    const char *name = "non-vision submit preserves draft";
    ResetTest(TEST_SINGLE, 0);
    PicoHost app;
    InitApp(&app);
    PicoHost_PrimaryWorkspace(&app)->models[0].vision = false;
    app.composer.text = JsonDup("  keep this draft  ");
    app.composer.length = (int)strlen(app.composer.text);
    app.composer.capacity = app.composer.length + 1;
    app.composer.cursor = app.composer.length;
    app.composer.sel_anchor = app.composer.length;
    g_composer_has_attachment = true;

    PicoHost_Submit(&app);
    bool ok = !PicoAgent_IsBusy(TestAgent(&app)) &&
              pico_agent_message_count(&app, pico_agent_active(&app)) == 0 &&
              g_composer_has_attachment &&
              app.composer.length == (int)strlen("  keep this draft  ") &&
              strcmp(app.composer.text, "  keep this draft  ") == 0 &&
              app.status_warn && strstr(app.status_warn, "doesn't accept images");
    g_composer_has_attachment = false;
    PicoHost_Shutdown(&app);
    return ok ? 0 : Fail(name, "unsupported image submit cleared or started the draft");
}

static int TestInvalidPayload(void)
{
    const char *name = "invalid ask payload";
    ResetTest(TEST_INVALID, 1);
    PicoHost app;
    InitApp(&app);
    PicoAgent_StartTurn(&app, TestAgent(&app), "start");
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
    PicoHost_Shutdown(&app);
    return ok ? 0 : Fail(name, "tool did not receive the validation error");
}

static int TestShutdownTimeout(void)
{
    const char *name = "shutdown timeout preservation";
    ResetTest(TEST_BLOCK, 1);
    PicoHost *app = (PicoHost *)calloc(1, sizeof(PicoHost));
    if (!app)
    {
        return Fail(name, "allocation failed");
    }
    InitApp(app);
    PicoAgent_StartTurn(app, TestAgent(app), "start");
    if (!WaitForBlock(app))
    {
        return Fail(name, "blocking tool did not start");
    }

    PicoWorkspace *ws = PicoHost_PrimaryWorkspace(app);
    PicoModel *models = ws ? ws->models : NULL;
    PicoHostShutdownResult shutdown = PicoHost_Shutdown(app);
    if (shutdown != PICO_HOST_SHUTDOWN_RETAINED || !PicoHost_ProcessRetired() ||
        g_plugin_shutdowns != 0 || g_auth_frees != 0 || (ws && ws->models != models) ||
        !ws || ws->tool_count != 1 || !ws->tools[0].run)
    {
        return Fail(name, "PicoHost_Shutdown tore down worker-visible state");
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

    PicoHost rejected;
    memset(&rejected, 0, sizeof(rejected));
    PicoHost_Start(&rejected, NULL, ".", false, PICO_SESSION_NONE, NULL);
    bool rejected_init = rejected.terminal_shutdown && rejected.workspace_count == 0;
    return saw_live_state && rejected_init ? 0 :
           Fail(name, "detached worker saw torn-down state or Pico reinitialized after retained shutdown");
}

static int TestRetiredRuntimeCap(void)
{
    const char *name = "retired runtime cap";
    ResetTest(TEST_BLOCK, PICO_MAX_RETIRED_RUNTIMES + 1);
    PicoHost app;
    InitApp(&app);

    bool ok = true;
    int failed_at = -1;
    const char *failed_why = NULL;
    for (int i = 0; i <= PICO_MAX_RETIRED_RUNTIMES; i++)
    {
        PicoAgent_StartTurn(&app, TestAgent(&app), "block");
        if (!WaitForBlockCount(&app, i + 1))
        {
            ok = false;
            failed_at = i;
            failed_why = "worker did not reach blocking tool";
            break;
        }
        PicoAgentRt *before = TestAgent(&app)->runtime;
        PicoAgent_ForceCancel(&app, TestAgent(&app));
        if ((i < PICO_MAX_RETIRED_RUNTIMES && TestAgent(&app)->runtime == before) ||
            (i == PICO_MAX_RETIRED_RUNTIMES && TestAgent(&app)->runtime != before))
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
    PicoHost_Shutdown(&app);
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
    PicoHost app;
    InitApp(&app);
    TestAddToolBeforeHook(&app, BlockingBefore);
    TestAddToolBeforeHook(&app, CountBefore);
    PicoAgent_StartTurn(&app, TestAgent(&app), "start");
    if (!WaitForBlock(&app))
    {
        return Fail(name, "blocking before hook did not start");
    }

    PicoAgent_ForceCancel(&app, TestAgent(&app));
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
        PicoAgent_Pump(&app, TestAgent(&app));
        if (!PicoAgent_BlocksReload(TestAgent(&app)))
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
    PicoHost_Shutdown(&app);
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
    PicoHost app;
    InitApp(&app);
    TestAddToolBeforeHook(&app, DenyBefore);
    PicoAgent_StartTurn(&app, TestAgent(&app), "start");
    if (!WaitForIdle(&app))
    {
        return Fail(name, "agent did not return idle");
    }
    pthread_mutex_lock(&g_test.mu);
    bool ok = g_test.tool_invocations == 0 && g_test.provider_tools_issued == 2;
    pthread_mutex_unlock(&g_test.mu);
    PicoHost_Shutdown(&app);
    return ok ? 0 : Fail(name, "denied tool ran or turn did not continue");
}

static int TestBeforeAskCancel(void)
{
    const char *name = "cancel during before ask skips tool";
    ResetTest(TEST_SINGLE, 1);
    PicoHost app;
    InitApp(&app);
    TestAddToolBeforeHook(&app, AskBefore);
    PicoAgent_StartTurn(&app, TestAgent(&app), "start");
    PicoToolAsk ask;
    if (!WaitForPending(&app, 0, &ask))
    {
        return Fail(name, "before-hook request was not published");
    }
    PicoAgent_Cancel(TestAgent(&app));
    if (!WaitForIdle(&app))
    {
        return Fail(name, "agent did not cancel");
    }
    pthread_mutex_lock(&g_test.mu);
    bool ok = g_test.tool_invocations == 0 && g_test.ask_rc[0][0] == PICO_ASK_CANCEL;
    pthread_mutex_unlock(&g_test.mu);
    PicoHost_Shutdown(&app);
    return ok ? 0 : Fail(name, "tool ran after cancelled before-ask");
}

static int TestBeforeRewriteArgs(void)
{
    const char *name = "before hook rewrites tool args";
    ResetTest(TEST_SINGLE, 1);
    PicoHost app;
    InitApp(&app);
    TestAddTool(&app, "echo_test", "echo", "{}", EchoTool, NULL);
    snprintf(g_test.issue_tool_name, sizeof(g_test.issue_tool_name), "echo_test");
    TestAddToolBeforeHook(&app, RewriteArgsBefore);
    PicoAgent_StartTurn(&app, TestAgent(&app), "start");
    if (!WaitForIdle(&app))
    {
        return Fail(name, "agent did not return idle");
    }
    pthread_mutex_lock(&g_test.mu);
    bool ok = g_test.tool_invocations == 1 &&
              g_test.tool_ctx_id == pico_agent_id(TestAgent(&app)) && g_test.tool_seen_args &&
              strcmp(g_test.tool_seen_args, "{\"rewritten\":true}") == 0;
    pthread_mutex_unlock(&g_test.mu);
    PicoHost_Shutdown(&app);
    return ok ? 0 : Fail(name, "tool did not see rewritten args");
}

static int TestAfterRewriteOutput(void)
{
    const char *name = "after hook rewrites tool output";
    ResetTest(TEST_SINGLE, 1);
    PicoHost app;
    InitApp(&app);
    TestAddTool(&app, "echo_test", "echo", "{}", EchoTool, NULL);
    snprintf(g_test.issue_tool_name, sizeof(g_test.issue_tool_name), "echo_test");
    TestAddToolAfterHook(&app, RewriteOutAfter);
    TestAddToolAfterHook(&app, CaptureAfter);
    PicoAgent_StartTurn(&app, TestAgent(&app), "start");
    if (!WaitForIdle(&app))
    {
        return Fail(name, "agent did not return idle");
    }
    pthread_mutex_lock(&g_test.mu);
    bool ok = g_test.last_input && strstr(g_test.last_input, "rewritten-output") != NULL &&
              g_test.after_seen_output && strcmp(g_test.after_seen_output, "rewritten-output") == 0;
    pthread_mutex_unlock(&g_test.mu);
    PicoHost_Shutdown(&app);
    return ok ? 0 : Fail(name, "rewritten output was not chained to the model and later hooks");
}

static int TestRewriteThenDenyArgs(void)
{
    const char *name = "denied call preserves rewritten args";
    ResetTest(TEST_SINGLE, 1);
    PicoHost app;
    InitApp(&app);
    TestAddToolBeforeHook(&app, RewriteArgsBefore);
    TestAddToolBeforeHook(&app, DenyBefore);
    TestAddToolAfterHook(&app, CaptureAfter);
    PicoAgent_StartTurn(&app, TestAgent(&app), "start");
    if (!WaitForIdle(&app))
    {
        return Fail(name, "agent did not return idle");
    }
    pthread_mutex_lock(&g_test.mu);
    bool ok = g_test.tool_invocations == 0 && g_test.after_seen_args &&
              strcmp(g_test.after_seen_args, "{\"rewritten\":true}") == 0 &&
              g_test.after_seen_output && strcmp(g_test.after_seen_output, "User denied this tool.") == 0;
    pthread_mutex_unlock(&g_test.mu);
    PicoHost_Shutdown(&app);
    return ok ? 0 : Fail(name, "after hook did not receive the denied call's current values");
}

static int TestStructuredToolDetails(void)
{
    const char *name = "structured tool details apply and reach hooks";
    ResetTest(TEST_SINGLE, 1);
    PicoHost app;
    InitApp(&app);
    if (!TestAddTool(&app, "details_test", "details", "{}", DetailsTool, ApplyDetails) ||
        TestAddTool(&app, "details_test", "duplicate", "{}", DetailsTool, ApplyDetails))
    {
        PicoHost_Shutdown(&app);
        return Fail(name, "tool name uniqueness contract failed");
    }
    snprintf(g_test.issue_tool_name, sizeof(g_test.issue_tool_name), "details_test");
    TestAddToolAfterHook(&app, CaptureAfter);
    PicoAgent_StartTurn(&app, TestAgent(&app), "start");
    if (!WaitForIdle(&app))
    {
        PicoHost_Shutdown(&app);
        return Fail(name, "agent did not return idle");
    }
    pthread_mutex_lock(&g_test.mu);
    bool ok = g_test.apply_calls == 1 && g_test.apply_agent_id == pico_agent_id(TestAgent(&app)) &&
              g_test.after_agent_id == pico_agent_id(TestAgent(&app)) &&
              g_test.after_executed && !g_test.after_is_error &&
              g_test.after_seen_details && strcmp(g_test.after_seen_details, "{\"value\":7}") == 0 &&
              g_test.last_input && strstr(g_test.last_input, "\"name\":\"details_test\"") &&
              strstr(g_test.last_input, "\"is_error\":false");
    pthread_mutex_unlock(&g_test.mu);
    PicoHost_Shutdown(&app);
    return ok ? 0 : Fail(name, "details were not applied or represented in the result input");
}

static int TestInvalidDetailsFailClosed(void)
{
    const char *name = "invalid structured details fail closed";
    ResetTest(TEST_SINGLE, 1);
    PicoHost app;
    InitApp(&app);
    TestAddTool(&app, "invalid_details", "details", "{}", InvalidDetailsTool, ApplyDetails);
    snprintf(g_test.issue_tool_name, sizeof(g_test.issue_tool_name), "invalid_details");
    TestAddToolAfterHook(&app, CaptureAfter);
    PicoAgent_StartTurn(&app, TestAgent(&app), "start");
    if (!WaitForIdle(&app))
    {
        PicoHost_Shutdown(&app);
        return Fail(name, "agent did not return idle");
    }
    pthread_mutex_lock(&g_test.mu);
    bool ok = g_test.apply_calls == 0 && g_test.after_executed && g_test.after_is_error &&
              g_test.after_seen_details && g_test.after_seen_details[0] == '\0';
    pthread_mutex_unlock(&g_test.mu);
    PicoHost_Shutdown(&app);
    return ok ? 0 : Fail(name, "invalid details were applied or exposed as successful");
}

static int TestDeniedDetailsDoNotApply(void)
{
    const char *name = "denied structured tool does not apply";
    ResetTest(TEST_SINGLE, 1);
    PicoHost app;
    InitApp(&app);
    TestAddTool(&app, "details_test", "details", "{}", DetailsTool, ApplyDetails);
    snprintf(g_test.issue_tool_name, sizeof(g_test.issue_tool_name), "details_test");
    TestAddToolBeforeHook(&app, DenyBefore);
    TestAddToolAfterHook(&app, CaptureAfter);
    PicoAgent_StartTurn(&app, TestAgent(&app), "start");
    if (!WaitForIdle(&app))
    {
        PicoHost_Shutdown(&app);
        return Fail(name, "agent did not return idle");
    }
    pthread_mutex_lock(&g_test.mu);
    bool ok = g_test.apply_calls == 0 && !g_test.after_executed && g_test.after_is_error;
    pthread_mutex_unlock(&g_test.mu);
    PicoHost_Shutdown(&app);
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
    PicoHost app;
    InitApp(&app);
    TestAddContextHook(&app, AddEphemeralContext);
    TestAddContextHook(&app, InspectBaseContext);
    PicoAgent_StartTurn(&app, TestAgent(&app), "first");
    if (!WaitForIdle(&app))
    {
        PicoHost_Shutdown(&app);
        return Fail(name, "first turn did not finish");
    }
    PicoAgent_StartTurn(&app, TestAgent(&app), "second");
    if (!WaitForIdle(&app))
    {
        PicoHost_Shutdown(&app);
        return Fail(name, "second turn did not finish");
    }
    pthread_mutex_lock(&g_test.mu);
    bool ok = g_test.context_saw_base_tail &&
              g_test.context_agent_id == pico_agent_id(TestAgent(&app)) && g_test.last_input &&
              CountOccurrences(g_test.last_input, "ephemeral-context") == 1 &&
              strstr(g_test.last_input, "\"type\":\"context\"") != NULL &&
              strstr(g_test.last_input,
                     "{\"type\":\"context\",\"parts\":[{\"type\":\"text\",\"text\":\"ephemeral-context\"}]}") !=
                  NULL &&
              (!g_test.last_instructions || strstr(g_test.last_instructions, "ephemeral-context") == NULL);
    pthread_mutex_unlock(&g_test.mu);
    PicoHost_Shutdown(&app);
    return ok ? 0 : Fail(name, "context persisted, entered instructions, used a user item, or changed the next hook's base history");
}

static int SilentProvider(PicoAgentContext *ctx, const PicoLlmTurn *turn, PicoLlmCancelFn cancel,
                          PicoLlmDeltaFn on_delta, void *user, PicoLlmResult *out, void *state)
{
    (void)state;
    (void)ctx;
    (void)turn;
    (void)cancel;
    (void)on_delta;
    (void)user;
    pico_llm_result_add_text(out, "ok");
    return PICO_LLM_OK;
}

static int TestUnmappedContextFails(void)
{
    const char *name = "unmapped context items fail the turn";
    ResetTest(TEST_SINGLE, 0);
    PicoHost app;
    InitApp(&app);
    TestAddProvider(&app, &(PicoProvider){.name = "legacy", .stream = SilentProvider});
    snprintf(PicoHost_PrimaryWorkspace(&app)->models[0].provider,
             sizeof(PicoHost_PrimaryWorkspace(&app)->models[0].provider), "legacy");
    TestAddContextHook(&app, AddEphemeralContext);
    PicoAgent_StartTurn(&app, TestAgent(&app), "hello");
    if (!WaitForIdle(&app))
    {
        PicoHost_Shutdown(&app);
        return Fail(name, "turn did not finish");
    }
    bool ok = TestAgent(&app)->state == PICO_AGENT_ERROR && TestAgent(&app)->error &&
              strstr(TestAgent(&app)->error, "does not map context") != NULL;
    PicoHost_Shutdown(&app);
    return ok ? 0 : Fail(name, "provider without map_context dropped context instead of failing");
}

static int TestLlmExtraInstructions(void)
{
    const char *name = "llm hook appends instructions";
    ResetTest(TEST_SINGLE, 0);
    PicoHost app;
    InitApp(&app);
    TestAddLlmHook(&app, ExtraInstructions);
    PicoAgent_StartTurn(&app, TestAgent(&app), "start");
    if (!WaitForIdle(&app))
    {
        return Fail(name, "agent did not return idle");
    }
    pthread_mutex_lock(&g_test.mu);
    const char *header = g_test.last_instructions
                             ? strstr(g_test.last_instructions, "## Additional instructions")
                             : NULL;
    const char *extra = g_test.last_instructions ? strstr(g_test.last_instructions, "injected-line") : NULL;
    bool ok = g_test.llm_agent_id == pico_agent_id(TestAgent(&app)) && header && extra &&
              extra > header;
    pthread_mutex_unlock(&g_test.mu);
    PicoHost_Shutdown(&app);
    return ok ? 0 : Fail(name, "provider did not receive extra instructions under the section header");
}

static int TestBuildInstructionsExtraSpan(void)
{
    const char *name = "previewed extra instructions are a distinct span";
    ResetTest(TEST_SINGLE, 0);
    PicoHost app;
    InitApp(&app);
    TestAddLlmHook(&app, ExtraInstructions);
    PicoPromptSpan spans[PICO_PROMPT_SPAN_MAX];
    int n = 0;
    char *preview = PicoAgent_BuildInstructionsSpans(&app, TestAgent(&app), spans, &n);
    bool ok = preview && n == 1 && spans[0].source == PICO_PROMPT_SOURCE_LLM_HOOK &&
              spans[0].start == 0 && spans[0].length == strlen(preview) &&
              strstr(preview, "## Additional instructions") == preview &&
              strstr(preview, "injected-line") != NULL;
    free(preview);
    PicoHost_Shutdown(&app);
    return ok ? 0 : Fail(name, "extra instructions were not a single LLM-hook span");
}

static int TestBuildInstructionsMatchTurn(void)
{
    const char *name = "previewed instructions match the next turn";
    ResetTest(TEST_SINGLE, 0);
    PicoHost app;
    InitApp(&app);
    TestAddLlmHook(&app, ExtraWhenTools);
    char *preview = PicoAgent_BuildInstructions(&app, TestAgent(&app));
    PicoAgent_StartTurn(&app, TestAgent(&app), "start");
    if (!WaitForIdle(&app))
    {
        free(preview);
        PicoHost_Shutdown(&app);
        return Fail(name, "agent did not return idle");
    }
    pthread_mutex_lock(&g_test.mu);
    bool ok = preview && strstr(preview, "tool-notes") != NULL && g_test.last_instructions &&
              strcmp(preview, g_test.last_instructions) == 0;
    pthread_mutex_unlock(&g_test.mu);
    free(preview);
    PicoHost_Shutdown(&app);
    return ok ? 0 : Fail(name, "preview did not match the instructions sent to the provider");
}

static int TestAgentPolicyPrecedesLlmHooks(void)
{
    const char *name = "agent policy precedes llm hooks";
    ResetTest(TEST_SINGLE, 0);
    PicoHost app;
    InitApp(&app);
    TestAgent(&app)->allowed_tools = (char **)calloc(1, sizeof(char *));
    TestAgent(&app)->allowed_tool_count = 0;
    TestAddLlmHook(&app, ExtraWhenTools);
    PicoAgent_StartTurn(&app, TestAgent(&app), "start");
    if (!WaitForIdle(&app))
    {
        PicoHost_Shutdown(&app);
        return Fail(name, "agent did not return idle");
    }
    pthread_mutex_lock(&g_test.mu);
    bool ok = g_test.last_tool_count == 0 && g_test.last_instructions &&
              strstr(g_test.last_instructions, "tool-notes") == NULL &&
              strstr(g_test.last_instructions, "## Additional instructions") == NULL;
    pthread_mutex_unlock(&g_test.mu);
    PicoHost_Shutdown(&app);
    return ok ? 0 : Fail(name, "LLM hook or provider saw a policy-hidden tool");
}

static int TestLlmExcludeTool(void)
{
    const char *name = "llm hook excludes a tool";
    ResetTest(TEST_SINGLE, 0);
    PicoHost app;
    InitApp(&app);
    TestAddLlmHook(&app, ExcludeAskTest);
    PicoAgent_StartTurn(&app, TestAgent(&app), "start");
    if (!WaitForIdle(&app))
    {
        return Fail(name, "agent did not return idle");
    }
    pthread_mutex_lock(&g_test.mu);
    bool ok = g_test.last_tool_count == 0 && strstr(g_test.last_tools, "ask_test") == NULL;
    pthread_mutex_unlock(&g_test.mu);
    PicoHost_Shutdown(&app);
    return ok ? 0 : Fail(name, "excluded tool was still in the catalog");
}

static int TestHiddenToolCallIsControlled(void)
{
    const char *name = "hidden tool call is controlled";
    ResetTest(TEST_SINGLE, 1);
    PicoHost app;
    InitApp(&app);
    TestAddLlmHook(&app, ExcludeAskTest);
    TestAddToolBeforeHook(&app, CountBefore);
    TestAddToolAfterHook(&app, CaptureAfter);
    PicoAgent_StartTurn(&app, TestAgent(&app), "start");
    if (!WaitForIdle(&app))
    {
        PicoHost_Shutdown(&app);
        return Fail(name, "agent did not finish the recovery round");
    }
    pthread_mutex_lock(&g_test.mu);
    bool ok = g_test.tool_invocations == 0 && g_test.followup_hook_calls == 0 &&
              !g_test.after_seen_output && g_test.last_input &&
              strstr(g_test.last_input, "tool was not offered for this request") != NULL;
    pthread_mutex_unlock(&g_test.mu);
    PicoHost_Shutdown(&app);
    return ok ? 0 : Fail(name, "hidden call executed a hook/tool or was not returned as a tool error");
}

static int TestOfferedCatalogSnapshot(void)
{
    const char *name = "offered tool snapshot survives registry mutation";
    ResetTest(TEST_CATALOG_BLOCK, 1);
    PicoHost app;
    InitApp(&app);
    PicoWorkspace *ws = PicoHost_PrimaryWorkspace(&app);
    PicoHost_BeginRegistration(&app, PICO_REG_WORKSPACE, ws);
    pico_add_tool(ws, "echo_test", "echo", "{}", EchoTool, NULL);
    PicoHost_PublishRegistration(&app, NULL);
    snprintf(g_test.issue_tool_name, sizeof(g_test.issue_tool_name), "echo_test");
    PicoAgent_StartTurn(&app, TestAgent(&app), "start");
    if (!WaitForBlock(&app))
    {
        PicoHost_Shutdown(&app);
        return Fail(name, "provider gate did not open");
    }
    for (int i = 0; ws && i < ws->tool_count; i++)
    {
        if (ws->tools[i].name && strcmp(ws->tools[i].name, "echo_test") == 0)
        {
            ws->tools[i].run = NULL;
        }
    }
    pthread_mutex_lock(&g_test.mu);
    g_test.block_release = true;
    pthread_cond_broadcast(&g_test.cv);
    pthread_mutex_unlock(&g_test.mu);
    if (!WaitForIdle(&app))
    {
        PicoHost_Shutdown(&app);
        return Fail(name, "agent did not finish");
    }
    pthread_mutex_lock(&g_test.mu);
    bool ok = g_test.tool_invocations == 1;
    pthread_mutex_unlock(&g_test.mu);
    PicoHost_Shutdown(&app);
    return ok ? 0 : Fail(name, "tool resolution used the mutated global registry");
}

static int TestMalformedCallsFail(TestMode mode, const char *expected)
{
    ResetTest(mode, 1);
    PicoHost app;
    InitApp(&app);
    PicoAgent_StartTurn(&app, TestAgent(&app), "start");
    if (!WaitForIdle(&app))
    {
        PicoHost_Shutdown(&app);
        return 1;
    }
    pthread_mutex_lock(&g_test.mu);
    bool no_tool = g_test.tool_invocations == 0;
    pthread_mutex_unlock(&g_test.mu);
    bool ok = no_tool && TestAgent(&app)->state == PICO_AGENT_ERROR && TestAgent(&app)->error &&
              strstr(TestAgent(&app)->error, expected) != NULL;
    PicoHost_Shutdown(&app);
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

static void AddLifeHooks(PicoHost *app)
{
    TestAddHook(app, PICO_HOOK_ON_TURN_END, LifeTurnEnd);
    TestAddHook(app, PICO_HOOK_ON_CANCEL, LifeCancel);
    TestAddHook(app, PICO_HOOK_ON_ERROR, LifeError);
    TestAddHook(app, PICO_HOOK_AFTER_COMPACT, LifeAfterCompact);
    TestAddHook(app, PICO_HOOK_ON_ASK, LifeAsk);
    TestAddHook(app, PICO_HOOK_ON_ASK_END, LifeAskEnd);
}

static int TestTurnEnd(void)
{
    const char *name = "turn end notification";
    ResetTest(TEST_SINGLE, 0);
    PicoHost app;
    InitApp(&app);
    AddLifeHooks(&app);
    PicoAgent_StartTurn(&app, TestAgent(&app), "start");
    if (!WaitForIdle(&app))
    {
        return Fail(name, "agent did not return idle");
    }
    bool ok = g_test.life_turn_end == 1 && g_test.hook_agent_id == pico_agent_id(TestAgent(&app)) &&
              g_test.life_cancel == 0 && g_test.life_error == 0 &&
              TestAgent(&app)->state == PICO_AGENT_IDLE;
    PicoHost_Shutdown(&app);
    return ok ? 0 : Fail(name, "ON_TURN_END did not fire once");
}

static int TestCancelNotification(void)
{
    const char *name = "cancel notification";
    ResetTest(TEST_SINGLE, 1);
    PicoHost app;
    InitApp(&app);
    AddLifeHooks(&app);
    PicoAgent_StartTurn(&app, TestAgent(&app), "start");
    PicoToolAsk ask;
    if (!WaitForPending(&app, 0, &ask))
    {
        return Fail(name, "request was not published");
    }
    PicoAgent_Cancel(TestAgent(&app));
    if (!WaitForIdle(&app))
    {
        return Fail(name, "agent did not cancel");
    }
    bool ok = g_test.life_cancel == 1 && g_test.life_turn_end == 0 && g_test.life_error == 0 &&
              g_test.life_ask == 1 && g_test.life_ask_end == 1;
    PicoHost_Shutdown(&app);
    return ok ? 0 : Fail(name, "ON_CANCEL did not fire (or TURN_END did)");
}

static int TestErrorNotification(void)
{
    const char *name = "error notification";
    ResetTest(TEST_SINGLE, 0);
    g_test.provider_fail = true;
    g_test.provider_tokens = 100;
    g_test.provider_cached_tokens = 80;
    PicoHost app;
    InitApp(&app);
    AddLifeHooks(&app);
    PicoAgent_StartTurn(&app, TestAgent(&app), "start");
    if (!WaitForIdle(&app))
    {
        return Fail(name, "agent did not leave the busy state");
    }
    bool ok = g_test.life_error == 1 && g_test.life_turn_end == 0 && g_test.life_cancel == 0 &&
              TestAgent(&app)->state == PICO_AGENT_ERROR && TestAgent(&app)->error && TestAgent(&app)->session_input_tokens == 0 &&
              TestAgent(&app)->session_cached_tokens == 0 && g_test.usage_log_count == 0;
    PicoHost_Shutdown(&app);
    return ok ? 0 : Fail(name, "failed request changed usage or did not fire ON_ERROR");
}


static int TestAskNotification(void)
{
    const char *name = "ask notification";
    ResetTest(TEST_SINGLE, 1);
    PicoHost app;
    InitApp(&app);
    AddLifeHooks(&app);
    PicoAgent_StartTurn(&app, TestAgent(&app), "start");
    PicoToolAsk ask;
    if (!WaitForPending(&app, 0, &ask))
    {
        PicoHost_Shutdown(&app);
        return Fail(name, "request was not published");
    }
    bool opened = g_test.life_ask == 1 && g_test.life_ask_end == 0 &&
                  g_test.hook_agent_id == pico_agent_id(TestAgent(&app));
    if (!pico_tool_answer(&app, ask.id, "{\"ok\":true}"))
    {
        PicoHost_Shutdown(&app);
        return Fail(name, "answer was rejected");
    }
    PicoAgent_Pump(&app, TestAgent(&app));
    bool closed = g_test.life_ask == 1 && g_test.life_ask_end == 1;
    if (!WaitForIdle(&app))
    {
        PicoHost_Shutdown(&app);
        return Fail(name, "agent did not return idle");
    }
    PicoHost_Shutdown(&app);
    return opened && closed ? 0 : Fail(name, "ON_ASK / ON_ASK_END did not fire once");
}

static int TestAskForceCancelNotification(void)
{
    const char *name = "ask force-cancel notification";
    ResetTest(TEST_SINGLE, 1);
    PicoHost app;
    InitApp(&app);
    AddLifeHooks(&app);
    PicoAgent_StartTurn(&app, TestAgent(&app), "start");
    PicoToolAsk ask;
    if (!WaitForPending(&app, 0, &ask))
    {
        PicoHost_Shutdown(&app);
        return Fail(name, "request was not published");
    }
    PicoAgent_ForceCancel(&app, TestAgent(&app));
    if (!WaitForIdle(&app))
    {
        PicoHost_Shutdown(&app);
        return Fail(name, "agent did not force-cancel");
    }
    bool ok = g_test.life_ask == 1 && g_test.life_ask_end == 1 && g_test.life_cancel == 1 &&
              g_test.life_turn_end == 0;
    PicoHost_Shutdown(&app);
    return ok ? 0 : Fail(name, "force-cancelled ask did not fire ON_ASK_END");
}

static int TestAskReplaceNotification(void)
{
    const char *name = "ask replace notification";
    ResetTest(TEST_SEQUENTIAL, 1);
    PicoHost app;
    InitApp(&app);
    AddLifeHooks(&app);
    PicoAgent_StartTurn(&app, TestAgent(&app), "start");
    PicoToolAsk first;
    if (!WaitForPending(&app, 0, &first) || !strstr(first.request_json, "first"))
    {
        PicoHost_Shutdown(&app);
        return Fail(name, "first request was not published");
    }
    if (g_test.life_ask != 1 || g_test.life_ask_end != 0)
    {
        PicoHost_Shutdown(&app);
        return Fail(name, "first ask did not fire ON_ASK once");
    }
    if (!pico_tool_answer(&app, first.id, "{\"step\":1}"))
    {
        PicoHost_Shutdown(&app);
        return Fail(name, "first answer was rejected");
    }
    bool first_done = false;
    for (int i = 0; i < 3000; i++)
    {
        pthread_mutex_lock(&g_test.mu);
        first_done = g_test.ask_rc[0][0] == PICO_ASK_OK;
        pthread_mutex_unlock(&g_test.mu);
        if (first_done)
        {
            break;
        }
        SleepOneMs();
    }
    if (!first_done)
    {
        PicoHost_Shutdown(&app);
        return Fail(name, "first ask was not answered");
    }
    /* The next pico_tool_ask is entered immediately after the first returns.
     * Do not pump until then, so one snapshot pass replaces old-id → new-id. */
    for (int i = 0; i < 50; i++)
    {
        SleepOneMs();
    }
    PicoAgent_Pump(&app, TestAgent(&app));
    PicoToolAsk second;
    if (!pico_tool_pending_ask(&app, &second) || second.id == first.id ||
        !strstr(second.request_json, "second"))
    {
        PicoHost_Shutdown(&app);
        return Fail(name, "second request was not published on the replacement pump");
    }
    bool replaced = g_test.life_ask == 2 && g_test.life_ask_end == 1 &&
                    g_test.hook_agent_id == pico_agent_id(TestAgent(&app));
    if (!pico_tool_answer(&app, second.id, "{\"step\":2}") || !WaitForIdle(&app))
    {
        PicoHost_Shutdown(&app);
        return Fail(name, "second ask did not finish");
    }
    PicoHost_Shutdown(&app);
    return replaced && g_test.life_ask == 2 && g_test.life_ask_end == 2
               ? 0
               : Fail(name, "ask replacement did not fire ON_ASK_END then ON_ASK");
}

static int TestCancelledProviderUsage(void)
{
    const char *name = "cancelled provider usage";
    ResetTest(TEST_PROVIDER_BLOCK, 0);
    g_test.provider_tokens = 100;
    g_test.provider_cached_tokens = 80;
    PicoHost app;
    InitApp(&app);
    PicoAgent_StartTurn(&app, TestAgent(&app), "start");
    PicoAgent_Cancel(TestAgent(&app));
    if (!WaitForIdle(&app))
    {
        PicoHost_Shutdown(&app);
        return Fail(name, "cancelled provider did not return idle");
    }
    bool ok = TestAgent(&app)->session_input_tokens == 0 && TestAgent(&app)->session_cached_tokens == 0 &&
              g_test.usage_log_count == 0;
    PicoHost_Shutdown(&app);
    return ok ? 0 : Fail(name, "cancelled provider result contributed usage");
}

static int TestSessionUsageAccumulation(void)
{
    const char *name = "session usage accumulates successful requests";
    ResetTest(TEST_SINGLE, 1);
    g_test.provider_tokens = 100;
    g_test.provider_cached_tokens = 25;
    PicoHost app;
    InitApp(&app);
    TestAddTool(&app, "echo_test", "echo", "{}", EchoTool, NULL);
    snprintf(g_test.issue_tool_name, sizeof(g_test.issue_tool_name), "echo_test");
    PicoAgent_StartTurn(&app, TestAgent(&app), "first");
    if (!WaitForIdle(&app))
    {
        PicoHost_Shutdown(&app);
        return Fail(name, "tool turn did not finish");
    }
    g_test.provider_tokens = 200;
    g_test.provider_cached_tokens = 150;
    PicoAgent_StartTurn(&app, TestAgent(&app), "second");
    if (!WaitForIdle(&app))
    {
        PicoHost_Shutdown(&app);
        return Fail(name, "second turn did not finish");
    }
    bool ok = TestAgent(&app)->tokens_used == 200 && TestAgent(&app)->tokens_cached == 150 && TestAgent(&app)->session_input_tokens == 400 &&
              TestAgent(&app)->session_cached_tokens == 200 && g_test.usage_log_count == 3;
    PicoHost_Shutdown(&app);
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
    PicoHost app;
    InitApp(&app);
    PicoHost_PrimaryWorkspace(&app)->settings.compact_enabled = true;
    PicoHost_PrimaryWorkspace(&app)->settings.compact_ratio = 0.5;
    TestAgent(&app)->compact_enabled = true;
    TestAgent(&app)->compact_ratio = 0.5;
    TestAgent(&app)->context_limit = 100;
    AddLifeHooks(&app);
    PicoAgent_StartTurn(&app, TestAgent(&app), "start");
    if (!WaitForIdle(&app))
    {
        return Fail(name, "agent did not return idle");
    }
    bool ok = g_test.life_after_compact == 1 && g_test.life_turn_end == 1 && g_test.life_cancel == 0 &&
              g_test.life_error == 0 && TestAgent(&app)->tokens_used == 0 && TestAgent(&app)->tokens_cached == 0 &&
              TestAgent(&app)->session_input_tokens == 200 && TestAgent(&app)->session_cached_tokens == 80 &&
              g_test.usage_log_count == 2;
    PicoHost_Shutdown(&app);
    return ok ? 0 : Fail(name, "compaction did not retain cumulative usage or fire lifecycle hooks");
}

static int TestToolTraceError(void)
{
    const char *name = "tool trace error flag";
    ResetTest(TEST_SINGLE, 1);
    PicoHost app;
    InitApp(&app);
    TestAddTool(&app, "echo_test", "echo", "{}", EchoTool, NULL);
    snprintf(g_test.issue_tool_name, sizeof(g_test.issue_tool_name), "echo_test");
    PicoAgent_StartTurn(&app, TestAgent(&app), "start");
    if (!WaitForIdle(&app))
    {
        return Fail(name, "successful tool did not finish");
    }
    PicoTraceLine *ok_line = LastToolTrace(&app);
    bool ok = ok_line && ok_line->tool_output && !ok_line->tool_error;
    PicoHost_Shutdown(&app);
    if (!ok)
    {
        return Fail(name, "successful tool was marked as error");
    }

    ResetTest(TEST_SINGLE, 1);
    InitApp(&app);
    snprintf(g_test.issue_tool_name, sizeof(g_test.issue_tool_name), "missing_tool");
    PicoAgent_StartTurn(&app, TestAgent(&app), "start");
    if (!WaitForIdle(&app))
    {
        return Fail(name, "unknown tool did not finish");
    }
    PicoTraceLine *err_line = LastToolTrace(&app);
    ok = err_line && err_line->tool_output && err_line->tool_error;
    PicoHost_Shutdown(&app);
    return ok ? 0 : Fail(name, "unknown tool was not marked as error");
}

static int TestQueuedToolCallProgress(void)
{
    const char *name = "later tool call stays queued";
    ResetTest(TEST_BATCH_TOOLS, 1);
    PicoHost app;
    InitApp(&app);
    PicoAgent_StartTurn(&app, TestAgent(&app), "start");
    if (!WaitForBlock(&app))
    {
        return Fail(name, "first tool did not start");
    }

    PicoAgent *agent = TestAgent(&app);
    PicoTraceLine *first = ToolTraceByCallId(&app, "call-1-0");
    PicoTraceLine *second = ToolTraceByCallId(&app, "call-1-1");
    pthread_mutex_lock(&g_test.mu);
    int invocations = g_test.tool_invocations;
    pthread_mutex_unlock(&g_test.mu);
    int live_ms = PicoAgent_LiveActionMs(agent);
    bool queued = first && !first->tool_output &&
                  PicoAgent_ToolCallProgress(agent, "call-1-0") == PICO_TOOL_CALL_RUNNING &&
                  second && !second->tool_output &&
                  PicoAgent_ToolCallProgress(agent, "call-1-1") == PICO_TOOL_CALL_QUEUED &&
                  invocations == 1 && live_ms > 0;

    pthread_mutex_lock(&g_test.mu);
    g_test.block_release = true;
    pthread_cond_broadcast(&g_test.cv);
    pthread_mutex_unlock(&g_test.mu);
    if (!WaitForIdle(&app))
    {
        PicoHost_Shutdown(&app);
        return Fail(name, "batched tools did not finish");
    }
    int idle_ms = PicoAgent_LiveActionMs(TestAgent(&app));
    PicoHost_Shutdown(&app);
    if (!queued)
    {
        return Fail(name, "the later call was running before the first finished");
    }
    return idle_ms == 0 ? 0 : Fail(name, "live action timer did not clear after tools finished");
}

static int TestTodoAgentIsolation(void)
{
    const char *name = "todo state is keyed by agent id";
    PicoHost app;
    memset(&app, 0, sizeof(app));
    (void)TestWs(&app);
    PicoWorkspace *ws = PicoHost_PrimaryWorkspace(&app);
    PicoExt ext = pico_ext_todo();
    void *todo_state = NULL;
    InitExt(&app, ws, ext, NULL, &todo_state);
    const char *first =
        "{\"version\":1,\"task\":\"First\",\"todos\":[{\"id\":\"a\",\"text\":\"first-agent\",\"status\":\"pending\"}]}";
    const char *second =
        "{\"version\":1,\"task\":\"Second\",\"todos\":[{\"id\":\"b\",\"text\":\"second-agent\",\"status\":\"in_progress\"}]}";
    bool ok = ws->tool_count == 1 && ws->tools[0].apply &&
              ws->tools[0].apply(ws, 11, first, true, todo_state) &&
              ws->tools[0].apply(ws, 22, second, true, todo_state);

    PicoContextEvent one = {.tools = ws->tools, .tool_count = ws->tool_count};
    PicoContextEvent two = {.tools = ws->tools, .tool_count = ws->tool_count};
    ws->context_hooks[0].fn(ws, 11, &one, todo_state);
    ws->context_hooks[0].fn(ws, 22, &two, todo_state);
    PicoContextEvent hidden = {0};
    ws->context_hooks[0].fn(ws, 22, &hidden, todo_state);
    ok = ok && !hidden.extra_context && one.extra_context && strstr(one.extra_context, "first-agent") &&
         !strstr(one.extra_context, "second-agent") && two.extra_context &&
         strstr(two.extra_context, "second-agent") && !strstr(two.extra_context, "first-agent");
    free(one.extra_context);
    free(two.extra_context);

    PicoHookEvent reset = {.hook = PICO_HOOK_ON_SESSION_RESET, .agent_id = 11};
    for (int i = 0; i < ws->hook_count; i++)
    {
        if (ws->hooks[i].hook == reset.hook)
        {
            ws->hooks[i].workspace_fn(ws, &reset, todo_state);
        }
    }
    one = (PicoContextEvent){.tools = ws->tools, .tool_count = ws->tool_count};
    two = (PicoContextEvent){.tools = ws->tools, .tool_count = ws->tool_count};
    ws->context_hooks[0].fn(PicoHost_PrimaryWorkspace(&app), 11, &one, todo_state);
    ws->context_hooks[0].fn(PicoHost_PrimaryWorkspace(&app), 22, &two, todo_state);
    ok = ok && !one.extra_context && two.extra_context && strstr(two.extra_context, "second-agent");
    free(one.extra_context);
    free(two.extra_context);
    ext.workspace_shutdown(ws, todo_state);
    pico_clear_registrations(&app);
    return ok ? 0 : Fail(name, "apply, context, or reset crossed agent boundaries");
}

static PicoTool *FindTodoTool(PicoWorkspace *ws)
{
    for (int i = 0; ws && i < ws->tool_count; i++)
    {
        if (ws->tools[i].name && strcmp(ws->tools[i].name, "todo_update") == 0)
        {
            return &ws->tools[i];
        }
    }
    return NULL;
}

static int TestTodoApplyRenamesSession(void)
{
    const char *name = "todo apply renames the session when task changes";
    PicoHost app;
    InitApp(&app);
    PicoWorkspace *ws = PicoHost_PrimaryWorkspace(&app);
    PicoExt ext = pico_ext_todo();
    void *todo_state = NULL;
    InitExt(&app, ws, ext, NULL, &todo_state);
    PicoTool *tool = FindTodoTool(ws);
    PicoAgent *agent = TestAgent(&app);
    const char *first =
        "{\"version\":1,\"task\":\"Name the session\",\"todos\":"
        "[{\"id\":\"a\",\"text\":\"work\",\"status\":\"pending\"}]}";
    const char *same =
        "{\"version\":1,\"task\":\"Name the session\",\"todos\":"
        "[{\"id\":\"a\",\"text\":\"later\",\"status\":\"in_progress\"}]}";
    const char *replayed_same =
        "{\"version\":1,\"task\":\"Name the session\",\"todos\":"
        "[{\"id\":\"a\",\"text\":\"work\",\"status\":\"pending\"}]}";
    const char *replayed_stale =
        "{\"version\":1,\"task\":\"Heal replayed title\",\"todos\":"
        "[{\"id\":\"a\",\"text\":\"work\",\"status\":\"pending\"}]}";
    const char *renamed =
        "{\"version\":1,\"task\":\"Rename again\",\"todos\":"
        "[{\"id\":\"a\",\"text\":\"work\",\"status\":\"completed\"}]}";
    g_log_title_calls = 0;
    g_logged_title[0] = '\0';
    bool ok = tool && tool->apply && agent && ext.workspace_on_frame &&
              tool->apply(PicoHost_PrimaryWorkspace(&app), agent->id, first, false, todo_state) && g_log_title_calls == 0;
    ext.workspace_on_frame(ws, todo_state, 0.0f);
    ok = ok && g_log_title_calls == 1 && strcmp(g_logged_title, "Name the session") == 0 &&
         tool->apply(PicoHost_PrimaryWorkspace(&app), agent->id, same, false, todo_state);
    ext.workspace_on_frame(ws, todo_state, 0.0f);
    ok = ok && g_log_title_calls == 1 && tool->apply(PicoHost_PrimaryWorkspace(&app), agent->id, replayed_same, true, todo_state);
    ext.workspace_on_frame(ws, todo_state, 0.0f);
    ok = ok && g_log_title_calls == 1 && tool->apply(PicoHost_PrimaryWorkspace(&app), agent->id, replayed_stale, true, todo_state);
    ext.workspace_on_frame(ws, todo_state, 0.0f);
    ok = ok && g_log_title_calls == 2 && strcmp(g_logged_title, "Heal replayed title") == 0 &&
         tool->apply(PicoHost_PrimaryWorkspace(&app), agent->id, renamed, false, todo_state) && g_log_title_calls == 2;
    ext.workspace_on_frame(ws, todo_state, 0.0f);
    ok = ok && g_log_title_calls == 3 && strcmp(g_logged_title, "Rename again") == 0;
    if (ext.workspace_shutdown)
    {
        ext.workspace_shutdown(ws, todo_state);
    }
    PicoHost_Shutdown(&app);
    return ok ? 0 : Fail(name, "live apply did not rename on task change, or replay/unchanged task wrote a title");
}

static bool AskUserRegistered(const PicoHost *app)
{
    const PicoWorkspace *ws = PicoHost_PrimaryWorkspaceConst(app);
    return app && ws && ws->tool_count == 1 && ws->tools[0].name &&
           strcmp(ws->tools[0].name, "ask_user") == 0 && ws->tools[0].run &&
           ws->llm_hook_count == 1 && app->view_count[PICO_SLOT_OVERLAY] == 1 &&
           app->hook_count == 2;
}

static int TestAskUserHiddenOmitsGuidance(void)
{
    const char *name = "hidden ask_user omits guidance";
    ResetTest(TEST_SINGLE, 0);
    PicoHost app;
    InitApp(&app);
    PicoExt ext = pico_ext_ask_user();
    InitExt(&app, TestWs(&app), ext, NULL, NULL);
    PicoHost_BeginRegistration(&app, PICO_REG_WORKSPACE, PicoHost_PrimaryWorkspace(&app));
    pico_add_llm_hook(PicoHost_PrimaryWorkspace(&app), ExcludeAskUser);
    PicoHost_PublishRegistration(&app, NULL);
    PicoAgent_StartTurn(&app, TestAgent(&app), "start");
    if (!WaitForIdle(&app))
    {
        if (ext.workspace_shutdown) ext.workspace_shutdown(TestWs(&app), NULL);
        ext.host_shutdown(&app, NULL);
        PicoHost_Shutdown(&app);
        return Fail(name, "agent did not finish");
    }
    pthread_mutex_lock(&g_test.mu);
    bool ok = g_test.last_instructions && strstr(g_test.last_instructions, "always use ask_user") == NULL &&
              (!g_test.last_input || strstr(g_test.last_input, "always use ask_user") == NULL);
    pthread_mutex_unlock(&g_test.mu);
    if (ext.workspace_shutdown) ext.workspace_shutdown(TestWs(&app), NULL);
    ext.host_shutdown(&app, NULL);
    PicoHost_Shutdown(&app);
    return ok ? 0 : Fail(name, "guidance remained after ask_user was excluded");
}

static int TestAskUserRegistrationReload(void)
{
    const char *name = "ask_user registration reload";
    PicoHost app;
    memset(&app, 0, sizeof(app));
    PicoWorkspace *ws = TestWs(&app);
    PicoExt ext = pico_ext_ask_user();
    InitExt(&app, ws, ext, NULL, NULL);
    if (!AskUserRegistered(&app))
    {
        if (ext.workspace_shutdown) ext.workspace_shutdown(ws, NULL);
        ext.host_shutdown(&app, NULL);
        return Fail(name, "builtin did not register its tool, instruction hook, and overlay");
    }
    pico_clear_registrations(&app);
    InitExt(&app, ws, ext, NULL, NULL);
    bool ok = AskUserRegistered(&app);
    if (ext.workspace_shutdown) ext.workspace_shutdown(ws, NULL);
    ext.host_shutdown(&app, NULL);
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
    PicoHost app;
    InitApp(&app);
    PicoExt ext = pico_ext_ask_user();
    InitExt(&app, TestWs(&app), ext, NULL, NULL);
    ConfigureAskUserCall();
    PicoAgent_StartTurn(&app, TestAgent(&app), "start");

    PicoToolAsk ask;
    if (!WaitForPending(&app, 0, &ask) || strcmp(ask.request_json, expected_request) != 0)
    {
        PicoAgent_Cancel(TestAgent(&app));
        WaitForIdle(&app);
        if (ext.workspace_shutdown) ext.workspace_shutdown(TestWs(&app), NULL);
        ext.host_shutdown(&app, NULL);
        PicoHost_Shutdown(&app);
        return Fail(name, "questionnaire request was not published unchanged");
    }
    if (!pico_tool_answer(&app, ask.id, answer) || !WaitForIdle(&app))
    {
        if (ext.workspace_shutdown) ext.workspace_shutdown(TestWs(&app), NULL);
        ext.host_shutdown(&app, NULL);
        PicoHost_Shutdown(&app);
        return Fail(name, "answer was not delivered to the tool");
    }

    PicoTraceLine *line = LastToolTrace(&app);
    bool ok = line && line->tool_output && strcmp(line->tool_output, answer) == 0 && !line->tool_error &&
              g_test.last_instructions && strstr(g_test.last_instructions, "always use ask_user");
    if (ext.workspace_shutdown) ext.workspace_shutdown(TestWs(&app), NULL);
    ext.host_shutdown(&app, NULL);
    PicoHost_Shutdown(&app);
    return ok ? 0 : Fail(name, "tool result or clarification guidance was not preserved");
}

static int TestAskUserToolCancellation(void)
{
    const char *name = "ask_user tool cancellation";
    ResetTest(TEST_SINGLE, 1);
    PicoHost app;
    InitApp(&app);
    PicoExt ext = pico_ext_ask_user();
    InitExt(&app, TestWs(&app), ext, NULL, NULL);
    ConfigureAskUserCall();
    PicoAgent_StartTurn(&app, TestAgent(&app), "start");

    PicoToolAsk ask;
    if (!WaitForPending(&app, 0, &ask))
    {
        if (ext.workspace_shutdown) ext.workspace_shutdown(TestWs(&app), NULL);
        ext.host_shutdown(&app, NULL);
        PicoHost_Shutdown(&app);
        return Fail(name, "questionnaire request was not published");
    }
    PicoAgent_Cancel(TestAgent(&app));
    if (!WaitForIdle(&app))
    {
        if (ext.workspace_shutdown) ext.workspace_shutdown(TestWs(&app), NULL);
        ext.host_shutdown(&app, NULL);
        PicoHost_Shutdown(&app);
        return Fail(name, "cancelled questionnaire did not return idle");
    }
    PicoTraceLine *line = LastToolTrace(&app);
    bool ok = line && line->tool_error && line->tool_output &&
              strcmp(line->tool_output, "{\"error\":\"questionnaire cancelled\"}") == 0;
    if (ext.workspace_shutdown) ext.workspace_shutdown(TestWs(&app), NULL);
    ext.host_shutdown(&app, NULL);
    PicoHost_Shutdown(&app);
    return ok ? 0 : Fail(name, "cancellation did not produce the expected tool error");
}

static int TestResumedToolCallArgs(void)
{
    const char *name = "resumed tool args match live format";
    PicoHost app;
    InitApp(&app);
    PicoHost_AddToolCall(&app, pico_agent_active(&app), "sh",
                         "{\"command\":\"cat examples/extra_instructions.c\"}");
    PicoTraceLine *line = LastToolTrace(&app);
    bool ok = line && line->tool_args &&
              strcmp(line->tool_args, "command: cat examples/extra_instructions.c") == 0 &&
              line->tool_args_json &&
              strcmp(line->tool_args_json,
                     "{\"command\":\"cat examples/extra_instructions.c\"}") == 0;
    PicoHost_Shutdown(&app);
    return ok ? 0 : Fail(name, "tool call did not preserve display and raw arguments");
}

static int TestShToolCallDescriptionArgs(void)
{
    const char *name = "sh tool rows show description";
    PicoHost app;
    InitApp(&app);
    PicoHost_AddToolCall(&app, pico_agent_active(&app), "sh",
                         "{\"description\":\"listing all folders in /tmp\","
                         "\"command\":\"ls -la /tmp\"}");
    PicoTraceLine *line = LastToolTrace(&app);
    bool ok = line && line->tool_args &&
              strcmp(line->tool_args, "listing all folders in /tmp") == 0 &&
              line->tool_args_json &&
              strstr(line->tool_args_json, "\"command\":\"ls -la /tmp\"");
    PicoHost_Shutdown(&app);
    return ok ? 0 : Fail(name, "sh tool row did not show description");
}

static int TestBackgroundToolCallDescriptionArgs(void)
{
    const char *name = "run_background tool rows show description only";
    PicoHost app;
    InitApp(&app);
    PicoHost_AddToolCall(&app, pico_agent_active(&app), "run_background",
                         "{\"description\":\"serving the site on port 8000\","
                         "\"command\":\"python3 -m http.server 8000\"}");
    PicoTraceLine *line = LastToolTrace(&app);
    bool ok = line && line->tool_args &&
              strcmp(line->tool_args, "serving the site on port 8000") == 0 &&
              line->tool_args_json &&
              strstr(line->tool_args_json, "\"command\":\"python3 -m http.server 8000\"");
    PicoHost_Shutdown(&app);
    return ok ? 0 : Fail(name, "run_background tool row did not show the description alone");
}

static int TestBackgroundToolCallCommandExtraction(void)
{
    const char *name = "run_background expanded row recovers exact command";
    PicoHost app;
    InitApp(&app);
    PicoHost_AddToolCall(&app, pico_agent_active(&app), "run_background",
                         "{\"description\":\"serving the site on port 8000\","
                         "\"command\":\"python3 -m http.server 8000\"}");
    PicoTraceLine *line = LastToolTrace(&app);
    char *command = line ? PicoAgent_FormatToolCommand(line->tool_name, line->tool_args_json)
                         : NULL;
    bool ok = command && strcmp(command, "python3 -m http.server 8000") == 0;
    free(command);
    PicoHost_AddToolCall(&app, pico_agent_active(&app), "run_background",
                         "{\"description\":\"no command\"}");
    line = LastToolTrace(&app);
    command = line ? PicoAgent_FormatToolCommand(line->tool_name, line->tool_args_json) : NULL;
    ok = ok && !command;
    PicoHost_Shutdown(&app);
    return ok ? 0 : Fail(name, "exact command was not recovered from run_background args");
}

static int TestShToolCallCommandExtraction(void)
{
    const char *name = "sh expanded row recovers exact command";
    PicoHost app;
    InitApp(&app);
    PicoHost_AddToolCall(&app, pico_agent_active(&app), "sh",
                         "{\"description\":\"listing all folders in /tmp\","
                         "\"command\":\"ls -la /tmp\"}");
    PicoTraceLine *line = LastToolTrace(&app);
    char *command = line ? PicoAgent_FormatToolCommand(line->tool_name, line->tool_args_json)
                         : NULL;
    bool ok = command && strcmp(command, "ls -la /tmp") == 0;
    free(command);
    PicoHost_AddToolCall(&app, pico_agent_active(&app), "sh", "{\"description\":\"no command\"}");
    line = LastToolTrace(&app);
    command = line ? PicoAgent_FormatToolCommand(line->tool_name, line->tool_args_json) : NULL;
    ok = ok && !command;
    PicoHost_AddToolCall(&app, pico_agent_active(&app), "read_file", "{\"path\":\"/tmp/x\"}");
    line = LastToolTrace(&app);
    command = line ? PicoAgent_FormatToolCommand(line->tool_name, line->tool_args_json) : NULL;
    ok = ok && !command;
    PicoHost_Shutdown(&app);
    return ok ? 0 : Fail(name, "exact command was not recovered from sh args");
}

static int TestToolActivityDescriptionScope(void)
{
    const char *name = "tool activity description is sh-only";
    ResetTest(TEST_BLOCK, 1);
    PicoHost app;
    InitApp(&app);
    snprintf(g_test.issue_tool_args, sizeof(g_test.issue_tool_args),
             "{\"description\":\"unrelated summary\",\"command\":\"echo original\"}");
    PicoAgent_StartTurn(&app, TestAgent(&app), "start");
    if (!WaitForBlock(&app))
    {
        PicoHost_Shutdown(&app);
        return Fail(name, "blocking tool did not start");
    }
    bool ok = strcmp(TestAgent(&app)->activity, "Running ask_test: echo original") == 0;
    pthread_mutex_lock(&g_test.mu);
    g_test.block_release = true;
    pthread_cond_broadcast(&g_test.cv);
    pthread_mutex_unlock(&g_test.mu);
    if (!WaitForIdle(&app))
    {
        ok = false;
    }
    PicoHost_Shutdown(&app);
    return ok ? 0 : Fail(name, "non-sh activity used its description argument");
}

static int TestToolCallListArgs(void)
{
    const char *name = "tool call lists show item counts";
    PicoHost app;
    InitApp(&app);
    PicoHost_AddToolCall(&app, pico_agent_active(&app), "todo_update",
                        "{\"todos\":[{\"id\":\"1\",\"content\":\"a\",\"status\":\"pending\"},"
                        "{\"id\":\"2\",\"content\":\"b\",\"status\":\"pending\"},"
                        "{\"id\":\"3\",\"content\":\"c\",\"status\":\"pending\"},"
                        "{\"id\":\"4\",\"content\":\"d\",\"status\":\"pending\"}]}");
    PicoTraceLine *line = LastToolTrace(&app);
    bool ok = line && line->tool_args && strcmp(line->tool_args, "todos: [4 items]") == 0 &&
              line->tool_args_json && strstr(line->tool_args_json, "\"todos\"");
    PicoHost_Shutdown(&app);
    return ok ? 0 : Fail(name, "list args were not summarized as an item count");
}

static int TestWorkerContextCapturesRegistrationGeneration(void)
{
    const char *name = "worker context captures registration generation";
    ResetTest(TEST_BLOCK, 1);
    PicoHost app;
    InitApp(&app);
    PicoHost_PrimaryWorkspace(&app)->registration_generation = 6;
    PicoWorkspace_PublishRegistrationGeneration(PicoHost_PrimaryWorkspace(&app));
    PicoAgent_StartTurn(&app, TestAgent(&app), "start");
    if (!WaitForBlock(&app))
    {
        PicoHost_Shutdown(&app);
        return Fail(name, "blocking tool did not start");
    }
    pthread_mutex_lock(&g_test.mu);
    uint64_t captured = g_test.block_registration_generation;
    g_test.block_release = true;
    pthread_cond_broadcast(&g_test.cv);
    pthread_mutex_unlock(&g_test.mu);
    for (int i = 0; i < 3000 && PicoAgent_IsBusy(TestAgent(&app)); i++)
    {
        PicoAgent_Pump(&app, TestAgent(&app));
        SleepOneMs();
    }
    PicoHost_Shutdown(&app);
    return captured == 7 ? 0 : Fail(name, "turn worker did not retain the workspace registration generation");
}

#include "workspace_test.c"
#include "subagent_config_test.c"
#include "subagent_test.c"

static int TestResultItemOrder(TestMode mode, bool assistant_first, const char *name)
{
    ResetTest(mode, 1);
    PicoHost app;
    InitApp(&app);
    PicoAgent_StartTurn(&app, TestAgent(&app), "start");
    if (!WaitForIdle(&app))
    {
        PicoHost_Shutdown(&app);
        return Fail(name, "item-order turn did not finish");
    }
    pthread_mutex_lock(&g_test.mu);
    const char *input = g_test.last_input ? g_test.last_input : "";
    const char *asst = strstr(input, "\"type\":\"assistant\"");
    const char *call = strstr(input, "\"type\":\"tool_call\"");
    bool history_ok = asst && call && (assistant_first ? asst < call : call < asst);
    bool session_ok = assistant_first
                          ? strncmp(g_test.session_item_order, "AT", 2) == 0
                          : strncmp(g_test.session_item_order, "TA", 2) == 0;
    bool grouping_ok = g_test.session_message_group_count >= 2 &&
                       g_test.session_message_groups[0] == g_test.session_message_groups[1];
    bool ok = history_ok && session_ok && grouping_ok;
    pthread_mutex_unlock(&g_test.mu);
    PicoHost_Shutdown(&app);
    return ok ? 0 : Fail(name, "result item order was not preserved in history");
}

static int TestMultipleAssistantItemsShareMessageGroup(void)
{
    const char *name = "multiple assistant items share one live message group";
    ResetTest(TEST_MULTI_ASSISTANT_ITEMS, 0);
    PicoHost app;
    InitApp(&app);
    PicoAgent_StartTurn(&app, TestAgent(&app), "start");
    if (!WaitForIdle(&app))
    {
        PicoHost_Shutdown(&app);
        return Fail(name, "multi-item turn did not finish");
    }
    PicoAgent *agent = TestAgent(&app);
    bool live_ok = agent->message_count >= 1 &&
                   agent->messages[agent->message_count - 1].role == PICO_ROLE_ASSISTANT &&
                   agent->messages[agent->message_count - 1].source &&
                   strcmp(agent->messages[agent->message_count - 1].source, "helloworld") == 0;
    pthread_mutex_lock(&g_test.mu);
    bool persisted_ok = strcmp(g_test.session_item_order, "AA") == 0 &&
                        g_test.session_message_group_count == 2 &&
                        g_test.session_message_groups[0] == g_test.session_message_groups[1];
    pthread_mutex_unlock(&g_test.mu);
    PicoHost_Shutdown(&app);
    return live_ok && persisted_ok
               ? 0
               : Fail(name, "live grouping and persisted message_group values diverged");
}

static int TestInterleavedTraceItemsKeepCanonicalOrder(void)
{
    const char *name = "interleaved assistant and tool traces keep canonical order";
    ResetTest(TEST_INTERLEAVED_TRACE_ITEMS, 1);
    PicoHost app;
    InitApp(&app);
    PicoAgent_StartTurn(&app, TestAgent(&app), "start");
    if (!WaitForIdle(&app))
    {
        PicoHost_Shutdown(&app);
        return Fail(name, "interleaved trace turn did not finish");
    }
    PicoAgent *agent = TestAgent(&app);
    PicoMessage *message = agent->message_count > 0
                               ? &agent->messages[agent->message_count - 1] : NULL;
    bool live_ok = message && message->role == PICO_ROLE_ASSISTANT &&
                   message->trace_count == 3 &&
                   !message->trace[0].is_tool && message->trace[0].text &&
                   strcmp(message->trace[0].text, "think-before") == 0 &&
                   message->trace[1].is_tool && message->trace[1].tool_call_id &&
                   strcmp(message->trace[1].tool_call_id, "call-order") == 0 &&
                   !message->trace[2].is_tool && message->trace[2].text &&
                   strcmp(message->trace[2].text, "think-after") == 0;
    pthread_mutex_lock(&g_test.mu);
    bool persisted_ok = strncmp(g_test.session_item_order, "ATA", 3) == 0 &&
                        g_test.session_message_group_count >= 3 &&
                        g_test.session_message_groups[0] == g_test.session_message_groups[1] &&
                        g_test.session_message_groups[1] == g_test.session_message_groups[2];
    pthread_mutex_unlock(&g_test.mu);
    PicoHost_Shutdown(&app);
    return live_ok && persisted_ok
               ? 0
               : Fail(name, "live trace order diverged from persisted item order");
}

static int TestMalformedCanonicalResult(void)
{
    const char *name = "malformed canonical result";
    ResetTest(TEST_MALFORMED_RESULT, 0);
    PicoHost app;
    InitApp(&app);
    PicoAgent_StartTurn(&app, TestAgent(&app), "start");
    for (int i = 0; i < 3000 && TestAgent(&app)->state != PICO_AGENT_ERROR; i++)
    {
        PicoAgent_Pump(&app, TestAgent(&app));
        SleepOneMs();
    }
    bool ok = TestAgent(&app)->state == PICO_AGENT_ERROR &&
              TestAgent(&app)->error &&
              strstr(TestAgent(&app)->error, "malformed result item array");
    PicoHost_Shutdown(&app);
    return ok ? 0 : Fail(name, "malformed public result did not fail safely");
}

static int TestMediaPersistenceFailureIsAtomic(void)
{
    const char *name = "media persistence failure is atomic";
    ResetTest(TEST_MEDIA_PERSIST_FAIL, 0);
    PicoHost app;
    InitApp(&app);
    PicoAgent_StartTurn(&app, TestAgent(&app), "start");
    for (int i = 0; i < 3000 && TestAgent(&app)->state != PICO_AGENT_ERROR; i++)
    {
        PicoAgent_Pump(&app, TestAgent(&app));
        SleepOneMs();
    }
    pthread_mutex_lock(&g_test.mu);
    bool no_result_events = g_test.session_item_order[0] == '\0';
    pthread_mutex_unlock(&g_test.mu);
    bool ok = no_result_events && TestAgent(&app)->state == PICO_AGENT_ERROR &&
              TestAgent(&app)->error &&
              strstr(TestAgent(&app)->error, "failed to persist provider media");
    PicoHost_Shutdown(&app);
    return ok ? 0 : Fail(name, "part of a failed media result was logged");
}

static int TestNonVisionMediaRejected(void)
{
    const char *name = "non-vision media rejection";
    ResetTest(TEST_SINGLE, 0);
    PicoHost app;
    InitApp(&app);
    PicoAgent_StartTurnParts(&app, TestAgent(&app), "see",
                             "[{\"type\":\"text\",\"text\":\"see\"},{\"type\":\"image\",\"path\":\"/tmp/pic.png\"}]");
    bool ok = TestAgent(&app)->state == PICO_AGENT_ERROR &&
              TestAgent(&app)->error &&
              strcmp(TestAgent(&app)->error, "model does not accept images") == 0;
    PicoHost_Shutdown(&app);
    return ok ? 0 : Fail(name, "media history was not rejected with the capability error");
}

static int TestCanonicalContinuationState(void)
{
    const char *name = "canonical continuation state";
    ResetTest(TEST_SIGNATURE_CONTINUATION, 1);
    PicoHost app;
    InitApp(&app);
    snprintf(g_test.issue_tool_name, sizeof(g_test.issue_tool_name), "missing_tool");
    PicoAgent_StartTurn(&app, TestAgent(&app), "start");
    if (!WaitForIdle(&app))
    {
        PicoHost_Shutdown(&app);
        return Fail(name, "signature continuation turn did not finish");
    }
    pthread_mutex_lock(&g_test.mu);
    bool signature_ok = g_test.last_input && strstr(g_test.last_input, "rs-only") &&
                        strstr(g_test.last_input, "thinking_signature");
    pthread_mutex_unlock(&g_test.mu);
    PicoHost_Shutdown(&app);
    return signature_ok ? 0 : Fail(name, "signature-only assistant state was not sent on the follow-up");
}

static int TestCancelledThinkingPersistence(void)
{
    const char *name = "cancelled thinking persistence";
    ResetTest(TEST_PROVIDER_THINK_BLOCK, 0);
    PicoHost app;
    InitApp(&app);
    PicoAgent_StartTurn(&app, TestAgent(&app), "start");
    if (!WaitForBlock(&app))
    {
        PicoHost_Shutdown(&app);
        return Fail(name, "thinking provider did not start");
    }
    int live_ms = PicoAgent_LiveActionMs(TestAgent(&app));
    PicoAgent_Cancel(TestAgent(&app));
    if (!WaitForIdle(&app))
    {
        PicoHost_Shutdown(&app);
        return Fail(name, "cancelled thinking provider did not return idle");
    }
    int idle_ms = PicoAgent_LiveActionMs(TestAgent(&app));
    if (live_ms <= 0)
    {
        PicoHost_Shutdown(&app);
        return Fail(name, "live thinking had no elapsed timer");
    }
    if (idle_ms != 0)
    {
        PicoHost_Shutdown(&app);
        return Fail(name, "live action timer did not clear after cancel");
    }
    pthread_mutex_lock(&g_test.mu);
    bool logged = strcmp(g_test.logged_thinking, "partial-think") == 0;
    g_test.mode = TEST_SINGLE;
    pthread_mutex_unlock(&g_test.mu);
    PicoAgent_StartTurn(&app, TestAgent(&app), "continue");
    if (!WaitForIdle(&app))
    {
        PicoHost_Shutdown(&app);
        return Fail(name, "follow-up after cancellation did not finish");
    }
    pthread_mutex_lock(&g_test.mu);
    bool history = g_test.last_input && strstr(g_test.last_input, "\"thinking\":\"partial-think\"");
    pthread_mutex_unlock(&g_test.mu);
    PicoHost_Shutdown(&app);
    return logged && history ? 0 : Fail(name, "cancelled thinking was not logged and replayed");
}

static int TestThinkSummaryCoalesce(void)
{
    const char *name = "think summaries coalesce until a tool";
    ResetTest(TEST_SINGLE, 1);
    g_test.emit_think_summaries = true;
    PicoHost app;
    InitApp(&app);
    TestAddTool(&app, "echo_test", "echo", "{}", EchoTool, NULL);
    snprintf(g_test.issue_tool_name, sizeof(g_test.issue_tool_name), "echo_test");
    PicoAgent_StartTurn(&app, TestAgent(&app), "start");
    if (!WaitForIdle(&app))
    {
        PicoHost_Shutdown(&app);
        return Fail(name, "turn did not finish");
    }
    PicoMessage *msg = NULL;
    for (int i = TestAgent(&app)->message_count - 1; i >= 0; i--)
    {
        if (TestAgent(&app)->messages[i].role == PICO_ROLE_ASSISTANT)
        {
            msg = &TestAgent(&app)->messages[i];
            break;
        }
    }
    bool ok = msg && msg->trace_count == 3 && !msg->trace[0].is_tool && msg->trace[0].think_steps == 2 &&
              msg->trace[0].think_part_count == 2 && msg->trace[0].think_parts &&
              msg->trace[0].think_parts[0] && strcmp(msg->trace[0].think_parts[0], "**first**") == 0 &&
              msg->trace[0].think_parts[1] && strcmp(msg->trace[0].think_parts[1], "**second**") == 0 &&
              msg->trace[0].text && strcmp(msg->trace[0].text, "**second**") == 0 &&
              (!msg->trace[0].text || !strstr(msg->trace[0].text, "2x ")) && msg->trace[1].is_tool &&
              !msg->trace[2].is_tool && msg->trace[2].think_steps == 1 && msg->trace[2].text &&
              strcmp(msg->trace[2].text, "**third**") == 0 && g_test.logged_thinking_parts;
    PicoHost_Shutdown(&app);
    return ok ? 0 : Fail(name, "consecutive summaries did not keep every step, or tool did not reset");
}

static int TestRestoredThinkKeepsUnknownDuration(void)
{
    const char *name = "restored thinking does not invent a duration";
    ResetTest(TEST_SINGLE, 0);
    PicoHost app;
    InitApp(&app);
    PicoAgent *agent = TestAgent(&app);
    PicoAgent_AddMessage(&app, agent, PICO_ROLE_ASSISTANT, "answer");
    PicoAgent_AppendThink(&app, agent, "historic-cot", 0);
    PicoAgent_AddToolCallWithId(&app, agent, "c1", "echo_test", "{}");
    PicoAgent_AppendThink(&app, agent, "later-cot", 0);
    PicoMessage *msg = agent->message_count > 0 ? &agent->messages[agent->message_count - 1] : NULL;
    bool unknown = msg && msg->trace_count == 3 && !msg->trace[0].is_tool && msg->trace[0].think_ms == 0 &&
                   msg->trace[1].is_tool && !msg->trace[2].is_tool && msg->trace[2].think_ms == 0;
    PicoAgent_AddMessage(&app, agent, PICO_ROLE_ASSISTANT, "timed");
    PicoAgent_AppendThink(&app, agent, "stored-cot", 12500);
    PicoAgent_AddToolCallWithId(&app, agent, "c2", "echo_test", "{}");
    PicoMessage *timed = &agent->messages[agent->message_count - 1];
    bool kept = timed->trace_count >= 1 && !timed->trace[0].is_tool && timed->trace[0].think_ms == 12500;
    PicoHost_Shutdown(&app);
    if (!unknown)
    {
        return Fail(name, "replay-style restore invented a duration after a following tool");
    }
    return kept ? 0 : Fail(name, "stored thinking_ms was not kept across a following tool");
}

static bool WaitForUi(PicoHost *app, const char *box_name, PicoUiPost *out)
{
    int i;
    for (i = 0; i < 3000; i++)
    {
        PicoAgent_Pump(app, TestAgent(app));
        if (pico_ui_latest(app, box_name, out))
        {
            return true;
        }
        SleepOneMs();
    }
    return false;
}

static void UseUiPostTool(PicoHost *app)
{
    TestAddTool(app, "ui_post", "test", "{}", UiPostTool, NULL);
    snprintf(g_test.issue_tool_name, sizeof(g_test.issue_tool_name), "ui_post");
}

static int TestUiPostAppendReplace(void)
{
    const char *name = "ui mailbox append and replace";
    PicoHost app;
    PicoUiPost stream;
    PicoUiPost other;
    PicoUiPost after_clear;
    bool ok;

    ResetTest(TEST_UI_POST, 1);
    InitApp(&app);
    UseUiPostTool(&app);
    PicoAgent_StartTurn(&app, TestAgent(&app), "start");
    if (!WaitForIdle(&app))
    {
        PicoHost_Shutdown(&app);
        return Fail(name, "agent did not return idle");
    }
    if (!pico_ui_latest(&app, "stream", &stream) || !pico_ui_latest(&app, "other", &other))
    {
        PicoHost_Shutdown(&app);
        return Fail(name, "published snapshots were missing");
    }
    ok = stream.status && strlen(stream.status) == PICO_UI_POST_STATUS_MAX &&
         stream.status[0] == 's' && stream.status[PICO_UI_POST_STATUS_MAX - 1] == 's' &&
         stream.text && strcmp(stream.text, "abcd") == 0 &&
         other.text && strcmp(other.text, "zz") == 0 &&
         stream.agent_id == pico_agent_active(&app) && stream.generation != 0;
    pico_ui_post(NULL, "stream", PICO_UI_POST_TEXT, "nope", 4);
    pico_ui_latest(&app, "stream", &stream);
    ok = ok && stream.text && strcmp(stream.text, "abcd") == 0;
    pico_ui_clear(&app, "stream");
    ok = ok && !pico_ui_latest(&app, "stream", &after_clear) &&
         pico_ui_latest(&app, "other", &other);
    PicoHost_Shutdown(&app);
    return ok ? 0 : Fail(name, "status/text snapshot contract failed");
}

static int TestUiPostCap(void)
{
    const char *name = "ui mailbox text keeps prefix at cap";
    PicoHost app;
    PicoUiPost post;
    bool ok;

    ResetTest(TEST_UI_POST_CAP, 1);
    InitApp(&app);
    UseUiPostTool(&app);
    PicoAgent_StartTurn(&app, TestAgent(&app), "start");
    if (!WaitForIdle(&app))
    {
        PicoHost_Shutdown(&app);
        return Fail(name, "agent did not return idle");
    }
    if (!pico_ui_latest(&app, "cap", &post) || !post.text)
    {
        PicoHost_Shutdown(&app);
        return Fail(name, "capped snapshot was missing");
    }
    ok = strlen(post.text) == PICO_UI_POST_TEXT_MAX &&
         post.text[0] == 'a' && post.text[PICO_UI_POST_TEXT_MAX - 1] == 'a';
    PicoHost_Shutdown(&app);
    return ok ? 0 : Fail(name, "overflow did not keep the prefix");
}

static int TestUiPostForceCancel(void)
{
    const char *name = "ui mailbox drops abandoned generation posts";
    PicoHost app;
    PicoUiPost post;
    bool reaped = false;
    int i;

    ResetTest(TEST_UI_POST_BLOCK, 1);
    InitApp(&app);
    UseUiPostTool(&app);
    PicoAgent_StartTurn(&app, TestAgent(&app), "start");
    if (!WaitForBlock(&app))
    {
        PicoHost_Shutdown(&app);
        return Fail(name, "blocking ui post tool did not start");
    }
    if (!WaitForUi(&app, "stream", &post) || !post.text || strcmp(post.text, "live") != 0)
    {
        PicoHost_Shutdown(&app);
        return Fail(name, "live snapshot was not published");
    }
    PicoAgent_ForceCancel(&app, TestAgent(&app));
    pthread_mutex_lock(&g_test.mu);
    g_test.block_release = true;
    pthread_cond_broadcast(&g_test.cv);
    pthread_mutex_unlock(&g_test.mu);
    for (i = 0; i < 3000; i++)
    {
        PicoAgent_Pump(&app, TestAgent(&app));
        if (!PicoAgent_BlocksReload(TestAgent(&app)))
        {
            reaped = true;
            break;
        }
        SleepOneMs();
    }
    if (!reaped || !pico_ui_latest(&app, "stream", &post) || !post.text ||
        strcmp(post.text, "live") != 0)
    {
        PicoHost_Shutdown(&app);
        return Fail(name, "abandoned post replaced the live snapshot");
    }
    PicoHost_Shutdown(&app);
    return 0;
}

static bool WaitForFlagNoPump(bool *flag)
{
    int i;
    for (i = 0; i < 3000; i++)
    {
        pthread_mutex_lock(&g_test.mu);
        bool done = *flag;
        pthread_mutex_unlock(&g_test.mu);
        if (done)
        {
            return true;
        }
        SleepOneMs();
    }
    return false;
}

static int TestUiPostUnpublishedIdentity(void)
{
    const char *name = "ui mailbox latest stays on last publish";
    PicoHost app;
    PicoUiPost post;
    uint64_t first_gen;
    PicoAgentId first_agent;
    bool ok;

    ResetTest(TEST_UI_POST_BLOCK, 1);
    InitApp(&app);
    UseUiPostTool(&app);
    PicoAgent_StartTurn(&app, TestAgent(&app), "start");
    if (!WaitForBlock(&app) || !WaitForUi(&app, "stream", &post) ||
        !post.text || strcmp(post.text, "live") != 0)
    {
        PicoHost_Shutdown(&app);
        return Fail(name, "first snapshot was not published");
    }
    first_gen = post.generation;
    first_agent = post.agent_id;

    pthread_mutex_lock(&g_test.mu);
    g_test.block_release = true;
    pthread_cond_broadcast(&g_test.cv);
    pthread_mutex_unlock(&g_test.mu);
    if (!WaitForFlagNoPump(&g_test.block_done))
    {
        PicoHost_Shutdown(&app);
        return Fail(name, "second post did not finish");
    }
    if (!pico_ui_latest(&app, "stream", &post) || !post.text || strcmp(post.text, "live") != 0 ||
        post.generation != first_gen || post.agent_id != first_agent)
    {
        PicoHost_Shutdown(&app);
        return Fail(name, "unpublished append leaked into latest");
    }
    PicoAgent_Pump(&app, TestAgent(&app));
    ok = pico_ui_latest(&app, "stream", &post) && post.text && strcmp(post.text, "livedead") == 0 &&
         post.generation == first_gen && post.agent_id == first_agent;
    PicoHost_Shutdown(&app);
    return ok ? 0 : Fail(name, "pump did not publish the staged append");
}

static int TestUiPostLimit(void)
{
    const char *name = "ui mailbox table rejects a new name when full";
    PicoHost app;
    PicoUiPost post;
    int i;
    bool ok = true;

    ResetTest(TEST_UI_POST_LIMIT, 1);
    InitApp(&app);
    UseUiPostTool(&app);
    PicoAgent_StartTurn(&app, TestAgent(&app), "start");
    if (!WaitForIdle(&app))
    {
        PicoHost_Shutdown(&app);
        return Fail(name, "agent did not return idle");
    }
    for (i = 0; i < PICO_MAX_UI_POSTS; i++)
    {
        char box[8];
        snprintf(box, sizeof(box), "b%d", i);
        if (!pico_ui_latest(&app, box, &post))
        {
            ok = false;
            break;
        }
    }
    {
        char overflow[8];
        snprintf(overflow, sizeof(overflow), "b%d", PICO_MAX_UI_POSTS);
        ok = ok && !pico_ui_latest(&app, overflow, &post);
    }
    PicoHost_Shutdown(&app);
    return ok ? 0 : Fail(name, "mailbox limit was not enforced");
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
    failed |= TestReloadQuiescence();
    failed |= TestDeferredWorkspaceChange();
    failed |= TestInvalidRestrictedPolicyPreservesSubmit();
    failed |= TestImageOnlySubmitStartsTurn();
    failed |= TestNonVisionSubmitPreservesDraft();
    failed |= TestInvalidPayload();
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
    failed |= TestUnmappedContextFails();
    failed |= TestLlmExtraInstructions();
    failed |= TestBuildInstructionsExtraSpan();
    failed |= TestBuildInstructionsMatchTurn();
    failed |= TestAgentPolicyPrecedesLlmHooks();
    failed |= TestLlmExcludeTool();
    failed |= TestHiddenToolCallIsControlled();
    failed |= TestOfferedCatalogSnapshot();
    failed |= TestMalformedToolCalls();
    failed |= TestTurnEnd();
    failed |= TestCancelNotification();
    failed |= TestErrorNotification();
    failed |= TestAskNotification();
    failed |= TestAskForceCancelNotification();
    failed |= TestAskReplaceNotification();
    failed |= TestCancelledProviderUsage();
    failed |= TestSessionUsageAccumulation();
    failed |= TestUsageNormalizationAndSaturation();
    failed |= TestAfterCompact();
    failed |= TestToolTraceError();
    failed |= TestQueuedToolCallProgress();
    failed |= TestTodoAgentIsolation();
    failed |= TestTodoApplyRenamesSession();
    failed |= TestAskUserHiddenOmitsGuidance();
    failed |= TestAskUserRegistrationReload();
    failed |= TestAskUserToolSuccess();
    failed |= TestAskUserToolCancellation();
    failed |= TestResumedToolCallArgs();
    failed |= TestShToolCallDescriptionArgs();
    failed |= TestBackgroundToolCallDescriptionArgs();
    failed |= TestBackgroundToolCallCommandExtraction();
    failed |= TestShToolCallCommandExtraction();
    failed |= TestToolActivityDescriptionScope();
    failed |= TestToolCallListArgs();
    failed |= TestManagerProfileRegistry();
    failed |= TestManagerConcurrencyAndIsolation();
    failed |= TestSubmitTargetsExplicitAgentWithoutChangingSelection();
    failed |= TestSubmitIsCompleteExplicitTurn();
    failed |= TestSubmitReportsResultCodes();
    failed |= TestLoginRoutesToSnapshottedAgent();
    failed |= TestResumeMissingAgentReturnsNotFound();
    failed |= TestResumeLeavesUnselectedAgentSelection();
    failed |= TestWorkerContextCapturesRegistrationGeneration();
    failed |= TestSubmitHookCannotRetarget();
    failed |= TestSubagentProfileResolution();
    failed |= TestSubagentProfileDiscovery();
    failed |= TestNamedSubagentDelegation();
    failed |= TestSubagentParentGuidance();
    failed |= TestSubagentParentCancellation();
    failed |= TestSubagentCancellationBeforeEnqueue();
    failed |= TestSubagentDirectChildCancellation();
    failed |= TestSubagentLiveInspect();
    failed |= TestSubagentInspectRetention();
    failed |= TestSubagentInspectThenContinue();
    failed |= TestSubagentSessionContinuation();
    failed |= TestSubagentContinuationEmptyAnswer();
    failed |= TestSubagentResumeFailures();
    failed |= TestSubagentChildAsk();
    failed |= TestSubagentDelegationCaps();
    failed |= TestCanonicalContinuationState();
    failed |= TestResultItemOrder(TEST_ITEM_ORDER, true, "assistant then tool_call history order");
    failed |= TestResultItemOrder(TEST_ITEM_ORDER_REVERSE, false, "tool_call then assistant history order");
    failed |= TestMultipleAssistantItemsShareMessageGroup();
    failed |= TestInterleavedTraceItemsKeepCanonicalOrder();
    failed |= TestMalformedCanonicalResult();
    failed |= TestMediaPersistenceFailureIsAtomic();
    failed |= TestNonVisionMediaRejected();
    failed |= TestCancelledThinkingPersistence();
    failed |= TestThinkSummaryCoalesce();
    failed |= TestRestoredThinkKeepsUnknownDuration();
    failed |= TestUiPostAppendReplace();
    failed |= TestUiPostCap();
    failed |= TestUiPostForceCancel();
    failed |= TestUiPostUnpublishedIdentity();
    failed |= TestUiPostLimit();
    /* Retained shutdown permanently retires Pico in this process, so run last. */
    failed |= TestShutdownTimeout();
    return failed ? 1 : 0;
}
