#ifndef PICO_HOST_INTERNAL_H
#define PICO_HOST_INTERNAL_H

#include "pico/app.h"
#include "pico/host.h"
#include "pico/plugin.h"
#include "workspace_internal.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum {
    PICO_REG_NONE = 0,
    PICO_REG_HOST,
    PICO_REG_WORKSPACE,
};

struct PicoHost {
    PicoWorkspace *workspaces[PICO_MAX_WORKSPACES];
    int workspace_count;
    PicoAgentId selected_agent_id;

    /* Cached pointer to the single live workspace manager in Phase 1. */
    PicoAgentManager *agents;

    PicoComposer composer;
    PicoSettings settings;
    Font *fonts;
    PicoSlotView views[PICO_SLOT_COUNT][PICO_MAX_SLOT_VIEWS];
    int view_count[PICO_SLOT_COUNT];
    PicoEmptyView empty_views[PICO_MAX_EMPTY_VIEWS];
    int empty_view_count;
    PicoHookEntry hooks[PICO_MAX_HOOKS];
    int hook_count;
    PicoToolBeforeEntry tool_before_hooks[PICO_MAX_TOOL_HOOKS];
    int tool_before_hook_count;
    PicoToolAfterEntry tool_after_hooks[PICO_MAX_TOOL_HOOKS];
    int tool_after_hook_count;
    PicoLlmHookEntry llm_hooks[PICO_MAX_LLM_HOOKS];
    int llm_hook_count;
    PicoContextHookEntry context_hooks[PICO_MAX_CONTEXT_HOOKS];
    int context_hook_count;
    PicoToolRowEntry tool_row_hooks[PICO_MAX_TOOL_ROW_HOOKS];
    int tool_row_hook_count;
    char ui_modals[PICO_MAX_UI_MODALS][PICO_UI_MODAL_NAME];
    int ui_modal_count;
    PicoTool tools[PICO_MAX_TOOLS];
    int tool_count;
    PicoCommand commands[PICO_MAX_COMMANDS];
    int command_count;
    PicoCompleter completers[PICO_MAX_COMPLETERS];
    int completer_count;
    PicoProvider providers[PICO_MAX_PROVIDERS];
    int provider_count;
    PicoAuth auths[PICO_MAX_AUTH];
    int auth_count;
    struct PicoAuthStore *auth_store;
    bool submit_cancel;
    char *agent_input;
    char *agent_parts;
    PicoScrollbar chat_scrollbar;
    PicoScrollbar composer_scrollbar;
    PicoChatSelect chat_sel;
    bool chat_follow_bottom;
    bool chat_overflow;
    bool composer_overflow;
    bool reinitialize_clay;
    bool debug_enabled;
    bool safe_mode;
    bool reload_queued;
    bool workspace_change_queued;
    bool terminal_shutdown;
    const char *hovered_link;
    bool hovered_tool;
    bool hovered_clickable;
    char pending_workspace[4096];
    char *status_warn;
    PicoModel *models;
    int model_count;

    uint64_t next_workspace_id;
    uint64_t next_agent_id;
    uint64_t next_ask_id;
    pthread_mutex_t ask_id_mu;
    bool curl_initialized;
    bool ask_id_mu_ready;

    int reg_scope;
    PicoWorkspace *reg_workspace;
    void *reg_state;
    int staged_view_count[PICO_SLOT_COUNT];
    int staged_empty_view_count;
    int staged_hook_count;
    int staged_tool_before_hook_count;
    int staged_tool_after_hook_count;
    int staged_llm_hook_count;
    int staged_context_hook_count;
    int staged_tool_row_hook_count;
    int staged_tool_count;
    int staged_command_count;
    int staged_completer_count;
    int staged_provider_count;
    int staged_auth_count;
};

static inline PicoWorkspace *PicoHost_PrimaryWorkspace(PicoHost *host)
{
    return (host && host->workspace_count > 0) ? host->workspaces[0] : NULL;
}

static inline const PicoWorkspace *PicoHost_PrimaryWorkspaceConst(const PicoHost *host)
{
    return (host && host->workspace_count > 0) ? host->workspaces[0] : NULL;
}

static inline const char *PicoWorkspace_Path(const PicoWorkspace *workspace)
{
    return (workspace && workspace->path[0]) ? workspace->path : "";
}

static inline PicoWorkspace *PicoAgent_Workspace(const PicoAgent *agent)
{
    return (agent && agent->manager) ? agent->manager->workspace : NULL;
}

static inline const char *PicoAgent_WorkspacePath(const PicoAgent *agent)
{
    return PicoWorkspace_Path(PicoAgent_Workspace(agent));
}

static inline void PicoHost_SetPath(PicoHost *host, const char *path)
{
    PicoWorkspace *workspace;
    if (!host)
    {
        return;
    }
    workspace = host->workspaces[0];
    if (!workspace)
    {
        workspace = (PicoWorkspace *)calloc(1, sizeof(*workspace));
        host->workspaces[0] = workspace;
        host->workspace_count = workspace ? 1 : 0;
    }
    if (!workspace)
    {
        return;
    }
    workspace->host = host;
    if (!workspace->id)
    {
        workspace->id = 1;
    }
    snprintf(workspace->path, sizeof(workspace->path), "%s", path ? path : "");
    workspace->state = PICO_WORKSPACE_OPEN;
}

static inline PicoHost *PicoWorkspace_Host(PicoWorkspace *workspace)
{
    return workspace ? workspace->host : NULL;
}

bool PicoHost_ProcessRetired(void);
PicoWorkspace *PicoHost_FindWorkspace(PicoHost *host, PicoWorkspaceId id);
const PicoWorkspace *PicoHost_FindWorkspaceConst(const PicoHost *host, PicoWorkspaceId id);
PicoAgent *PicoHost_FindAgent(PicoHost *host, PicoAgentId id);
const PicoAgent *PicoHost_FindAgentConst(const PicoHost *host, PicoAgentId id);
/* UI-only. Backend code must take an explicit agent ID or pointer. */
PicoAgent *PicoHost_SelectedAgent(PicoHost *host);
const PicoAgent *PicoHost_SelectedAgentConst(const PicoHost *host);

/* UI adapters: selected agent's workspace, else the host's primary workspace. */
static inline PicoWorkspace *PicoHost_SelectedWorkspace(PicoHost *host)
{
    PicoAgentId id = host ? host->selected_agent_id : 0;
    int i;
    int j;
    if (!host || id == 0)
    {
        return PicoHost_PrimaryWorkspace(host);
    }
    for (i = 0; i < host->workspace_count; i++)
    {
        PicoWorkspace *workspace = host->workspaces[i];
        PicoAgentManager *manager = workspace ? workspace->agents : NULL;
        if (!manager)
        {
            continue;
        }
        for (j = 0; j < manager->count; j++)
        {
            if (manager->agents[j] && manager->agents[j]->id == id)
            {
                return workspace;
            }
        }
    }
    return PicoHost_PrimaryWorkspace(host);
}

static inline const PicoWorkspace *PicoHost_SelectedWorkspaceConst(const PicoHost *host)
{
    return PicoHost_SelectedWorkspace((PicoHost *)host);
}

uint64_t PicoHost_AllocAskId(PicoHost *host);
int PicoHost_TotalAgentCount(const PicoHost *host);
void PicoHost_BeginRegistration(PicoHost *host, int scope, PicoWorkspace *workspace);
void PicoHost_PublishRegistration(PicoHost *host, void *state);
void PicoHost_DiscardRegistration(PicoHost *host);

#endif
