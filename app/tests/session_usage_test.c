#define _POSIX_C_SOURCE 200809L

#include "agent.h"
#include "json.h"
#include "path.h"
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
static char g_status_warning[512];

static int Fail(const char *message)
{
    fprintf(stderr, "session usage: %s\n", message);
    return 1;
}

bool Pico_ConfigDir(char *out, size_t cap)
{
    return PicoPath_Format(out, cap, "%s", g_config_dir);
}

bool PicoAgentManager_ReserveSession(PicoAgentManager *manager, PicoAgentId owner, const char *path)
{
    (void)manager; (void)owner; (void)path;
    return true;
}

void PicoAgentManager_ReleaseSessions(PicoAgentManager *manager, PicoAgentId owner)
{
    (void)manager; (void)owner;
}

bool PicoAgentManager_SessionReserved(const PicoAgentManager *manager, const char *path,
                                      PicoAgentId except_owner)
{
    (void)manager; (void)path; (void)except_owner;
    return false;
}

void pico_status_warn(PicoApp *app, const char *msg)
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

void pico_run_hooks(PicoApp *app, PicoHook hook, PicoAgentId agent_id)
{
    if (!app)
    {
        return;
    }
    PicoHookEvent event = {.hook = hook, .agent_id = agent_id};
    for (int i = 0; i < app->hook_count; i++)
    {
        if (app->hooks[i].hook == hook && app->hooks[i].fn)
        {
            app->hooks[i].fn(app, &event);
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

static void ResetHook(PicoApp *app, const PicoHookEvent *event)
{
    (void)app;
    (void)event;
    g_reset_hooks++;
}

static void ReplayTool(PicoAgentContext *ctx, const char *args_json, PicoToolResult *out)
{
    (void)ctx;
    (void)args_json;
    (void)out;
}

static bool ReplayApply(PicoApp *app, PicoAgentId agent_id, const char *details_json, bool replay)
{
    (void)app;
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
    writer_agent.persistence = PICO_SESSION_DURABLE;
    snprintf(writer_agent.model, sizeof(writer_agent.model), "saved-model");
    snprintf(writer.workspace, sizeof(writer.workspace), "/workspace");
    snprintf(writer.settings.model, sizeof(writer.settings.model), "default-model");
    PicoSession_LogUsage(&writer, &writer_agent, 100, 20);
    PicoSession_LogUsage(&writer, &writer_agent, 200, 150);
    PicoSession_LogAssistant(&writer, &writer_agent, "assistant response");
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
    if (!file || !strstr(file, "\"version\":3") || !strstr(file, "\"kind\":\"normal\"") ||
        !strstr(file, "\"type\":\"usage\"") || strstr(file, "\"usage\":{"))
    {
        free(file);
        return Fail("session schema did not use version 3 headers and dedicated usage events");
    }
    free(file);

    PicoAgent child_agent;
    memset(&child_agent, 0, sizeof(child_agent));
    child_agent.persistence = PICO_SESSION_DURABLE;
    child_agent.kind = PICO_AGENT_SUBAGENT;
    snprintf(child_agent.model, sizeof(child_agent.model), "saved-model");
    snprintf(child_agent.profile, sizeof(child_agent.profile), "review");
    snprintf(child_agent.purpose, sizeof(child_agent.purpose), "Review carefully");
    snprintf(child_agent.parent_session_id, sizeof(child_agent.parent_session_id), "parent-session");
    PicoSession_LogUser(&writer, &child_agent, "delegated task", "delegated task");
    PicoSessionHeader child_header;
    if (!child_agent.session_path[0] || PicoSession_ReadHeader(child_agent.session_path, &child_header) != 0 ||
        child_header.version != 3 || child_header.kind != PICO_AGENT_SUBAGENT ||
        strcmp(child_header.profile, "review") != 0 ||
        strcmp(child_header.initial_purpose, "Review carefully") != 0 ||
        strcmp(child_header.parent_session_id, "parent-session") != 0)
    {
        return Fail("subagent session header did not preserve durable profile metadata");
    }

    PicoApp compacted;
    PicoAgent compacted_agent;
    memset(&compacted, 0, sizeof(compacted));
    memset(&compacted_agent, 0, sizeof(compacted_agent));
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

    PicoApp replayed;
    PicoAgent replayed_agent;
    memset(&replayed, 0, sizeof(replayed));
    memset(&replayed_agent, 0, sizeof(replayed_agent));
    PicoSession_Start(&replayed, &replayed_agent, PICO_SESSION_NEW, writer_agent.session_path);
    if (replayed_agent.session_input_tokens != 360 || replayed_agent.session_cached_tokens != 180 ||
        replayed_agent.tokens_used != 10 || replayed_agent.tokens_cached != 10)
    {
        return Fail("replay did not normalize and aggregate usage events");
    }

    PicoApp opened;
    PicoAgent opened_agent;
    memset(&opened, 0, sizeof(opened));
    memset(&opened_agent, 0, sizeof(opened_agent));
    snprintf(opened.workspace, sizeof(opened.workspace), "/workspace");
    opened_agent.session_input_tokens = 999;
    opened_agent.session_cached_tokens = 999;
    if (PicoSession_Open(&opened, &opened_agent, writer_agent.session_id) != 0 || opened_agent.session_input_tokens != 360 ||
        opened_agent.session_cached_tokens != 180 || opened_agent.tokens_used != 10 || opened_agent.tokens_cached != 10)
    {
        return Fail("session open did not reset and rebuild usage totals");
    }

    replayed.hooks[0] = (PicoHookEntry){.hook = PICO_HOOK_ON_SESSION_RESET, .fn = ResetHook};
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
        PicoSession_LogUser(&opened, &failed_persistence, "cannot persist", "cannot persist");
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
    PicoApp long_path_app;
    PicoAgent long_path_agent;
    memset(&long_path_app, 0, sizeof(long_path_app));
    memset(&long_path_agent, 0, sizeof(long_path_agent));
    snprintf(long_path_app.workspace, sizeof(long_path_app.workspace), "/workspace");
    long_path_agent.persistence = PICO_SESSION_DURABLE;
    PicoSessionWriteResult long_path_result =
        PicoSession_LogUser(&long_path_app, &long_path_agent, "cannot persist", "cannot persist");
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
    if (PicoSession_LogUser(&opened, &ephemeral, "not persisted", "not persisted") !=
        PICO_SESSION_WRITE_SKIPPED)
    {
        return Fail("ephemeral session did not report a skipped write");
    }

    char bad_kind_path[4096];
    snprintf(bad_kind_path, sizeof(bad_kind_path), "%s/bad-kind.jsonl", temp);
    if (!AppendRaw(bad_kind_path,
                   "{\"type\":\"session\",\"version\":3,\"id\":\"bad\","
                   "\"kind\":\"unknown\",\"model\":\"saved-model\"}"))
    {
        return Fail("could not create invalid version 3 header");
    }
    PicoSessionHeader invalid_header;
    PicoAgent bad_kind_agent;
    memset(&bad_kind_agent, 0, sizeof(bad_kind_agent));
    if (PicoSession_ReadHeader(bad_kind_path, &invalid_header) == 0 ||
        PicoSession_Replay(&opened, &bad_kind_agent, bad_kind_path, false) == 0)
    {
        return Fail("version 3 session accepted an unknown agent kind");
    }

    char incomplete_child_path[4096];
    snprintf(incomplete_child_path, sizeof(incomplete_child_path), "%s/incomplete-child.jsonl", temp);
    if (!AppendRaw(incomplete_child_path,
                   "{\"type\":\"session\",\"version\":3,\"id\":\"bad-child\","
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
    unlink(incomplete_child_path);
    unlink(child_agent.session_path);
    unlink(writer_agent.session_path);
    return 0;
}
