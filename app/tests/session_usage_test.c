#define _POSIX_C_SOURCE 200809L

#include "agent.h"
#include "json.h"
#include "session.h"
#include "settings.h"
#include "usage.h"

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static char g_config_dir[4096];
static int g_restored_value;
static int g_reset_hooks;

static int Fail(const char *message)
{
    fprintf(stderr, "session usage: %s\n", message);
    return 1;
}

void Pico_ConfigDir(char *out, size_t cap)
{
    snprintf(out, cap, "%s", g_config_dir);
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

void pico_run_hooks(PicoApp *app, PicoHook hook)
{
    if (!app)
    {
        return;
    }
    for (int i = 0; i < app->hook_count; i++)
    {
        if (app->hooks[i].hook == hook && app->hooks[i].fn)
        {
            app->hooks[i].fn(app);
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

void PicoAgent_PushHistoryAssistant(PicoAgent *agent, const char *text)
{
    (void)agent;
    (void)text;
}

void PicoAgent_PushHistoryFunctionCall(PicoAgent *agent, const char *call_id, const char *name, const char *args)
{
    (void)agent;
    (void)call_id;
    (void)name;
    (void)args;
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

void PicoApp_AddMessage(PicoApp *app, PicoRole role, const char *text)
{
    (void)app;
    (void)role;
    (void)text;
}

void PicoApp_AppendAssistant(PicoApp *app, const char *text)
{
    (void)app;
    (void)text;
}

void PicoApp_AddToolCall(PicoApp *app, const char *name, const char *args_json)
{
    (void)app;
    (void)name;
    (void)args_json;
}

void PicoApp_SetLastToolOutput(PicoApp *app, const char *output, bool is_error)
{
    (void)app;
    (void)output;
    (void)is_error;
}

void PicoApp_ClearMessages(PicoApp *app)
{
    (void)app;
}

void PicoAgent_AddMessage(PicoApp *app, PicoAgent *agent, PicoRole role, const char *text)
{
    (void)app; (void)agent; (void)role; (void)text;
}

void PicoAgent_AppendAssistant(PicoApp *app, PicoAgent *agent, const char *text)
{
    (void)app; (void)agent; (void)text;
}

void PicoAgent_AddToolCall(PicoApp *app, PicoAgent *agent, const char *name, const char *args_json)
{
    (void)app; (void)agent; (void)name; (void)args_json;
}

void PicoAgent_SetLastToolOutput(PicoAgent *agent, const char *output, bool is_error)
{
    (void)agent; (void)output; (void)is_error;
}

void PicoAgent_ClearMessages(PicoAgent *agent)
{
    (void)agent;
}

PicoModel *PicoSettings_ActiveModel(PicoApp *app, const PicoAgent *agent)
{
    if (!app || !agent) return NULL;
    for (int i = 0; i < app->model_count; i++)
    {
        if (strcmp(app->models[i].id, agent->model) == 0) return &app->models[i];
    }
    return NULL;
}

void PicoSettings_SyncAgent(const PicoApp *app, PicoAgent *agent)
{
    PicoModel *model = PicoSettings_ActiveModel((PicoApp *)app, agent);
    if (!agent) return;
    snprintf(agent->model_name, sizeof(agent->model_name), "%s",
             model && model->name[0] ? model->name : agent->model);
    agent->context_limit = model ? model->context_limit : 0;
    if (!agent->effort[0])
    {
        snprintf(agent->effort, sizeof(agent->effort), "%s",
                 model && model->default_effort[0] ? model->default_effort : "none");
    }
}

static void ResetHook(PicoApp *app)
{
    (void)app;
    g_reset_hooks++;
}

static void ReplayTool(PicoApp *app, const char *args_json, PicoToolResult *out)
{
    (void)app;
    (void)args_json;
    (void)out;
}

static bool ReplayApply(PicoApp *app, const char *details_json, bool replay)
{
    (void)app;
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

static void RegisterReplayTool(PicoApp *app)
{
    app->tools[0] = (PicoTool){.name = "state_test", .run = ReplayTool, .apply = ReplayApply};
    app->tool_count = 1;
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

int main(void)
{
    char temp[] = "/tmp/pico-session-usage-XXXXXX";
    if (!mkdtemp(temp))
    {
        return Fail("could not create temporary directory");
    }
    snprintf(g_config_dir, sizeof(g_config_dir), "%s", temp);

    PicoApp writer;
    PicoAgent writer_agent;
    memset(&writer, 0, sizeof(writer));
    memset(&writer_agent, 0, sizeof(writer_agent));
    writer.agent = &writer_agent;
    writer_agent.persistence = PICO_SESSION_DURABLE;
    snprintf(writer_agent.model, sizeof(writer_agent.model), "saved-model");
    snprintf(writer.workspace, sizeof(writer.workspace), "/workspace");
    snprintf(writer.settings.model, sizeof(writer.settings.model), "default-model");
    PicoSession_LogUsage(&writer, writer.agent, 100, 20);
    PicoSession_LogUsage(&writer, writer.agent, 200, 150);
    PicoSession_LogAssistant(&writer, writer.agent, "assistant response");
    PicoSession_LogCompaction(&writer, writer.agent, "brief", 200);
    PicoSession_LogToolResult(&writer, writer.agent, "state-1", "state_test", "saved", false, "{\"value\":7}");
    PicoSession_LogToolResult(&writer, writer.agent, "state-2", "state_test", "failed", true, "{\"value\":8}");
    PicoSession_LogToolResult(&writer, writer.agent, "state-3", "state_test", "bad snapshot", false, "{\"value\":99}");
    if (!writer.agent->session_path[0])
    {
        return Fail("usage did not create a session file");
    }

    size_t file_len = 0;
    char *file = Pico_ReadFile(writer.agent->session_path, &file_len);
    if (!file || !strstr(file, "\"version\":2") || !strstr(file, "\"type\":\"usage\"") ||
        strstr(file, "\"usage\":{"))
    {
        free(file);
        return Fail("session schema did not use dedicated version 2 usage events");
    }
    free(file);

    PicoApp compacted;
    PicoAgent compacted_agent;
    memset(&compacted, 0, sizeof(compacted));
    memset(&compacted_agent, 0, sizeof(compacted_agent));
    compacted.agent = &compacted_agent;
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
    compacted.models = replay_models;
    compacted.model_count = 2;
    snprintf(compacted_agent.model, sizeof(compacted_agent.model), "default-model");
    snprintf(compacted_agent.effort, sizeof(compacted_agent.effort), "high");
    compacted_agent.context_limit = 111;
    RegisterReplayTool(&compacted);
    g_restored_value = 0;
    PicoSession_Start(&compacted, compacted.agent, PICO_SESSION_NEW, writer.agent->session_path);
    if (compacted.agent->session_input_tokens != 300 || compacted.agent->session_cached_tokens != 170 ||
        compacted.agent->tokens_used != 0 || compacted.agent->tokens_cached != 0 || g_restored_value != 7)
    {
        return Fail("replay did not retain totals, restore the latest valid tool details, or clear compacted usage");
    }
    if (strcmp(compacted_agent.model, "saved-model") != 0 || strcmp(compacted_agent.effort, "low") != 0 ||
        compacted_agent.context_limit != 222 || strcmp(compacted_agent.model_name, "Saved") != 0)
    {
        return Fail("session header did not synchronize model-specific effort and context on the replay target");
    }
    g_restored_value = 0;
    PicoSession_ReplayToolDetails(&compacted, compacted.agent);
    if (g_restored_value != 7)
    {
        return Fail("details-only replay did not restore extension state after reload");
    }

    if (!AppendRaw(writer.agent->session_path, "{\"type\":\"usage\",\"input_tokens\":50,\"cached_tokens\":-3}") ||
        !AppendRaw(writer.agent->session_path, "{\"type\":\"usage\",\"input_tokens\":-4,\"cached_tokens\":2}") ||
        !AppendRaw(writer.agent->session_path, "{\"type\":\"usage\",\"input_tokens\":10,\"cached_tokens\":20}"))
    {
        return Fail("could not append replay boundary cases");
    }

    PicoApp replayed;
    PicoAgent replayed_agent;
    memset(&replayed, 0, sizeof(replayed));
    memset(&replayed_agent, 0, sizeof(replayed_agent));
    replayed.agent = &replayed_agent;
    PicoSession_Start(&replayed, replayed.agent, PICO_SESSION_NEW, writer.agent->session_path);
    if (replayed.agent->session_input_tokens != 360 || replayed.agent->session_cached_tokens != 180 ||
        replayed.agent->tokens_used != 10 || replayed.agent->tokens_cached != 10)
    {
        return Fail("replay did not normalize and aggregate usage events");
    }

    PicoApp opened;
    PicoAgent opened_agent;
    memset(&opened, 0, sizeof(opened));
    memset(&opened_agent, 0, sizeof(opened_agent));
    opened.agent = &opened_agent;
    snprintf(opened.workspace, sizeof(opened.workspace), "/workspace");
    opened.agent->session_input_tokens = 999;
    opened.agent->session_cached_tokens = 999;
    if (PicoSession_Open(&opened, opened.agent, writer.agent->session_id) != 0 || opened.agent->session_input_tokens != 360 ||
        opened.agent->session_cached_tokens != 180 || opened.agent->tokens_used != 10 || opened.agent->tokens_cached != 10)
    {
        return Fail("session open did not reset and rebuild usage totals");
    }

    replayed.hooks[0] = (PicoHookEntry){.hook = PICO_HOOK_ON_SESSION_RESET, .fn = ResetHook};
    replayed.hook_count = 1;
    g_reset_hooks = 0;
    PicoSession_Reset(&replayed, replayed.agent);
    if (replayed.agent->session_input_tokens != 0 || replayed.agent->session_cached_tokens != 0 ||
        replayed.agent->tokens_used != 0 || replayed.agent->tokens_cached != 0 || g_reset_hooks != 1)
    {
        return Fail("session reset did not clear usage state");
    }

    unlink(writer.agent->session_path);
    return 0;
}
