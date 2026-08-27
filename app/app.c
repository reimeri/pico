#include "pico/plugin.h"
#include "pico/md_view.h"
#include "agent.h"
#include "agent_manager.h"
#include "session.h"
#include "settings.h"
#include "docs_path.h"
#include "auth.h"
#include "chat_sel.h"
#include "canonical.h"
#include "composer_internal.h"
#include "json.h"
#include "overlay.h"
#include "scrollbar.h"
#include "builtins/chat.h"
#include "builtins/todo.h"

#include "clay/clay.h"

#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

void Clay_Raylib_Render(Clay_RenderCommandArray renderCommands, Font *fonts);

static bool g_pico_process_retired;

bool PicoApp_ProcessRetired(void)
{
    return g_pico_process_retired;
}

void pico_add_view(PicoApp *app, PicoUiSlot slot, int z, PicoViewFn render)
{
    if (slot < 0 || slot >= PICO_SLOT_COUNT || !render)
    {
        return;
    }
    int n = app->view_count[slot];
    if (n >= PICO_MAX_SLOT_VIEWS)
    {
        return;
    }
    int i = n;
    while (i > 0 && app->views[slot][i - 1].z > z)
    {
        app->views[slot][i] = app->views[slot][i - 1];
        i--;
    }
    app->views[slot][i].render = render;
    app->views[slot][i].z = z;
    app->view_count[slot]++;
}

void pico_add_empty_view(PicoApp *app, PicoEmptyKind kind, int z, PicoViewFn render)
{
    if (!app || !render)
    {
        return;
    }
    if (kind != PICO_EMPTY_ABOVE && kind != PICO_EMPTY_BELOW && kind != PICO_EMPTY_REPLACE)
    {
        return;
    }
    int n = app->empty_view_count;
    if (n >= PICO_MAX_EMPTY_VIEWS)
    {
        return;
    }
    int i = n;
    while (i > 0 && app->empty_views[i - 1].z > z)
    {
        app->empty_views[i] = app->empty_views[i - 1];
        i--;
    }
    app->empty_views[i].render = render;
    app->empty_views[i].kind = kind;
    app->empty_views[i].z = z;
    app->empty_view_count++;
}

void pico_add_hook(PicoApp *app, PicoHook hook, PicoHookFn fn)
{
    if (!fn || app->hook_count >= PICO_MAX_HOOKS)
    {
        return;
    }
    app->hooks[app->hook_count].hook = hook;
    app->hooks[app->hook_count].fn = fn;
    app->hook_count++;
}

void pico_add_tool_before_hook(PicoApp *app, PicoToolBeforeFn fn)
{
    if (!app || !fn || app->tool_before_hook_count >= PICO_MAX_TOOL_HOOKS)
    {
        return;
    }
    app->tool_before_hooks[app->tool_before_hook_count++] = fn;
}

void pico_add_tool_after_hook(PicoApp *app, PicoToolAfterFn fn)
{
    if (!app || !fn || app->tool_after_hook_count >= PICO_MAX_TOOL_HOOKS)
    {
        return;
    }
    app->tool_after_hooks[app->tool_after_hook_count++] = fn;
}

void pico_add_llm_hook(PicoApp *app, PicoLlmHookFn fn)
{
    if (!app || !fn || app->llm_hook_count >= PICO_MAX_LLM_HOOKS)
    {
        return;
    }
    app->llm_hooks[app->llm_hook_count] = fn;
    app->llm_hook_count++;
}

void pico_add_context_hook(PicoApp *app, PicoContextHookFn fn)
{
    if (!app || !fn || app->context_hook_count >= PICO_MAX_CONTEXT_HOOKS)
    {
        return;
    }
    app->context_hooks[app->context_hook_count++] = fn;
}

void pico_status_warn(PicoApp *app, const char *msg)
{
    if (!app || !msg || !msg[0])
    {
        return;
    }
    size_t extra = strlen(msg) + 2;
    size_t old = app->status_warn ? strlen(app->status_warn) : 0;
    char *next = (char *)realloc(app->status_warn, old + extra);
    if (!next)
    {
        return;
    }
    app->status_warn = next;
    memcpy(app->status_warn + old, msg, extra - 1);
    app->status_warn[old + extra - 2] = '\n';
    app->status_warn[old + extra - 1] = '\0';
}

static const char *ToolParamsError(const char *params_json)
{
    if (!params_json || !params_json[0])
    {
        return NULL;
    }
    size_t len = strlen(params_json);
    if (!JsonValidSyntax(params_json, len))
    {
        return "params_json is not valid JSON";
    }
    JsonDoc doc;
    if (JsonParse(&doc, params_json, len) != 0)
    {
        return "params_json is not valid JSON";
    }
    bool valid = JsonIsObject(&doc, 0) && JsonSkip(&doc, 0) == doc.ntoks;
    JsonFree(&doc);
    return valid ? NULL : "params_json must be a JSON object";
}

static void ToolAddFail(PicoApp *app, const char *name, const char *reason)
{
    char line[1024];
    if (name && name[0])
    {
        snprintf(line, sizeof(line), "tool \"%s\": %s", name, reason);
    }
    else
    {
        snprintf(line, sizeof(line), "tool: %s", reason);
    }
    pico_status_warn(app, line);
}

bool pico_add_tool(PicoApp *app, const char *name, const char *description, const char *params_json,
                   PicoToolFn run, PicoToolApplyFn apply)
{
    if (!app)
    {
        return false;
    }
    if (!name || !name[0])
    {
        ToolAddFail(app, name, "missing name");
        return false;
    }
    if (!run)
    {
        ToolAddFail(app, name, "missing run function");
        return false;
    }
    const char *params_err = ToolParamsError(params_json);
    if (params_err)
    {
        ToolAddFail(app, name, params_err);
        return false;
    }
    if (app->tool_count >= PICO_MAX_TOOLS)
    {
        char reason[64];
        snprintf(reason, sizeof(reason), "tool limit reached (%d)", PICO_MAX_TOOLS);
        ToolAddFail(app, name, reason);
        return false;
    }
    for (int i = 0; i < app->tool_count; i++)
    {
        if (app->tools[i].name && strcmp(app->tools[i].name, name) == 0)
        {
            ToolAddFail(app, name, "already registered");
            return false;
        }
    }
    app->tools[app->tool_count].name = name;
    app->tools[app->tool_count].description = description;
    app->tools[app->tool_count].params_json = params_json;
    app->tools[app->tool_count].run = run;
    app->tools[app->tool_count].apply = apply;
    app->tool_count++;
    return true;
}

void pico_add_command(PicoApp *app, const char *name, const char *help, PicoCmdFn run)
{
    if (!name || !run || app->command_count >= PICO_MAX_COMMANDS)
    {
        return;
    }
    app->commands[app->command_count].name = name;
    app->commands[app->command_count].help = help;
    app->commands[app->command_count].run = run;
    app->command_count++;
}

void pico_add_completer(PicoApp *app, char trigger, bool bol_only, PicoCompleteQueryFn query,
                        PicoCompleteAcceptFn accept)
{
    if (!query || app->completer_count >= PICO_MAX_COMPLETERS)
    {
        return;
    }
    app->completers[app->completer_count].trigger = trigger;
    app->completers[app->completer_count].bol_only = bol_only;
    app->completers[app->completer_count].query = query;
    app->completers[app->completer_count].accept = accept;
    app->completer_count++;
}

void pico_add_provider(PicoApp *app, const PicoProvider *p)
{
    if (!app || !p || !p->name || !p->name[0] || !p->stream || app->provider_count >= PICO_MAX_PROVIDERS)
    {
        return;
    }
    app->providers[app->provider_count] = *p;
    app->provider_count++;
}

const PicoProvider *pico_find_provider(const PicoApp *app, const char *name)
{
    if (!app || !name || !name[0])
    {
        return NULL;
    }
    for (int i = 0; i < app->provider_count; i++)
    {
        if (app->providers[i].name && strcmp(app->providers[i].name, name) == 0)
        {
            return &app->providers[i];
        }
    }
    return NULL;
}

void pico_clear_registrations(PicoApp *app)
{
    memset(app->views, 0, sizeof(app->views));
    memset(app->view_count, 0, sizeof(app->view_count));
    memset(app->empty_views, 0, sizeof(app->empty_views));
    app->empty_view_count = 0;
    memset(app->hooks, 0, sizeof(app->hooks));
    app->hook_count = 0;
    memset(app->tool_before_hooks, 0, sizeof(app->tool_before_hooks));
    app->tool_before_hook_count = 0;
    memset(app->tool_after_hooks, 0, sizeof(app->tool_after_hooks));
    app->tool_after_hook_count = 0;
    memset(app->llm_hooks, 0, sizeof(app->llm_hooks));
    app->llm_hook_count = 0;
    memset(app->context_hooks, 0, sizeof(app->context_hooks));
    app->context_hook_count = 0;
    memset(app->tool_row_hooks, 0, sizeof(app->tool_row_hooks));
    app->tool_row_hook_count = 0;
    memset(app->tools, 0, sizeof(app->tools));
    app->tool_count = 0;
    memset(app->commands, 0, sizeof(app->commands));
    app->command_count = 0;
    memset(app->completers, 0, sizeof(app->completers));
    app->completer_count = 0;
    memset(app->providers, 0, sizeof(app->providers));
    app->provider_count = 0;
    memset(app->auths, 0, sizeof(app->auths));
    app->auth_count = 0;
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

static char LayoutLetter(int key)
{
    const char *name = glfwGetKeyName(key, 0);
    if (name && name[0] && (unsigned char)name[0] < 128 && name[1] == '\0')
    {
        char c = name[0];
        if (c >= 'A' && c <= 'Z')
        {
            c = (char)(c - 'A' + 'a');
        }
        if (c >= 'a' && c <= 'z')
        {
            return c;
        }
    }
    if (key >= KEY_A && key <= KEY_Z)
    {
        return (char)(key - KEY_A + 'a');
    }
    return 0;
}

static bool LayoutKeyHit(char letter, bool include_repeat)
{
    if (letter >= 'A' && letter <= 'Z')
    {
        letter = (char)(letter - 'A' + 'a');
    }
    if (letter < 'a' || letter > 'z')
    {
        return false;
    }
    for (int key = KEY_A; key <= KEY_Z; key++)
    {
        if (!IsKeyPressed(key) && !(include_repeat && IsKeyPressedRepeat(key)))
        {
            continue;
        }
        if (LayoutLetter(key) == letter)
        {
            return true;
        }
    }
    return false;
}

bool Pico_ShortcutPressed(char letter)
{
    return LayoutKeyHit(letter, false);
}

bool Pico_ShortcutRepeat(char letter)
{
    return LayoutKeyHit(letter, true);
}

static void RunSlot(PicoApp *app, PicoUiSlot slot)
{
    for (int i = 0; i < app->view_count[slot]; i++)
    {
        if (app->views[slot][i].render)
        {
            app->views[slot][i].render(app);
        }
    }
}

void PicoAgent_AddMessage(PicoApp *app, PicoAgent *agent, PicoRole role, const char *markdown)
{
    if (!app || !agent)
    {
        return;
    }
    if (agent->message_count >= agent->message_capacity)
    {
        int capacity = agent->message_capacity == 0 ? 8 : agent->message_capacity * 2;
        PicoMessage *next = (PicoMessage *)realloc(agent->messages, (size_t)capacity * sizeof(PicoMessage));
        if (!next)
        {
            return;
        }
        agent->messages = next;
        agent->message_capacity = capacity;
    }
    PicoMessage *msg = &agent->messages[agent->message_count++];
    memset(msg, 0, sizeof(*msg));
    msg->role = role;
    size_t len = markdown ? strlen(markdown) : 0;
    msg->source = (char *)malloc(len + 1);
    if (msg->source)
    {
        memcpy(msg->source, markdown ? markdown : "", len + 1);
    }
    msg->doc = MdDocument_ParseEx(markdown ? markdown : "", len,
                                  role == PICO_ROLE_USER ? MD_PARSE_PRESERVE_NEWLINES : MD_PARSE_DEFAULT);
    pico_run_hooks(app, PICO_HOOK_ON_MESSAGE, agent->id);
}

void PicoAgent_AppendAssistant(PicoApp *app, PicoAgent *agent, const char *text)
{
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
    PicoMessage *m = &agent->messages[agent->message_count - 1];
    size_t old = m->source ? strlen(m->source) : 0;
    size_t n = strlen(text);
    char *next = (char *)realloc(m->source, old + n + 1);
    if (!next)
    {
        return;
    }
    memcpy(next + old, text, n + 1);
    m->source = next;
    MdDocument_Free(&m->doc);
    m->doc = MdDocument_ParseEx(m->source, old + n, MD_PARSE_DEFAULT);
}

static void FlattenPut(JsonBuf *b, const char *s, size_t max)
{
    if (!s)
    {
        return;
    }
    for (; *s && b->len < max; s++)
    {
        char c = (*s == '\n' || *s == '\r' || *s == '\t') ? ' ' : *s;
        JsonBuf_Putc(b, c);
    }
}

static char *FormatToolProps(const char *args_json)
{
    JsonBuf b;
    JsonBuf_Init(&b);
    if (!args_json || !args_json[0])
    {
        return JsonBuf_Steal(&b);
    }
    JsonDoc doc;
    if (JsonParse(&doc, args_json, strlen(args_json)) != 0)
    {
        FlattenPut(&b, args_json, 240);
        return JsonBuf_Steal(&b);
    }
    if (JsonIsObject(&doc, 0))
    {
        int n = JsonObjLen(&doc, 0);
        for (int i = 0; i < n; i++)
        {
            int key_tok = -1;
            int val_tok = -1;
            if (!JsonObjPair(&doc, 0, i, &key_tok, &val_tok))
            {
                continue;
            }
            if (b.len)
            {
                JsonBuf_Puts(&b, "  ");
            }
            char *key = JsonStrDup(&doc, key_tok);
            FlattenPut(&b, key, 240);
            free(key);
            JsonBuf_Puts(&b, ": ");
            if (JsonIsArray(&doc, val_tok))
            {
                int count = JsonArrayLen(&doc, val_tok);
                JsonBuf_Puts(&b, "[");
                JsonBuf_Int(&b, count);
                JsonBuf_Puts(&b, count == 1 ? " item]" : " items]");
            }
            else
            {
                char *val = NULL;
                if (JsonIsObject(&doc, val_tok))
                {
                    val = JsonRawDup(&doc, val_tok);
                }
                else
                {
                    val = JsonStrDup(&doc, val_tok);
                    if (!val)
                    {
                        val = JsonRawDup(&doc, val_tok);
                    }
                }
                FlattenPut(&b, val, 240);
                free(val);
            }
            if (b.len > 240)
            {
                JsonBuf_Puts(&b, "...");
                break;
            }
        }
    }
    else
    {
        char *raw = JsonRawDup(&doc, 0);
        FlattenPut(&b, raw, 240);
        free(raw);
    }
    JsonFree(&doc);
    return JsonBuf_Steal(&b);
}

void PicoAgent_AddToolCallWithId(PicoApp *app, PicoAgent *agent, const char *call_id,
                                const char *name, const char *args)
{
    if (!agent)
    {
        return;
    }
    if (agent->message_count <= 0 || agent->messages[agent->message_count - 1].role != PICO_ROLE_ASSISTANT)
    {
        PicoAgent_AddMessage(app, agent, PICO_ROLE_ASSISTANT, "");
    }
    PicoMessage *m = &agent->messages[agent->message_count - 1];
    if (m->trace_count > 0 && !m->trace[m->trace_count - 1].is_tool)
    {
        PicoTraceLine_FreezeThink(&m->trace[m->trace_count - 1]);
    }
    PicoTraceLine *next =
        (PicoTraceLine *)realloc(m->trace, (size_t)(m->trace_count + 1) * sizeof(PicoTraceLine));
    if (!next)
    {
        return;
    }
    m->trace = next;
    PicoTraceLine *line = &m->trace[m->trace_count++];
    memset(line, 0, sizeof(*line));
    line->is_tool = true;
    line->tool_name = JsonDup(name && name[0] ? name : "tool");
    line->tool_call_id = call_id && call_id[0] ? JsonDup(call_id) : NULL;
    line->tool_args = FormatToolProps(args);
    line->tool_args_json = JsonDup(args ? args : "");
}

void PicoAgent_AddToolCall(PicoApp *app, PicoAgent *agent, const char *name, const char *args)
{
    PicoAgent_AddToolCallWithId(app, agent, NULL, name, args);
}

void PicoAgent_SetLastToolOutput(PicoAgent *agent, const char *output, bool is_error)
{
    if (!agent || agent->message_count <= 0)
    {
        return;
    }
    PicoMessage *m = &agent->messages[agent->message_count - 1];
    for (int t = m->trace_count - 1; t >= 0; t--)
    {
        if (m->trace[t].is_tool)
        {
            free(m->trace[t].tool_output);
            m->trace[t].tool_output = JsonDup(output ? output : "");
            m->trace[t].tool_error = is_error;
            return;
        }
    }
}

void PicoAgent_SetToolArgsByCallId(PicoAgent *agent, const char *call_id,
                                   const char *args)
{
    if (!agent || !call_id || !call_id[0])
    {
        return;
    }
    for (int i = agent->message_count - 1; i >= 0; i--)
    {
        PicoMessage *message = &agent->messages[i];
        for (int t = message->trace_count - 1; t >= 0; t--)
        {
            PicoTraceLine *line = &message->trace[t];
            if (line->is_tool && line->tool_call_id &&
                strcmp(line->tool_call_id, call_id) == 0)
            {
                char *display = FormatToolProps(args);
                char *raw = JsonDup(args ? args : "");
                if (!display || !raw)
                {
                    free(display);
                    free(raw);
                    return;
                }
                free(line->tool_args);
                free(line->tool_args_json);
                line->tool_args = display;
                line->tool_args_json = raw;
                return;
            }
        }
    }
}

void PicoAgent_SetToolOutputByCallId(PicoAgent *agent, const char *call_id,
                                     const char *output, bool is_error)
{
    if (!agent || !call_id || !call_id[0])
    {
        return;
    }
    for (int i = agent->message_count - 1; i >= 0; i--)
    {
        PicoMessage *message = &agent->messages[i];
        for (int t = message->trace_count - 1; t >= 0; t--)
        {
            PicoTraceLine *line = &message->trace[t];
            if (line->is_tool && line->tool_call_id &&
                strcmp(line->tool_call_id, call_id) == 0)
            {
                free(line->tool_output);
                line->tool_output = JsonDup(output ? output : "");
                line->tool_error = is_error;
                return;
            }
        }
    }
}

void PicoApp_AddMessage(PicoApp *app, PicoRole role, const char *markdown)
{
    PicoAgent_AddMessage(app, PicoApp_ActiveAgent(app), role, markdown);
}

void PicoApp_AppendAssistant(PicoApp *app, const char *text)
{
    PicoAgent_AppendAssistant(app, PicoApp_ActiveAgent(app), text);
}

void PicoApp_AddToolCall(PicoApp *app, const char *name, const char *args)
{
    PicoAgent_AddToolCall(app, PicoApp_ActiveAgent(app), name, args);
}

void PicoApp_SetLastToolOutput(PicoApp *app, const char *output, bool is_error)
{
    PicoAgent_SetLastToolOutput(PicoApp_ActiveAgent(app), output, is_error);
}

PicoSessionWriteResult pico_session_log_custom(PicoApp *app, PicoAgentId agent_id,
                                                const char *ext, const char *data_json)
{
    PicoAgent *agent = app && app->agents ? PicoAgentManager_Find(app->agents, agent_id) : NULL;
    if (!agent)
    {
        return PICO_SESSION_WRITE_FAILED;
    }
    return PicoSession_LogCustom(app, agent, ext, data_json);
}

void pico_agent_set_compact_summary(PicoApp *app, PicoAgentId agent_id, char *summary)
{
    PicoAgent *agent = app && app->agents ? PicoAgentManager_Find(app->agents, agent_id) : NULL;
    if (!agent)
    {
        free(summary);
        return;
    }
    free(agent->compact_summary);
    agent->compact_summary = summary;
}

void PicoApp_Submit(PicoApp *app)
{
    PicoAgent *active = PicoApp_ActiveAgent(app);
    if (!app || !PicoAgentManager_AcceptsNewWork(app->agents) ||
        !active || active->state == PICO_AGENT_LLM_WAIT || active->state == PICO_AGENT_TOOL_WAIT ||
        active->state == PICO_AGENT_COMPACT_WAIT)
    {
        return;
    }
    if (!PicoAgent_RevalidateToolPolicy(app, active))
    {
        pico_status_warn(app, "This agent's restricted tool policy references a tool that is not currently registered.");
        return;
    }

    PicoComposer *c = &app->composer;
    bool has_attach = PicoComposer_HasAttachments(app);
    PicoModel *model = PicoSettings_ActiveModel(app, active);
    if (has_attach && model && !model->vision)
    {
        free(app->agent_input);
        app->agent_input = NULL;
        free(app->agent_parts);
        app->agent_parts = NULL;
        app->submit_cancel = false;
        pico_status_warn(app, "This model doesn't accept images.");
        return;
    }
    int start = 0;
    int end = (c->text && c->length > 0) ? c->length : 0;
    while (start < end && (c->text[start] == ' ' || c->text[start] == '\n' || c->text[start] == '\t'))
    {
        start++;
    }
    while (end > start && (c->text[end - 1] == ' ' || c->text[end - 1] == '\n' || c->text[end - 1] == '\t'))
    {
        end--;
    }
    if (end <= start && !has_attach)
    {
        return;
    }
    if (end <= start)
    {
        if (c->text)
        {
            c->text[0] = '\0';
        }
        c->length = 0;
        c->cursor = 0;
        c->sel_anchor = 0;
    }
    else
    {
        if (start > 0)
        {
            memmove(c->text, c->text + start, (size_t)(end - start));
            end -= start;
        }
        c->length = end;
        c->text[c->length] = '\0';
        c->cursor = c->length;
        c->sel_anchor = c->length;
    }

    free(app->agent_input);
    app->agent_input = NULL;
    free(app->agent_parts);
    app->agent_parts = NULL;
    app->submit_cancel = false;
    pico_run_hooks(app, PICO_HOOK_BEFORE_SUBMIT, active->id);
    if (app->submit_cancel)
    {
        free(app->agent_input);
        app->agent_input = NULL;
        free(app->agent_parts);
        app->agent_parts = NULL;
        return;
    }
    if (app->agent_parts)
    {
        char *normalized = NULL;
        if (!pico_canonical_normalize_user_parts(app->agent_parts, &normalized))
        {
            free(app->agent_input);
            app->agent_input = NULL;
            free(app->agent_parts);
            app->agent_parts = NULL;
            pico_status_warn(app, "Submit hook returned invalid canonical user parts.");
            return;
        }
        free(app->agent_parts);
        app->agent_parts = normalized;
    }
    if (!PicoComposer_ApplyAttachments(app))
    {
        free(app->agent_input);
        app->agent_input = NULL;
        free(app->agent_parts);
        app->agent_parts = NULL;
        pico_status_warn(app, "Could not prepare the attached images.");
        return;
    }

    const char *typed = c->text ? c->text : "";
    const char *agent = app->agent_input && app->agent_input[0] ? app->agent_input : typed;
    char *display_owned = has_attach ? pico_composer_display_message(typed) : NULL;
    const char *display = display_owned ? display_owned : typed;
    PicoApp_AddMessage(app, PICO_ROLE_USER, display);
    app->chat_follow_bottom = true;
    PicoSession_LogUser(app, active, agent, display, app->agent_parts);
    PicoAgent_StartTurn(app, active, agent);

    c->length = 0;
    c->cursor = 0;
    c->sel_anchor = 0;
    if (c->text)
    {
        c->text[0] = '\0';
    }
    PicoComposer_ReleaseAttachments();
    free(display_owned);
    free(app->agent_input);
    app->agent_input = NULL;
    free(app->agent_parts);
    app->agent_parts = NULL;
    pico_run_hooks(app, PICO_HOOK_ON_SUBMIT, active->id);
}

void PicoApp_Cancel(PicoApp *app)
{
    PicoAgent_Cancel(PicoApp_ActiveAgent(app));
}

bool PicoUi_ModalOpen(const PicoApp *app)
{
    return pico_ui_modal_claimed(app) || PicoAgent_AskUiOpen(PicoApp_ActiveAgentConst(app));
}

void PicoApp_Init(PicoApp *app, Font *fonts, const char *workspace, bool safe_mode,
                 PicoSessionStart session_start, const char *session_file)
{
    if (!app)
    {
        return;
    }
    PicoChat_InspectClose();
    memset(app, 0, sizeof(*app));
    Pico_DocsSetAppDir(GetApplicationDirectory());
    if (g_pico_process_retired)
    {
        app->terminal_shutdown = true;
        pico_status_warn(app, "Pico cannot be initialized again after a retained shutdown; exit the process.");
        return;
    }
    app->fonts = fonts;
    app->chat_sel.msg = -1;
    app->chat_follow_bottom = true;
    app->chat_overflow = true;
    app->safe_mode = safe_mode;
    if (workspace && workspace[0])
    {
        snprintf(app->workspace, sizeof(app->workspace), "%s", workspace);
    }
    else
    {
        snprintf(app->workspace, sizeof(app->workspace), ".");
    }
    app->composer.capacity = 256;
    app->composer.text = (char *)malloc((size_t)app->composer.capacity);
    if (app->composer.text)
    {
        app->composer.text[0] = '\0';
    }

    PicoSettings_Load(app);
    PicoAuth_Load(app);
    app->agents = PicoAgentManager_Create(app);
    if (!app->agents)
    {
        pico_status_warn(app, "Could not create the agent manager.");
        return;
    }
    PicoAgentCreateOptions options = {
        .kind = PICO_AGENT_NORMAL,
        .session_start = session_start == PICO_SESSION_NONE ? PICO_SESSION_NONE : PICO_SESSION_NEW,
        .select = true,
    };
    PicoAgentId initial_id = 0;
    if (pico_agent_create(app, &options, &initial_id) != PICO_AGENT_RESULT_OK)
    {
        pico_status_warn(app, "Could not create the agent runtime.");
        return;
    }
    PicoPlugins_Load(app);
    PicoAgentManager_LoadProfiles(app->agents);
    PicoAgent *initial = PicoApp_ActiveAgent(app);
    pico_run_hooks(app, PICO_HOOK_ON_SESSION_RESET, initial_id);
    if (session_file && session_file[0])
    {
        PicoSession_Reset(app, initial);
        PicoSession_Start(app, initial, session_start, session_file);
    }
    else if (session_start == PICO_SESSION_RESUME || app->settings.resume_last)
    {
        PicoSession_Start(app, initial, session_start, NULL);
    }
}

void PicoApp_RequestReload(PicoApp *app)
{
    if (!app || app->terminal_shutdown || g_pico_process_retired)
    {
        return;
    }
    app->reload_queued = true;
    PicoAgentManager_SetAcceptingWork(app->agents, false);
    if (!app->workspace_change_queued && !PicoAgentManager_BlocksReload(app->agents))
    {
        PicoPlugins_Reload(app);
    }
}

static void FormatHomePath(const char *path, char *out, size_t cap)
{
    const char *home = getenv("HOME");
    if (home && home[0] && path)
    {
        size_t n = strlen(home);
        while (n > 1 && home[n - 1] == '/')
        {
            n--;
        }
        if (strncmp(path, home, n) == 0 && (path[n] == '\0' || path[n] == '/'))
        {
            snprintf(out, cap, "~%s", path + n);
            return;
        }
    }
    snprintf(out, cap, "%s", path ? path : "");
}

static int ExpandUserPath(const char *workspace, const char *arg, char *out, size_t cap)
{
    if (!arg || !arg[0] || !out || cap < 2)
    {
        return -1;
    }
    if (arg[0] == '~' && (arg[1] == '\0' || arg[1] == '/'))
    {
        const char *home = getenv("HOME");
        if (!home || !home[0])
        {
            return -1;
        }
        if (arg[1] == '\0')
        {
            snprintf(out, cap, "%s", home);
            return 0;
        }
        int n = snprintf(out, cap, "%s%s", home, arg + 1);
        if (n < 0 || (size_t)n >= cap)
        {
            return -1;
        }
        return 0;
    }
    if (arg[0] == '/')
    {
        if (strlen(arg) >= cap)
        {
            return -1;
        }
        snprintf(out, cap, "%s", arg);
        return 0;
    }
    const char *ws = (workspace && workspace[0]) ? workspace : ".";
    int n = snprintf(out, cap, "%s/%s", ws, arg);
    if (n < 0 || (size_t)n >= cap)
    {
        return -1;
    }
    return 0;
}

static int ResolveWorkspaceDir(const char *workspace, const char *arg, char *out, size_t cap)
{
    char expanded[4096];
    if (ExpandUserPath(workspace, arg, expanded, sizeof(expanded)) != 0)
    {
        return -1;
    }
    char real[4096];
    if (!realpath(expanded, real))
    {
        return -1;
    }
    struct stat st;
    if (stat(real, &st) != 0 || !S_ISDIR(st.st_mode))
    {
        return -1;
    }
    if (strlen(real) >= cap)
    {
        return -1;
    }
    snprintf(out, cap, "%s", real);
    return 0;
}

bool PicoApp_ChangeWorkspace(PicoApp *app, const char *path)
{
    if (!app || app->terminal_shutdown || g_pico_process_retired)
    {
        return false;
    }

    while (path && *path && isspace((unsigned char)*path))
    {
        path++;
    }
    if (!path || !path[0])
    {
        return false;
    }

    char trimmed[4096];
    snprintf(trimmed, sizeof(trimmed), "%s", path);
    size_t tlen = strlen(trimmed);
    while (tlen > 0 && isspace((unsigned char)trimmed[tlen - 1]))
    {
        trimmed[--tlen] = '\0';
    }

    char resolved[4096];
    if (ResolveWorkspaceDir(app->workspace, trimmed, resolved, sizeof(resolved)) != 0)
    {
        char shown[400];
        snprintf(shown, sizeof(shown), "%s", trimmed);
        char line[512];
        snprintf(line, sizeof(line), "Not a directory `%s`.", shown);
        PicoOverlay_Notify(app, line);
        return false;
    }

    char current[4096];
    const char *ws = app->workspace[0] ? app->workspace : ".";
    if (realpath(ws, current) && strcmp(current, resolved) == 0)
    {
        char pretty[400];
        FormatHomePath(resolved, pretty, sizeof(pretty));
        char line[512];
        snprintf(line, sizeof(line), "Already in `%s`.", pretty);
        PicoOverlay_Notify(app, line);
        return false;
    }

    snprintf(app->pending_workspace, sizeof(app->pending_workspace), "%s", resolved);
    app->workspace_change_queued = true;
    PicoAgentManager_SetAcceptingWork(app->agents, false);

    char pretty[400];
    FormatHomePath(resolved, pretty, sizeof(pretty));
    char line[512];
    if (PicoAgentManager_BlocksReload(app->agents))
    {
        snprintf(line, sizeof(line), "Workspace change to `%s` queued until all agents are quiescent.", pretty);
    }
    else
    {
        snprintf(line, sizeof(line), "Changing workspace to `%s`…", pretty);
    }
    PicoOverlay_Notify(app, line);
    return true;
}

static void WorkspacePreflightFailed(PicoApp *app, const char *message)
{
    app->pending_workspace[0] = '\0';
    app->workspace_change_queued = false;
    PicoAgentManager_SetAcceptingWork(app->agents, !app->reload_queued);
    pico_status_warn(app, message);
}

static void ApplyWorkspaceChange(PicoApp *app)
{
    char target[4096];
    snprintf(target, sizeof(target), "%s", app->pending_workspace);
    if (!target[0])
    {
        app->workspace_change_queued = false;
        PicoAgentManager_SetAcceptingWork(app->agents, !app->reload_queued);
        return;
    }

    /* Stage every allocation needed for a usable replacement before the old
     * manager or workspace is changed. The staged worker is idle and has no
     * session or extension-owned state. */
    PicoAgentManager *replacement = PicoAgentManager_Create(app);
    if (!replacement)
    {
        WorkspacePreflightFailed(app, "Could not prepare an agent manager for the new workspace.");
        return;
    }
    PicoAgent *initial = PicoAgent_Create(app);
    if (!initial)
    {
        (void)PicoAgentManager_Destroy(replacement);
        WorkspacePreflightFailed(app, "Could not prepare an agent for the new workspace.");
        return;
    }
    PicoAgent_PrepareReload(initial);

    PicoAgentManager *old = app->agents;
    PicoChat_InspectClose();
    if (!PicoAgentManager_Destroy(old))
    {
        (void)PicoAgent_Destroy(initial);
        (void)PicoAgentManager_Destroy(replacement);
        app->terminal_shutdown = true;
        g_pico_process_retired = true;
        PicoOverlay_Notify(app, "A worker detached during workspace transition; Pico must now exit.");
        return;
    }

    app->agents = NULL;
    PicoPlugins_Shutdown(app);
    app->agents = replacement;
    snprintf(app->workspace, sizeof(app->workspace), "%s", target);
    app->pending_workspace[0] = '\0';
    app->workspace_change_queued = false;
    app->reload_queued = true;
    float prev_font_scale = Pico_FontScale();
    PicoSettings_Load(app);
    if (Pico_FontScale() != prev_font_scale)
    {
        Clay_ResetMeasureTextCache();
    }
    PicoSettings_InitAgent(app, initial);
    if (!PicoAgentManager_AdoptInitial(replacement, initial))
    {
        (void)PicoAgent_Destroy(initial);
        app->terminal_shutdown = true;
        pico_status_warn(app, "Workspace replacement could not publish its prepared agent; Pico must exit.");
        return;
    }
    PicoPlugins_Load(app);
    PicoAgentManager_LoadProfiles(app->agents);
    PicoAgentManager_RevalidateToolPolicies(app->agents);
    PicoAgentManager_NotifySessions(app->agents);
    PicoAgentManager_ReplayToolDetails(app->agents);
    app->reload_queued = false;
    PicoAgentManager_SetAcceptingWork(app->agents, true);
    PicoChatSel_Clear(app);
    memset(&app->chat_scrollbar, 0, sizeof(app->chat_scrollbar));
    app->chat_follow_bottom = true;
    app->chat_overflow = true;

    char pretty[400];
    FormatHomePath(target, pretty, sizeof(pretty));
    char line[512];
    snprintf(line, sizeof(line), "Workspace `%s`.", pretty);
    PicoOverlay_Notify(app, line);
}

void PicoApp_PumpLifecycle(PicoApp *app)
{
    if (!app || app->terminal_shutdown || g_pico_process_retired)
    {
        return;
    }
    PicoAgentManager_Pump(app->agents);
    if (app->workspace_change_queued)
    {
        if (!PicoAgentManager_BlocksReload(app->agents))
        {
            ApplyWorkspaceChange(app);
        }
        return;
    }
    if (app->reload_queued && !PicoAgentManager_BlocksReload(app->agents))
    {
        PicoPlugins_Reload(app);
    }
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
        MdDocument_Free(&messages[i].doc);
    }
    free(messages);
}

void PicoMessages_PrepareDocs(PicoMessage *messages, int count)
{
    for (int i = 0; i < count; i++)
    {
        PicoMessage *msg = &messages[i];
        if (msg->doc.block_count == 0 && msg->source && msg->source[0])
        {
            MdDocument_Free(&msg->doc);
            msg->doc = MdDocument_ParseEx(msg->source, strlen(msg->source),
                                          msg->role == PICO_ROLE_USER ? MD_PARSE_PRESERVE_NEWLINES
                                                                      : MD_PARSE_DEFAULT);
        }
        for (int t = 0; t < msg->trace_count; t++)
        {
            PicoTraceLine *line = &msg->trace[t];
            if (line->doc.block_count == 0 && line->text && line->text[0] && line->think_steps >= 1)
            {
                MdDocument_Free(&line->doc);
                line->doc = MdDocument_ParseEx(line->text, strlen(line->text), MD_PARSE_DEFAULT);
            }
        }
    }
}

bool PicoMessages_Copy(const PicoMessage *src, int count, PicoMessage **dst, int *dst_count)
{
    if (dst)
    {
        *dst = NULL;
    }
    if (dst_count)
    {
        *dst_count = 0;
    }
    if (!dst || !dst_count || count < 0 || (count > 0 && !src))
    {
        return false;
    }
    if (count == 0)
    {
        return true;
    }
    PicoMessage *copy = (PicoMessage *)calloc((size_t)count, sizeof(PicoMessage));
    if (!copy)
    {
        return false;
    }
    for (int i = 0; i < count; i++)
    {
        copy[i].role = src[i].role;
        copy[i].source = src[i].source ? JsonDup(src[i].source) : NULL;
        if (src[i].trace_count > 0)
        {
            copy[i].trace = (PicoTraceLine *)calloc((size_t)src[i].trace_count, sizeof(PicoTraceLine));
            if (!copy[i].trace)
            {
                PicoMessages_Free(copy, i + 1);
                return false;
            }
            copy[i].trace_count = src[i].trace_count;
            for (int t = 0; t < src[i].trace_count; t++)
            {
                const PicoTraceLine *from = &src[i].trace[t];
                PicoTraceLine *to = &copy[i].trace[t];
                to->is_tool = from->is_tool;
                to->tool_error = from->tool_error;
                to->expanded = from->expanded;
                to->think_steps = from->think_steps;
                to->think_ms = from->think_ms;
                to->child_id = from->child_id;
                snprintf(to->child_session_id, sizeof(to->child_session_id), "%s",
                         from->child_session_id);
                to->text = from->text ? JsonDup(from->text) : NULL;
                to->tool_name = from->tool_name ? JsonDup(from->tool_name) : NULL;
                to->tool_call_id = from->tool_call_id ? JsonDup(from->tool_call_id) : NULL;
                to->tool_args = from->tool_args ? JsonDup(from->tool_args) : NULL;
                to->tool_args_json = from->tool_args_json ? JsonDup(from->tool_args_json) : NULL;
                to->tool_output = from->tool_output ? JsonDup(from->tool_output) : NULL;
                if (from->think_part_count > 0 && from->think_parts)
                {
                    to->think_parts =
                        (char **)calloc((size_t)from->think_part_count, sizeof(char *));
                    if (!to->think_parts)
                    {
                        PicoMessages_Free(copy, i + 1);
                        return false;
                    }
                    to->think_part_count = from->think_part_count;
                    for (int p = 0; p < from->think_part_count; p++)
                    {
                        to->think_parts[p] =
                            from->think_parts[p] ? JsonDup(from->think_parts[p]) : NULL;
                    }
                }
            }
        }
    }
    PicoMessages_PrepareDocs(copy, count);
    *dst = copy;
    *dst_count = count;
    return true;
}

void PicoAgent_ClearMessages(PicoAgent *agent)
{
    if (!agent)
    {
        return;
    }
    for (int i = 0; i < agent->message_count; i++)
    {
        free(agent->messages[i].source);
        for (int t = 0; t < agent->messages[i].trace_count; t++)
        {
            PicoTraceLine_Release(&agent->messages[i].trace[t]);
        }
        free(agent->messages[i].trace);
        MdDocument_Free(&agent->messages[i].doc);
        memset(&agent->messages[i], 0, sizeof(agent->messages[i]));
    }
    agent->message_count = 0;
}

void PicoApp_ClearMessages(PicoApp *app)
{
    PicoAgent_ClearMessages(PicoApp_ActiveAgent(app));
    if (app)
    {
        PicoChatSel_Clear(app);
    }
}

PicoAppShutdownResult PicoApp_Free(PicoApp *app)
{
    if (!app)
    {
        return PICO_APP_SHUTDOWN_CLEAN;
    }
    if (g_pico_process_retired)
    {
        return PICO_APP_SHUTDOWN_RETAINED;
    }
    /* A detached worker can still reach registrations, auth, and its manager. */
    PicoChat_InspectClose();
    if (!PicoAgentManager_Destroy(app->agents))
    {
        app->terminal_shutdown = true;
        g_pico_process_retired = true;
        return PICO_APP_SHUTDOWN_RETAINED;
    }
    app->agents = NULL;
    PicoPlugins_Shutdown(app);
    PicoAuth_Free(app);
    PicoApp_ClearMessages(app);
    free(app->composer.text);
    free(app->status_warn);
    free(app->models);
    free(app->agent_input);
    free(app->agent_parts);
    memset(app, 0, sizeof(*app));
    return PICO_APP_SHUTDOWN_CLEAN;
}

static Clay_RenderCommandArray CreateShellLayout(PicoApp *app, float delta_time)
{
    Clay_BeginLayout();
    MdView_BeginFrame();

    /* The shell owns the viewport height. A vertical GROW root can retain Clay's
     * sub-pixel compression remainder and feed it back through scrolling. */
    CLAY(CLAY_ID("Root"),
         {.layout = {.layoutDirection = CLAY_TOP_TO_BOTTOM,
                     .sizing = {.width = CLAY_SIZING_GROW(0),
                                .height = CLAY_SIZING_FIXED((float)GetScreenHeight())},
                     .padding = {CONTENT_PADDING, 12, 16, 12},
                     .childGap = 6},
          .backgroundColor = COLOR_BG})
    {
        CLAY(CLAY_ID("Body"),
             {.layout = {.layoutDirection = CLAY_LEFT_TO_RIGHT,
                         .childGap = 12,
                         .sizing = {.width = CLAY_SIZING_GROW(0), .height = CLAY_SIZING_GROW(0)}}})
        {
            if (app->view_count[PICO_SLOT_SIDEBAR] > 0)
            {
                CLAY(CLAY_ID("Sidebar"),
                     {.layout = {.layoutDirection = CLAY_TOP_TO_BOTTOM,
                                 .childGap = 8,
                                 .padding = {8, 8, 8, 8},
                                 .sizing = {.width = CLAY_SIZING_FIT(120, 280), .height = CLAY_SIZING_GROW(0)}},
                      .backgroundColor = COLOR_CONTENT_BG,
                      .cornerRadius = CLAY_CORNER_RADIUS(8)})
                {
                    RunSlot(app, PICO_SLOT_SIDEBAR);
                }
            }
            CLAY(CLAY_ID("MainColumn"),
                 {.layout = {.layoutDirection = CLAY_TOP_TO_BOTTOM,
                             .sizing = {.width = CLAY_SIZING_GROW(0), .height = CLAY_SIZING_GROW(0)},
                             .childGap = 12}})
            {
                RunSlot(app, PICO_SLOT_MAIN);
                RunSlot(app, PICO_SLOT_COMPOSER);
            }
        }
        RunSlot(app, PICO_SLOT_FOOTER);
    }
    RunSlot(app, PICO_SLOT_OVERLAY);

    app->hovered_link = MdView_HoveredLink();
    return Clay_EndLayout(delta_time);
}

static void UpdateChatScrollbarDrag(PicoApp *app)
{
    PicoScrollbar_UpdateDrag(&app->chat_scrollbar, CLAY_STRING("ChatScroll"),
                             CLAY_STRING("ChatScrollBarHandle"));
}

#define CHAT_FOLLOW_SLACK 8.0f

static bool ChatScrollAtBottom(Clay_ScrollContainerData data)
{
    if (!data.found || !data.scrollPosition)
    {
        return true;
    }
    float overflow = data.contentDimensions.height - data.scrollContainerDimensions.height;
    if (overflow <= 0.5f)
    {
        return true;
    }
    float bottom = data.scrollContainerDimensions.height - data.contentDimensions.height;
    return data.scrollPosition->y <= bottom + CHAT_FOLLOW_SLACK;
}

static void ApplyPaneWheel(Clay_String container_id, Clay_Vector2 delta)
{
    Clay_ScrollContainerData data = Clay_GetScrollContainerData(Clay_GetElementId(container_id));
    if (!data.found || !data.scrollPosition)
    {
        return;
    }
    if (data.config.vertical)
    {
        float overflow = data.contentDimensions.height - data.scrollContainerDimensions.height;
        float min_y = overflow > 0.0f ? -overflow : 0.0f;
        float y = data.scrollPosition->y + delta.y * 10.0f;
        if (y > 0.0f)
        {
            y = 0.0f;
        }
        else if (y < min_y)
        {
            y = min_y;
        }
        data.scrollPosition->y = y;
    }
    if (data.config.horizontal)
    {
        float overflow = data.contentDimensions.width - data.scrollContainerDimensions.width;
        float min_x = overflow > 0.0f ? -overflow : 0.0f;
        float x = data.scrollPosition->x + delta.x * 10.0f;
        if (x > 0.0f)
        {
            x = 0.0f;
        }
        else if (x < min_x)
        {
            x = min_x;
        }
        data.scrollPosition->x = x;
    }
}

static void UpdateChatFollowFromUserScroll(PicoApp *app, bool over_chat, bool modal_open, float wheel_y)
{
    if (modal_open)
    {
        return;
    }
    bool bar_drag = app->chat_scrollbar.mouse_down;
    bool wheel = over_chat && wheel_y != 0.0f;
    if (!bar_drag && !wheel)
    {
        return;
    }
    Clay_ScrollContainerData data = Clay_GetScrollContainerData(Clay_GetElementId(CLAY_STRING("ChatScroll")));
    app->chat_follow_bottom = ChatScrollAtBottom(data);
}

/* Wayland delivers the first xdg_toplevel configure during glfwCreateWindow,
 * before raylib registers WindowSizeCallback, so CORE.Window.screen stays at
 * the 1100x800 create hint while the surface is already tiled. Invoke the
 * callback with the real size so viewport, layout, and mouse hit-tests match. */
static void SyncRaylibWindowSize(void)
{
    GLFWwindow *win = GetWindowHandle();
    if (!win)
    {
        return;
    }
    int width = 0;
    int height = 0;
    glfwGetWindowSize(win, &width, &height);
    if (width <= 0 || height <= 0)
    {
        return;
    }
    if (width == GetScreenWidth() && height == GetScreenHeight())
    {
        return;
    }
    GLFWwindowsizefun prev = glfwSetWindowSizeCallback(win, NULL);
    glfwSetWindowSizeCallback(win, prev);
    if (prev)
    {
        prev(win, width, height);
    }
}

void PicoApp_Frame(PicoApp *app)
{
    if (!app)
    {
        return;
    }
    SyncRaylibWindowSize();
    if (app->terminal_shutdown)
    {
        CloseWindow();
        return;
    }
    if (!PicoApp_ActiveAgent(app))
    {
        return;
    }
    Vector2 mouse_delta = GetMouseWheelMoveV();
    mouse_delta.x *= 5.0f;
    mouse_delta.y *= 5.0f;

    if (IsKeyPressed(KEY_F2))
    {
        PicoExts_Toggle();
    }
#ifdef PICO_CLAY_DEBUG
    if (IsKeyPressed(KEY_F3))
    {
        app->debug_enabled = !app->debug_enabled;
        Clay_SetDebugModeEnabled(app->debug_enabled);
    }
#endif
    if (IsKeyPressed(KEY_F12))
    {
        TakeScreenshot("pico_screenshot.png");
    }
    if (IsKeyPressed(KEY_F5))
    {
        PicoApp_RequestReload(app);
    }

    PicoPlugins_Poll(app);
    PicoApp_PumpLifecycle(app);
    PicoScrollbar_BeginFrame();

    bool had_warn = app->status_warn != NULL;
    bool had_complete = PicoComplete_IsOpen();
    bool had_todo = PicoTodo_IsExpanded(app);
    bool had_modal = pico_ui_modal_claimed(app);
    PicoPlugins_OnFrame(app, GetFrameTime());
    if (!had_warn && !had_complete && !had_todo && !had_modal && IsKeyPressed(KEY_ESCAPE))
    {
        PicoAgent *active = PicoApp_ActiveAgent(app);
        if (PicoAgent_IsBusy(active))
        {
            if (PicoAgent_CancelRequested(active))
            {
                PicoAgent_ForceCancel(app, active);
            }
            else
            {
                PicoApp_Cancel(app);
            }
        }
        else if (active->state == PICO_AGENT_ERROR)
        {
            PicoAgent_DismissError(active);
        }
    }

    Clay_Vector2 mouse_position = {.x = GetMousePosition().x, .y = GetMousePosition().y};
    bool over_composer = Clay_PointerOver(Clay_GetElementId(CLAY_STRING("Composer")));
    bool over_chat = Clay_PointerOver(Clay_GetElementId(CLAY_STRING("ChatScroll")));
    bool modal_open = PicoUi_ModalOpen(app);
    if (!modal_open)
    {
        UpdateChatScrollbarDrag(app);
    }
    bool bar_drag = PicoScrollbar_AnyDragging();
    Clay_SetPointerState(mouse_position, IsMouseButtonDown(0) && !bar_drag);
    Clay_SetLayoutDimensions((Clay_Dimensions){(float)GetScreenWidth(), (float)GetScreenHeight()});
    PicoChat_HandleToolRelease(app);

    /* Think headers clip long titles, which Clay treats as nested scrollers that
     * would eat the wheel. Route it to the chat / inspect pane instead. */
    bool over_inspect = PicoChat_InspectIsOpen() &&
                        Clay_PointerOver(Clay_GetElementId(CLAY_STRING("SubagentChatScroll")));
    bool pane_wheel = over_inspect || (over_chat && !modal_open);
    Clay_Vector2 wheel = {.x = mouse_delta.x, .y = mouse_delta.y};
    Clay_UpdateScrollContainers(
        !bar_drag && (modal_open || (!over_composer && !over_chat && !app->chat_sel.mouse_selecting)),
        pane_wheel ? (Clay_Vector2){0, 0} : wheel, GetFrameTime());
    if (pane_wheel)
    {
        ApplyPaneWheel(over_inspect ? CLAY_STRING("SubagentChatScroll") : CLAY_STRING("ChatScroll"),
                       wheel);
    }
    UpdateChatFollowFromUserScroll(app, over_chat, modal_open, mouse_delta.y);

    Clay_RenderCommandArray render_commands = CreateShellLayout(app, GetFrameTime());

    app->chat_overflow = PicoScrollbar_Overflows(CLAY_STRING("ChatScroll"));

    pico_run_hooks(app, PICO_HOOK_AFTER_LAYOUT, pico_agent_active(app));

    if (!modal_open && PicoComposer_PointerOverAttachmentRemove())
    {
        SetMouseCursor(MOUSE_CURSOR_POINTING_HAND);
    }
    else if ((!modal_open && PicoComposer_PointerOverAttachments()) ||
             Clay_PointerOver(Clay_GetElementId(CLAY_STRING("CompScrollBarHandle"))) ||
             Clay_PointerOver(Clay_GetElementId(CLAY_STRING("CompScrollTrack"))) ||
             Clay_PointerOver(Clay_GetElementId(CLAY_STRING("ChatScrollBarHandle"))))
    {
        SetMouseCursor(MOUSE_CURSOR_DEFAULT);
    }
    else if (app->hovered_link || app->hovered_tool || app->hovered_clickable)
    {
        SetMouseCursor(MOUSE_CURSOR_POINTING_HAND);
    }
    else if (Clay_PointerOver(Clay_GetElementId(CLAY_STRING("Composer"))) || PicoChatSel_PointerOverText() ||
             app->chat_sel.mouse_selecting)
    {
        SetMouseCursor(MOUSE_CURSOR_IBEAM);
    }
    else
    {
        SetMouseCursor(MOUSE_CURSOR_DEFAULT);
    }

    bool relayout = false;
    if (Pico_NeedsClayReinit())
    {
        fprintf(stderr, "clay-scroll: skip restore/remember (reinit pending)\n");
    }
    else
    {
        relayout = Pico_RestoreClayScroll();
        if (app->chat_follow_bottom)
        {
            Clay_ScrollContainerData data = Clay_GetScrollContainerData(Clay_GetElementId(CLAY_STRING("ChatScroll")));
            float chat_y = (data.found && data.scrollPosition) ? data.scrollPosition->y : 0.0f;
            bool pinned = data.found &&
                          PicoScrollbar_PinToBottom(data.scrollContainerDimensions.height, data.contentDimensions.height,
                                                    data.scrollPosition ? &data.scrollPosition->y : NULL);
            if (pinned)
            {
                fprintf(stderr, "clay-scroll: pin-bottom chat_y %.1f -> %.1f view_h=%.1f content_h=%.1f\n",
                        (double)chat_y, (double)(data.scrollPosition ? data.scrollPosition->y : 0.0f),
                        (double)data.scrollContainerDimensions.height, (double)data.contentDimensions.height);
                relayout = true;
            }
        }
        Pico_RememberClayScroll();
    }
    if (relayout)
    {
        fprintf(stderr, "clay-scroll: relayout follow=%d\n", app->chat_follow_bottom ? 1 : 0);
        /* Clay has already generated command bounds with the prior offset.
         * Rebuild once so the corrected offset is visible this frame. */
        render_commands = CreateShellLayout(app, 0.0f);
        app->chat_overflow = PicoScrollbar_Overflows(CLAY_STRING("ChatScroll"));
        if (!Pico_NeedsClayReinit())
        {
            Pico_RememberClayScroll();
        }
    }

    if (app->hovered_link && IsMouseButtonReleased(MOUSE_BUTTON_LEFT) && !app->chat_sel.dragging &&
        !PicoChatSel_HasSelection(app))
    {
        OpenURL(app->hovered_link);
    }

    BeginDrawing();
    ClearBackground((Color){(unsigned char)COLOR_BG.r, (unsigned char)COLOR_BG.g, (unsigned char)COLOR_BG.b, 255});
    Clay_Raylib_Render(render_commands, app->fonts);
    pico_run_hooks(app, PICO_HOOK_AFTER_RENDER, pico_agent_active(app));
    EndDrawing();
}
