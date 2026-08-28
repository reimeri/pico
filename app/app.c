#include "pico/plugin.h"
#include "pico/md_view.h"
#include "agent.h"
#include "workspace_internal.h"
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
#include "host_internal.h"
#include "path.h"

#include <curl/curl.h>

#include "clay/clay.h"

#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>

#include <ctype.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

void Clay_Raylib_Render(Clay_RenderCommandArray renderCommands, Font *fonts);

static bool g_pico_process_retired;

bool PicoHost_ProcessRetired(void)
{
    return g_pico_process_retired;
}

PicoHost *pico_workspace_host(PicoWorkspace *workspace)
{
    return workspace ? workspace->host : NULL;
}

PicoWorkspace *PicoHost_FindWorkspace(PicoHost *host, PicoWorkspaceId id)
{
    if (!host || id == 0)
    {
        return NULL;
    }
    for (int i = 0; i < host->workspace_count; i++)
    {
        if (host->workspaces[i] && host->workspaces[i]->id == id)
        {
            return host->workspaces[i];
        }
    }
    return NULL;
}

const PicoWorkspace *PicoHost_FindWorkspaceConst(const PicoHost *host, PicoWorkspaceId id)
{
    return PicoHost_FindWorkspace((PicoHost *)host, id);
}

PicoAgent *PicoHost_FindAgent(PicoHost *host, PicoAgentId id)
{
    if (!host || id == 0)
    {
        return NULL;
    }
    for (int i = 0; i < host->workspace_count; i++)
    {
        PicoWorkspace *workspace = host->workspaces[i];
        if (!workspace)
        {
            continue;
        }
        PicoAgent *agent = PicoWorkspace_FindAgent(workspace, id);
        if (agent)
        {
            return agent;
        }
    }
    return NULL;
}

const PicoAgent *PicoHost_FindAgentConst(const PicoHost *host, PicoAgentId id)
{
    return PicoHost_FindAgent((PicoHost *)host, id);
}

PicoAgent *PicoHost_SelectedAgent(PicoHost *host)
{
    return host ? PicoHost_FindAgent(host, host->selected_agent_id) : NULL;
}

const PicoAgent *PicoHost_SelectedAgentConst(const PicoHost *host)
{
    return PicoHost_SelectedAgent((PicoHost *)host);
}

int PicoHost_TotalAgentCount(const PicoHost *host)
{
    int total = 0;
    if (!host)
    {
        return 0;
    }
    for (int i = 0; i < host->workspace_count; i++)
    {
        if (host->workspaces[i])
        {
            total += host->workspaces[i]->count;
        }
    }
    return total;
}

uint64_t PicoHost_AllocAskId(PicoHost *host)
{
    uint64_t id = 0;
    if (!host)
    {
        return 0;
    }
    pthread_mutex_lock(&host->ask_id_mu);
    id = ++host->next_ask_id;
    pthread_mutex_unlock(&host->ask_id_mu);
    return id;
}

static void SnapshotRegistrationCounts(PicoHost *host)
{
    if (!host)
    {
        return;
    }
    memcpy(host->staged_view_count, host->view_count, sizeof(host->view_count));
    host->staged_empty_view_count = host->empty_view_count;
    host->staged_hook_count = host->hook_count;
    host->staged_tool_before_hook_count = host->tool_before_hook_count;
    host->staged_tool_after_hook_count = host->tool_after_hook_count;
    host->staged_llm_hook_count = host->llm_hook_count;
    host->staged_context_hook_count = host->context_hook_count;
    host->staged_tool_row_hook_count = host->tool_row_hook_count;
    host->staged_tool_count = host->tool_count;
    host->staged_command_count = host->command_count;
    host->staged_completer_count = host->completer_count;
    host->staged_provider_count = host->provider_count;
    host->staged_auth_count = host->auth_count;
}

void PicoHost_BeginRegistration(PicoHost *host, int scope, PicoWorkspace *workspace)
{
    if (!host)
    {
        return;
    }
    host->reg_scope = scope;
    host->reg_workspace = workspace;
    host->reg_state = NULL;
    SnapshotRegistrationCounts(host);
}

static void ApplyStateToNewRegistrations(PicoHost *host, void *state)
{
    int slot;
    int i;
    if (!host)
    {
        return;
    }
    for (slot = 0; slot < PICO_SLOT_COUNT; slot++)
    {
        for (i = host->staged_view_count[slot]; i < host->view_count[slot]; i++)
        {
            host->views[slot][i].state = state;
        }
    }
    for (i = host->staged_empty_view_count; i < host->empty_view_count; i++)
    {
        host->empty_views[i].state = state;
    }
    for (i = host->staged_hook_count; i < host->hook_count; i++)
    {
        host->hooks[i].state = state;
    }
    for (i = host->staged_tool_before_hook_count; i < host->tool_before_hook_count; i++)
    {
        host->tool_before_hooks[i].state = state;
    }
    for (i = host->staged_tool_after_hook_count; i < host->tool_after_hook_count; i++)
    {
        host->tool_after_hooks[i].state = state;
    }
    for (i = host->staged_llm_hook_count; i < host->llm_hook_count; i++)
    {
        host->llm_hooks[i].state = state;
    }
    for (i = host->staged_context_hook_count; i < host->context_hook_count; i++)
    {
        host->context_hooks[i].state = state;
    }
    for (i = host->staged_tool_row_hook_count; i < host->tool_row_hook_count; i++)
    {
        host->tool_row_hooks[i].state = state;
    }
    for (i = host->staged_tool_count; i < host->tool_count; i++)
    {
        host->tools[i].state = state;
    }
    for (i = host->staged_command_count; i < host->command_count; i++)
    {
        host->commands[i].state = state;
    }
    for (i = host->staged_completer_count; i < host->completer_count; i++)
    {
        host->completers[i].state = state;
    }
    for (i = host->staged_provider_count; i < host->provider_count; i++)
    {
        host->providers[i].state = state;
    }
    for (i = host->staged_auth_count; i < host->auth_count; i++)
    {
        host->auths[i].state = state;
    }
}

static void SortSlotViews(PicoHost *host, PicoUiSlot slot)
{
    int n;
    int i;
    int j;
    PicoSlotView cur;
    if (!host || slot < 0 || slot >= PICO_SLOT_COUNT)
    {
        return;
    }
    n = host->view_count[slot];
    for (i = 1; i < n; i++)
    {
        cur = host->views[slot][i];
        j = i;
        while (j > 0 && host->views[slot][j - 1].z > cur.z)
        {
            host->views[slot][j] = host->views[slot][j - 1];
            j--;
        }
        host->views[slot][j] = cur;
    }
}

static void SortEmptyViews(PicoHost *host)
{
    int i;
    int j;
    PicoEmptyView cur;
    if (!host)
    {
        return;
    }
    for (i = 1; i < host->empty_view_count; i++)
    {
        cur = host->empty_views[i];
        j = i;
        while (j > 0 && host->empty_views[j - 1].z > cur.z)
        {
            host->empty_views[j] = host->empty_views[j - 1];
            j--;
        }
        host->empty_views[j] = cur;
    }
}

void PicoHost_PublishRegistration(PicoHost *host, void *state)
{
    int slot;
    if (!host)
    {
        return;
    }
    ApplyStateToNewRegistrations(host, state);
    for (slot = 0; slot < PICO_SLOT_COUNT; slot++)
    {
        SortSlotViews(host, slot);
    }
    SortEmptyViews(host);
    if (host->reg_scope == PICO_REG_WORKSPACE && host->reg_workspace)
    {
        host->reg_workspace->registration_generation++;
    }
    host->reg_scope = PICO_REG_NONE;
    host->reg_workspace = NULL;
    host->reg_state = NULL;
}

void PicoHost_DiscardRegistration(PicoHost *host)
{
    int slot;
    if (!host)
    {
        return;
    }
    for (slot = 0; slot < PICO_SLOT_COUNT; slot++)
    {
        if (host->view_count[slot] > host->staged_view_count[slot])
        {
            memset(&host->views[slot][host->staged_view_count[slot]], 0,
                   sizeof(PicoSlotView) * (size_t)(host->view_count[slot] - host->staged_view_count[slot]));
            host->view_count[slot] = host->staged_view_count[slot];
        }
    }
    if (host->empty_view_count > host->staged_empty_view_count)
    {
        memset(&host->empty_views[host->staged_empty_view_count], 0,
               sizeof(PicoEmptyView) * (size_t)(host->empty_view_count - host->staged_empty_view_count));
        host->empty_view_count = host->staged_empty_view_count;
    }
    host->hook_count = host->staged_hook_count;
    host->tool_before_hook_count = host->staged_tool_before_hook_count;
    host->tool_after_hook_count = host->staged_tool_after_hook_count;
    host->llm_hook_count = host->staged_llm_hook_count;
    host->context_hook_count = host->staged_context_hook_count;
    host->tool_row_hook_count = host->staged_tool_row_hook_count;
    host->tool_count = host->staged_tool_count;
    host->command_count = host->staged_command_count;
    host->completer_count = host->staged_completer_count;
    host->provider_count = host->staged_provider_count;
    host->auth_count = host->staged_auth_count;
    host->reg_scope = PICO_REG_NONE;
    host->reg_workspace = NULL;
    host->reg_state = NULL;
}

static void InsertSlotView(PicoHost *host, PicoUiSlot slot, int z, PicoSlotView view)
{
    int n;
    int i;
    if (!host || slot < 0 || slot >= PICO_SLOT_COUNT)
    {
        return;
    }
    n = host->view_count[slot];
    if (n >= PICO_MAX_SLOT_VIEWS)
    {
        return;
    }
    view.z = z;
    if (host->reg_scope != PICO_REG_NONE)
    {
        host->views[slot][n] = view;
        host->view_count[slot]++;
        return;
    }
    i = n;
    while (i > 0 && host->views[slot][i - 1].z > z)
    {
        host->views[slot][i] = host->views[slot][i - 1];
        i--;
    }
    host->views[slot][i] = view;
    host->view_count[slot]++;
}

void pico_host_set_hovered_clickable(PicoHost *host)
{
    if (host)
    {
        host->hovered_clickable = true;
    }
}

void pico_host_request_submit_cancel(PicoHost *host)
{
    if (host)
    {
        host->submit_cancel = true;
    }
}

void pico_host_set_agent_input(PicoHost *host, char *text)
{
    if (!host)
    {
        free(text);
        return;
    }
    free(host->agent_input);
    host->agent_input = text;
}

void pico_host_set_agent_parts(PicoHost *host, char *parts_json)
{
    if (!host)
    {
        free(parts_json);
        return;
    }
    free(host->agent_parts);
    host->agent_parts = parts_json;
}

void pico_host_add_view(PicoHost *host, PicoUiSlot slot, int z, PicoHostViewFn render)
{
    PicoSlotView view;
    if (!host || !render)
    {
        return;
    }
    memset(&view, 0, sizeof(view));
    view.host_render = render;
    InsertSlotView(host, slot, z, view);
}

void pico_workspace_add_view(PicoWorkspace *workspace, PicoUiSlot slot, int z, PicoWorkspaceViewFn render)
{
    PicoSlotView view;
    if (!workspace || !workspace->host || !render)
    {
        return;
    }
    memset(&view, 0, sizeof(view));
    view.workspace_render = render;
    view.workspace = workspace;
    InsertSlotView(workspace->host, slot, z, view);
}

static void InsertEmptyView(PicoHost *host, PicoEmptyKind kind, int z, PicoEmptyView view)
{
    int n;
    int i;
    if (!host)
    {
        return;
    }
    if (kind != PICO_EMPTY_ABOVE && kind != PICO_EMPTY_BELOW && kind != PICO_EMPTY_REPLACE)
    {
        return;
    }
    n = host->empty_view_count;
    if (n >= PICO_MAX_EMPTY_VIEWS)
    {
        return;
    }
    view.kind = kind;
    view.z = z;
    if (host->reg_scope != PICO_REG_NONE)
    {
        host->empty_views[n] = view;
        host->empty_view_count++;
        return;
    }
    i = n;
    while (i > 0 && host->empty_views[i - 1].z > z)
    {
        host->empty_views[i] = host->empty_views[i - 1];
        i--;
    }
    host->empty_views[i] = view;
    host->empty_view_count++;
}

void pico_host_add_empty_view(PicoHost *host, PicoEmptyKind kind, int z, PicoHostViewFn render)
{
    PicoEmptyView view;
    if (!host || !render)
    {
        return;
    }
    memset(&view, 0, sizeof(view));
    view.host_render = render;
    InsertEmptyView(host, kind, z, view);
}

void pico_workspace_add_empty_view(PicoWorkspace *workspace, PicoEmptyKind kind, int z,
                                   PicoWorkspaceViewFn render)
{
    PicoEmptyView view;
    if (!workspace || !workspace->host || !render)
    {
        return;
    }
    memset(&view, 0, sizeof(view));
    view.workspace_render = render;
    view.workspace = workspace;
    InsertEmptyView(workspace->host, kind, z, view);
}

void pico_host_add_hook(PicoHost *host, PicoHook hook, PicoHostHookFn fn)
{
    if (!host || !fn || host->hook_count >= PICO_MAX_HOOKS)
    {
        return;
    }
    host->hooks[host->hook_count].hook = hook;
    host->hooks[host->hook_count].host_fn = fn;
    host->hooks[host->hook_count].workspace_fn = NULL;
    host->hooks[host->hook_count].workspace = NULL;
    host->hook_count++;
}

void pico_workspace_add_hook(PicoWorkspace *workspace, PicoHook hook, PicoWorkspaceHookFn fn)
{
    PicoHost *host = workspace ? workspace->host : NULL;
    if (!host || !fn || host->hook_count >= PICO_MAX_HOOKS)
    {
        return;
    }
    host->hooks[host->hook_count].hook = hook;
    host->hooks[host->hook_count].host_fn = NULL;
    host->hooks[host->hook_count].workspace_fn = fn;
    host->hooks[host->hook_count].workspace = workspace;
    host->hook_count++;
}

void pico_add_tool_before_hook(PicoWorkspace *workspace, PicoToolBeforeFn fn)
{
    PicoHost *host = workspace ? workspace->host : NULL;
    if (!host || !fn || host->tool_before_hook_count >= PICO_MAX_TOOL_HOOKS)
    {
        return;
    }
    host->tool_before_hooks[host->tool_before_hook_count].fn = fn;
    host->tool_before_hook_count++;
}

void pico_add_tool_after_hook(PicoWorkspace *workspace, PicoToolAfterFn fn)
{
    PicoHost *host = workspace ? workspace->host : NULL;
    if (!host || !fn || host->tool_after_hook_count >= PICO_MAX_TOOL_HOOKS)
    {
        return;
    }
    host->tool_after_hooks[host->tool_after_hook_count].fn = fn;
    host->tool_after_hook_count++;
}

void pico_add_llm_hook(PicoWorkspace *workspace, PicoLlmHookFn fn)
{
    PicoHost *host = workspace ? workspace->host : NULL;
    if (!host || !fn || host->llm_hook_count >= PICO_MAX_LLM_HOOKS)
    {
        return;
    }
    host->llm_hooks[host->llm_hook_count].fn = fn;
    host->llm_hook_count++;
}

void pico_add_context_hook(PicoWorkspace *workspace, PicoContextHookFn fn)
{
    PicoHost *host = workspace ? workspace->host : NULL;
    if (!host || !fn || host->context_hook_count >= PICO_MAX_CONTEXT_HOOKS)
    {
        return;
    }
    host->context_hooks[host->context_hook_count].fn = fn;
    host->context_hook_count++;
}

void pico_status_warn(PicoHost *host, const char *msg)
{
    if (!host || !msg || !msg[0])
    {
        return;
    }
    size_t extra = strlen(msg) + 2;
    size_t old = host->status_warn ? strlen(host->status_warn) : 0;
    char *next = (char *)realloc(host->status_warn, old + extra);
    if (!next)
    {
        return;
    }
    host->status_warn = next;
    memcpy(host->status_warn + old, msg, extra - 1);
    host->status_warn[old + extra - 2] = '\n';
    host->status_warn[old + extra - 1] = '\0';
}

void pico_workspace_status_warn(PicoWorkspace *workspace, const char *msg)
{
    pico_status_warn(workspace ? workspace->host : NULL, msg);
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

static void ToolAddFail(PicoHost *host, const char *name, const char *reason)
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
    pico_status_warn(host, line);
}

bool pico_add_tool(PicoWorkspace *workspace, const char *name, const char *description,
                   const char *params_json, PicoToolFn run, PicoToolApplyFn apply)
{
    PicoHost *host = workspace ? workspace->host : NULL;
    if (!host)
    {
        return false;
    }
    if (!name || !name[0])
    {
        ToolAddFail(host, name, "missing name");
        return false;
    }
    if (!run)
    {
        ToolAddFail(host, name, "missing run function");
        return false;
    }
    const char *params_err = ToolParamsError(params_json);
    if (params_err)
    {
        ToolAddFail(host, name, params_err);
        return false;
    }
    if (host->tool_count >= PICO_MAX_TOOLS)
    {
        char reason[64];
        snprintf(reason, sizeof(reason), "tool limit reached (%d)", PICO_MAX_TOOLS);
        ToolAddFail(host, name, reason);
        return false;
    }
    for (int i = 0; i < host->tool_count; i++)
    {
        if (host->tools[i].name && strcmp(host->tools[i].name, name) == 0)
        {
            ToolAddFail(host, name, "already registered");
            return false;
        }
    }
    host->tools[host->tool_count].name = name;
    host->tools[host->tool_count].description = description;
    host->tools[host->tool_count].params_json = params_json;
    host->tools[host->tool_count].run = run;
    host->tools[host->tool_count].apply = apply;
    host->tools[host->tool_count].state = NULL;
    host->tool_count++;
    return true;
}

void pico_host_add_command(PicoHost *host, const char *name, const char *help, PicoHostCmdFn run)
{
    if (!host || !name || !run || host->command_count >= PICO_MAX_COMMANDS)
    {
        return;
    }
    host->commands[host->command_count].name = name;
    host->commands[host->command_count].help = help;
    host->commands[host->command_count].host_run = run;
    host->commands[host->command_count].workspace_run = NULL;
    host->commands[host->command_count].workspace = NULL;
    host->command_count++;
}

void pico_workspace_add_command(PicoWorkspace *workspace, const char *name, const char *help,
                                PicoWorkspaceCmdFn run)
{
    PicoHost *host = workspace ? workspace->host : NULL;
    if (!host || !name || !run || host->command_count >= PICO_MAX_COMMANDS)
    {
        return;
    }
    host->commands[host->command_count].name = name;
    host->commands[host->command_count].help = help;
    host->commands[host->command_count].host_run = NULL;
    host->commands[host->command_count].workspace_run = run;
    host->commands[host->command_count].workspace = workspace;
    host->command_count++;
}

void pico_host_add_completer(PicoHost *host, char trigger, bool bol_only, PicoHostCompleteQueryFn query,
                             PicoHostCompleteAcceptFn accept)
{
    if (!host || !query || host->completer_count >= PICO_MAX_COMPLETERS)
    {
        return;
    }
    memset(&host->completers[host->completer_count], 0, sizeof(PicoCompleter));
    host->completers[host->completer_count].trigger = trigger;
    host->completers[host->completer_count].bol_only = bol_only;
    host->completers[host->completer_count].host_query = query;
    host->completers[host->completer_count].host_accept = accept;
    host->completer_count++;
}

void pico_workspace_add_completer(PicoWorkspace *workspace, char trigger, bool bol_only,
                                  PicoWorkspaceCompleteQueryFn query, PicoWorkspaceCompleteAcceptFn accept)
{
    PicoHost *host = workspace ? workspace->host : NULL;
    if (!host || !query || host->completer_count >= PICO_MAX_COMPLETERS)
    {
        return;
    }
    memset(&host->completers[host->completer_count], 0, sizeof(PicoCompleter));
    host->completers[host->completer_count].trigger = trigger;
    host->completers[host->completer_count].bol_only = bol_only;
    host->completers[host->completer_count].workspace_query = query;
    host->completers[host->completer_count].workspace_accept = accept;
    host->completers[host->completer_count].workspace = workspace;
    host->completer_count++;
}

void pico_add_provider(PicoWorkspace *workspace, const PicoProvider *p)
{
    PicoHost *host = workspace ? workspace->host : NULL;
    if (!host || !p || !p->name || !p->name[0] || !p->stream || host->provider_count >= PICO_MAX_PROVIDERS)
    {
        return;
    }
    host->providers[host->provider_count] = *p;
    host->provider_count++;
}

const PicoProvider *pico_find_provider(const PicoHost *host, const char *name)
{
    if (!host || !name || !name[0])
    {
        return NULL;
    }
    for (int i = 0; i < host->provider_count; i++)
    {
        if (host->providers[i].name && strcmp(host->providers[i].name, name) == 0)
        {
            return &host->providers[i];
        }
    }
    return NULL;
}

void pico_clear_registrations(PicoHost *app)
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

void pico_run_hooks(PicoHost *host, PicoHook hook, PicoAgentId agent_id)
{
    PicoHookEvent event;
    if (!host)
    {
        return;
    }
    event.hook = hook;
    event.agent_id = agent_id;
    for (int i = 0; i < host->hook_count; i++)
    {
        if (host->hooks[i].hook != hook)
        {
            continue;
        }
        if (host->hooks[i].host_fn)
        {
            host->hooks[i].host_fn(host, &event, host->hooks[i].state);
        }
        if (host->hooks[i].workspace_fn && host->hooks[i].workspace)
        {
            host->hooks[i].workspace_fn(host->hooks[i].workspace, &event, host->hooks[i].state);
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

static void RunSlot(PicoHost *host, PicoUiSlot slot)
{
    const PicoAgent *selected = PicoHost_SelectedAgentConst(host);
    PicoAgentId selected_id = selected ? selected->id : 0;
    for (int i = 0; i < host->view_count[slot]; i++)
    {
        PicoSlotView *view = &host->views[slot][i];
        if (view->host_render)
        {
            view->host_render(host, view->state);
        }
        if (view->workspace_render && view->workspace)
        {
            view->workspace_render(view->workspace, selected_id, view->state);
        }
    }
}

void PicoAgent_AddMessage(PicoHost *app, PicoAgent *agent, PicoRole role, const char *markdown)
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

void PicoAgent_AppendAssistant(PicoHost *app, PicoAgent *agent, const char *text)
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

void PicoAgent_AddToolCallWithId(PicoHost *app, PicoAgent *agent, const char *call_id,
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

void PicoAgent_AddToolCall(PicoHost *app, PicoAgent *agent, const char *name, const char *args)
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

void PicoHost_AddMessage(PicoHost *app, PicoAgentId agent_id, PicoRole role, const char *markdown)
{
    PicoAgent_AddMessage(app, PicoHost_FindAgent(app, agent_id), role, markdown);
}

void PicoHost_AppendAssistant(PicoHost *app, PicoAgentId agent_id, const char *text)
{
    PicoAgent_AppendAssistant(app, PicoHost_FindAgent(app, agent_id), text);
}

void PicoHost_AddToolCall(PicoHost *app, PicoAgentId agent_id, const char *name, const char *args)
{
    PicoAgent_AddToolCall(app, PicoHost_FindAgent(app, agent_id), name, args);
}

void PicoHost_SetLastToolOutput(PicoHost *app, PicoAgentId agent_id, const char *output, bool is_error)
{
    PicoAgent_SetLastToolOutput(PicoHost_FindAgent(app, agent_id), output, is_error);
}

PicoSessionWriteResult pico_session_log_custom(PicoHost *app, PicoAgentId agent_id,
                                                const char *ext, const char *data_json)
{
    PicoAgent *agent = PicoHost_FindAgent(app, agent_id);
    if (!agent)
    {
        return PICO_SESSION_WRITE_FAILED;
    }
    return PicoSession_LogCustom(app, agent, ext, data_json);
}

void pico_agent_set_compact_summary(PicoHost *app, PicoAgentId agent_id, char *summary)
{
    PicoAgent *agent = PicoHost_FindAgent(app, agent_id);
    if (!agent)
    {
        free(summary);
        return;
    }
    free(agent->compact_summary);
    agent->compact_summary = summary;
}

static PicoResult SubmitPreparedTurn(PicoHost *host, PicoAgent *agent, const char *text,
                                     const char *display, const char *parts_json)
{
    char *normalized = NULL;
    const char *parts = NULL;
    bool has_text = text && text[0];
    bool has_parts;
    const char *shown;

    if (!host || !agent)
    {
        return PICO_NOT_FOUND;
    }
    if (parts_json && parts_json[0])
    {
        if (!pico_canonical_normalize_user_parts(parts_json, &normalized))
        {
            return PICO_INVALID;
        }
        parts = normalized;
    }
    has_parts = parts && parts[0] == '[';
    if (!has_text && !has_parts)
    {
        free(normalized);
        return PICO_INVALID;
    }
    if (!agent->runtime)
    {
        free(normalized);
        return PICO_INVALID;
    }
    if (PicoAgent_IsBusy(agent) || !PicoWorkspace_AcceptsNewWork(agent->workspace))
    {
        free(normalized);
        return PICO_BUSY;
    }
    if (!PicoAgent_RevalidateToolPolicy(host, agent))
    {
        pico_status_warn(host, "This agent's restricted tool policy references a tool that is not currently registered.");
        free(normalized);
        return PICO_INVALID;
    }
    shown = display && display[0] ? display : (text ? text : "");
    PicoAgent_AddMessage(host, agent, PICO_ROLE_USER, shown);
    PicoSession_LogUser(host, agent, text ? text : "", shown, parts);
    PicoAgent_StartTurnParts(host, agent, text, parts);
    free(normalized);
    return PICO_OK;
}

void PicoHost_Submit(PicoHost *app)
{
    PicoAgentId id = app ? app->selected_agent_id : 0;
    PicoAgent *active = PicoHost_FindAgent(app, id);
    if (!app || !active || !PicoWorkspace_AcceptsNewWork(active->workspace) ||
        active->state == PICO_AGENT_LLM_WAIT || active->state == PICO_AGENT_TOOL_WAIT ||
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
    PicoModel *model = PicoSettings_ActiveModel(active);
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
    pico_run_hooks(app, PICO_HOOK_BEFORE_SUBMIT, id);
    if (app->submit_cancel)
    {
        free(app->agent_input);
        app->agent_input = NULL;
        free(app->agent_parts);
        app->agent_parts = NULL;
        return;
    }
    active = PicoHost_FindAgent(app, id);
    if (!active)
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
    const char *text = app->agent_input && app->agent_input[0] ? app->agent_input : typed;
    char *display_owned = has_attach ? pico_composer_display_message(typed) : NULL;
    const char *display = display_owned ? display_owned : typed;
    app->chat_follow_bottom = true;
    if (SubmitPreparedTurn(app, active, text, display, app->agent_parts) != PICO_OK)
    {
        free(display_owned);
        free(app->agent_input);
        app->agent_input = NULL;
        free(app->agent_parts);
        app->agent_parts = NULL;
        return;
    }

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
    pico_run_hooks(app, PICO_HOOK_ON_SUBMIT, id);
}

void PicoHost_RequestSubmitCancel(PicoHost *host)
{
    pico_host_request_submit_cancel(host);
}

void PicoHost_Cancel(PicoHost *app)
{
    PicoAgentId id = app ? app->selected_agent_id : 0;
    pico_agent_cancel(app, id);
}

bool PicoUi_ModalOpen(const PicoHost *app)
{
    return pico_ui_modal_claimed(app) || PicoAgent_AskUiOpen(PicoHost_SelectedAgentConst(app));
}

static void PicoHost_InitFields(PicoHost *host, Font *fonts, bool safe_mode)
{
    memset(host, 0, sizeof(*host));
    host->next_workspace_id = 1;
    host->next_agent_id = 1;
    host->next_ask_id = 0;
    pthread_mutex_init(&host->settings_mu, NULL);
    pthread_mutex_init(&host->ask_id_mu, NULL);
    host->ask_id_mu_ready = true;
    host->fonts = fonts;
    host->chat_sel.msg = -1;
    host->chat_follow_bottom = true;
    host->chat_overflow = true;
    host->safe_mode = safe_mode;
    host->composer.capacity = 256;
    host->composer.text = (char *)malloc((size_t)host->composer.capacity);
    if (host->composer.text)
    {
        host->composer.text[0] = '\0';
    }
}

static PicoResult MapAgentResult(PicoAgentResult result)
{
    switch (result)
    {
    case PICO_AGENT_RESULT_OK:
        return PICO_OK;
    case PICO_AGENT_RESULT_INVALID:
        return PICO_INVALID;
    case PICO_AGENT_RESULT_NOT_FOUND:
        return PICO_NOT_FOUND;
    case PICO_AGENT_RESULT_BUSY:
        return PICO_BUSY;
    case PICO_AGENT_RESULT_LIMIT:
        return PICO_LIMIT;
    case PICO_AGENT_RESULT_SESSION_IN_USE:
        return PICO_SESSION_IN_USE;
    case PICO_AGENT_RESULT_SESSION_INVALID:
        return PICO_SESSION_INVALID;
    case PICO_AGENT_RESULT_NO_MEMORY:
        return PICO_NO_MEMORY;
    default:
        return PICO_INVALID;
    }
}

static int CanonicalizeWorkspacePath(const char *path, char *out, size_t cap)
{
    char trimmed[4096];
    char real[4096];
    struct stat st;
    size_t n;
    if (!path || !out || cap < 2)
    {
        return -1;
    }
    while (*path && isspace((unsigned char)*path))
    {
        path++;
    }
    if (!path[0])
    {
        return -1;
    }
    snprintf(trimmed, sizeof(trimmed), "%s", path);
    n = strlen(trimmed);
    while (n > 0 && isspace((unsigned char)trimmed[n - 1]))
    {
        trimmed[--n] = '\0';
    }
    if (!realpath(trimmed, real))
    {
        return -1;
    }
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

PicoResult pico_host_init(PicoHost **out, Font *fonts, bool safe_mode)
{
    PicoHost *host;
    if (out)
    {
        *out = NULL;
    }
    if (!out)
    {
        return PICO_INVALID;
    }
    if (g_pico_process_retired)
    {
        return PICO_INVALID;
    }
    host = (PicoHost *)calloc(1, sizeof(PicoHost));
    if (!host)
    {
        return PICO_NO_MEMORY;
    }
    Pico_DocsSetAppDir(GetApplicationDirectory());
    PicoHost_InitFields(host, fonts, safe_mode);
    if (curl_global_init(CURL_GLOBAL_DEFAULT) != CURLE_OK)
    {
        pthread_mutex_destroy(&host->ask_id_mu);
        free(host->composer.text);
        free(host);
        return PICO_NO_MEMORY;
    }
    host->curl_initialized = true;
    PicoHostPreferences_Load(host);
    PicoAuth_Load(host);
    *out = host;
    return PICO_OK;
}

int pico_workspace_count(const PicoHost *host)
{
    return host ? host->workspace_count : 0;
}

bool pico_workspace_info(const PicoHost *host, int index, PicoWorkspaceInfo *out)
{
    const PicoWorkspace *workspace;
    int i;
    if (!host || !out || index < 0 || index >= host->workspace_count || !host->workspaces[index])
    {
        return false;
    }
    workspace = host->workspaces[index];
    memset(out, 0, sizeof(*out));
    out->id = workspace->id;
    out->state = workspace->state;
    snprintf(out->path, sizeof(out->path), "%s", workspace->path);
    out->total_agent_count = workspace->count;
    for (i = 0; i < workspace->count; i++)
    {
        if (workspace->agents[i] && workspace->agents[i]->kind == PICO_AGENT_MAIN)
        {
            out->main_agent_count++;
        }
    }
    return true;
}

PicoResult pico_workspace_open(PicoHost *host, const char *path, PicoWorkspaceId *out)
{
    char canonical[4096];
    PicoWorkspace *workspace;
    int i;
    if (out)
    {
        *out = 0;
    }
    if (!host || host->terminal_shutdown || g_pico_process_retired)
    {
        return PICO_INVALID;
    }
    if (CanonicalizeWorkspacePath(path, canonical, sizeof(canonical)) != 0)
    {
        return PICO_INVALID;
    }
    for (i = 0; i < host->workspace_count; i++)
    {
        if (host->workspaces[i] && strcmp(host->workspaces[i]->path, canonical) == 0 &&
            host->workspaces[i]->state != PICO_WORKSPACE_CLOSED)
        {
            if (out)
            {
                *out = host->workspaces[i]->id;
            }
            return PICO_ALREADY_OPEN;
        }
    }
    if (host->workspace_count >= 1)
    {
        return PICO_LIMIT;
    }
    workspace = (PicoWorkspace *)calloc(1, sizeof(PicoWorkspace));
    if (!workspace)
    {
        return PICO_NO_MEMORY;
    }
    workspace->host = host;
    workspace->id = host->next_workspace_id++;
    snprintf(workspace->path, sizeof(workspace->path), "%s", canonical);
    workspace->state = PICO_WORKSPACE_OPEN;
    pthread_mutex_init(&workspace->settings_mu, NULL);
    pthread_mutex_init(&workspace->delegation_mu, NULL);
    pthread_mutex_init(&workspace->lifecycle_mu, NULL);
    pthread_mutex_init(&workspace->ui_post_mu, NULL);
    workspace->accepting_work = true;
    host->workspaces[host->workspace_count++] = workspace;
    PicoWorkspaceSettings_Load(workspace);
    if (out)
    {
        *out = workspace->id;
    }
    return PICO_OK;
}

PicoResult pico_workspace_request_reload(PicoHost *host, PicoWorkspaceId id)
{
    PicoWorkspace *workspace = PicoHost_FindWorkspace(host, id);
    if (!host || !workspace)
    {
        return PICO_NOT_FOUND;
    }
    if (workspace->state == PICO_WORKSPACE_CLOSING || workspace->state == PICO_WORKSPACE_CLOSED)
    {
        return PICO_BUSY;
    }
    PicoHost_RequestReload(host);
    if (workspace->state == PICO_WORKSPACE_OPEN)
    {
        workspace->state = PICO_WORKSPACE_RELOADING;
    }
    return PICO_OK;
}

PicoResult pico_workspace_request_close(PicoHost *host, PicoWorkspaceId id)
{
    PicoWorkspace *workspace = PicoHost_FindWorkspace(host, id);
    int i;
    if (!host || !workspace)
    {
        return PICO_NOT_FOUND;
    }
    if (workspace->state == PICO_WORKSPACE_CLOSED)
    {
        return PICO_INVALID;
    }
    workspace->state = PICO_WORKSPACE_CLOSING;
    PicoWorkspace_SetAcceptingWork(workspace, false);
    for (i = 0; i < workspace->count; i++)
    {
        if (workspace->agents[i])
        {
            PicoAgent_Cancel(workspace->agents[i]);
        }
    }
    return PICO_OK;
}

PicoResult pico_main_agent_create(PicoHost *host, PicoWorkspaceId workspace_id,
                                  const PicoAgentCreateOptions *options, PicoAgentId *out)
{
    PicoWorkspace *workspace = PicoHost_FindWorkspace(host, workspace_id);
    PicoAgentCreateOptions copy;
    if (out)
    {
        *out = 0;
    }
    if (!host || !workspace || !options)
    {
        return PICO_INVALID;
    }
    if (options->kind != PICO_AGENT_MAIN)
    {
        return PICO_INVALID;
    }
    if (workspace->state != PICO_WORKSPACE_OPEN && workspace->state != PICO_WORKSPACE_RELOADING)
    {
        return PICO_BUSY;
    }
    if (PicoHost_TotalAgentCount(host) >= PICO_MAX_TOTAL_AGENTS)
    {
        return PICO_LIMIT;
    }
    copy = *options;
    copy.kind = PICO_AGENT_MAIN;
    return MapAgentResult(pico_agent_create(host, &copy, out));
}

PicoResult pico_agent_submit(PicoHost *host, PicoAgentId id, const char *text, const char *parts_json)
{
    PicoAgent *agent;
    if (!host)
    {
        return PICO_INVALID;
    }
    agent = PicoHost_FindAgent(host, id);
    if (!agent)
    {
        return PICO_NOT_FOUND;
    }
    return SubmitPreparedTurn(host, agent, text, text, parts_json);
}

static void PicoHost_PumpLifecycle(PicoHost *host);

void pico_host_pump(PicoHost *host)
{
    PicoHost_PumpLifecycle(host);
}

PicoResult pico_host_init_and_start(PicoHost **out, Font *fonts, const char *workspace, bool safe_mode,
                                    PicoSessionStart session_start, const char *session_file)
{
    PicoResult result = pico_host_init(out, fonts, safe_mode);
    if (result != PICO_OK)
    {
        return result;
    }
    PicoHost_Start(*out, fonts, workspace, safe_mode, session_start, session_file);
    return PICO_OK;
}

void PicoHost_Start(PicoHost *host, Font *fonts, const char *workspace, bool safe_mode,
                    PicoSessionStart session_start, const char *session_file)
{
    PicoWorkspaceId workspace_id = 0;
    PicoAgentCreateOptions options;
    PicoAgentId initial_id = 0;
    PicoAgent *initial;
    if (!host)
    {
        return;
    }
    if (g_pico_process_retired)
    {
        host->terminal_shutdown = true;
        pico_status_warn(host, "Pico cannot be initialized again after a retained shutdown; exit the process.");
        return;
    }
    if (!host->ask_id_mu_ready)
    {
        Pico_DocsSetAppDir(GetApplicationDirectory());
        PicoHost_InitFields(host, fonts, safe_mode);
        if (curl_global_init(CURL_GLOBAL_DEFAULT) != CURLE_OK)
        {
            pico_status_warn(host, "Could not initialize HTTP.");
            return;
        }
        host->curl_initialized = true;
        PicoHostPreferences_Load(host);
        PicoAuth_Load(host);
    }
    else
    {
        host->fonts = fonts;
        host->safe_mode = safe_mode;
    }
    if (pico_workspace_open(host, workspace && workspace[0] ? workspace : ".", &workspace_id) != PICO_OK &&
        pico_workspace_count(host) == 0)
    {
        pico_status_warn(host, "Could not open the workspace.");
        return;
    }
    if (pico_workspace_count(host) == 0)
    {
        return;
    }
    workspace_id = host->workspaces[0]->id;
    memset(&options, 0, sizeof(options));
    options.kind = PICO_AGENT_MAIN;
    options.session_start = session_start == PICO_SESSION_NONE ? PICO_SESSION_NONE : PICO_SESSION_NEW;
    options.select = true;
    if (pico_main_agent_create(host, workspace_id, &options, &initial_id) != PICO_OK)
    {
        pico_status_warn(host, "Could not create the agent runtime.");
        return;
    }
    PicoPlugins_Load(host);
    PicoWorkspace_LoadProfiles(PicoHost_FindWorkspace(host, workspace_id));
    initial = PicoHost_FindAgent(host, initial_id);
    pico_run_hooks(host, PICO_HOOK_ON_SESSION_RESET, initial_id);
    if (session_file && session_file[0])
    {
        PicoSession_Reset(host, initial);
        PicoSession_Start(host, initial, session_start, session_file);
    }
    else if (session_start == PICO_SESSION_RESUME || host->workspaces[0]->settings.resume_last)
    {
        PicoSession_Start(host, initial, session_start, NULL);
    }
}

void PicoHost_RequestReload(PicoHost *app)
{
    if (!app || app->terminal_shutdown || g_pico_process_retired)
    {
        return;
    }
    app->reload_queued = true;
    PicoWorkspace *primary = PicoHost_PrimaryWorkspace(app);
    if (primary)
    {
        PicoWorkspace_SetAcceptingWork(primary, false);
    }
    if (!app->workspace_change_queued && primary && !PicoWorkspace_BlocksReload(primary))
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

bool PicoHost_ChangeWorkspace(PicoHost *app, const char *path)
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
    const char *ws = PicoWorkspace_Path(PicoHost_PrimaryWorkspaceConst(app));
    if (ResolveWorkspaceDir(ws[0] ? ws : ".", trimmed, resolved, sizeof(resolved)) != 0)
    {
        char shown[400];
        snprintf(shown, sizeof(shown), "%s", trimmed);
        char line[512];
        snprintf(line, sizeof(line), "Not a directory `%s`.", shown);
        PicoOverlay_Notify(app, line);
        return false;
    }

    char current[4096];
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
    PicoWorkspace *primary = PicoHost_PrimaryWorkspace(app);
    if (primary)
    {
        PicoWorkspace_SetAcceptingWork(primary, false);
    }

    char pretty[400];
    FormatHomePath(resolved, pretty, sizeof(pretty));
    char line[512];
    if (primary && PicoWorkspace_BlocksReload(primary))
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

static void WorkspacePreflightFailed(PicoHost *app, const char *message)
{
    app->pending_workspace[0] = '\0';
    app->workspace_change_queued = false;
    PicoWorkspace *primary = PicoHost_PrimaryWorkspace(app);
    if (primary)
    {
        PicoWorkspace_SetAcceptingWork(primary, !app->reload_queued);
    }
    pico_status_warn(app, message);
}

static void ApplyWorkspaceChange(PicoHost *host)
{
    char target[4096];
    PicoWorkspace *replacement;
    PicoWorkspace *old_ws;
    PicoAgent *initial;
    float prev_font_scale;
    char pretty[400];
    char line[512];

    snprintf(target, sizeof(target), "%s", host->pending_workspace);
    if (!target[0])
    {
        host->workspace_change_queued = false;
        PicoWorkspace *primary_ws = PicoHost_PrimaryWorkspace(host);
        if (primary_ws)
        {
            PicoWorkspace_SetAcceptingWork(primary_ws, !host->reload_queued);
        }
        return;
    }

    replacement = (PicoWorkspace *)calloc(1, sizeof(PicoWorkspace));
    if (!replacement)
    {
        WorkspacePreflightFailed(host, "Could not prepare a workspace for the new directory.");
        return;
    }
    replacement->host = host;
    replacement->id = host->next_workspace_id++;
    snprintf(replacement->path, sizeof(replacement->path), "%s", target);
    replacement->state = PICO_WORKSPACE_OPEN;
    pthread_mutex_init(&replacement->settings_mu, NULL);
    pthread_mutex_init(&replacement->delegation_mu, NULL);
    pthread_mutex_init(&replacement->lifecycle_mu, NULL);
    pthread_mutex_init(&replacement->ui_post_mu, NULL);
    replacement->accepting_work = true;

    initial = PicoAgent_Create(host, replacement);
    if (!initial)
    {
        (void)PicoWorkspace_Destroy(replacement);
        WorkspacePreflightFailed(host, "Could not prepare an agent for the new workspace.");
        return;
    }
    PicoAgent_PrepareReload(initial);

    old_ws = PicoHost_PrimaryWorkspace(host);
    PicoChat_InspectClose();
    if (!PicoWorkspace_Quiesce(old_ws))
    {
        (void)PicoAgent_Destroy(initial);
        (void)PicoWorkspace_Destroy(replacement);
        host->terminal_shutdown = true;
        g_pico_process_retired = true;
        PicoOverlay_Notify(host, "A worker detached during workspace transition; Pico must now exit.");
        return;
    }

    PicoPlugins_Shutdown(host);
    PicoWorkspace_Free(old_ws);
    host->workspaces[0] = replacement;
    host->workspace_count = 1;
    host->pending_workspace[0] = '\0';
    host->workspace_change_queued = false;
    host->reload_queued = true;
    prev_font_scale = Pico_FontScale();
    PicoHostPreferences_Load(host);
    PicoWorkspaceSettings_Load(replacement);
    if (Pico_FontScale() != prev_font_scale)
    {
        Clay_ResetMeasureTextCache();
    }
    PicoSettings_InitAgent(initial);
    if (!PicoWorkspace_AdoptInitial(replacement, initial))
    {
        (void)PicoAgent_Destroy(initial);
        host->terminal_shutdown = true;
        pico_status_warn(host, "Workspace replacement could not publish its prepared agent; Pico must exit.");
        return;
    }
    PicoPlugins_Load(host);
    PicoWorkspace_LoadProfiles(replacement);
    PicoWorkspace_RevalidateToolPolicies(replacement);
    PicoWorkspace_NotifySessions(replacement);
    PicoWorkspace_ReplayToolDetails(replacement);
    host->reload_queued = false;
    PicoWorkspace_SetAcceptingWork(replacement, true);
    PicoChatSel_Clear(host);
    memset(&host->chat_scrollbar, 0, sizeof(host->chat_scrollbar));
    host->chat_follow_bottom = true;
    host->chat_overflow = true;

    FormatHomePath(target, pretty, sizeof(pretty));
    snprintf(line, sizeof(line), "Workspace `%s`.", pretty);
    PicoOverlay_Notify(host, line);
}

static void PicoHost_PumpLifecycle(PicoHost *host)
{
    PicoWorkspace *workspace;
    int i;
    if (!host || host->terminal_shutdown || g_pico_process_retired)
    {
        return;
    }
    for (i = 0; i < host->workspace_count; i++)
    {
        workspace = host->workspaces[i];
        if (!workspace)
        {
            continue;
        }
        PicoWorkspace_Pump(workspace);
        if (workspace->state == PICO_WORKSPACE_RELOADING && host->reload_queued &&
            !PicoWorkspace_BlocksReload(workspace))
        {
            workspace->state = PICO_WORKSPACE_OPEN;
        }
    }
    if (host->workspace_change_queued)
    {
        PicoWorkspace *primary = PicoHost_PrimaryWorkspace(host);
        if (!primary || !PicoWorkspace_BlocksReload(primary))
        {
            ApplyWorkspaceChange(host);
        }
        return;
    }
    PicoWorkspace *primary = PicoHost_PrimaryWorkspace(host);
    if (host->reload_queued && primary && !PicoWorkspace_BlocksReload(primary))
    {
        PicoPlugins_Reload(host);
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

void PicoHost_ClearMessages(PicoHost *app, PicoAgentId agent_id)
{
    PicoAgent_ClearMessages(PicoHost_FindAgent(app, agent_id));
    if (app)
    {
        PicoChatSel_Clear(app);
    }
}

PicoHostShutdownResult PicoHost_Shutdown(PicoHost *host)
{
    int i;
    if (!host)
    {
        return PICO_HOST_SHUTDOWN_CLEAN;
    }
    if (g_pico_process_retired)
    {
        return PICO_HOST_SHUTDOWN_RETAINED;
    }
    PicoChat_InspectClose();
    bool clean = true;
    for (i = 0; i < host->workspace_count; i++)
    {
        if (host->workspaces[i])
        {
            if (!PicoWorkspace_Quiesce(host->workspaces[i]))
            {
                clean = false;
            }
        }
    }
    if (!clean)
    {
        host->terminal_shutdown = true;
        g_pico_process_retired = true;
        return PICO_HOST_SHUTDOWN_RETAINED;
    }
    PicoPlugins_Shutdown(host);
    for (i = 0; i < host->workspace_count; i++)
    {
        if (host->workspaces[i])
        {
            PicoWorkspace_Free(host->workspaces[i]);
            host->workspaces[i] = NULL;
        }
    }
    host->workspace_count = 0;
    PicoAuth_Free(host);
    PicoHost_ClearMessages(host, host->selected_agent_id);
    free(host->composer.text);
    free(host->status_warn);
    free(host->agent_input);
    free(host->agent_parts);
    if (host->curl_initialized)
    {
        curl_global_cleanup();
        host->curl_initialized = false;
    }
    if (host->ask_id_mu_ready)
    {
        pthread_mutex_destroy(&host->ask_id_mu);
        host->ask_id_mu_ready = false;
    }
    pthread_mutex_destroy(&host->settings_mu);
    memset(host, 0, sizeof(*host));
    return PICO_HOST_SHUTDOWN_CLEAN;
}

PicoHostShutdownResult pico_host_free(PicoHost *host)
{
    PicoHostShutdownResult result = PicoHost_Shutdown(host);
    if (result == PICO_HOST_SHUTDOWN_CLEAN)
    {
        free(host);
    }
    return result;
}

static Clay_RenderCommandArray CreateShellLayout(PicoHost *app, float delta_time)
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

static void UpdateChatScrollbarDrag(PicoHost *app)
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

static void UpdateChatFollowFromUserScroll(PicoHost *app, bool over_chat, bool modal_open, float wheel_y)
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

static bool ClayCapacityErrorOverlay(Clay_RenderCommandArray commands)
{
    for (int32_t i = 0; i < commands.length; i++)
    {
        Clay_RenderCommand *cmd = Clay_RenderCommandArray_Get(&commands, i);
        if (!cmd || cmd->commandType != CLAY_RENDER_COMMAND_TYPE_TEXT)
        {
            continue;
        }
        Clay_StringSlice text = cmd->renderData.text.stringContents;
        if (text.chars && text.length >= 11 && memcmp(text.chars, "Clay Error:", 11) == 0)
        {
            return true;
        }
    }
    return false;
}

static bool ClayLayoutUnusable(Clay_RenderCommandArray commands)
{
    return Pico_NeedsClayReinit() || ClayCapacityErrorOverlay(commands);
}

static void SkipClayPresent(Clay_RenderCommandArray commands)
{
    fprintf(stderr, "clay-scroll: skip present cmds=%d overlay=%d needs=%d\n", commands.length,
            ClayCapacityErrorOverlay(commands) ? 1 : 0, Pico_NeedsClayReinit() ? 1 : 0);
    GLFWwindow *win = GetWindowHandle();
    if (win)
    {
        glfwPollEvents();
    }
}

static Clay_RenderCommandArray RecoverClayLayoutIfNeeded(PicoHost *app, Clay_RenderCommandArray commands)
{
    for (int attempt = 0; ClayLayoutUnusable(commands) && attempt < 4; attempt++)
    {
        fprintf(stderr, "clay-scroll: recover attempt=%d cmds=%d overlay=%d needs=%d\n", attempt, commands.length,
                ClayCapacityErrorOverlay(commands) ? 1 : 0, Pico_NeedsClayReinit() ? 1 : 0);
        if (!Pico_NeedsClayReinit())
        {
            int32_t before = Clay_GetMaxElementCount();
            Clay_SetMaxElementCount(before * 2);
            fprintf(stderr, "clay-scroll: overlay without handler, doubled %d -> %d\n", (int)before,
                    (int)Clay_GetMaxElementCount());
        }
        Pico_ReinitClay(app->fonts, app->debug_enabled);
        commands = CreateShellLayout(app, 0.0f);
    }
    return commands;
}

void PicoHost_Frame(PicoHost *app)
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
    if (!PicoHost_SelectedAgent(app))
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
        PicoHost_RequestReload(app);
    }

    PicoPlugins_Poll(app);
    pico_host_pump(app);
    PicoScrollbar_BeginFrame();

    bool had_warn = app->status_warn != NULL;
    bool had_complete = PicoComplete_IsOpen();
    bool had_todo = PicoTodo_IsExpanded(app);
    bool had_modal = pico_ui_modal_claimed(app);
    PicoPlugins_OnFrame(app, GetFrameTime());
    if (!had_warn && !had_complete && !had_todo && !had_modal && IsKeyPressed(KEY_ESCAPE))
    {
        PicoAgentId id = app->selected_agent_id;
        PicoAgent *active = PicoHost_FindAgent(app, id);
        if (PicoAgent_IsBusy(active))
        {
            if (PicoAgent_CancelRequested(active))
            {
                pico_agent_force_cancel(app, id);
            }
            else
            {
                pico_agent_cancel(app, id);
            }
        }
        else if (active && active->state == PICO_AGENT_ERROR)
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

    Clay_RenderCommandArray render_commands =
        RecoverClayLayoutIfNeeded(app, CreateShellLayout(app, GetFrameTime()));

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
    if (ClayLayoutUnusable(render_commands))
    {
        (void)PicoChat_TakeVirtualRelayout();
        fprintf(stderr, "clay-scroll: skip restore/remember (layout unusable)\n");
    }
    else
    {
        PicoChat_HarvestVirtualHeights(app);
        relayout = PicoChat_TakeVirtualRelayout();
        if (Pico_RestoreClayScroll())
        {
            relayout = true;
        }
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
        render_commands = RecoverClayLayoutIfNeeded(app, CreateShellLayout(app, 0.0f));
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

    if (ClayLayoutUnusable(render_commands))
    {
        SkipClayPresent(render_commands);
        return;
    }

    BeginDrawing();
    ClearBackground((Color){(unsigned char)COLOR_BG.r, (unsigned char)COLOR_BG.g, (unsigned char)COLOR_BG.b, 255});
    Clay_Raylib_Render(render_commands, app->fonts);
    pico_run_hooks(app, PICO_HOOK_AFTER_RENDER, pico_agent_active(app));
    EndDrawing();
}
