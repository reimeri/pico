#include "pico/plugin.h"
#include "host_internal.h"

#include "agent_manager.h"
#include "json.h"

#include <stdlib.h>
#include <string.h>

static const char *kSubagentParams =
    "{\"type\":\"object\",\"properties\":{"
    "\"profile\":{\"type\":\"string\",\"description\":\"Discovered named subagent profile\"},"
    "\"task\":{\"type\":\"string\",\"description\":\"Task delegated to the child agent\"},"
    "\"session_id\":{\"type\":\"string\",\"description\":\"Optional exact previous child session ID\"}},"
    "\"required\":[\"profile\",\"task\"]}";

static void SubagentRun(PicoAgentContext *ctx, const char *args_json, PicoToolResult *out, void *state)
{
    (void)state;
    if (out)
    {
        memset(out, 0, sizeof(*out));
    }
    JsonDoc doc;
    if (!args_json || JsonParse(&doc, args_json, strlen(args_json)) != 0 || !JsonIsObject(&doc, 0))
    {
        if (out)
        {
            out->output = JsonDup("subagent: arguments must be a JSON object");
            out->is_error = true;
        }
        return;
    }
    char *profile = JsonObjStr(&doc, 0, "profile");
    char *task = JsonObjStr(&doc, 0, "task");
    char *session_id = JsonObjStr(&doc, 0, "session_id");
    bool wrong_session_type = JsonObjGet(&doc, 0, "session_id") >= 0 && !session_id;
    JsonFree(&doc);
    if (!profile || !profile[0] || !task || !task[0] || wrong_session_type)
    {
        if (out)
        {
            out->output = JsonDup("subagent: profile and task are required strings; session_id must be a string when present");
            out->is_error = true;
        }
        free(profile);
        free(task);
        free(session_id);
        return;
    }
    bool is_error = false;
    char *result = PicoAgentManager_Delegate(ctx, profile, task, session_id, &is_error);
    if (out)
    {
        out->output = result ? result : JsonDup("subagent: delegation failed");
        out->is_error = is_error || !result;
    }
    else
    {
        free(result);
    }
    free(profile);
    free(task);
    free(session_id);
}

static void SubagentGuidance(PicoWorkspace *workspace, PicoAgentId agent_id, PicoLlmEvent *event, void *state)
{
    PicoHost *app = workspace ? workspace->host : NULL;
    (void)state;
    (void)agent_id;
    bool offered = false;
    for (int i = 0; event && event->include_tools && i < event->tool_count; i++)
    {
        if (event->tools[i].name && strcmp(event->tools[i].name, "subagent") == 0 &&
            (!event->exclude || !event->exclude[i]))
        {
            offered = true;
            break;
        }
    }
    if (!offered)
    {
        return;
    }
    JsonBuf b;
    JsonBuf_Init(&b);
    JsonBuf_Puts(&b, "The subagent tool delegates synchronously using one of these named profiles:");
    int count = pico_subagent_profile_count(app);
    for (int i = 0; i < count; i++)
    {
        PicoSubagentProfileInfo profile;
        if (!pico_subagent_profile_info(app, i, &profile))
        {
            continue;
        }
        JsonBuf_Puts(&b, "\n- ");
        JsonBuf_Puts(&b, profile.name);
        if (profile.description[0])
        {
            JsonBuf_Puts(&b, ": ");
            JsonBuf_Puts(&b, profile.description);
        }
    }
    if (count == 0)
    {
        JsonBuf_Puts(&b, "\n- (none configured)");
    }
    event->extra_instructions = JsonBuf_Steal(&b);
}

static int SubagentInit(PicoHost *app, void **state_out)
{
    (void)state_out;
    pico_add_tool(PicoHost_PrimaryWorkspace(app), "subagent",
                  "Delegate a task synchronously to a discovered named subagent profile",
                  kSubagentParams, SubagentRun, NULL);
    pico_add_llm_hook(PicoHost_PrimaryWorkspace(app), SubagentGuidance);
    return 0;
}

PicoExt pico_ext_subagent(void)
{
    return (PicoExt){
        .abi = PICO_EXT_ABI,
        .name = "subagent",
        .description = "Named synchronous subagent delegation",
        .host_init = SubagentInit,
    };
}
