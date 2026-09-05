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
#include <time.h>

enum {
    PICO_REG_NONE = 0,
    PICO_REG_HOST,
    PICO_REG_WORKSPACE,
};

#define PICO_MAX_MODULE_GENERATIONS 256
#define PICO_MAX_EXTENSION_SLOTS 64

typedef struct PicoModuleGeneration {
    char source[4096];
    char so_path[4096];
    time_t mtime;
    uint64_t content_hash;
    uint64_t generation;
    void *handle;
    PicoExt ext;
    int ref_count;
    bool builtin;
    bool desired;
    bool compile_failed;
} PicoModuleGeneration;

typedef struct PicoExtensionInstance {
    PicoModuleGeneration *module;
    void *state;
    bool host_scoped;
    bool initialized;
} PicoExtensionInstance;

typedef struct PicoHostStaging {
    /* Host staged registrations */
    PicoSlotView host_views[PICO_SLOT_COUNT][PICO_MAX_SLOT_VIEWS];
    int host_view_count[PICO_SLOT_COUNT];
    PicoHookEntry host_hooks[PICO_MAX_HOOKS];
    int host_hook_count;
    PicoCommand host_commands[PICO_MAX_COMMANDS];
    int host_command_count;
    PicoCompleter host_completers[PICO_MAX_COMPLETERS];
    int host_completer_count;
    PicoAuth host_auths[PICO_MAX_AUTH];
    int host_auth_count;

    /* Workspace staged registrations */
    PicoSlotView ws_views[PICO_SLOT_COUNT][PICO_MAX_SLOT_VIEWS];
    int ws_view_count[PICO_SLOT_COUNT];
    PicoEmptyView ws_empty_views[PICO_MAX_EMPTY_VIEWS];
    int ws_empty_view_count;
    PicoHookEntry ws_hooks[PICO_MAX_HOOKS];
    int ws_hook_count;
    PicoToolBeforeEntry ws_tool_before_hooks[PICO_MAX_TOOL_HOOKS];
    int ws_tool_before_hook_count;
    PicoToolAfterEntry ws_tool_after_hooks[PICO_MAX_TOOL_HOOKS];
    int ws_tool_after_hook_count;
    PicoLlmHookEntry ws_llm_hooks[PICO_MAX_LLM_HOOKS];
    int ws_llm_hook_count;
    PicoContextHookEntry ws_context_hooks[PICO_MAX_CONTEXT_HOOKS];
    int ws_context_hook_count;
    PicoToolRowEntry ws_tool_row_hooks[PICO_MAX_TOOL_ROW_HOOKS];
    int ws_tool_row_hook_count;
    PicoTool ws_tools[PICO_MAX_TOOLS];
    int ws_tool_count;
    PicoCommand ws_commands[PICO_MAX_COMMANDS];
    int ws_command_count;
    PicoCompleter ws_completers[PICO_MAX_COMPLETERS];
    int ws_completer_count;
    PicoProvider ws_providers[PICO_MAX_PROVIDERS];
    int ws_provider_count;
} PicoHostStaging;

typedef enum PicoPersistJobKind {
    PICO_PERSIST_JOB_SESSION = 0,
    PICO_PERSIST_JOB_CATALOG_ORDER,
} PicoPersistJobKind;

#define PICO_PERSIST_QUEUE_CAPACITY (PICO_MAX_TOTAL_AGENTS + 1)

typedef struct PicoSessionPersistJob {
    PicoPersistJobKind job_kind;
    PicoAgentId agent_id;
    PicoAgentKind kind;
    PicoSessionPersistence persistence;
    char session_id[40];
    char session_path[4096];
    char workspace_path[4096];
    char *header_json;
    char *event_json;
    char *catalog_order_json;
    uint64_t catalog_order_generation;
} PicoSessionPersistJob;

typedef struct PicoSessionPersistFailure {
    PicoAgentId agent_id;
    char session_id[40];
    char error[256];
} PicoSessionPersistFailure;

struct PicoHost {
    PicoWorkspace *workspaces[PICO_MAX_WORKSPACES];
    int workspace_count;
    int pump_rr_index;
    PicoAgentId selected_agent_id;

    PicoComposer composer;
    PicoHostPreferences preferences;
    Font *fonts;
    PicoSlotView views[PICO_SLOT_COUNT][PICO_MAX_SLOT_VIEWS];
    int view_count[PICO_SLOT_COUNT];
    PicoHookEntry hooks[PICO_MAX_HOOKS];
    int hook_count;
    char ui_modals[PICO_MAX_UI_MODALS][PICO_UI_MODAL_NAME];
    int ui_modal_count;
    PicoCommand commands[PICO_MAX_COMMANDS];
    int command_count;
    PicoCompleter completers[PICO_MAX_COMPLETERS];
    int completer_count;
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
    bool terminal_shutdown;
    const char *hovered_link;
    bool hovered_tool;
    bool hovered_clickable;
    bool hovered_drag;
    bool ui_drag_active;
    bool hovered_text;
    char *status_warn;

    uint64_t next_workspace_id;
    uint64_t next_agent_id;
    uint64_t next_ask_id;
    pthread_mutex_t ask_id_mu;
    pthread_mutex_t settings_mu;
    bool curl_initialized;
    bool ask_id_mu_ready;
    PicoPluginSlot host_plugins[PICO_MAX_EXTENSION_SLOTS];
    int host_plugin_count;
    PicoModuleGeneration *modules;
    int module_count;
    int module_capacity;
    uint64_t next_module_generation;
    double plugin_last_poll;
    struct PicoCompileJob *plugin_compile;
    bool plugin_compile_pending;
    bool plugin_reload_pending;
    bool plugin_reload_retry;
    bool plugin_rollout_pending;

    int reg_scope;
    PicoWorkspace *reg_workspace;
    PicoWorkspace *reg_workspace_target;
    void *reg_state;
    PicoHostStaging staging;

    bool persist_ready;
    bool persist_stop;
    pthread_t persist_thread;
    pthread_mutex_t persist_mu;
    pthread_cond_t persist_cv;
    PicoSessionPersistJob *persist_pending;
    int persist_pending_count;
    PicoAgentId persist_flight_agent_id;
    bool persist_flight_catalog_order;
    uint64_t persist_catalog_next_generation;
    uint64_t persist_catalog_completed_generation;
    uint64_t persist_catalog_failed_generation;
    char persist_catalog_error[256];
    PicoSessionPersistFailure persist_failures[PICO_MAX_TOTAL_AGENTS];
    int persist_failure_count;
};

bool PicoHost_AgentEscapeEnabled(const PicoHost *host, bool had_warn,
                                 bool had_complete, bool had_todo, bool had_modal);

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
    return agent ? agent->workspace : NULL;
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
        if (workspace)
        {
            pthread_mutex_init(&workspace->delegation_mu, NULL);
            pthread_mutex_init(&workspace->lifecycle_mu, NULL);
            pthread_mutex_init(&workspace->ui_post_mu, NULL);
            pthread_mutex_init(&workspace->settings_mu, NULL);
            workspace->accepting_work = true;
        }
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

int PicoHost_ResolveWorkspaceDir(const char *workspace, const char *arg, char *out, size_t cap);
bool PicoHost_ProcessRetired(void);
Clay_RenderCommandArray PicoHost_LayoutShell(PicoHost *host, float viewport_height, float delta_time);
PicoWorkspace *PicoHost_SourceWorkspace(const PicoHost *host, const char *source);
PicoWorkspace *PicoHost_FindWorkspace(PicoHost *host, PicoWorkspaceId id);
PicoAgent *PicoHost_FindAgent(PicoHost *host, PicoAgentId id);
const PicoAgent *PicoHost_FindAgentConst(const PicoHost *host, PicoAgentId id);
/* UI-only. Backend code must take an explicit agent ID or pointer. */
PicoAgent *PicoHost_SelectedAgent(PicoHost *host);
const PicoAgent *PicoHost_SelectedAgentConst(const PicoHost *host);
/* Host-extension replacement only. Does not pause or reload workspaces. */
void PicoHost_RequestHostReload(PicoHost *host);

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
        if (!workspace)
        {
            continue;
        }
        for (j = 0; j < workspace->count; j++)
        {
            if (workspace->agents[j] && workspace->agents[j]->id == id)
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
void PicoPlugins_CancelCompiles(PicoHost *host);
void PicoModule_Retain(PicoModuleGeneration *module);
void PicoModule_Release(PicoModuleGeneration *module);

void PicoHost_BeginRegistration(PicoHost *host, int scope, PicoWorkspace *workspace);
void PicoHost_BeginWorkspaceRegistration(PicoHost *host, PicoWorkspace *workspace,
                                         PicoWorkspace *target);
void PicoHost_PublishRegistration(PicoHost *host, void *state);
void PicoHost_DiscardRegistration(PicoHost *host);

bool PicoHostExtensions_Activate(PicoHost *host, PicoModuleGeneration *module);
void PicoHostExtensions_Shutdown(PicoHost *host);
void PicoHostExtensions_ShutdownModule(PicoHost *host, PicoModuleGeneration *module);
void PicoHostExtensions_OnFrame(PicoHost *host, float dt);
void *PicoHostExtensions_State(const PicoHost *host, const char *name);
bool PicoHostExtensions_Reload(PicoHost *host);
bool PicoHost_ExtensionDisabled(const PicoHost *host, const char *name);

#endif
