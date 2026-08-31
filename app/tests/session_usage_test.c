#define _POSIX_C_SOURCE 200809L

#include "agent.h"
#include "json.h"
#include "path.h"
#include "session.h"
#include "settings.h"
#include "usage.h"
#include "host_internal.h"

#include <stdbool.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#include <utime.h>

static char g_config_dir[4096];
static int g_restored_value;
static int g_reset_hooks;
static char g_status_warning[512];
static int g_reserve_calls;
static char g_last_think[2048];
static char g_last_sig[4096];
static char g_last_item_id[128];
static char g_last_user_parts[4096];
static char g_last_assistant_parts[4096];
static const char *g_session_fail_stage;
static int g_title_ready_fd = -1;
static int g_title_continue_fd = -1;
static int g_catalog_ready_fd = -1;
static int g_catalog_continue_fd = -1;
static int g_lock_attempt_fd = -1;
static int g_scan_session_calls;

static const PicoCatalogWorkspace *FindCatalogPath(PicoCatalogWorkspace *list, int n,
                                                    const char *path);

static bool TransferByte(int fd, bool write_byte)
{
    char byte = 'x';
    ssize_t result;
    do
    {
        result = write_byte ? write(fd, &byte, 1) : read(fd, &byte, 1);
    } while (result < 0 && errno == EINTR);
    return result == 1;
}

bool PicoSession_TestHook(const char *stage)
{
    if (stage && strcmp(stage, "scan_session_file") == 0)
    {
        g_scan_session_calls++;
    }
    if (stage && strcmp(stage, "title_after_copy") == 0 &&
        g_title_ready_fd >= 0 && g_title_continue_fd >= 0)
    {
        if (!TransferByte(g_title_ready_fd, true) || !TransferByte(g_title_continue_fd, false))
        {
            return true;
        }
    }
    if (stage && strcmp(stage, "catalog_before_upsert") == 0 &&
        g_catalog_ready_fd >= 0 && g_catalog_continue_fd >= 0)
    {
        int ready_fd = g_catalog_ready_fd;
        int continue_fd = g_catalog_continue_fd;
        g_catalog_ready_fd = -1;
        g_catalog_continue_fd = -1;
        if (!TransferByte(ready_fd, true) || !TransferByte(continue_fd, false))
        {
            return true;
        }
    }
    if (stage && strcmp(stage, "lock_before_wait") == 0 && g_lock_attempt_fd >= 0)
    {
        int fd = g_lock_attempt_fd;
        g_lock_attempt_fd = -1;
        if (!TransferByte(fd, true))
        {
            return true;
        }
    }
    return stage && g_session_fail_stage && strcmp(stage, g_session_fail_stage) == 0;
}

static int Fail(const char *message)
{
    fprintf(stderr, "session usage: %s\n", message);
    return 1;
}

void PicoMessages_Free(PicoMessage *messages, int count);

bool Pico_ConfigDir(char *out, size_t cap)
{
    return PicoPath_Format(out, cap, "%s", g_config_dir);
}

bool PicoWorkspace_ReserveSession(PicoWorkspace *workspace, PicoAgentId owner, const char *path)
{
    (void)workspace; (void)owner; (void)path;
    g_reserve_calls++;
    return true;
}

void PicoWorkspace_ReleaseSessions(PicoWorkspace *workspace, PicoAgentId owner)
{
    (void)workspace; (void)owner;
}

bool PicoWorkspace_SessionReserved(const PicoWorkspace *workspace, const char *path,
                                   PicoAgentId except_owner)
{
    (void)workspace; (void)path; (void)except_owner;
    return false;
}

void pico_status_warn(PicoHost *app, const char *msg)
{
    (void)app;
    snprintf(g_status_warning, sizeof(g_status_warning), "%s", msg ? msg : "");
}

void Pico_MkdirP(const char *path)
{
    char buf[4096];
    snprintf(buf, sizeof(buf), "%s", path ? path : "");
    for (char *p = buf + 1; *p; p++)
    {
        if (*p == '/')
        {
            *p = '\0';
            mkdir(buf, 0700);
            *p = '/';
        }
    }
    mkdir(buf, 0700);
}

void Pico_RandomHex(char *out, size_t cap)
{
    static unsigned value;
    snprintf(out, cap, "%08x", ++value);
}

void Pico_IsoTime(char *out, size_t cap, bool filename)
{
    snprintf(out, cap, "%s", filename ? "20260101_000000" : "2026-01-01T00:00:00Z");
}

const char *PicoAgent_CacheKey(const PicoAgent *agent)
{
    (void)agent;
    return "cache-key";
}

void PicoAgent_SetCacheKey(PicoAgent *agent, const char *key)
{
    (void)agent;
    (void)key;
}

void PicoAgent_RotateCacheKey(PicoAgent *agent)
{
    (void)agent;
}

bool PicoAgent_IsBusy(const PicoAgent *agent)
{
    (void)agent;
    return false;
}

void PicoAgent_DismissError(PicoAgent *agent)
{
    if (agent) { free(agent->error); agent->error = NULL; }
}

void pico_run_hooks(PicoHost *app, PicoHook hook, PicoAgentId agent_id)
{
    if (!app)
    {
        return;
    }
    PicoHookEvent event = {.hook = hook, .agent_id = agent_id};
    for (int i = 0; i < app->hook_count; i++)
    {
        if (app->hooks[i].hook != hook)
        {
            continue;
        }
        if (app->hooks[i].host_fn)
        {
            app->hooks[i].host_fn(app, &event, app->hooks[i].state);
        }
        if (app->hooks[i].workspace_fn && app->hooks[i].workspace)
        {
            app->hooks[i].workspace_fn(app->hooks[i].workspace, &event, app->hooks[i].state);
        }
    }
}

void PicoAgent_ClearInput(PicoAgent *agent)
{
    (void)agent;
}

void PicoAgent_PushHistoryUser(PicoAgent *agent, const char *text)
{
    (void)agent;
    (void)text;
}

void PicoAgent_PushHistoryUserParts(PicoAgent *agent, const char *text, const char *parts_json)
{
    (void)agent;
    (void)text;
    snprintf(g_last_user_parts, sizeof(g_last_user_parts), "%s", parts_json ? parts_json : "");
}

void PicoAgent_PushHistoryAssistant(PicoAgent *agent, const char *text, const char *thinking,
                                    const char *signature)
{
    (void)agent;
    snprintf(g_last_think, sizeof(g_last_think), "%s", thinking ? thinking : "");
    snprintf(g_last_sig, sizeof(g_last_sig), "%s", signature ? signature : "");
    (void)text;
}

void PicoAgent_PushHistoryAssistantParts(PicoAgent *agent, const char *text, const char *thinking,
                                         const char *signature, const char *parts_json)
{
    snprintf(g_last_assistant_parts, sizeof(g_last_assistant_parts), "%s", parts_json ? parts_json : "");
    PicoAgent_PushHistoryAssistant(agent, text, thinking, signature);
}

void PicoAgent_PushHistoryFunctionCall(PicoAgent *agent, const char *call_id, const char *name, const char *args,
                                       const char *item_id)
{
    (void)agent;
    (void)call_id;
    (void)name;
    (void)args;
    snprintf(g_last_item_id, sizeof(g_last_item_id), "%s", item_id ? item_id : "");
}

static bool StubGrowMessages(PicoAgent *agent)
{
    if (agent->message_count < agent->message_capacity)
    {
        return true;
    }
    int capacity = agent->message_capacity == 0 ? 8 : agent->message_capacity * 2;
    PicoMessage *next = (PicoMessage *)realloc(agent->messages, (size_t)capacity * sizeof(PicoMessage));
    if (!next)
    {
        return false;
    }
    agent->messages = next;
    agent->message_capacity = capacity;
    return true;
}

static PicoTraceLine *StubPushTrace(PicoMessage *message, bool is_tool)
{
    PicoTraceLine *next =
        (PicoTraceLine *)realloc(message->trace, (size_t)(message->trace_count + 1) * sizeof(PicoTraceLine));
    if (!next)
    {
        return NULL;
    }
    message->trace = next;
    PicoTraceLine *line = &message->trace[message->trace_count++];
    memset(line, 0, sizeof(*line));
    line->is_tool = is_tool;
    return line;
}

void PicoAgent_AppendThink(PicoHost *app, PicoAgent *agent, const char *text, int think_ms)
{
    PicoMessage *message;
    PicoTraceLine *line;
    size_t old;
    size_t n;
    char *next;

    (void)app;
    if (!agent || !text || !text[0] || agent->message_count <= 0 ||
        agent->messages[agent->message_count - 1].role != PICO_ROLE_ASSISTANT)
    {
        return;
    }
    message = &agent->messages[agent->message_count - 1];
    if (message->trace_count > 0 && !message->trace[message->trace_count - 1].is_tool &&
        message->trace[message->trace_count - 1].think_steps == 0)
    {
        line = &message->trace[message->trace_count - 1];
    }
    else
    {
        line = StubPushTrace(message, false);
    }
    if (!line)
    {
        return;
    }
    old = line->text ? strlen(line->text) : 0;
    n = strlen(text);
    next = (char *)realloc(line->text, old + n + 1);
    if (!next)
    {
        return;
    }
    memcpy(next + old, text, n + 1);
    line->text = next;
    line->think_t0 = 0.0;
    if (think_ms > 0 && line->think_ms == 0)
    {
        line->think_ms = think_ms;
    }
}

void PicoAgent_AppendThinkSummary(PicoHost *app, PicoAgent *agent, const char *text,
                                  int step, int think_ms)
{
    (void)app;
    if (!agent || !text || !text[0] || step <= 0 || agent->message_count <= 0 ||
        agent->messages[agent->message_count - 1].role != PICO_ROLE_ASSISTANT)
    {
        return;
    }
    PicoMessage *message = &agent->messages[agent->message_count - 1];
    PicoTraceLine *line = NULL;
    if (message->trace_count > 0 && !message->trace[message->trace_count - 1].is_tool &&
        message->trace[message->trace_count - 1].think_steps > 0)
    {
        line = &message->trace[message->trace_count - 1];
    }
    else
    {
        line = StubPushTrace(message, false);
    }
    if (!line)
    {
        return;
    }
    if (step > line->think_part_count)
    {
        char **next = (char **)realloc(line->think_parts, (size_t)step * sizeof(char *));
        if (!next)
        {
            return;
        }
        for (int i = line->think_part_count; i < step; i++)
        {
            next[i] = NULL;
        }
        line->think_parts = next;
        line->think_part_count = step;
    }
    char *copy = strdup(text);
    if (!copy)
    {
        return;
    }
    free(line->think_parts[step - 1]);
    line->think_parts[step - 1] = copy;
    line->think_steps = line->think_part_count;
    char *latest = strdup(line->think_parts[line->think_part_count - 1]);
    if (latest)
    {
        free(line->text);
        line->text = latest;
    }
    line->think_t0 = 0.0;
    if (think_ms > 0 && line->think_ms == 0)
    {
        line->think_ms = think_ms;
    }
}

void PicoAgent_PushHistoryFunctionOutput(PicoAgent *agent, const char *call_id, const char *name,
                                         const char *output, bool is_error)
{
    (void)agent;
    (void)call_id;
    (void)name;
    (void)output;
    (void)is_error;
}

void PicoHost_AddMessage(PicoHost *app, PicoAgentId agent_id, PicoRole role, const char *text)
{
    (void)app;
    (void)agent_id;
    (void)role;
    (void)text;
}

void PicoHost_AppendAssistant(PicoHost *app, PicoAgentId agent_id, const char *text)
{
    (void)app;
    (void)agent_id;
    (void)text;
}

void PicoHost_AddToolCall(PicoHost *app, PicoAgentId agent_id, const char *name, const char *args_json)
{
    (void)app;
    (void)agent_id;
    (void)name;
    (void)args_json;
}

void PicoHost_SetLastToolOutput(PicoHost *app, PicoAgentId agent_id, const char *output, bool is_error)
{
    (void)app;
    (void)agent_id;
    (void)output;
    (void)is_error;
}

void PicoHost_ClearMessages(PicoHost *app, PicoAgentId agent_id)
{
    (void)app;
    (void)agent_id;
}

void PicoAgent_AddMessage(PicoHost *app, PicoAgent *agent, PicoRole role, const char *text)
{
    PicoMessage *message;

    (void)app;
    if (!agent || !StubGrowMessages(agent))
    {
        return;
    }
    message = &agent->messages[agent->message_count++];
    memset(message, 0, sizeof(*message));
    message->role = role;
    message->source = JsonDup(text ? text : "");
}

void PicoAgent_AppendAssistant(PicoHost *app, PicoAgent *agent, const char *text)
{
    PicoMessage *message;
    size_t old;
    size_t n;
    char *next;

    if (!agent)
    {
        return;
    }
    if (!text)
    {
        text = "";
    }
    if (agent->message_count <= 0 || agent->messages[agent->message_count - 1].role != PICO_ROLE_ASSISTANT)
    {
        PicoAgent_AddMessage(app, agent, PICO_ROLE_ASSISTANT, text);
        return;
    }
    if (!text[0])
    {
        return;
    }
    message = &agent->messages[agent->message_count - 1];
    old = message->source ? strlen(message->source) : 0;
    n = strlen(text);
    next = (char *)realloc(message->source, old + n + 1);
    if (!next)
    {
        return;
    }
    memcpy(next + old, text, n + 1);
    message->source = next;
}

void PicoAgent_AddToolCall(PicoHost *app, PicoAgent *agent, const char *name, const char *args_json)
{
    PicoAgent_AddToolCallWithId(app, agent, NULL, name, args_json);
}

void PicoAgent_AddToolCallWithId(PicoHost *app, PicoAgent *agent, const char *call_id,
                                const char *name, const char *args_json)
{
    PicoMessage *message;
    PicoTraceLine *line;

    if (!agent)
    {
        return;
    }
    if (agent->message_count <= 0 || agent->messages[agent->message_count - 1].role != PICO_ROLE_ASSISTANT)
    {
        PicoAgent_AddMessage(app, agent, PICO_ROLE_ASSISTANT, "");
    }
    message = &agent->messages[agent->message_count - 1];
    if (message->trace_count > 0 && !message->trace[message->trace_count - 1].is_tool)
    {
        PicoTraceLine_FreezeThink(&message->trace[message->trace_count - 1]);
    }
    line = StubPushTrace(message, true);
    if (!line)
    {
        return;
    }
    line->tool_name = JsonDup(name && name[0] ? name : "tool");
    line->tool_call_id = call_id && call_id[0] ? JsonDup(call_id) : NULL;
    line->tool_args = PicoAgent_FormatToolArgs(line->tool_name, args_json);
    line->tool_args_json = JsonDup(args_json ? args_json : "");
}

void PicoAgent_SetLastToolOutput(PicoAgent *agent, const char *output, bool is_error)
{
    PicoMessage *message;
    int t;

    if (!agent || agent->message_count <= 0)
    {
        return;
    }
    message = &agent->messages[agent->message_count - 1];
    for (t = message->trace_count - 1; t >= 0; t--)
    {
        if (message->trace[t].is_tool)
        {
            free(message->trace[t].tool_output);
            message->trace[t].tool_output = JsonDup(output ? output : "");
            message->trace[t].tool_error = is_error;
            return;
        }
    }
}

void PicoAgent_SetToolOutputByCallId(PicoAgent *agent, const char *call_id,
                                     const char *output, bool is_error)
{
    int i;

    if (!agent || !call_id || !call_id[0])
    {
        return;
    }
    for (i = agent->message_count - 1; i >= 0; i--)
    {
        PicoMessage *message = &agent->messages[i];
        int t;
        for (t = message->trace_count - 1; t >= 0; t--)
        {
            PicoTraceLine *line = &message->trace[t];
            if (line->is_tool && line->tool_call_id && strcmp(line->tool_call_id, call_id) == 0)
            {
                free(line->tool_output);
                line->tool_output = JsonDup(output ? output : "");
                line->tool_error = is_error;
                return;
            }
        }
    }
}

void PicoAgent_ClearMessages(PicoAgent *agent)
{
    if (!agent)
    {
        return;
    }
    PicoMessages_Free(agent->messages, agent->message_count);
    agent->messages = NULL;
    agent->message_count = 0;
    agent->message_capacity = 0;
}

void PicoTraceLine_Release(PicoTraceLine *line)
{
    int i;

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
        for (i = 0; i < line->think_part_count; i++)
        {
            free(line->think_parts[i]);
        }
        free(line->think_parts);
    }
    memset(line, 0, sizeof(*line));
}

void PicoTraceLine_FreezeThink(PicoTraceLine *line)
{
    double elapsed;
    int ms;

    if (!line || line->think_ms > 0 || line->think_t0 <= 0.0)
    {
        return;
    }
    elapsed = 0.001;
    ms = (int)(elapsed * 1000.0 + 0.5);
    if (ms < 1)
    {
        ms = 1;
    }
    line->think_ms = ms;
}

void PicoMessages_Free(PicoMessage *messages, int count)
{
    if (!messages)
    {
        return;
    }
    for (int i = 0; i < count; i++)
    {
        free(messages[i].source);
        for (int t = 0; t < messages[i].trace_count; t++)
        {
            PicoTraceLine_Release(&messages[i].trace[t]);
        }
        free(messages[i].trace);
    }
    free(messages);
}

void PicoMessages_PrepareDocs(PicoMessage *messages, int count)
{
    (void)messages;
    (void)count;
}

PicoModel *PicoSettings_ActiveModel(const PicoAgent *agent)
{
    if (!agent || !agent->workspace) return NULL;
    for (int i = 0; i < agent->workspace->model_count; i++)
    {
        if (strcmp(agent->workspace->models[i].id, agent->model) == 0) return &agent->workspace->models[i];
    }
    return NULL;
}

void PicoSettings_SyncAgent(PicoAgent *agent)
{
    if (!agent) return;
    PicoModel *model = PicoSettings_ActiveModel(agent);
    snprintf(agent->model_name, sizeof(agent->model_name), "%s",
             model && model->name[0] ? model->name : agent->model);
    agent->context_limit = model ? model->context_limit : 0;
    if (!agent->effort[0])
    {
        snprintf(agent->effort, sizeof(agent->effort), "%s",
                 model && model->default_effort[0] ? model->default_effort : "none");
    }
}

static void ResetHook(PicoHost *app, const PicoHookEvent *event, void *state)
{
    (void)state;
    (void)app;
    (void)event;
    g_reset_hooks++;
}

static void ReplayTool(PicoAgentContext *ctx, const char *args_json, PicoToolResult *out, void *state)
{
    (void)state;
    (void)ctx;
    (void)args_json;
    (void)out;
}

static bool ReplayApply(PicoWorkspace *workspace, PicoAgentId agent_id, const char *details_json, bool replay, void *state)
{
    (void)state;
    (void)workspace;
    (void)agent_id;
    if (!replay)
    {
        return false;
    }
    JsonDoc doc;
    if (JsonParse(&doc, details_json, strlen(details_json)) != 0)
    {
        return false;
    }
    int value = JsonObjInt(&doc, 0, "value", -1);
    JsonFree(&doc);
    if (value < 0 || value > 10)
    {
        return false;
    }
    g_restored_value = value;
    return true;
}

static void RegisterReplayTool(PicoHost *app)
{
    PicoWorkspace *ws = PicoHost_PrimaryWorkspace(app);
    if (ws)
    {
        ws->tools[0] = (PicoTool){.name = "state_test", .run = ReplayTool, .apply = ReplayApply};
        ws->tool_count = 1;
    }
}

static bool AppendRaw(const char *path, const char *line)
{
    FILE *f = fopen(path, "ab");
    if (!f)
    {
        return false;
    }
    fprintf(f, "%s\n", line);
    fclose(f);
    return true;
}

static int CountSubstr(const char *hay, const char *needle)
{
    int n = 0;
    size_t needle_len = needle ? strlen(needle) : 0;
    if (!hay || !needle || needle_len == 0)
    {
        return 0;
    }
    for (const char *p = hay; (p = strstr(p, needle)); p += needle_len)
    {
        n++;
    }
    return n;
}

static bool ListedTitleIs(PicoHost *app, const char *session_id, const char *title)
{
    PicoSessionInfo *listed = NULL;
    int listed_n = PicoSession_List(PicoHost_PrimaryWorkspace(app), &listed, true);
    bool match = false;
    for (int i = 0; i < listed_n; i++)
    {
        if (strcmp(listed[i].id, session_id) == 0)
        {
            match = title && strcmp(listed[i].title, title) == 0;
            break;
        }
    }
    free(listed);
    return match;
}

static int TestSessionTitle(void)
{
    PicoHost app;
    PicoAgent agent;
    memset(&app, 0, sizeof(app));
    memset(&agent, 0, sizeof(agent));
    agent.persistence = PICO_SESSION_DURABLE;
    snprintf(agent.model, sizeof(agent.model), "saved-model");
    PicoHost_SetPath(&app, "/workspace");
    PicoSession_LogUser(&app, &agent, "first user message should not win",
                        "first user message should not win", NULL);
    if (!agent.session_path[0])
    {
        return Fail("title test did not create a session file");
    }
    if (!ListedTitleIs(&app, agent.session_id, "first user message should not win"))
    {
        unlink(agent.session_path);
        return Fail("listing did not use the first user message before a title exists");
    }

    PicoAgent ephemeral;
    memset(&ephemeral, 0, sizeof(ephemeral));
    ephemeral.persistence = PICO_SESSION_EPHEMERAL;
    if (PicoSession_LogTitle(&app, &ephemeral, "Nope") != PICO_SESSION_WRITE_SKIPPED)
    {
        unlink(agent.session_path);
        return Fail("ephemeral title write was not skipped");
    }

    if (PicoSession_LogTitle(&app, &agent, "Add todo task field") != PICO_SESSION_WRITE_OK)
    {
        unlink(agent.session_path);
        return Fail("first title write failed");
    }
    PicoSessionHeader header;
    if (PicoSession_ReadHeader(agent.session_path, &header) != 0 ||
        strcmp(header.title, "Add todo task field") != 0)
    {
        unlink(agent.session_path);
        return Fail("header title was not stored");
    }
    size_t file_len = 0;
    char *file = Pico_ReadFile(agent.session_path, &file_len);
    bool first_ok = file && CountSubstr(file, "\"type\":\"title\"") == 1 &&
                    strstr(file, "\"title\":\"Add todo task field\"");
    free(file);
    if (!first_ok || !ListedTitleIs(&app, agent.session_id, "Add todo task field"))
    {
        unlink(agent.session_path);
        return Fail("listing did not use the header title after the first rename");
    }
    if (PicoSession_LogTitle(&app, &agent, "Add todo task field") != PICO_SESSION_WRITE_OK)
    {
        unlink(agent.session_path);
        return Fail("unchanged title was not accepted");
    }
    file = Pico_ReadFile(agent.session_path, &file_len);
    bool unchanged_ok = file && CountSubstr(file, "\"type\":\"title\"") == 1;
    free(file);
    if (!unchanged_ok)
    {
        unlink(agent.session_path);
        return Fail("unchanged title appended a duplicate event");
    }

    if (PicoSession_LogTitle(&app, &agent, "Rename again") != PICO_SESSION_WRITE_OK)
    {
        unlink(agent.session_path);
        return Fail("second title write failed");
    }
    if (PicoSession_ReadHeader(agent.session_path, &header) != 0 ||
        strcmp(header.title, "Rename again") != 0)
    {
        unlink(agent.session_path);
        return Fail("header title was not updated");
    }
    file = Pico_ReadFile(agent.session_path, &file_len);
    bool second_ok = file && CountSubstr(file, "\"type\":\"title\"") == 2 &&
                     strstr(file, "\"title\":\"Rename again\"");
    free(file);
    if (!second_ok || !ListedTitleIs(&app, agent.session_id, "Rename again"))
    {
        unlink(agent.session_path);
        return Fail("second rename did not update the header and append a title event");
    }

    PicoHost reader;
    PicoAgent reader_agent;
    memset(&reader, 0, sizeof(reader));
    memset(&reader_agent, 0, sizeof(reader_agent));
    reader_agent.persistence = PICO_SESSION_DURABLE;
    PicoHost_SetPath(&reader, "/workspace");
    PicoSession_Start(&reader, &reader_agent, PICO_SESSION_NEW, agent.session_path);
    PicoAgent_ClearMessages(&reader_agent);

    char *before = Pico_ReadFile(agent.session_path, &file_len);
    g_status_warning[0] = '\0';
    g_session_fail_stage = "title_before_rename";
    PicoSessionWriteResult failed = PicoSession_LogTitle(&app, &agent, "Should fail");
    g_session_fail_stage = NULL;
    char *after = Pico_ReadFile(agent.session_path, &file_len);
    bool intact = before && after && strcmp(before, after) == 0;
    free(before);
    free(after);
    bool failed_ok = failed == PICO_SESSION_WRITE_FAILED &&
                     agent.persistence == PICO_SESSION_FAILED &&
                     strstr(g_status_warning, "no longer resumable") && intact;
    unlink(agent.session_path);
    return failed_ok ? 0 : Fail("title rewrite failure did not preserve the original file");
}

static int TestTitleFailureStage(const char *stage, bool expect_original)
{
    PicoHost app;
    PicoAgent agent;
    memset(&app, 0, sizeof(app));
    memset(&agent, 0, sizeof(agent));
    agent.persistence = PICO_SESSION_DURABLE;
    snprintf(agent.model, sizeof(agent.model), "saved-model");
    PicoHost_SetPath(&app, "/workspace");
    PicoSession_LogUser(&app, &agent, "seed", "seed", NULL);
    size_t file_len = 0;
    char *before = Pico_ReadFile(agent.session_path, &file_len);
    g_session_fail_stage = stage;
    PicoSessionWriteResult result = PicoSession_LogTitle(&app, &agent, "Injected failure");
    g_session_fail_stage = NULL;
    char *after = Pico_ReadFile(agent.session_path, &file_len);
    bool content_ok = expect_original ? before && after && strcmp(before, after) == 0
                                      : after && strstr(after, "\"title\":\"Injected failure\"");
    bool ok = result == PICO_SESSION_WRITE_FAILED &&
              agent.persistence == PICO_SESSION_FAILED && content_ok;
    free(before);
    free(after);
    unlink(agent.session_path);
    return ok ? 0 : Fail("injected title failure had the wrong persistence or preservation result");
}

static int TestSessionTitleFailureStages(void)
{
    return TestTitleFailureStage("title_after_copy", true) |
           TestTitleFailureStage("title_fsync", true) |
           TestTitleFailureStage("title_dir_fsync", false);
}

static int TestSessionTitleUtf8(void)
{
    PicoHost app;
    PicoAgent agent;
    memset(&app, 0, sizeof(app));
    memset(&agent, 0, sizeof(agent));
    agent.persistence = PICO_SESSION_DURABLE;
    snprintf(agent.model, sizeof(agent.model), "saved-model");
    PicoHost_SetPath(&app, "/workspace");

    char title[PICO_SESSION_TITLE_MAX_BYTES + 1];
    for (int i = 0; i < 72; i++)
    {
        memcpy(title + i * 4, "\xF0\x9F\x98\x80", 4);
    }
    title[PICO_SESSION_TITLE_MAX_BYTES] = '\0';
    if (PicoSession_LogTitle(&app, &agent, title) != PICO_SESSION_WRITE_OK)
    {
        unlink(agent.session_path);
        return Fail("72 four-byte title code points were rejected");
    }
    PicoSessionHeader header;
    bool ok = sizeof(((PicoCompleteItem *)0)->label) >= sizeof(title) &&
              PicoSession_ReadHeader(agent.session_path, &header) == 0 &&
              strcmp(header.title, title) == 0 &&
              ListedTitleIs(&app, agent.session_id, title);
    unlink(agent.session_path);
    return ok ? 0 : Fail("72 four-byte title code points were truncated");
}

static int TestConcurrentAppendDuringTitle(void)
{
    PicoHost app;
    PicoAgent agent;
    memset(&app, 0, sizeof(app));
    memset(&agent, 0, sizeof(agent));
    agent.persistence = PICO_SESSION_DURABLE;
    snprintf(agent.model, sizeof(agent.model), "saved-model");
    PicoHost_SetPath(&app, "/workspace");
    PicoSession_LogUser(&app, &agent, "seed", "seed", NULL);

    int ready[2];
    int attempted[2];
    if (pipe(ready) != 0 || pipe(attempted) != 0)
    {
        unlink(agent.session_path);
        return Fail("could not create concurrency test pipes");
    }
    pid_t child = fork();
    if (child < 0)
    {
        close(ready[0]); close(ready[1]); close(attempted[0]); close(attempted[1]);
        unlink(agent.session_path);
        return Fail("could not fork concurrent session writer");
    }
    if (child == 0)
    {
        close(ready[1]);
        close(attempted[0]);
        if (!TransferByte(ready[0], false))
        {
            _exit(2);
        }
        close(ready[0]);
        g_lock_attempt_fd = attempted[1];
        PicoSessionWriteResult result = PicoSession_LogAssistant(
            &app, &agent, 1, "concurrent append", NULL, NULL, NULL, NULL, 0);
        close(attempted[1]);
        _exit(result == PICO_SESSION_WRITE_OK ? 0 : 3);
    }

    close(ready[0]);
    close(attempted[1]);
    g_title_ready_fd = ready[1];
    g_title_continue_fd = attempted[0];
    PicoSessionWriteResult title_result = PicoSession_LogTitle(&app, &agent, "Locked rewrite");
    g_title_ready_fd = -1;
    g_title_continue_fd = -1;
    close(ready[1]);
    close(attempted[0]);
    int status = 0;
    waitpid(child, &status, 0);

    size_t file_len = 0;
    char *file = Pico_ReadFile(agent.session_path, &file_len);
    bool ok = title_result == PICO_SESSION_WRITE_OK && WIFEXITED(status) && WEXITSTATUS(status) == 0 &&
              file && strstr(file, "\"title\":\"Locked rewrite\"") &&
              strstr(file, "concurrent append");
    free(file);
    unlink(agent.session_path);
    return ok ? 0 : Fail("title rewrite lost or corrupted a concurrent append");
}

static int TestConcurrentDoneCatalog(void)
{
    char ws[] = "/tmp/pico-done-race-XXXXXX";
    PicoHost app;
    PicoAgent agent;
    PicoCatalogWorkspace *catalog = NULL;
    const PicoCatalogWorkspace *found;
    int catalog_n;
    int ready[2];
    int attempted[2];
    int status = 0;
    bool found_row = false;
    bool catalog_done = false;
    if (!mkdtemp(ws))
    {
        return Fail("done race workspace");
    }
    memset(&app, 0, sizeof(app));
    memset(&agent, 0, sizeof(agent));
    agent.persistence = PICO_SESSION_DURABLE;
    snprintf(agent.model, sizeof(agent.model), "saved-model");
    PicoHost_SetPath(&app, ws);
    if (PicoSession_LogUser(&app, &agent, "seed", "seed", NULL) != PICO_SESSION_WRITE_OK ||
        pipe(ready) != 0 || pipe(attempted) != 0)
    {
        unlink(agent.session_path);
        return Fail("done race setup");
    }
    pid_t child = fork();
    if (child < 0)
    {
        close(ready[0]); close(ready[1]); close(attempted[0]); close(attempted[1]);
        unlink(agent.session_path);
        return Fail("done race fork");
    }
    if (child == 0)
    {
        close(ready[1]);
        close(attempted[0]);
        if (!TransferByte(ready[0], false))
        {
            _exit(2);
        }
        close(ready[0]);
        g_lock_attempt_fd = attempted[1];
        PicoSessionWriteResult result = PicoSession_LogAssistant(
            &app, &agent, 1, "concurrent ordinary append", NULL, NULL, NULL, NULL, 0);
        close(attempted[1]);
        _exit(result == PICO_SESSION_WRITE_OK ? 0 : 3);
    }
    close(ready[0]);
    close(attempted[1]);
    g_catalog_ready_fd = ready[1];
    g_catalog_continue_fd = attempted[0];
    PicoSessionWriteResult parent_result = PicoSession_LogUnseenComplete(&app, &agent, true);
    g_catalog_ready_fd = -1;
    g_catalog_continue_fd = -1;
    close(ready[1]);
    close(attempted[0]);
    waitpid(child, &status, 0);
    catalog_n = PicoCatalog_Scan(&catalog);
    found = FindCatalogPath(catalog, catalog_n, ws);
    if (found)
    {
        for (int i = 0; i < found->session_count; i++)
        {
            if (strcmp(found->sessions[i].id, agent.session_id) == 0)
            {
                found_row = true;
                catalog_done = found->sessions[i].unseen_complete;
                break;
            }
        }
    }
    PicoCatalog_Free(catalog, catalog_n);
    unlink(agent.session_path);
    return parent_result == PICO_SESSION_WRITE_OK && WIFEXITED(status) &&
                   WEXITSTATUS(status) == 0 && found_row && catalog_done
               ? 0
               : Fail("ordinary concurrent append overwrote the authoritative done state");
}

static bool HasRestoredThinkSummary(const PicoMessage *messages, int count, int think_ms)
{
    for (int i = 0; i < count; i++)
    {
        for (int t = 0; t < messages[i].trace_count; t++)
        {
            const PicoTraceLine *line = &messages[i].trace[t];
            if (!line->is_tool && line->think_part_count == 2 && line->think_parts &&
                line->think_parts[0] && strcmp(line->think_parts[0], "**first**") == 0 &&
                line->think_parts[1] && strcmp(line->think_parts[1], "**second**") == 0 &&
                line->text && strcmp(line->text, "**second**") == 0 &&
                line->think_ms == think_ms)
            {
                return true;
            }
        }
    }
    return false;
}

static int TestThinkingRoundTrip(void)
{
    PicoHost app;
    PicoAgent agent;
    memset(&app, 0, sizeof(app));
    memset(&agent, 0, sizeof(agent));
    agent.persistence = PICO_SESSION_DURABLE;
    snprintf(agent.model, sizeof(agent.model), "saved-model");
    PicoHost_SetPath(&app, "/workspace");
    const char *sig =
        "{\"type\":\"reasoning\",\"id\":\"rs_test\",\"encrypted_content\":\"blob\"}";
    const char *thinking_parts = "[\"**first**\",\"**second**\"]";
    PicoSession_LogAssistant(&app, &agent, 1, "visible", "think-hard", sig, NULL,
                             thinking_parts, 12500);
    PicoSession_LogToolCall(&app, &agent, 1, "call-1", "sh", "{}", "fc_abc");
    if (!agent.session_path[0])
    {
        return Fail("thinking log did not create a session file");
    }
    size_t n = 0;
    char *file = Pico_ReadFile(agent.session_path, &n);
    if (!file || !strstr(file, "\"thinking\":\"think-hard\"") || !strstr(file, "encrypted_content") ||
        !strstr(file, "\"item_id\":\"fc_abc\"") || !strstr(file, "\"thinking_ms\":12500") ||
        !strstr(file, "\"thinking_parts\":[\"**first**\",\"**second**\"]"))
    {
        free(file);
        return Fail("session file omitted thinking, summary steps, signature, item_id, or duration");
    }
    free(file);

    g_last_think[0] = '\0';
    g_last_sig[0] = '\0';
    g_last_item_id[0] = '\0';
    PicoHost reader;
    PicoAgent reader_agent;
    memset(&reader, 0, sizeof(reader));
    memset(&reader_agent, 0, sizeof(reader_agent));
    reader_agent.persistence = PICO_SESSION_DURABLE;
    PicoHost_SetPath(&reader, "/workspace");
    PicoSession_Start(&reader, &reader_agent, PICO_SESSION_NEW, agent.session_path);
    if (strcmp(g_last_think, "think-hard") != 0 || !strstr(g_last_sig, "rs_test") ||
        strcmp(g_last_item_id, "fc_abc") != 0)
    {
        unlink(agent.session_path);
        return Fail("session replay did not restore thinking, signature, or item_id");
    }
    {
        PicoMessage *msg = NULL;
        for (int i = reader_agent.message_count - 1; i >= 0; i--)
        {
            if (reader_agent.messages[i].role == PICO_ROLE_ASSISTANT)
            {
                msg = &reader_agent.messages[i];
                break;
            }
        }
        if (!msg || !HasRestoredThinkSummary(reader_agent.messages, reader_agent.message_count, 12500))
        {
            unlink(agent.session_path);
            return Fail("session replay did not restore structured thinking and its duration");
        }
    }
    {
        PicoMessage *loaded = NULL;
        int loaded_n = 0;
        PicoHost loader;
        memset(&loader, 0, sizeof(loader));
        PicoHost_SetPath(&loader, "/workspace");
        if (PicoSession_LoadTranscript(PicoHost_PrimaryWorkspace(&app), agent.session_id, &loaded, &loaded_n) != 0)
        {
            unlink(agent.session_path);
            return Fail("LoadTranscript failed after thinking log");
        }
        bool loaded_summary = HasRestoredThinkSummary(loaded, loaded_n, 12500);
        PicoMessages_Free(loaded, loaded_n);
        if (!loaded_summary)
        {
            unlink(agent.session_path);
            return Fail("LoadTranscript did not restore structured thinking and its duration");
        }
    }
    unlink(agent.session_path);

    PicoAgent signature_only;
    memset(&signature_only, 0, sizeof(signature_only));
    signature_only.persistence = PICO_SESSION_DURABLE;
    snprintf(signature_only.model, sizeof(signature_only.model), "saved-model");
    PicoSession_LogAssistant(&app, &signature_only, 1, "", NULL, sig, NULL, NULL, 0);
    if (!signature_only.session_path[0])
    {
        return Fail("signature-only assistant did not create a session file");
    }
    {
        size_t sig_n = 0;
        char *sig_file = Pico_ReadFile(signature_only.session_path, &sig_n);
        bool omitted = sig_file && !strstr(sig_file, "thinking_ms");
        free(sig_file);
        if (!omitted)
        {
            unlink(signature_only.session_path);
            return Fail("assistant event without thinking included thinking_ms");
        }
    }
    g_last_think[0] = '\0';
    g_last_sig[0] = '\0';
    memset(&reader, 0, sizeof(reader));
    memset(&reader_agent, 0, sizeof(reader_agent));
    reader_agent.persistence = PICO_SESSION_DURABLE;
    PicoHost_SetPath(&reader, "/workspace");
    PicoSession_Start(&reader, &reader_agent, PICO_SESSION_NEW, signature_only.session_path);
    bool signature_restored = strcmp(g_last_think, "") == 0 && strstr(g_last_sig, "rs_test");
    unlink(signature_only.session_path);
    if (!signature_restored)
    {
        return Fail("session replay dropped a signature-only assistant event");
    }

    PicoAgent plain;
    memset(&plain, 0, sizeof(plain));
    plain.persistence = PICO_SESSION_DURABLE;
    snprintf(plain.model, sizeof(plain.model), "saved-model");
    PicoSession_LogAssistant(&app, &plain, 1, "hello", NULL, NULL, NULL, NULL, 999);
    if (!plain.session_path[0])
    {
        return Fail("content-only assistant did not create a session file");
    }
    {
        size_t n = 0;
        char *file = Pico_ReadFile(plain.session_path, &n);
        bool omitted = file && strstr(file, "\"content\":\"hello\"") && !strstr(file, "thinking_ms");
        free(file);
        unlink(plain.session_path);
        if (!omitted)
        {
            return Fail("assistant event without thinking included thinking_ms");
        }
    }
    return 0;
}

static bool TranscriptMatchesLiveGroups(const PicoMessage *messages, int count)
{
    return count == 4 &&
           messages[0].role == PICO_ROLE_USER &&
           messages[0].source && strcmp(messages[0].source, "task") == 0 &&
           messages[1].role == PICO_ROLE_ASSISTANT &&
           messages[1].source && strcmp(messages[1].source, "firstsecondafter") == 0 &&
           messages[1].trace_count == 3 &&
           !messages[1].trace[0].is_tool &&
           messages[1].trace[0].text && strcmp(messages[1].trace[0].text, "think-1") == 0 &&
           messages[1].trace[0].think_ms == 0 &&
           messages[1].trace[1].is_tool &&
           messages[1].trace[1].tool_call_id &&
           strcmp(messages[1].trace[1].tool_call_id, "call-1") == 0 &&
           messages[1].trace[1].tool_args &&
           strcmp(messages[1].trace[1].tool_args, "run first command") == 0 &&
           messages[1].trace[1].tool_args_json &&
           strcmp(messages[1].trace[1].tool_args_json,
                  "{\"description\":\"run first command\",\"command\":\"true\"}") == 0 &&
           messages[1].trace[1].tool_output &&
           strcmp(messages[1].trace[1].tool_output, "ok-1") == 0 &&
           !messages[1].trace[2].is_tool &&
           messages[1].trace[2].text &&
           strcmp(messages[1].trace[2].text, "think-after-tool") == 0 &&
           messages[1].trace[2].think_ms == 0 &&
           messages[2].role == PICO_ROLE_ASSISTANT &&
           messages[2].source && strcmp(messages[2].source, "third") == 0 &&
           messages[2].trace_count == 1 &&
           !messages[2].trace[0].is_tool &&
           messages[2].trace[0].text && strcmp(messages[2].trace[0].text, "think-2") == 0 &&
           messages[2].trace[0].think_ms == 0 &&
           messages[3].role == PICO_ROLE_ASSISTANT &&
           messages[3].source && strcmp(messages[3].source, "fourth") == 0 &&
           messages[3].trace_count == 2 &&
           messages[3].trace[0].is_tool &&
           messages[3].trace[0].tool_call_id &&
           strcmp(messages[3].trace[0].tool_call_id, "call-2") == 0 &&
           messages[3].trace[0].tool_args &&
           strcmp(messages[3].trace[0].tool_args, "run second command") == 0 &&
           messages[3].trace[0].tool_args_json &&
           strcmp(messages[3].trace[0].tool_args_json,
                  "{\"description\":\"run second command\",\"command\":\"true\"}") == 0 &&
           messages[3].trace[0].tool_output &&
           strcmp(messages[3].trace[0].tool_output, "ok-2") == 0 &&
           !messages[3].trace[1].is_tool &&
           messages[3].trace[1].text && strcmp(messages[3].trace[1].text, "think-3") == 0 &&
           messages[3].trace[1].think_ms == 0;
}

static int TestTranscriptMessageGroups(void)
{
    PicoHost writer;
    PicoAgent writer_agent;
    PicoHost reader;
    PicoAgent reader_agent;
    PicoMessage *loaded = NULL;
    int loaded_n = 0;

    memset(&writer, 0, sizeof(writer));
    memset(&writer_agent, 0, sizeof(writer_agent));
    writer_agent.persistence = PICO_SESSION_DURABLE;
    snprintf(writer_agent.model, sizeof(writer_agent.model), "saved-model");
    PicoHost_SetPath(&writer, "/workspace");
    PicoSession_LogUser(&writer, &writer_agent, "task", "task", NULL);
    PicoSession_LogAssistant(&writer, &writer_agent, 1, "first", "think-1", NULL, NULL, NULL, 0);
    PicoSession_LogAssistant(&writer, &writer_agent, 1, "second", NULL, NULL, NULL, NULL, 0);
    PicoSession_LogToolCall(&writer, &writer_agent, 1, "call-1", "sh",
                            "{\"description\":\"run first command\",\"command\":\"true\"}", NULL);
    PicoSession_LogToolResult(&writer, &writer_agent, "call-1", "sh", "ok-1", false, NULL);
    PicoSession_LogAssistant(&writer, &writer_agent, 1, "after", "think-after-tool", NULL, NULL, NULL, 0);
    PicoSession_LogAssistant(&writer, &writer_agent, 2, "third", "think-2", NULL, NULL, NULL, 0);
    PicoSession_LogToolCall(&writer, &writer_agent, 3, "call-2", "sh",
                            "{\"description\":\"run second command\",\"command\":\"true\"}", NULL);
    PicoSession_LogToolResult(&writer, &writer_agent, "call-2", "sh", "ok-2", false, NULL);
    PicoSession_LogAssistant(&writer, &writer_agent, 3, "fourth", "think-3", NULL, NULL, NULL, 0);
    if (!writer_agent.session_path[0])
    {
        return Fail("message-group log did not create a session file");
    }

    if (PicoSession_LoadTranscript(PicoHost_PrimaryWorkspace(&writer), writer_agent.session_id, &loaded,
                                  &loaded_n) != 0 ||
        !TranscriptMatchesLiveGroups(loaded, loaded_n))
    {
        PicoMessages_Free(loaded, loaded_n);
        unlink(writer_agent.session_path);
        return Fail("read-only transcript did not preserve explicit assistant message groups");
    }
    PicoMessages_Free(loaded, loaded_n);

    memset(&reader, 0, sizeof(reader));
    memset(&reader_agent, 0, sizeof(reader_agent));
    reader_agent.persistence = PICO_SESSION_DURABLE;
    PicoHost_SetPath(&reader, "/workspace");
    PicoSession_Start(&reader, &reader_agent, PICO_SESSION_NEW, writer_agent.session_path);
    if (!TranscriptMatchesLiveGroups(reader_agent.messages, reader_agent.message_count))
    {
        PicoAgent_ClearMessages(&reader_agent);
        unlink(writer_agent.session_path);
        return Fail("session replay did not preserve explicit assistant message groups");
    }
    PicoAgent_ClearMessages(&reader_agent);
    unlink(writer_agent.session_path);
    return 0;
}

static int TestPartsReplay(void)
{
    PicoHost app;
    PicoAgent agent;
    memset(&app, 0, sizeof(app));
    memset(&agent, 0, sizeof(agent));
    agent.persistence = PICO_SESSION_DURABLE;
    snprintf(agent.model, sizeof(agent.model), "saved-model");
    PicoHost_SetPath(&app, "/workspace");
    const char *parts =
        "[{\"type\":\"refusal\",\"text\":\"nope\"},{\"type\":\"image\",\"path\":\"/tmp/pic.png\"}]";
    PicoSession_LogAssistant(&app, &agent, 1, "nope", NULL, NULL, parts, NULL, 0);
    if (!agent.session_path[0])
    {
        return Fail("parts log did not create a session file");
    }
    g_last_assistant_parts[0] = '\0';
    PicoHost reader;
    PicoAgent reader_agent;
    memset(&reader, 0, sizeof(reader));
    memset(&reader_agent, 0, sizeof(reader_agent));
    reader_agent.persistence = PICO_SESSION_DURABLE;
    PicoHost_SetPath(&reader, "/workspace");
    PicoSession_Start(&reader, &reader_agent, PICO_SESSION_NEW, agent.session_path);
    bool ok = strstr(g_last_assistant_parts, "\"type\":\"refusal\"") &&
              strstr(g_last_assistant_parts, "/tmp/pic.png");
    unlink(agent.session_path);
    return ok ? 0 : Fail("session replay did not restore image path and refusal parts");
}

static const PicoCatalogWorkspace *FindCatalogPath(PicoCatalogWorkspace *list, int n, const char *path)
{
    int i;
    for (i = 0; i < n; i++)
    {
        if (strcmp(list[i].path, path) == 0)
        {
            return &list[i];
        }
    }
    return NULL;
}

static int TestCatalog(void)
{
    char ws[] = "/tmp/pico-catalog-ws-XXXXXX";
    PicoCatalogWorkspace *list = NULL;
    int n = 0;
    const PicoCatalogWorkspace *found;
    char key[4096];
    char meta[4096];
    char jsonl[4096];
    char *raw;
    size_t raw_len = 0;
    const char *slash;

    if (!mkdtemp(ws))
    {
        return Fail("catalog mkdtemp");
    }
    if (PicoCatalog_Ensure(ws) != 0)
    {
        return Fail("catalog ensure");
    }
    n = PicoCatalog_Scan(&list);
    found = FindCatalogPath(list, n, ws);
    slash = strrchr(ws, '/');
    if (!found || !slash || strcmp(found->name, slash + 1) != 0 || found->session_count != 0)
    {
        PicoCatalog_Free(list, n);
        return Fail("ensure did not create a catalog workspace named after the folder");
    }
    snprintf(key, sizeof(key), "%s", found->key);
    if (!PicoPath_Format(meta, sizeof(meta), "%s/sessions/%s/.workspace.json", g_config_dir, key) ||
        !PicoPath_Format(jsonl, sizeof(jsonl), "%s/sessions/%s/2026-01-01T00-00-00Z_catalogsess.jsonl",
                         g_config_dir, key))
    {
        PicoCatalog_Free(list, n);
        return Fail("catalog paths");
    }
    raw = Pico_ReadFile(meta, &raw_len);
    if (!raw || !strstr(raw, ws) || !strstr(raw, found->name))
    {
        free(raw);
        PicoCatalog_Free(list, n);
        return Fail("workspace.json missing path or name");
    }
    free(raw);
    PicoCatalog_Free(list, n);

    {
        FILE *f = fopen(jsonl, "wb");
        if (!f)
        {
            return Fail("catalog jsonl write");
        }
        fprintf(f,
                "{\"type\":\"session\",\"version\":4,\"id\":\"catalogsess\","
                "\"kind\":\"normal\",\"model\":\"header-model\",\"cwd\":\"%s\"}\n",
                ws);
        fclose(f);
    }
    if (!AppendRaw(jsonl, "{\"type\":\"message\",\"role\":\"user\",\"content\":\"hello catalog\"}") ||
        !AppendRaw(jsonl, "{\"type\":\"model_change\",\"model\":\"changed-model\",\"effort\":\"high\"}"))
    {
        return Fail("catalog jsonl write");
    }

    n = PicoCatalog_Scan(&list);
    found = FindCatalogPath(list, n, ws);
    if (!found || found->session_count != 1 || strcmp(found->sessions[0].id, "catalogsess") != 0 ||
        strcmp(found->sessions[0].model, "changed-model") != 0 ||
        strcmp(found->sessions[0].effort, "high") != 0 ||
        !strstr(found->sessions[0].title, "hello catalog"))
    {
        PicoCatalog_Free(list, n);
        return Fail("scan did not take parent jsonl title and last model_change");
    }
    PicoCatalog_Free(list, n);

    {
        FILE *f = fopen(meta, "wb");
        if (!f)
        {
            return Fail("open meta for ghost");
        }
        fprintf(f,
                "{\"version\":1,\"key\":\"%s\",\"path\":\"%s\",\"name\":\"%s\",\"order\":0,"
                "\"collapsed\":false,\"sessions\":[{\"id\":\"ghostid\",\"model\":\"x\","
                "\"effort\":\"\",\"mtime\":1}]}",
                key, ws, slash + 1);
        fclose(f);
    }
    n = PicoCatalog_Scan(&list);
    found = FindCatalogPath(list, n, ws);
    if (!found || found->session_count != 1 || strcmp(found->sessions[0].id, "catalogsess") != 0)
    {
        PicoCatalog_Free(list, n);
        return Fail("session-only metadata id must not appear without jsonl");
    }
    PicoCatalog_Free(list, n);

    if (PicoCatalog_SetCollapsed(ws, true) != 0)
    {
        return Fail("set collapsed");
    }
    n = PicoCatalog_Scan(&list);
    found = FindCatalogPath(list, n, ws);
    if (!found || !found->collapsed)
    {
        PicoCatalog_Free(list, n);
        return Fail("collapsed did not persist");
    }
    PicoCatalog_Free(list, n);

    unlink(meta);
    n = PicoCatalog_Scan(&list);
    found = FindCatalogPath(list, n, ws);
    raw = Pico_ReadFile(meta, &raw_len);
    if (!found || !raw || !strstr(raw, ws))
    {
        free(raw);
        PicoCatalog_Free(list, n);
        return Fail("legacy folder without metadata was not recovered from jsonl cwd");
    }
    free(raw);
    PicoCatalog_Free(list, n);

    if (PicoCatalog_SetSessionModel(ws, "catalogsess", "overlay-model", "low") != 0)
    {
        return Fail("set session model");
    }
    n = PicoCatalog_Scan(&list);
    found = FindCatalogPath(list, n, ws);
    if (!found || found->session_count < 1 || strcmp(found->sessions[0].id, "catalogsess") != 0 ||
        strcmp(found->sessions[0].model, "overlay-model") != 0)
    {
        PicoCatalog_Free(list, n);
        return Fail("listing cache must trust overlay when the jsonl stat generation matches");
    }
    PicoCatalog_Free(list, n);
    return 0;
}

static int TestCatalogListingCache(void)
{
    char ws[] = "/tmp/pico-catalog-cache-XXXXXX";
    PicoCatalogWorkspace *list = NULL;
    int n = 0;
    const PicoCatalogWorkspace *found;
    char key[4096];
    char meta[4096];
    char jsonl[4096];
    char *raw = NULL;
    size_t raw_len = 0;
    struct stat st;
    struct utimbuf times;
    char *garbage = NULL;
    const char *slash;

    if (!mkdtemp(ws))
    {
        return Fail("catalog cache mkdtemp");
    }
    if (PicoCatalog_Ensure(ws) != 0)
    {
        return Fail("catalog cache ensure");
    }
    n = PicoCatalog_Scan(&list);
    found = FindCatalogPath(list, n, ws);
    if (!found)
    {
        PicoCatalog_Free(list, n);
        return Fail("catalog cache scan missing workspace");
    }
    snprintf(key, sizeof(key), "%s", found->key);
    PicoCatalog_Free(list, n);
    if (!PicoPath_Format(meta, sizeof(meta), "%s/sessions/%s/.workspace.json", g_config_dir, key) ||
        !PicoPath_Format(jsonl, sizeof(jsonl), "%s/sessions/%s/2026-01-01T00-00-00Z_cachesess.jsonl",
                         g_config_dir, key))
    {
        return Fail("catalog cache paths");
    }
    {
        FILE *f = fopen(jsonl, "wb");
        if (!f)
        {
            return Fail("catalog cache jsonl write");
        }
        fprintf(f,
                "{\"type\":\"session\",\"version\":4,\"id\":\"cachesess\","
                "\"kind\":\"normal\",\"model\":\"header-model\",\"cwd\":\"%s\"}\n",
                ws);
        fclose(f);
    }
    if (!AppendRaw(jsonl, "{\"type\":\"message\",\"role\":\"user\",\"content\":\"cached title source\"}"))
    {
        return Fail("catalog cache jsonl message");
    }

    n = PicoCatalog_Scan(&list);
    found = FindCatalogPath(list, n, ws);
    raw = Pico_ReadFile(meta, &raw_len);
    if (!found || found->session_count != 1 || strcmp(found->sessions[0].id, "cachesess") != 0 ||
        !strstr(found->sessions[0].title, "cached title source") || !raw ||
        !strstr(raw, "cached title source") || !strstr(raw, "\"mtime_nsec\":") ||
        !strstr(raw, "\"ctime_nsec\":") || !strstr(raw, "\"inode\":") ||
        !strstr(raw, "\"size\":"))
    {
        free(raw);
        PicoCatalog_Free(list, n);
        return Fail("first scan must cache title and size in workspace.json");
    }
    free(raw);
    PicoCatalog_Free(list, n);

    g_scan_session_calls = 0;
    n = PicoCatalog_Scan(&list);
    found = FindCatalogPath(list, n, ws);
    if (!found || found->session_count != 1 ||
        !strstr(found->sessions[0].title, "cached title source") || g_scan_session_calls != 0)
    {
        PicoCatalog_Free(list, n);
        return Fail("unchanged jsonl must use the listing cache without parsing");
    }
    PicoCatalog_Free(list, n);

    if (stat(jsonl, &st) != 0)
    {
        return Fail("catalog cache stat");
    }
    garbage = (char *)malloc((size_t)st.st_size);
    if (!garbage)
    {
        return Fail("catalog cache garbage alloc");
    }
    memset(garbage, 'x', (size_t)st.st_size);
    if (st.st_size > 0)
    {
        garbage[st.st_size - 1] = '\n';
    }
    {
        FILE *f = fopen(jsonl, "wb");
        if (!f)
        {
            free(garbage);
            return Fail("catalog cache rewrite");
        }
        if (fwrite(garbage, 1, (size_t)st.st_size, f) != (size_t)st.st_size)
        {
            fclose(f);
            free(garbage);
            return Fail("catalog cache rewrite bytes");
        }
        fclose(f);
    }
    free(garbage);
    times.actime = st.st_atime;
    times.modtime = st.st_mtime;
    if (utime(jsonl, &times) != 0)
    {
        return Fail("catalog cache utime");
    }

    g_scan_session_calls = 0;
    n = PicoCatalog_Scan(&list);
    found = FindCatalogPath(list, n, ws);
    if (!found || found->session_count != 1 ||
        strstr(found->sessions[0].title, "cached title source") || g_scan_session_calls == 0)
    {
        PicoCatalog_Free(list, n);
        return Fail("same-size rewrite with restored mtime must invalidate the listing cache");
    }
    PicoCatalog_Free(list, n);

    if (!AppendRaw(jsonl, "x"))
    {
        return Fail("catalog cache size bump");
    }
    n = PicoCatalog_Scan(&list);
    found = FindCatalogPath(list, n, ws);
    if (!found || found->session_count != 1 ||
        strstr(found->sessions[0].title, "cached title source"))
    {
        PicoCatalog_Free(list, n);
        return Fail("size change must invalidate the listing cache");
    }
    PicoCatalog_Free(list, n);

    {
        FILE *f = fopen(jsonl, "wb");
        if (!f)
        {
            return Fail("catalog cache subagent rewrite");
        }
        fprintf(f,
                "{\"type\":\"session\",\"version\":4,\"id\":\"childsess\","
                "\"kind\":\"subagent\",\"profile\":\"p\",\"initial_purpose\":\"t\","
                "\"cwd\":\"%s\"}\n",
                ws);
        fclose(f);
    }
    n = PicoCatalog_Scan(&list);
    found = FindCatalogPath(list, n, ws);
    if (!found || found->session_count != 0)
    {
        PicoCatalog_Free(list, n);
        return Fail("subagent jsonl must stay omitted from parent listing");
    }
    PicoCatalog_Free(list, n);
    slash = strrchr(ws, '/');
    (void)slash;
    return 0;
}

static int TestSessionListCompleteness(void)
{
    char ws[] = "/tmp/pico-list-complete-XXXXXX";
    char key[4096];
    char sessions_dir[4096];
    PicoCatalogWorkspace *catalog = NULL;
    const PicoCatalogWorkspace *found;
    PicoSessionInfo *list = NULL;
    PicoWorkspace workspace;
    int catalog_n;
    int listed_n;
    bool found_child = false;
    if (!mkdtemp(ws) || PicoCatalog_Ensure(ws) != 0)
    {
        return Fail("complete listing setup");
    }
    catalog_n = PicoCatalog_Scan(&catalog);
    found = FindCatalogPath(catalog, catalog_n, ws);
    if (!found)
    {
        PicoCatalog_Free(catalog, catalog_n);
        return Fail("complete listing catalog workspace");
    }
    snprintf(key, sizeof(key), "%s", found->key);
    PicoCatalog_Free(catalog, catalog_n);
    if (!PicoPath_Format(sessions_dir, sizeof(sessions_dir), "%s/sessions/%s", g_config_dir, key))
    {
        return Fail("complete listing session directory");
    }
    for (int i = 0; i < PICO_MAX_CATALOG_SESSIONS + 4; i++)
    {
        char path[4096];
        FILE *f;
        if (!PicoPath_Format(path, sizeof(path), "%s/2026-01-01T00-00-%03dZ_list%03d.jsonl",
                             sessions_dir, i, i))
        {
            return Fail("complete listing path");
        }
        f = fopen(path, "wb");
        if (!f)
        {
            return Fail("complete listing file");
        }
        fprintf(f,
                "{\"type\":\"session\",\"version\":4,\"id\":\"list%03d\","
                "\"kind\":\"%s\",\"model\":\"header-model\",\"cwd\":\"%s\"}\n",
                i, i == PICO_MAX_CATALOG_SESSIONS + 3 ? "subagent" : "normal", ws);
        fprintf(f,
                "{\"type\":\"message\",\"role\":\"user\",\"content\":\"title-%03d\"}\n",
                i);
        if (i == PICO_MAX_CATALOG_SESSIONS + 3)
        {
            fprintf(f,
                    "{\"type\":\"model_change\",\"model\":\"child-model\","
                    "\"effort\":\"high\"}\n"
                    "{\"type\":\"unseen_complete\",\"complete\":true}\n");
        }
        fclose(f);
    }
    memset(&workspace, 0, sizeof(workspace));
    snprintf(workspace.path, sizeof(workspace.path), "%s", ws);
    listed_n = PicoSession_List(&workspace, &list, false);
    if (listed_n != PICO_MAX_CATALOG_SESSIONS + 4)
    {
        free(list);
        return Fail("complete listing must return every session");
    }
    for (int i = 0; i < listed_n; i++)
    {
        if (!strstr(list[i].title, "title-"))
        {
            free(list);
            return Fail("complete listing must derive every uncached title");
        }
        if (strcmp(list[i].id, "list259") == 0)
        {
            found_child = list[i].kind == PICO_AGENT_SUBAGENT &&
                          strcmp(list[i].model, "child-model") == 0 &&
                          strcmp(list[i].effort, "high") == 0 && list[i].unseen_complete;
        }
    }
    free(list);
    if (!found_child)
    {
        return Fail("complete listing must fully parse subagent events");
    }
    return 0;
}

static int TestUnseenCompleteRoundTrip(void)
{
    char wsdir[] = "/tmp/pico-unseen-ws-XXXXXX";
    PicoHost writer;
    PicoWorkspace writer_ws;
    PicoAgent writer_agent;
    PicoAgent resumed;
    PicoAgent ephemeral;
    PicoSessionInfo *listed = NULL;
    int listed_n;
    PicoCatalogWorkspace *catalog = NULL;
    int catalog_n;
    const PicoCatalogWorkspace *found;
    bool listed_done = false;
    int i;

    if (!mkdtemp(wsdir))
    {
        return Fail("unseen complete mkdtemp");
    }
    memset(&writer, 0, sizeof(writer));
    memset(&writer_ws, 0, sizeof(writer_ws));
    memset(&writer_agent, 0, sizeof(writer_agent));
    memset(&resumed, 0, sizeof(resumed));
    memset(&ephemeral, 0, sizeof(ephemeral));
    writer_ws.host = &writer;
    snprintf(writer_ws.path, sizeof(writer_ws.path), "%s", wsdir);
    writer.workspaces[0] = &writer_ws;
    writer.workspace_count = 1;
    writer_agent.workspace = &writer_ws;
    writer_agent.persistence = PICO_SESSION_DURABLE;
    writer_agent.kind = PICO_AGENT_MAIN;
    snprintf(writer_agent.model, sizeof(writer_agent.model), "saved-model");
    ephemeral.persistence = PICO_SESSION_EPHEMERAL;
    if (PicoSession_LogUnseenComplete(&writer, &ephemeral, true) != PICO_SESSION_WRITE_SKIPPED)
    {
        return Fail("ephemeral unseen complete was not skipped");
    }
    if (PicoSession_LogUser(&writer, &writer_agent, "task", "task", NULL) != PICO_SESSION_WRITE_OK ||
        PicoSession_LogUnseenComplete(&writer, &writer_agent, true) != PICO_SESSION_WRITE_OK)
    {
        unlink(writer_agent.session_path);
        return Fail("unseen complete jsonl write");
    }
    listed_n = PicoSession_List(&writer_ws, &listed, true);
    for (i = 0; i < listed_n; i++)
    {
        if (strcmp(listed[i].id, writer_agent.session_id) == 0)
        {
            listed_done = listed[i].unseen_complete;
            break;
        }
    }
    free(listed);
    if (!listed_done)
    {
        unlink(writer_agent.session_path);
        return Fail("listing did not surface unseen complete");
    }
    catalog_n = PicoCatalog_Scan(&catalog);
    found = FindCatalogPath(catalog, catalog_n, wsdir);
    if (!found || found->session_count < 1 || !found->sessions[0].unseen_complete)
    {
        PicoCatalog_Free(catalog, catalog_n);
        unlink(writer_agent.session_path);
        return Fail("catalog scan did not surface unseen complete");
    }
    PicoCatalog_Free(catalog, catalog_n);

    resumed.workspace = &writer_ws;
    resumed.kind = PICO_AGENT_MAIN;
    if (PicoSession_Replay(&writer, &resumed, writer_agent.session_path, false) != 0 ||
        !resumed.unseen_complete)
    {
        unlink(writer_agent.session_path);
        return Fail("replay must restore unseen complete");
    }
    PicoSession_Reset(&writer, &resumed);
    if (resumed.unseen_complete)
    {
        unlink(writer_agent.session_path);
        return Fail("reset must clear unseen complete");
    }
    if (PicoSession_LogUnseenComplete(&writer, &writer_agent, false) != PICO_SESSION_WRITE_OK)
    {
        unlink(writer_agent.session_path);
        return Fail("unseen complete clear write");
    }
    memset(&resumed, 0, sizeof(resumed));
    resumed.workspace = &writer_ws;
    resumed.kind = PICO_AGENT_MAIN;
    listed = NULL;
    listed_done = false;
    listed_n = PicoSession_List(&writer_ws, &listed, true);
    for (i = 0; i < listed_n; i++)
    {
        if (strcmp(listed[i].id, writer_agent.session_id) == 0)
        {
            listed_done = listed[i].unseen_complete;
            break;
        }
    }
    free(listed);
    if (listed_done || PicoSession_Replay(&writer, &resumed, writer_agent.session_path, false) != 0 ||
        resumed.unseen_complete)
    {
        unlink(writer_agent.session_path);
        return Fail("last unseen complete event must win");
    }
    unlink(writer_agent.session_path);
    rmdir(wsdir);
    return 0;
}

static int TestCatalogOmitsMissingPath(void)
{
    char ws[] = "/tmp/pico-catalog-gone-XXXXXX";
    PicoCatalogWorkspace *list = NULL;
    int n = 0;

    if (!mkdtemp(ws))
    {
        return Fail("missing-path mkdtemp");
    }
    if (PicoCatalog_Ensure(ws) != 0)
    {
        rmdir(ws);
        return Fail("missing-path ensure");
    }
    if (rmdir(ws) != 0)
    {
        return Fail("missing-path rmdir");
    }
    n = PicoCatalog_Scan(&list);
    if (FindCatalogPath(list, n, ws))
    {
        PicoCatalog_Free(list, n);
        return Fail("scan listed a catalog workspace whose directory is gone");
    }
    PicoCatalog_Free(list, n);
    return 0;
}

static int CountType(const char *file, const char *type)
{
    int n = 0;
    const char *p = file;
    char needle[64];
    snprintf(needle, sizeof(needle), "\"type\":\"%s\"", type);
    while ((p = strstr(p, needle)))
    {
        n++;
        p += 1;
    }
    return n;
}

static int TestQueuedModelChangeOrdersUserWrite(void)
{
    PicoHost writer;
    PicoWorkspace writer_ws;
    PicoAgent writer_agent;
    size_t file_len = 0;
    char *file = NULL;
    const char *user;
    const char *change;
    const char *header;

    memset(&writer, 0, sizeof(writer));
    memset(&writer_ws, 0, sizeof(writer_ws));
    memset(&writer_agent, 0, sizeof(writer_agent));
    writer_ws.host = &writer;
    snprintf(writer_ws.path, sizeof(writer_ws.path), "/workspace");
    writer.workspaces[0] = &writer_ws;
    writer.workspace_count = 1;
    writer_agent.workspace = &writer_ws;
    writer_ws.agents[0] = &writer_agent;
    writer_ws.count = 1;
    writer_agent.persistence = PICO_SESSION_DURABLE;
    writer_agent.kind = PICO_AGENT_MAIN;
    writer_agent.id = 1;
    snprintf(writer_agent.model, sizeof(writer_agent.model), "first-model");
    snprintf(writer_agent.effort, sizeof(writer_agent.effort), "low");
    PicoSessionPersist_Init(&writer);
    PicoSession_EnqueueModelChange(&writer, &writer_agent);
    if (!writer_agent.session_id[0] || !writer_agent.session_path[0])
    {
        PicoSessionPersist_Shutdown(&writer);
        return Fail("queued model change must assign a session identity before the jsonl exists");
    }
    snprintf(writer_agent.model, sizeof(writer_agent.model), "second-model");
    snprintf(writer_agent.effort, sizeof(writer_agent.effort), "high");
    PicoSession_EnqueueModelChange(&writer, &writer_agent);
    if (PicoSession_LogUser(&writer, &writer_agent, "hello", "hello", NULL) != PICO_SESSION_WRITE_OK)
    {
        PicoSessionPersist_Shutdown(&writer);
        unlink(writer_agent.session_path);
        return Fail("user write did not drain queued model change");
    }
    PicoSessionPersist_Shutdown(&writer);
    file = Pico_ReadFile(writer_agent.session_path, &file_len);
    unlink(writer_agent.session_path);
    if (!file)
    {
        return Fail("queued model change did not create a session file");
    }
    header = strstr(file, "\"type\":\"session\"");
    user = strstr(file, "\"type\":\"message\"");
    change = NULL;
    {
        const char *cursor = file;
        while ((cursor = strstr(cursor, "\"type\":\"model_change\"")))
        {
            change = cursor;
            cursor += 1;
        }
    }
    if (!header || !change || !user || header > change || change > user ||
        !strstr(change, "\"model\":\"second-model\"") || !strstr(change, "\"effort\":\"high\"") ||
        CountType(file, "session") != 1)
    {
        free(file);
        return Fail("user write must follow the header and latest queued model_change");
    }
    free(file);
    return 0;
}

static int TestQueuedModelChangeFailureThenDrain(void)
{
    PicoHost writer;
    PicoWorkspace writer_ws;
    PicoAgent writer_agent;

    memset(&writer, 0, sizeof(writer));
    memset(&writer_ws, 0, sizeof(writer_ws));
    memset(&writer_agent, 0, sizeof(writer_agent));
    writer_ws.host = &writer;
    snprintf(writer_ws.path, sizeof(writer_ws.path), "/workspace");
    writer.workspaces[0] = &writer_ws;
    writer.workspace_count = 1;
    writer_agent.workspace = &writer_ws;
    writer_ws.agents[0] = &writer_agent;
    writer_ws.count = 1;
    writer_agent.persistence = PICO_SESSION_DURABLE;
    writer_agent.kind = PICO_AGENT_MAIN;
    writer_agent.id = 2;
    snprintf(writer_agent.model, sizeof(writer_agent.model), "fail-model");
    PicoSessionPersist_Init(&writer);
    g_status_warning[0] = '\0';
    g_session_fail_stage = "append_write";
    PicoSession_EnqueueModelChange(&writer, &writer_agent);
    if (PicoSession_LogUser(&writer, &writer_agent, "hello", "hello", NULL) != PICO_SESSION_WRITE_FAILED ||
        writer_agent.persistence != PICO_SESSION_FAILED || g_status_warning[0] == '\0')
    {
        g_session_fail_stage = NULL;
        PicoSessionPersist_Shutdown(&writer);
        if (writer_agent.session_path[0])
        {
            unlink(writer_agent.session_path);
        }
        return Fail("queued model-change write failure was not applied on drain");
    }
    g_session_fail_stage = NULL;
    PicoSessionPersist_Shutdown(&writer);
    if (writer_agent.session_path[0])
    {
        unlink(writer_agent.session_path);
    }
    return 0;
}

static int TestQueuedModelChangeCreatesHeaderPerAgent(void)
{
    char ws[] = "/tmp/pico-persist-multi-XXXXXX";
    PicoHost writer;
    PicoWorkspace writer_ws;
    PicoAgent first;
    PicoAgent second;
    int ready[2];
    int proceed[2];
    size_t file_len = 0;
    char *file = NULL;

    if (!mkdtemp(ws))
    {
        return Fail("multi-agent persist workspace");
    }
    memset(&writer, 0, sizeof(writer));
    memset(&writer_ws, 0, sizeof(writer_ws));
    memset(&first, 0, sizeof(first));
    memset(&second, 0, sizeof(second));
    writer_ws.host = &writer;
    snprintf(writer_ws.path, sizeof(writer_ws.path), "%s", ws);
    writer.workspaces[0] = &writer_ws;
    writer.workspace_count = 1;
    first.workspace = &writer_ws;
    second.workspace = &writer_ws;
    writer_ws.agents[0] = &first;
    writer_ws.agents[1] = &second;
    writer_ws.count = 2;
    first.persistence = second.persistence = PICO_SESSION_DURABLE;
    first.kind = second.kind = PICO_AGENT_MAIN;
    first.id = 3;
    second.id = 4;
    snprintf(first.model, sizeof(first.model), "first-agent-model");
    snprintf(second.model, sizeof(second.model), "second-agent-model");
    if (pipe(ready) != 0 || pipe(proceed) != 0)
    {
        return Fail("multi-agent persist pipes");
    }

    PicoSessionPersist_Init(&writer);
    g_catalog_ready_fd = ready[1];
    g_catalog_continue_fd = proceed[0];
    PicoSession_EnqueueModelChange(&writer, &first);
    if (!TransferByte(ready[0], false))
    {
        PicoSessionPersist_Shutdown(&writer);
        return Fail("first agent did not reach blocked catalog write");
    }
    PicoSession_EnqueueModelChange(&writer, &second);
    if (!TransferByte(proceed[1], true))
    {
        PicoSessionPersist_Shutdown(&writer);
        return Fail("could not release blocked catalog write");
    }
    PicoSession_DrainPersist(&writer, &first);
    PicoSession_DrainPersist(&writer, &second);
    PicoSessionPersist_Shutdown(&writer);
    close(ready[0]); close(ready[1]); close(proceed[0]); close(proceed[1]);

    file = Pico_ReadFile(second.session_path, &file_len);
    unlink(first.session_path);
    unlink(second.session_path);
    if (!file || CountType(file, "session") != 1 || CountType(file, "model_change") != 1 ||
        strstr(file, "\"type\":\"session\"") > strstr(file, "\"type\":\"model_change\""))
    {
        free(file);
        return Fail("each fresh agent must persist its own header before model_change");
    }
    free(file);
    rmdir(ws);
    return 0;
}

static int TestQueuedModelChangeDrainDeadline(void)
{
    char ws[] = "/tmp/pico-persist-deadline-XXXXXX";
    PicoHost writer;
    PicoWorkspace writer_ws;
    PicoAgent agent;
    struct timespec deadline;
    int ready[2];
    int proceed[2];
    bool timed_out;

    if (!mkdtemp(ws))
    {
        return Fail("persist deadline workspace");
    }
    memset(&writer, 0, sizeof(writer));
    memset(&writer_ws, 0, sizeof(writer_ws));
    memset(&agent, 0, sizeof(agent));
    writer_ws.host = &writer;
    snprintf(writer_ws.path, sizeof(writer_ws.path), "%s", ws);
    writer.workspaces[0] = &writer_ws;
    writer.workspace_count = 1;
    agent.workspace = &writer_ws;
    writer_ws.agents[0] = &agent;
    writer_ws.count = 1;
    agent.persistence = PICO_SESSION_DURABLE;
    agent.kind = PICO_AGENT_MAIN;
    agent.id = 5;
    snprintf(agent.model, sizeof(agent.model), "deadline-model");
    if (pipe(ready) != 0 || pipe(proceed) != 0)
    {
        return Fail("persist deadline pipes");
    }

    PicoSessionPersist_Init(&writer);
    g_catalog_ready_fd = ready[1];
    g_catalog_continue_fd = proceed[0];
    PicoSession_EnqueueModelChange(&writer, &agent);
    if (!TransferByte(ready[0], false))
    {
        PicoSessionPersist_Shutdown(&writer);
        return Fail("persist deadline did not reach blocked catalog write");
    }
    clock_gettime(CLOCK_REALTIME, &deadline);
    deadline.tv_nsec += 100000000L;
    if (deadline.tv_nsec >= 1000000000L)
    {
        deadline.tv_sec++;
        deadline.tv_nsec -= 1000000000L;
    }
    timed_out = !PicoSession_DrainPersistBefore(&writer, &agent, &deadline);
    if (!TransferByte(proceed[1], true))
    {
        PicoSessionPersist_Shutdown(&writer);
        return Fail("could not release persist deadline write");
    }
    PicoSession_DrainPersist(&writer, &agent);
    PicoSessionPersist_Shutdown(&writer);
    close(ready[0]); close(ready[1]); close(proceed[0]); close(proceed[1]);
    unlink(agent.session_path);
    rmdir(ws);
    return timed_out ? 0 : Fail("blocked persistence must report the shared drain deadline");
}

static int TestModelResumeReplay(void)
{
    PicoHost writer;
    PicoWorkspace writer_ws;
    PicoAgent writer_agent;
    PicoAgent resumed;

    memset(&writer, 0, sizeof(writer));
    memset(&writer_ws, 0, sizeof(writer_ws));
    memset(&writer_agent, 0, sizeof(writer_agent));
    memset(&resumed, 0, sizeof(resumed));
    writer_ws.host = &writer;
    snprintf(writer_ws.path, sizeof(writer_ws.path), "/workspace");
    snprintf(writer_ws.settings.default_model, sizeof(writer_ws.settings.default_model), "default-model");
    writer.workspaces[0] = &writer_ws;
    writer.workspace_count = 1;
    writer_agent.workspace = &writer_ws;
    writer_agent.persistence = PICO_SESSION_DURABLE;
    writer_agent.kind = PICO_AGENT_MAIN;
    snprintf(writer_agent.model, sizeof(writer_agent.model), "%s", writer_ws.settings.default_model);
    if (PicoSession_LogUser(&writer, &writer_agent, "seed", "seed", NULL) != PICO_SESSION_WRITE_OK ||
        PicoSession_LogModelChange(&writer, &writer_agent, "changed-model", "high") != PICO_SESSION_WRITE_OK)
    {
        return Fail("model resume jsonl write");
    }
    resumed.workspace = &writer_ws;
    resumed.kind = PICO_AGENT_MAIN;
    snprintf(resumed.model, sizeof(resumed.model), "%s", writer_ws.settings.default_model);
    if (PicoSession_Replay(&writer, &resumed, writer_agent.session_path, false) != 0 ||
        strcmp(resumed.model, "changed-model") != 0 || strcmp(resumed.effort, "high") != 0)
    {
        unlink(writer_agent.session_path);
        return Fail("resume must load the stored model and effort from jsonl");
    }
    unlink(writer_agent.session_path);
    return 0;
}

int main(void)
{
    char temp[] = "/tmp/pico-session-usage-XXXXXX";
    if (!mkdtemp(temp))
    {
        return Fail("could not create temporary directory");
    }
    snprintf(g_config_dir, sizeof(g_config_dir), "%s", temp);

    PicoHost writer;
    PicoWorkspace writer_ws;
    PicoAgent writer_agent;
    memset(&writer, 0, sizeof(writer));
    memset(&writer_ws, 0, sizeof(writer_ws));
    memset(&writer_agent, 0, sizeof(writer_agent));
    writer_ws.host = &writer;
    snprintf(writer_ws.path, sizeof(writer_ws.path), "/workspace");
    writer.workspaces[0] = &writer_ws;
    writer.workspace_count = 1;
    writer_agent.workspace = &writer_ws;
    writer_agent.persistence = PICO_SESSION_DURABLE;
    snprintf(writer_agent.model, sizeof(writer_agent.model), "saved-model");
    snprintf(writer_ws.settings.default_model, sizeof(writer_ws.settings.default_model), "default-model");
    PicoSession_LogUsage(&writer, &writer_agent, 100, 20);
    PicoSession_LogUsage(&writer, &writer_agent, 200, 150);
    PicoSession_LogAssistant(&writer, &writer_agent, 0, "assistant response", NULL, NULL, NULL, NULL, 0);
    PicoSession_LogCompaction(&writer, &writer_agent, "brief", 200);
    PicoSession_LogToolResult(&writer, &writer_agent, "state-1", "state_test", "saved", false, "{\"value\":7}");
    PicoSession_LogToolResult(&writer, &writer_agent, "state-2", "state_test", "failed", true, "{\"value\":8}");
    PicoSession_LogToolResult(&writer, &writer_agent, "state-3", "state_test", "bad snapshot", false, "{\"value\":99}");
    if (!writer_agent.session_path[0])
    {
        return Fail("usage did not create a session file");
    }

    size_t file_len = 0;
    char *file = Pico_ReadFile(writer_agent.session_path, &file_len);
    if (!file || !strstr(file, "\"version\":4") || !strstr(file, "\"kind\":\"normal\"") ||
        !strstr(file, "\"type\":\"usage\"") || strstr(file, "\"usage\":{"))
    {
        free(file);
        return Fail("session schema did not use version 4 headers and dedicated usage events");
    }
    free(file);

    PicoAgent child_agent;
    memset(&child_agent, 0, sizeof(child_agent));
    child_agent.workspace = &writer_ws;
    child_agent.persistence = PICO_SESSION_DURABLE;
    child_agent.kind = PICO_AGENT_SUBAGENT;
    snprintf(child_agent.model, sizeof(child_agent.model), "saved-model");
    snprintf(child_agent.profile, sizeof(child_agent.profile), "review");
    snprintf(child_agent.purpose, sizeof(child_agent.purpose), "Review carefully");
    snprintf(child_agent.parent_session_id, sizeof(child_agent.parent_session_id), "parent-session");
    PicoSession_LogUser(&writer, &child_agent, "delegated task", "delegated task", NULL);
    PicoSession_LogAssistant(&writer, &child_agent, 1, "child findings", NULL, NULL, NULL, NULL, 0);
    PicoSession_LogToolCall(&writer, &child_agent, 1, "child-call-1", "subagent",
                            "{\"task\":\"nested\"}", NULL);
    PicoSession_LogToolCall(&writer, &child_agent, 1, "child-call-2", "sh",
                            "{\"command\":\"true\"}", NULL);
    PicoSession_LogToolResult(&writer, &child_agent, "child-call-1", "subagent",
                              "{\"session_id\":\"nested-child\"}", false, NULL);
    PicoSession_LogToolResult(&writer, &child_agent, "child-call-2", "sh", "ok", false, NULL);
    PicoSessionHeader child_header;
    if (!child_agent.session_path[0] || PicoSession_ReadHeader(child_agent.session_path, &child_header) != 0 ||
        child_header.version != 4 || child_header.kind != PICO_AGENT_SUBAGENT ||
        strcmp(child_header.profile, "review") != 0 ||
        strcmp(child_header.initial_purpose, "Review carefully") != 0 ||
        strcmp(child_header.parent_session_id, "parent-session") != 0)
    {
        return Fail("subagent session header did not preserve durable profile metadata");
    }
    PicoAgent main_resume;
    PicoAgent subagent_resume;
    memset(&main_resume, 0, sizeof(main_resume));
    memset(&subagent_resume, 0, sizeof(subagent_resume));
    main_resume.workspace = &writer_ws;
    main_resume.kind = PICO_AGENT_MAIN;
    subagent_resume.workspace = &writer_ws;
    subagent_resume.kind = PICO_AGENT_SUBAGENT;
    if (PicoSession_Replay(&writer, &main_resume, child_agent.session_path, false) == 0 ||
        main_resume.kind != PICO_AGENT_MAIN ||
        PicoSession_Replay(&writer, &subagent_resume, writer_agent.session_path, false) == 0 ||
        subagent_resume.kind != PICO_AGENT_SUBAGENT)
    {
        return Fail("session replay accepted a stored agent kind incompatible with the requested agent");
    }
    int reserves_before_load = g_reserve_calls;
    PicoMessage *loaded = NULL;
    int loaded_n = 0;
    if (PicoSession_LoadTranscript(PicoHost_PrimaryWorkspace(&writer), child_agent.session_id, &loaded,
                                  &loaded_n) != 0 ||
        loaded_n < 2 || !loaded || !loaded[0].source || strcmp(loaded[0].source, "delegated task") != 0 ||
        !loaded[1].source || strcmp(loaded[1].source, "child findings") != 0 ||
        loaded[1].trace_count != 2 || !loaded[1].trace[0].tool_call_id ||
        strcmp(loaded[1].trace[0].tool_call_id, "child-call-1") != 0 ||
        !loaded[1].trace[0].tool_output ||
        strcmp(loaded[1].trace[0].tool_output, "{\"session_id\":\"nested-child\"}") != 0 ||
        !loaded[1].trace[1].tool_call_id ||
        strcmp(loaded[1].trace[1].tool_call_id, "child-call-2") != 0 ||
        !loaded[1].trace[1].tool_output || strcmp(loaded[1].trace[1].tool_output, "ok") != 0 ||
        g_reserve_calls != reserves_before_load)
    {
        PicoMessages_Free(loaded, loaded_n);
        return Fail("read-only child transcript load reserved the session or dropped messages");
    }
    PicoMessages_Free(loaded, loaded_n);

    PicoSessionInfo *listed = NULL;
    int listed_n = PicoSession_List(PicoHost_PrimaryWorkspace(&writer), &listed, true);
    bool parent_offered = listed_n == 1 && listed &&
                          strcmp(listed[0].id, writer_agent.session_id) == 0 &&
                          listed[0].kind == PICO_AGENT_MAIN;
    char child_resolved[4096];
    bool child_by_id = PicoSession_Resolve(PicoHost_PrimaryWorkspace(&writer), child_agent.session_id, false,
                                           child_resolved, sizeof(child_resolved)) == 0;
    free(listed);
    if (!parent_offered || !child_by_id)
    {
        return Fail("/resume listing included a subagent session, or exact child ids stopped resolving");
    }

    PicoHost compacted;
    PicoWorkspace compacted_ws;
    PicoAgent compacted_agent;
    memset(&compacted, 0, sizeof(compacted));
    memset(&compacted_ws, 0, sizeof(compacted_ws));
    memset(&compacted_agent, 0, sizeof(compacted_agent));
    compacted_ws.host = &compacted;
    snprintf(compacted_ws.path, sizeof(compacted_ws.path), "/workspace");
    compacted.workspaces[0] = &compacted_ws;
    compacted.workspace_count = 1;
    compacted_agent.workspace = &compacted_ws;
    PicoModel replay_models[2];
    memset(replay_models, 0, sizeof(replay_models));
    snprintf(replay_models[0].id, sizeof(replay_models[0].id), "default-model");
    snprintf(replay_models[0].name, sizeof(replay_models[0].name), "Default");
    replay_models[0].context_limit = 111;
    snprintf(replay_models[0].default_effort, sizeof(replay_models[0].default_effort), "high");
    snprintf(replay_models[1].id, sizeof(replay_models[1].id), "saved-model");
    snprintf(replay_models[1].name, sizeof(replay_models[1].name), "Saved");
    replay_models[1].context_limit = 222;
    snprintf(replay_models[1].default_effort, sizeof(replay_models[1].default_effort), "low");
    compacted_ws.models = replay_models;
    compacted_ws.model_count = 2;
    snprintf(compacted_agent.model, sizeof(compacted_agent.model), "default-model");
    snprintf(compacted_agent.effort, sizeof(compacted_agent.effort), "high");
    compacted_agent.context_limit = 111;
    RegisterReplayTool(&compacted);
    g_restored_value = 0;
    PicoSession_Start(&compacted, &compacted_agent, PICO_SESSION_NEW, writer_agent.session_path);
    if (compacted_agent.session_input_tokens != 300 || compacted_agent.session_cached_tokens != 170 ||
        compacted_agent.tokens_used != 0 || compacted_agent.tokens_cached != 0 || g_restored_value != 7)
    {
        return Fail("replay did not retain totals, restore the latest valid tool details, or clear compacted usage");
    }
    if (strcmp(compacted_agent.model, "saved-model") != 0 || strcmp(compacted_agent.effort, "low") != 0 ||
        compacted_agent.context_limit != 222 || strcmp(compacted_agent.model_name, "Saved") != 0)
    {
        return Fail("session header did not synchronize model-specific effort and context on the replay target");
    }
    g_restored_value = 0;
    PicoSession_ReplayToolDetails(&compacted, &compacted_agent);
    if (g_restored_value != 7)
    {
        return Fail("details-only replay did not restore extension state after reload");
    }

    if (!AppendRaw(writer_agent.session_path, "{\"type\":\"usage\",\"input_tokens\":50,\"cached_tokens\":-3}") ||
        !AppendRaw(writer_agent.session_path, "{\"type\":\"usage\",\"input_tokens\":-4,\"cached_tokens\":2}") ||
        !AppendRaw(writer_agent.session_path, "{\"type\":\"usage\",\"input_tokens\":10,\"cached_tokens\":20}"))
    {
        return Fail("could not append replay boundary cases");
    }

    PicoHost replayed;
    PicoAgent replayed_agent;
    memset(&replayed, 0, sizeof(replayed));
    memset(&replayed_agent, 0, sizeof(replayed_agent));
    PicoSession_Start(&replayed, &replayed_agent, PICO_SESSION_NEW, writer_agent.session_path);
    if (replayed_agent.session_input_tokens != 360 || replayed_agent.session_cached_tokens != 180 ||
        replayed_agent.tokens_used != 10 || replayed_agent.tokens_cached != 10)
    {
        return Fail("replay did not normalize and aggregate usage events");
    }

    PicoHost opened;
    PicoAgent opened_agent;
    memset(&opened, 0, sizeof(opened));
    memset(&opened_agent, 0, sizeof(opened_agent));
    PicoHost_SetPath(&opened, "/workspace");
    opened_agent.session_input_tokens = 999;
    opened_agent.session_cached_tokens = 999;
    if (PicoSession_Open(&opened, &opened_agent, writer_agent.session_id) != 0 || opened_agent.session_input_tokens != 360 ||
        opened_agent.session_cached_tokens != 180 || opened_agent.tokens_used != 10 || opened_agent.tokens_cached != 10)
    {
        return Fail("session open did not reset and rebuild usage totals");
    }

    replayed.hooks[0] = (PicoHookEntry){.hook = PICO_HOOK_ON_SESSION_RESET, .host_fn = ResetHook};
    replayed.hook_count = 1;
    g_reset_hooks = 0;
    PicoSession_Reset(&replayed, &replayed_agent);
    if (replayed_agent.session_input_tokens != 0 || replayed_agent.session_cached_tokens != 0 ||
        replayed_agent.tokens_used != 0 || replayed_agent.tokens_cached != 0 || g_reset_hooks != 1)
    {
        return Fail("session reset did not clear usage state");
    }

    PicoAgent failed_persistence;
    memset(&failed_persistence, 0, sizeof(failed_persistence));
    failed_persistence.persistence = PICO_SESSION_DURABLE;
    snprintf(failed_persistence.session_id, sizeof(failed_persistence.session_id), "not-resumable");
    snprintf(failed_persistence.session_path, sizeof(failed_persistence.session_path), "/dev/full");
    PicoSessionWriteResult failed_write =
        PicoSession_LogUser(&opened, &failed_persistence, "cannot persist", "cannot persist", NULL);
    if (failed_write != PICO_SESSION_WRITE_FAILED ||
        failed_persistence.persistence != PICO_SESSION_FAILED ||
        strcmp(failed_persistence.session_id, "not-resumable") != 0 ||
        strcmp(failed_persistence.session_path, "/dev/full") != 0 ||
        !strstr(g_status_warning, "no longer resumable"))
    {
        return Fail("persistence failure was not returned and surfaced as non-resumable");
    }
    PicoSession_Reset(&opened, &failed_persistence);
    if (failed_persistence.persistence != PICO_SESSION_DURABLE ||
        failed_persistence.session_id[0] || failed_persistence.session_path[0])
    {
        return Fail("new session did not recover from the prior persistence failure");
    }

    char saved_config_dir[sizeof(g_config_dir)];
    memcpy(saved_config_dir, g_config_dir, sizeof(saved_config_dir));
    memset(g_config_dir, 'x', sizeof(g_config_dir) - 1);
    g_config_dir[sizeof(g_config_dir) - 1] = '\0';
    g_status_warning[0] = '\0';
    PicoHost long_path_app;
    PicoAgent long_path_agent;
    memset(&long_path_app, 0, sizeof(long_path_app));
    memset(&long_path_agent, 0, sizeof(long_path_agent));
    PicoHost_SetPath(&long_path_app, "/workspace");
    long_path_agent.persistence = PICO_SESSION_DURABLE;
    PicoSessionWriteResult long_path_result =
        PicoSession_LogUser(&long_path_app, &long_path_agent, "cannot persist", "cannot persist", NULL);
    memcpy(g_config_dir, saved_config_dir, sizeof(g_config_dir));
    if (long_path_result != PICO_SESSION_WRITE_FAILED ||
        long_path_agent.persistence != PICO_SESSION_FAILED ||
        long_path_agent.session_path[0] ||
        !strstr(g_status_warning, "session directory path is too long"))
    {
        return Fail("overlong session directory did not fail without using a truncated path");
    }

    PicoAgent ephemeral;
    memset(&ephemeral, 0, sizeof(ephemeral));
    ephemeral.persistence = PICO_SESSION_EPHEMERAL;
    if (PicoSession_LogUser(&opened, &ephemeral, "not persisted", "not persisted", NULL) !=
        PICO_SESSION_WRITE_SKIPPED)
    {
        return Fail("ephemeral session did not report a skipped write");
    }

    char bad_kind_path[4096];
    snprintf(bad_kind_path, sizeof(bad_kind_path), "%s/bad-kind.jsonl", temp);
    if (!AppendRaw(bad_kind_path,
                   "{\"type\":\"session\",\"version\":4,\"id\":\"bad\","
                   "\"kind\":\"unknown\",\"model\":\"saved-model\"}"))
    {
        return Fail("could not create invalid version 4 header");
    }
    PicoSessionHeader invalid_header;
    PicoAgent bad_kind_agent;
    memset(&bad_kind_agent, 0, sizeof(bad_kind_agent));
    if (PicoSession_ReadHeader(bad_kind_path, &invalid_header) == 0 ||
        PicoSession_Replay(&opened, &bad_kind_agent, bad_kind_path, false) == 0)
    {
        return Fail("version 4 session accepted an unknown agent kind");
    }

    char missing_group_path[4096];
    snprintf(missing_group_path, sizeof(missing_group_path), "%s/missing-group.jsonl", temp);
    if (!AppendRaw(missing_group_path,
                   "{\"type\":\"session\",\"version\":4,\"id\":\"missing-group\","
                   "\"kind\":\"normal\",\"model\":\"saved-model\"}") ||
        !AppendRaw(missing_group_path,
                   "{\"type\":\"message\",\"role\":\"assistant\",\"content\":\"lost\"}") ||
        PicoSession_ReadHeader(missing_group_path, &invalid_header) != 0 ||
        PicoSession_Replay(&opened, &bad_kind_agent, missing_group_path, false) == 0)
    {
        return Fail("version 4 session accepted an assistant event without message_group");
    }

    char typed_group_path[4096];
    snprintf(typed_group_path, sizeof(typed_group_path), "%s/typed-group.jsonl", temp);
    if (!AppendRaw(typed_group_path,
                   "{\"type\":\"session\",\"version\":4,\"id\":\"typed-group\","
                   "\"kind\":\"normal\",\"model\":\"saved-model\"}") ||
        !AppendRaw(typed_group_path,
                   "{\"type\":\"message\",\"role\":\"assistant\","
                   "\"message_group\":\"1\",\"content\":\"lost\"}") ||
        PicoSession_Replay(&opened, &bad_kind_agent, typed_group_path, false) == 0)
    {
        return Fail("version 4 session accepted a non-integer assistant message_group");
    }

    char typed_tool_group_path[4096];
    snprintf(typed_tool_group_path, sizeof(typed_tool_group_path), "%s/typed-tool-group.jsonl", temp);
    if (!AppendRaw(typed_tool_group_path,
                   "{\"type\":\"session\",\"version\":4,\"id\":\"typed-tool-group\","
                   "\"kind\":\"normal\",\"model\":\"saved-model\"}") ||
        !AppendRaw(typed_tool_group_path,
                   "{\"type\":\"tool_call\",\"message_group\":true,"
                   "\"call_id\":\"call\",\"name\":\"sh\",\"arguments\":\"{}\"}") ||
        PicoSession_Replay(&opened, &bad_kind_agent, typed_tool_group_path, false) == 0)
    {
        return Fail("version 4 session accepted a non-integer tool message_group");
    }

    char incomplete_child_path[4096];
    snprintf(incomplete_child_path, sizeof(incomplete_child_path), "%s/incomplete-child.jsonl", temp);
    if (!AppendRaw(incomplete_child_path,
                   "{\"type\":\"session\",\"version\":4,\"id\":\"bad-child\","
                   "\"kind\":\"subagent\",\"profile\":\"review\","
                   "\"model\":\"saved-model\"}") ||
        PicoSession_ReadHeader(incomplete_child_path, &invalid_header) == 0)
    {
        return Fail("subagent header without durable purpose metadata was accepted");
    }

    char empty_path[4096];
    snprintf(empty_path, sizeof(empty_path), "%s/empty.jsonl", temp);
    FILE *empty = fopen(empty_path, "wb");
    if (empty)
    {
        fclose(empty);
    }
    PicoAgent invalid_agent;
    memset(&invalid_agent, 0, sizeof(invalid_agent));
    if (PicoSession_Replay(&opened, &invalid_agent, empty_path, false) == 0)
    {
        return Fail("empty session replay was accepted");
    }

    unlink(empty_path);
    unlink(bad_kind_path);
    unlink(missing_group_path);
    unlink(typed_group_path);
    unlink(typed_tool_group_path);
    unlink(incomplete_child_path);
    unlink(child_agent.session_path);
    unlink(writer_agent.session_path);
    if (TestThinkingRoundTrip() != 0 || TestPartsReplay() != 0 ||
        TestTranscriptMessageGroups() != 0 || TestSessionTitle() != 0 ||
        TestSessionTitleFailureStages() != 0 || TestSessionTitleUtf8() != 0 ||
        TestConcurrentAppendDuringTitle() != 0 || TestConcurrentDoneCatalog() != 0 ||
        TestCatalog() != 0 ||
        TestCatalogListingCache() != 0 || TestSessionListCompleteness() != 0 ||
        TestUnseenCompleteRoundTrip() != 0 ||
        TestCatalogOmitsMissingPath() != 0 || TestModelResumeReplay() != 0 ||
        TestQueuedModelChangeOrdersUserWrite() != 0 || TestQueuedModelChangeFailureThenDrain() != 0 ||
        TestQueuedModelChangeCreatesHeaderPerAgent() != 0 ||
        TestQueuedModelChangeDrainDeadline() != 0)
    {
        return 1;
    }
    return 0;
}
