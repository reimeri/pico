#define _POSIX_C_SOURCE 200809L

#include "agent_manager.h"
#include "agent.h"
#include "json.h"
#include "session.h"
#include "settings.h"
#include "subagent_config.h"
#include "workspace.h"

#include <curl/curl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

typedef enum PicoDelegationState {
    PICO_DELEGATION_REQUESTED = 0,
    PICO_DELEGATION_STARTING,
    PICO_DELEGATION_RUNNING,
    PICO_DELEGATION_DONE,
    PICO_DELEGATION_ERROR,
    PICO_DELEGATION_CANCELLED,
    PICO_DELEGATION_ABANDONED,
} PicoDelegationState;

typedef struct PicoDelegationJob {
    struct PicoDelegationJob *next;
    pthread_mutex_t mu;
    pthread_cond_t cv;
    int refs;
    PicoDelegationState state;
    PicoAgentId parent_id;
    uint64_t parent_generation;
    PicoAgentId child_id;
    int child_message_start;
    char profile[65];
    char *task;
    char session_id[40];
    char model[128];
    char effort[PICO_EFFORT_LEN];
    char *result;
    bool result_is_error;
} PicoDelegationJob;

static void ProcessDelegationRequests(PicoAgentManager *manager);
static void ProcessDelegationTerminals(PicoAgentManager *manager);
void PicoApp_RunHookSnapshot(PicoApp *app, PicoHook hook, PicoAgentId agent_id,
                             const char *workspace_key, const char *workspace_path);
static void DropDelegations(PicoAgentManager *manager);

static int FindIndex(const PicoAgentManager *manager, PicoAgentId id)
{
    if (!manager || !id)
    {
        return -1;
    }
    for (int i = 0; i < manager->count; i++)
    {
        if (manager->agents[i] && manager->agents[i]->id == id)
        {
            return i;
        }
    }
    return -1;
}

PicoAgent *PicoAgentManager_Find(PicoAgentManager *manager, PicoAgentId id)
{
    int index = FindIndex(manager, id);
    return index >= 0 ? manager->agents[index] : NULL;
}

const PicoAgent *PicoAgentManager_FindConst(const PicoAgentManager *manager, PicoAgentId id)
{
    int index = FindIndex(manager, id);
    return index >= 0 ? manager->agents[index] : NULL;
}

PicoAgent *PicoAgentManager_Active(PicoAgentManager *manager)
{
    return PicoAgentManager_Find(manager, manager ? manager->active_id : 0);
}

const PicoAgent *PicoAgentManager_ActiveConst(const PicoAgentManager *manager)
{
    return PicoAgentManager_FindConst(manager, manager ? manager->active_id : 0);
}

PicoAgentManager *PicoAgentManager_Create(PicoApp *app)
{
    PicoAgentManager *manager = (PicoAgentManager *)calloc(1, sizeof(*manager));
    if (!manager)
    {
        return NULL;
    }
    manager->app = app;
    pthread_mutex_init(&manager->delegation_mu, NULL);
    pthread_mutex_init(&manager->lifecycle_mu, NULL);
    manager->accepting_work = true;
    if (curl_global_init(CURL_GLOBAL_DEFAULT) != CURLE_OK)
    {
        pthread_mutex_destroy(&manager->lifecycle_mu);
        pthread_mutex_destroy(&manager->delegation_mu);
        free(manager);
        return NULL;
    }
    manager->curl_initialized = true;
    return manager;
}

static void FreeAgentFieldsOnCreateFailure(PicoAgent *agent)
{
    (void)PicoAgent_Destroy(agent);
}

static bool CopyToolPolicy(PicoApp *app, PicoAgent *agent,
                           const char *const *tools, int tool_count)
{
    if (!tools)
    {
        return tool_count == 0;
    }
    if (tool_count < 0 || tool_count > PICO_MAX_TOOLS)
    {
        return false;
    }
    agent->allowed_tools = (char **)calloc((size_t)(tool_count > 0 ? tool_count : 1), sizeof(char *));
    if (!agent->allowed_tools)
    {
        return false;
    }
    for (int i = 0; i < tool_count; i++)
    {
        if (!tools[i] || !tools[i][0])
        {
            return false;
        }
        bool registered = false;
        for (int t = 0; t < app->tool_count; t++)
        {
            if (app->tools[t].name && strcmp(app->tools[t].name, tools[i]) == 0)
            {
                registered = true;
                break;
            }
        }
        for (int j = 0; j < i; j++)
        {
            if (strcmp(tools[j], tools[i]) == 0)
            {
                return false;
            }
        }
        if (!registered)
        {
            return false;
        }
        agent->allowed_tools[i] = JsonDup(tools[i]);
        if (!agent->allowed_tools[i])
        {
            return false;
        }
        agent->allowed_tool_count++;
    }
    return true;
}

static PicoAgentResult ConfigureAgent(PicoApp *app, PicoAgent *agent,
                                      const PicoAgentCreateOptions *options)
{
    if (!agent || !options)
    {
        return PICO_AGENT_RESULT_INVALID;
    }
    if (options->kind != PICO_AGENT_NORMAL && options->kind != PICO_AGENT_SUBAGENT)
    {
        return PICO_AGENT_RESULT_INVALID;
    }
    agent->kind = options->kind;
    agent->parent_id = options->parent_id;
    PicoAgent *parent = NULL;
    if (options->parent_id)
    {
        parent = PicoAgentManager_Find(app->agents, options->parent_id);
        if (!parent)
        {
            return PICO_AGENT_RESULT_NOT_FOUND;
        }
        if (options->kind != PICO_AGENT_SUBAGENT)
        {
            return PICO_AGENT_RESULT_INVALID;
        }
        agent->depth = parent->depth + 1;
        if (agent->depth > PICO_MAX_DELEGATION_DEPTH)
        {
            return PICO_AGENT_RESULT_LIMIT;
        }
    }
    else if (options->kind == PICO_AGENT_SUBAGENT)
    {
        return PICO_AGENT_RESULT_INVALID;
    }
    if (options->kind == PICO_AGENT_SUBAGENT)
    {
        if (options->workspace_key)
        {
            return PICO_AGENT_RESULT_INVALID;
        }
        snprintf(agent->workspace_key, sizeof(agent->workspace_key), "%s", parent->workspace_key);
        snprintf(agent->workspace_path, sizeof(agent->workspace_path), "%s", parent->workspace_path);
    }
    else
    {
        const PicoWorkspace *workspace = NULL;
        if (options->workspace_key)
        {
            workspace = app->workspaces
                            ? PicoWorkspaceRegistry_FindKey(app->workspaces, options->workspace_key)
                            : NULL;
            if (!workspace || !workspace->available)
            {
                return PICO_AGENT_RESULT_INVALID;
            }
        }
        else
        {
            const PicoAgent *active = PicoApp_ActiveAgentConst(app);
            if (active && active->kind == PICO_AGENT_NORMAL)
            {
                snprintf(agent->workspace_key, sizeof(agent->workspace_key), "%s",
                         active->workspace_key);
                snprintf(agent->workspace_path, sizeof(agent->workspace_path), "%s",
                         active->workspace_path);
            }
        }
        if (workspace)
        {
            snprintf(agent->workspace_key, sizeof(agent->workspace_key), "%s", workspace->key);
            snprintf(agent->workspace_path, sizeof(agent->workspace_path), "%s", workspace->path);
        }
        else if (!agent->workspace_path[0])
        {
            char canonical[4096];
            const char *source = app->workspace[0] ? app->workspace : ".";
            const char *path = realpath(source, canonical) ? canonical : source;
            snprintf(agent->workspace_path, sizeof(agent->workspace_path), "%s", path);
            if (!PicoWorkspace_EncodePath(path, agent->workspace_key,
                                          sizeof(agent->workspace_key)))
            {
                return PICO_AGENT_RESULT_INVALID;
            }
        }
    }
    if ((options->profile && strlen(options->profile) > 64) ||
        (options->purpose && strlen(options->purpose) > 1024))
    {
        return PICO_AGENT_RESULT_INVALID;
    }
    snprintf(agent->profile, sizeof(agent->profile), "%s", options->profile ? options->profile : "");
    snprintf(agent->purpose, sizeof(agent->purpose), "%s", options->purpose ? options->purpose : "");
    if (options->model && options->model[0])
    {
        const PicoModel *model = PicoSettings_FindModelConst(app, options->model);
        if (!model)
        {
            return PICO_AGENT_RESULT_INVALID;
        }
        snprintf(agent->model, sizeof(agent->model), "%s", model->id);
        agent->effort[0] = '\0';
        PicoSettings_SyncAgent(app, agent);
    }
    if (options->effort && options->effort[0])
    {
        const PicoModel *model = PicoSettings_ActiveModelConst(app, agent);
        if (!PicoSettings_EffortAllowed(model, options->effort) &&
            !(model && model->effort_count == 0 && strcmp(options->effort, "none") == 0))
        {
            return PICO_AGENT_RESULT_INVALID;
        }
        snprintf(agent->effort, sizeof(agent->effort), "%s", options->effort);
    }
    if (!CopyToolPolicy(app, agent, options->tools, options->tool_count))
    {
        return options->tools ? PICO_AGENT_RESULT_INVALID : PICO_AGENT_RESULT_NO_MEMORY;
    }
    return PICO_AGENT_RESULT_OK;
}

static void SetActive(PicoAgentManager *manager, PicoAgent *agent)
{
    if (!manager || !agent || agent->kind != PICO_AGENT_NORMAL)
    {
        return;
    }
    bool changed = manager->active_id != agent->id;
    manager->active_id = agent->id;
    agent->last_selected_seq = ++manager->selected_seq;
    agent->unread_completion = false;
    if (manager->app)
    {
        snprintf(manager->app->workspace, sizeof(manager->app->workspace), "%s",
                 agent->workspace_path);
        if (changed)
        {
            PicoApp_PrepareSelection(manager->app);
            agent->ui.restore_scroll = true;
        }
    }
}

static void PublishAgent(PicoAgentManager *manager, PicoAgent *agent, bool select)
{
    manager->agents[manager->count++] = agent;
    if ((!manager->active_id || select) && agent->kind == PICO_AGENT_NORMAL)
    {
        SetActive(manager, agent);
    }
}

PicoAgentId PicoAgent_RootId(const PicoAgentManager *manager, PicoAgentId id)
{
    const PicoAgent *agent = PicoAgentManager_FindConst(manager, id);
    while (agent && agent->parent_id)
    {
        const PicoAgent *parent = PicoAgentManager_FindConst(manager, agent->parent_id);
        if (!parent)
        {
            break;
        }
        agent = parent;
    }
    return agent ? agent->id : 0;
}

bool PicoAgent_InTree(const PicoAgentManager *manager, PicoAgentId root_id, PicoAgentId id)
{
    if (!manager || !root_id || !id)
    {
        return false;
    }
    while (id)
    {
        if (id == root_id)
        {
            return true;
        }
        const PicoAgent *agent = PicoAgentManager_FindConst(manager, id);
        if (!agent)
        {
            return false;
        }
        id = agent->parent_id;
    }
    return false;
}

PicoAgent *PicoAgentManager_MostRecentInWorkspace(PicoAgentManager *manager, const char *workspace_key)
{
    PicoAgent *best = NULL;
    if (!manager || !workspace_key || !workspace_key[0])
    {
        return NULL;
    }
    for (int i = 0; i < manager->count; i++)
    {
        PicoAgent *agent = manager->agents[i];
        if (!agent || agent->kind != PICO_AGENT_NORMAL ||
            strcmp(agent->workspace_key, workspace_key) != 0)
        {
            continue;
        }
        if (!best || agent->last_selected_seq > best->last_selected_seq)
        {
            best = agent;
        }
    }
    return best;
}

PicoAgent *PicoAgentManager_FindSession(PicoAgentManager *manager, const char *path)
{
    if (!manager || !path || !path[0])
    {
        return NULL;
    }
    for (int i = 0; i < manager->count; i++)
    {
        PicoAgent *agent = manager->agents[i];
        if (agent && agent->session_path[0] && strcmp(agent->session_path, path) == 0)
        {
            return agent;
        }
    }
    return NULL;
}

static void AskStoreRemoveAt(PicoAgentManager *manager, int index)
{
    if (!manager || index < 0 || index >= manager->ask_count)
    {
        return;
    }
    manager->asks[index] = manager->asks[--manager->ask_count];
}

void PicoAskStore_RemoveAgent(PicoApp *app, PicoAgentId id)
{
    PicoAgentManager *manager = app ? app->agents : NULL;
    if (!manager || !id)
    {
        return;
    }
    for (int i = manager->ask_count - 1; i >= 0; i--)
    {
        if (manager->asks[i].owner_id == id || manager->asks[i].root_id == id)
        {
            AskStoreRemoveAt(manager, i);
        }
    }
}

void PicoAskStore_RemoveGeneration(PicoApp *app, PicoAgentId id, uint64_t generation)
{
    PicoAgentManager *manager = app ? app->agents : NULL;
    if (!manager || !id)
    {
        return;
    }
    for (int i = manager->ask_count - 1; i >= 0; i--)
    {
        if (manager->asks[i].owner_id == id && manager->asks[i].runtime_generation == generation)
        {
            AskStoreRemoveAt(manager, i);
        }
    }
}

bool PicoAskStore_Get(const PicoApp *app, uint64_t ask_id, PicoAgentId *owner_out,
                      uint64_t *generation_out, PicoAgentId *root_out)
{
    const PicoAgentManager *manager = app ? app->agents : NULL;
    if (!manager || !ask_id)
    {
        return false;
    }
    for (int i = 0; i < manager->ask_count; i++)
    {
        if (manager->asks[i].ask_id == ask_id)
        {
            if (owner_out)
            {
                *owner_out = manager->asks[i].owner_id;
            }
            if (generation_out)
            {
                *generation_out = manager->asks[i].runtime_generation;
            }
            if (root_out)
            {
                *root_out = manager->asks[i].root_id;
            }
            return true;
        }
    }
    return false;
}

void PicoAskStore_Sync(PicoApp *app)
{
    PicoAgentManager *manager = app ? app->agents : NULL;
    if (!manager)
    {
        return;
    }
    uint64_t live[PICO_MAX_AGENTS];
    int live_n = 0;
    for (int i = 0; i < manager->count; i++)
    {
        PicoAgent *agent = manager->agents[i];
        PicoToolAsk ask;
        if (!agent || !PicoAgent_PendingAsk(agent, &ask) || live_n >= PICO_MAX_AGENTS)
        {
            continue;
        }
        live[live_n++] = ask.id;
        bool found = false;
        for (int j = 0; j < manager->ask_count; j++)
        {
            if (manager->asks[j].ask_id == ask.id)
            {
                manager->asks[j].owner_id = agent->id;
                manager->asks[j].runtime_generation = agent->runtime_generation;
                manager->asks[j].root_id = PicoAgent_RootId(manager, agent->id);
                found = true;
                break;
            }
        }
        if (!found && manager->ask_count < PICO_MAX_AGENTS)
        {
            PicoAskUiEntry *entry = &manager->asks[manager->ask_count++];
            entry->ask_id = ask.id;
            entry->owner_id = agent->id;
            entry->runtime_generation = agent->runtime_generation;
            entry->root_id = PicoAgent_RootId(manager, agent->id);
        }
    }
    for (int i = manager->ask_count - 1; i >= 0; i--)
    {
        bool keep = false;
        for (int j = 0; j < live_n; j++)
        {
            if (manager->asks[i].ask_id == live[j])
            {
                keep = true;
                break;
            }
        }
        if (!keep)
        {
            AskStoreRemoveAt(manager, i);
        }
    }
}

bool PicoAgentManager_TreeHasAsk(const PicoAgentManager *manager, PicoAgentId root_id)
{
    if (!manager || !root_id)
    {
        return false;
    }
    for (int i = 0; i < manager->count; i++)
    {
        PicoToolAsk ask;
        if (PicoAgent_InTree(manager, root_id, manager->agents[i]->id) &&
            PicoAgent_PendingAsk(manager->agents[i], &ask))
        {
            return true;
        }
    }
    return false;
}

bool PicoAgentManager_TreeHasError(const PicoAgentManager *manager, PicoAgentId root_id)
{
    if (!manager || !root_id)
    {
        return false;
    }
    for (int i = 0; i < manager->count; i++)
    {
        PicoAgent *agent = manager->agents[i];
        if (PicoAgent_InTree(manager, root_id, agent->id) &&
            (agent->error || agent->state == PICO_AGENT_ERROR))
        {
            return true;
        }
    }
    return false;
}

bool PicoAgentManager_TreeBusy(const PicoAgentManager *manager, PicoAgentId root_id)
{
    if (!manager || !root_id)
    {
        return false;
    }
    for (int i = 0; i < manager->count; i++)
    {
        if (PicoAgent_InTree(manager, root_id, manager->agents[i]->id) &&
            PicoAgent_IsBusy(manager->agents[i]))
        {
            return true;
        }
    }
    return false;
}

PicoAgentResult pico_agent_create(PicoApp *app, const PicoAgentCreateOptions *options,
                                  PicoAgentId *out)
{
    if (out)
    {
        *out = 0;
    }
    if (!app || !app->agents || !options)
    {
        return PICO_AGENT_RESULT_INVALID;
    }
    PicoAgentManager *manager = app->agents;
    PicoAgent_ReapRetired(manager);
    if (manager->count >= PICO_MAX_AGENTS)
    {
        return PICO_AGENT_RESULT_LIMIT;
    }
    PicoAgent *agent = PicoAgent_Create(app);
    if (!agent)
    {
        return PICO_AGENT_RESULT_NO_MEMORY;
    }
    PicoAgentResult result = ConfigureAgent(app, agent, options);
    if (result != PICO_AGENT_RESULT_OK)
    {
        FreeAgentFieldsOnCreateFailure(agent);
        return result;
    }

    if (options->session_start == PICO_SESSION_NONE)
    {
        PicoSession_Start(app, agent, PICO_SESSION_NONE, NULL);
    }
    else if (options->session_start == PICO_SESSION_RESUME)
    {
        char path[4096];
        if (!options->session_id || PicoSession_Resolve(app, agent, options->session_id, false,
                                                       path, sizeof(path)) != 0)
        {
            FreeAgentFieldsOnCreateFailure(agent);
            return PICO_AGENT_RESULT_SESSION_INVALID;
        }
        if (!PicoAgentManager_ReserveSession(manager, agent->id, path))
        {
            FreeAgentFieldsOnCreateFailure(agent);
            return PICO_AGENT_RESULT_SESSION_IN_USE;
        }
        agent->persistence = PICO_SESSION_DURABLE;
        if (PicoSession_Replay(app, agent, path, false) != 0)
        {
            PicoAgentManager_ReleaseSessions(manager, agent->id);
            FreeAgentFieldsOnCreateFailure(agent);
            return PICO_AGENT_RESULT_SESSION_INVALID;
        }
    }
    else
    {
        agent->persistence = PICO_SESSION_DURABLE;
    }

    PublishAgent(manager, agent, options->select);
    if (options->session_start == PICO_SESSION_RESUME)
    {
        PicoSession_AppendInterrupted(app, agent);
    }
    pico_run_hooks(app, PICO_HOOK_ON_SESSION_RESET, agent->id);
    if (out)
    {
        *out = agent->id;
    }
    return PICO_AGENT_RESULT_OK;
}

int pico_agent_count(const PicoApp *app)
{
    return app && app->agents ? app->agents->count : 0;
}

bool pico_agent_info(const PicoApp *app, int index, PicoAgentInfo *out)
{
    if (!app || !app->agents || !out || index < 0 || index >= app->agents->count)
    {
        return false;
    }
    PicoAgent_CopyInfo(app->agents->agents[index], out);
    return true;
}

bool pico_agent_find(const PicoApp *app, PicoAgentId id, PicoAgentInfo *out)
{
    const PicoAgent *agent = app && app->agents ? PicoAgentManager_FindConst(app->agents, id) : NULL;
    if (!agent || !out)
    {
        return false;
    }
    PicoAgent_CopyInfo(agent, out);
    return true;
}

PicoAgentId pico_agent_active(const PicoApp *app)
{
    return app && app->agents ? app->agents->active_id : 0;
}

bool pico_agent_select(PicoApp *app, PicoAgentId id)
{
    PicoAgent *agent = app && app->agents ? PicoAgentManager_Find(app->agents, id) : NULL;
    if (!agent || agent->kind != PICO_AGENT_NORMAL)
    {
        return false;
    }
    if (app->agents->active_id == id)
    {
        agent->unread_completion = false;
        return true;
    }
    SetActive(app->agents, agent);
    return true;
}

PicoAgentResult pico_agent_close(PicoApp *app, PicoAgentId id)
{
    if (!app || !app->agents)
    {
        return PICO_AGENT_RESULT_INVALID;
    }
    PicoAgentManager *manager = app->agents;
    PicoAgent_ReapRetired(manager);
    int index = FindIndex(manager, id);
    if (index < 0)
    {
        return PICO_AGENT_RESULT_NOT_FOUND;
    }
    PicoAgent *agent = manager->agents[index];
    int normal_count = 0;
    for (int i = 0; i < manager->count; i++)
    {
        normal_count += manager->agents[i]->kind == PICO_AGENT_NORMAL;
    }
    if ((agent->kind == PICO_AGENT_NORMAL && normal_count == 1) ||
        PicoAgent_IsBusy(agent) || PicoAgent_RetiredReferences(manager, id) ||
        PicoAgentManager_JobReferences(manager, id))
    {
        return PICO_AGENT_RESULT_BUSY;
    }
    char workspace_key[4096];
    char workspace_path[4096];
    snprintf(workspace_key, sizeof(workspace_key), "%s", agent->workspace_key);
    snprintf(workspace_path, sizeof(workspace_path), "%s", agent->workspace_path);
    PicoAskStore_RemoveAgent(app, id);
    if (!PicoAgent_Destroy(agent))
    {
        return PICO_AGENT_RESULT_BUSY;
    }
    for (int i = index + 1; i < manager->count; i++)
    {
        manager->agents[i - 1] = manager->agents[i];
    }
    manager->agents[--manager->count] = NULL;
    if (manager->active_id == id)
    {
        PicoAgent *fallback = PicoAgentManager_MostRecentInWorkspace(manager, workspace_key);
        if (!fallback)
        {
            for (int i = 0; i < manager->count; i++)
            {
                if (manager->agents[i]->kind == PICO_AGENT_NORMAL &&
                    (!fallback || manager->agents[i]->last_selected_seq > fallback->last_selected_seq))
                {
                    fallback = manager->agents[i];
                }
            }
        }
        if (fallback)
        {
            SetActive(manager, fallback);
        }
    }
    PicoAgentManager_ReleaseSessions(manager, id);
    PicoApp_RunHookSnapshot(app, PICO_HOOK_ON_AGENT_DESTROY, id,
                            workspace_key, workspace_path);
    return PICO_AGENT_RESULT_OK;
}

PicoAgentResult pico_agent_cancel(PicoApp *app, PicoAgentId id)
{
    PicoAgent *agent = app && app->agents ? PicoAgentManager_Find(app->agents, id) : NULL;
    if (!agent)
    {
        return PICO_AGENT_RESULT_NOT_FOUND;
    }
    PicoAgent_Cancel(agent);
    return PICO_AGENT_RESULT_OK;
}

PicoAgentResult pico_agent_force_cancel(PicoApp *app, PicoAgentId id)
{
    PicoAgent *agent = app && app->agents ? PicoAgentManager_Find(app->agents, id) : NULL;
    if (!agent)
    {
        return PICO_AGENT_RESULT_NOT_FOUND;
    }
    PicoAgent_ForceCancel(app, agent);
    return PICO_AGENT_RESULT_OK;
}

void PicoAgentManager_Pump(PicoAgentManager *manager)
{
    if (!manager)
    {
        return;
    }
    PicoAgent_ReapRetired(manager);
    ProcessDelegationRequests(manager);
    for (int i = 0; i < manager->count; i++)
    {
        PicoAgent_Pump(manager->app, manager->agents[i]);
    }
    ProcessDelegationTerminals(manager);
    PicoAskStore_Sync(manager->app);
}

bool PicoAgentManager_AcceptsNewWork(const PicoAgentManager *manager)
{
    if (!manager)
    {
        return true;
    }
    pthread_mutex_lock((pthread_mutex_t *)&manager->lifecycle_mu);
    bool accepting = manager->accepting_work && !manager->retained_shutdown;
    pthread_mutex_unlock((pthread_mutex_t *)&manager->lifecycle_mu);
    return accepting;
}

void PicoAgentManager_SetAcceptingWork(PicoAgentManager *manager, bool accepting)
{
    if (!manager)
    {
        return;
    }
    pthread_mutex_lock(&manager->lifecycle_mu);
    manager->accepting_work = accepting && !manager->retained_shutdown;
    pthread_mutex_unlock(&manager->lifecycle_mu);
}

bool PicoAgentManager_BlocksReload(const PicoAgentManager *manager)
{
    if (!manager)
    {
        return false;
    }
    if (manager->retired_count > 0)
    {
        return true;
    }
    pthread_mutex_lock((pthread_mutex_t *)&manager->delegation_mu);
    bool delegating = manager->delegations != NULL;
    pthread_mutex_unlock((pthread_mutex_t *)&manager->delegation_mu);
    if (delegating)
    {
        return true;
    }
    for (int i = 0; i < manager->count; i++)
    {
        if (PicoAgent_BlocksReload(manager->agents[i]))
        {
            return true;
        }
    }
    return false;
}

void PicoAgentManager_PrepareReload(PicoAgentManager *manager)
{
    if (!manager || PicoAgentManager_BlocksReload(manager))
    {
        return;
    }
    for (int i = 0; i < manager->count; i++)
    {
        PicoAgent_PrepareReload(manager->agents[i]);
    }
}

void PicoAgentManager_RevalidateToolPolicies(PicoAgentManager *manager)
{
    if (!manager)
    {
        return;
    }
    for (int i = 0; i < manager->count; i++)
    {
        PicoAgent *agent = manager->agents[i];
        if (!PicoAgent_RevalidateToolPolicy(manager->app, agent))
        {
            char line[384];
            if (agent->profile[0])
            {
                snprintf(line, sizeof(line),
                         "Agent %llu (profile %s) has a restricted tool policy containing an unavailable tool.",
                         (unsigned long long)agent->id, agent->profile);
            }
            else
            {
                snprintf(line, sizeof(line),
                         "Agent %llu has a restricted tool policy containing an unavailable tool.",
                         (unsigned long long)agent->id);
            }
            pico_status_warn(manager->app, line);
        }
    }
}

void PicoAgentManager_NotifySessions(PicoAgentManager *manager)
{
    for (int i = 0; manager && i < manager->count; i++)
    {
        pico_run_hooks(manager->app, PICO_HOOK_ON_SESSION_RESET, manager->agents[i]->id);
    }
}

bool PicoAgentManager_Destroy(PicoAgentManager *manager)
{
    if (!manager)
    {
        return true;
    }
    if (manager->retained_shutdown)
    {
        return false;
    }
    PicoAgentManager_SetAcceptingWork(manager, false);
    struct timespec deadline;
    clock_gettime(CLOCK_REALTIME, &deadline);
    deadline.tv_sec += 1;
    PicoAgentManager_CancelDelegations(manager, 0, 0);
    for (int i = 0; i < manager->count; i++)
    {
        PicoAgent_Cancel(manager->agents[i]);
    }
    bool clean = true;
    for (int i = 0; i < manager->count; i++)
    {
        pico_run_hooks(manager->app, PICO_HOOK_ON_AGENT_DESTROY, manager->agents[i]->id);
        if (!PicoAgent_DestroyBefore(manager->agents[i], &deadline))
        {
            clean = false;
        }
        manager->agents[i] = NULL;
    }
    manager->count = 0;
    if (!PicoAgent_ShutdownRetired(manager, &deadline))
    {
        clean = false;
    }
    if (!clean)
    {
        pthread_mutex_lock(&manager->lifecycle_mu);
        manager->retained_shutdown = true;
        manager->accepting_work = false;
        pthread_mutex_unlock(&manager->lifecycle_mu);
        manager->app = NULL;
        return false;
    }
    DropDelegations(manager);
    if (manager->curl_initialized)
    {
        curl_global_cleanup();
    }
    pthread_mutex_destroy(&manager->lifecycle_mu);
    pthread_mutex_destroy(&manager->delegation_mu);
    free(manager);
    return true;
}

bool PicoAgentManager_ReserveSession(PicoAgentManager *manager, PicoAgentId owner,
                                     const char *path)
{
    if (!manager || !owner || !path || !path[0] ||
        manager->reservation_count >= (int)(sizeof(manager->reservations) / sizeof(manager->reservations[0])))
    {
        return false;
    }
    for (int i = 0; i < manager->reservation_count; i++)
    {
        if (strcmp(manager->reservations[i].path, path) == 0)
        {
            return manager->reservations[i].owner == owner;
        }
    }
    PicoSessionReservation *reservation = &manager->reservations[manager->reservation_count++];
    reservation->owner = owner;
    snprintf(reservation->path, sizeof(reservation->path), "%s", path);
    return true;
}

void PicoAgentManager_ReleaseSessions(PicoAgentManager *manager, PicoAgentId owner)
{
    if (!manager)
    {
        return;
    }
    for (int i = 0; i < manager->reservation_count;)
    {
        if (manager->reservations[i].owner != owner)
        {
            i++;
            continue;
        }
        manager->reservations[i] = manager->reservations[--manager->reservation_count];
    }
}

bool PicoAgentManager_SessionReserved(const PicoAgentManager *manager, const char *path,
                                      PicoAgentId except_owner)
{
    for (int i = 0; manager && path && i < manager->reservation_count; i++)
    {
        if (manager->reservations[i].owner != except_owner &&
            strcmp(manager->reservations[i].path, path) == 0)
        {
            return true;
        }
    }
    return false;
}

bool pico_tool_pending_ask(const PicoApp *app, PicoToolAsk *out)
{
    if (!app || !app->agents || !out)
    {
        return false;
    }
    PicoAgentId root = pico_agent_active(app);
    bool found = false;
    PicoToolAsk oldest = {0};
    for (int i = 0; i < app->agents->count; i++)
    {
        PicoToolAsk ask;
        if (PicoAgent_InTree(app->agents, root, app->agents->agents[i]->id) &&
            PicoAgent_PendingAsk(app->agents->agents[i], &ask) &&
            (!found || ask.id < oldest.id))
        {
            oldest = ask;
            found = true;
        }
    }
    if (found)
    {
        *out = oldest;
    }
    return found;
}

bool pico_tool_answer(PicoApp *app, uint64_t id, const char *answer_json)
{
    if (!app || !app->agents || !id)
    {
        return false;
    }
    for (int i = 0; i < app->agents->count; i++)
    {
        if (PicoAgent_AnswerAsk(app->agents->agents[i], id, answer_json))
        {
            return true;
        }
    }
    return false;
}

int pico_agent_message_count(const PicoApp *app, PicoAgentId id)
{
    const PicoAgent *agent = app && app->agents ? PicoAgentManager_FindConst(app->agents, id) : NULL;
    return agent ? agent->message_count : 0;
}

const PicoMessage *pico_agent_message(const PicoApp *app, PicoAgentId id, int index)
{
    const PicoAgent *agent = app && app->agents ? PicoAgentManager_FindConst(app->agents, id) : NULL;
    return agent && index >= 0 && index < agent->message_count ? &agent->messages[index] : NULL;
}

void PicoAgentManager_ReplayToolDetails(PicoAgentManager *manager)
{
    for (int i = 0; manager && i < manager->count; i++)
    {
        PicoSession_ReplayToolDetails(manager->app, manager->agents[i]);
    }
}

void PicoAgentManager_LoadProfiles(PicoAgentManager *manager)
{
    PicoSubagentConfig_Load(manager);
}

int pico_subagent_profile_count(const PicoApp *app)
{
    return app && app->agents ? app->agents->profile_count : 0;
}

bool pico_subagent_profile_info(const PicoApp *app, int index,
                                PicoSubagentProfileInfo *out)
{
    if (!app || !app->agents || !out || index < 0 || index >= app->agents->profile_count)
    {
        return false;
    }
    *out = app->agents->profiles[index];
    return true;
}

PicoAgentResult PicoAgentManager_OpenSession(PicoApp *app, PicoAgent *workspace_agent,
                                             const char *id, bool allow_prefix, bool select)
{
    if (!app || !app->agents || !workspace_agent || !id || !id[0])
    {
        return PICO_AGENT_RESULT_INVALID;
    }
    char path[4096];
    if (PicoSession_Resolve(app, workspace_agent, id, allow_prefix, path, sizeof(path)) != 0)
    {
        return PICO_AGENT_RESULT_SESSION_INVALID;
    }
    PicoAgent *live = PicoAgentManager_FindSession(app->agents, path);
    if (live)
    {
        if (live->kind != PICO_AGENT_NORMAL)
        {
            return PICO_AGENT_RESULT_SESSION_IN_USE;
        }
        if (select && !pico_agent_select(app, live->id))
        {
            return PICO_AGENT_RESULT_INVALID;
        }
        return PICO_AGENT_RESULT_OK;
    }
    PicoSessionHeader header;
    if (PicoSession_ReadHeader(path, &header) != 0 || !header.id[0])
    {
        return PICO_AGENT_RESULT_SESSION_INVALID;
    }
    PicoAgentCreateOptions options = {
        .kind = PICO_AGENT_NORMAL,
        .workspace_key = workspace_agent->workspace_key,
        .session_start = PICO_SESSION_RESUME,
        .session_id = header.id,
        .select = select,
    };
    return pico_agent_create(app, &options, NULL);
}

PicoAgentResult PicoAgentManager_ResumeActive(PicoApp *app, const char *id, bool allow_prefix)
{
    return PicoAgentManager_OpenSession(app, PicoApp_ActiveAgent(app), id, allow_prefix, true);
}

static bool DelegationTerminal(PicoDelegationState state)
{
    return state == PICO_DELEGATION_DONE || state == PICO_DELEGATION_ERROR ||
           state == PICO_DELEGATION_CANCELLED || state == PICO_DELEGATION_ABANDONED;
}

static void DelegationRetain(PicoDelegationJob *job)
{
    pthread_mutex_lock(&job->mu);
    job->refs++;
    pthread_mutex_unlock(&job->mu);
}

static void DelegationRelease(PicoDelegationJob *job)
{
    if (!job)
    {
        return;
    }
    pthread_mutex_lock(&job->mu);
    bool free_job = --job->refs == 0;
    pthread_mutex_unlock(&job->mu);
    if (!free_job)
    {
        return;
    }
    pthread_mutex_destroy(&job->mu);
    pthread_cond_destroy(&job->cv);
    free(job->task);
    free(job->result);
    free(job);
}

static char *DelegationResultJson(const PicoDelegationJob *job, const char *status,
                                  const PicoAgent *child, const char *answer)
{
    bool resumable = child && child->persistence == PICO_SESSION_DURABLE &&
                     child->session_id[0] && child->session_path[0];
    JsonBuf b;
    JsonBuf_Init(&b);
    JsonBuf_Puts(&b, "{\"status\":");
    JsonBuf_String(&b, status);
    JsonBuf_Puts(&b, ",\"profile\":");
    JsonBuf_String(&b, job->profile);
    JsonBuf_Puts(&b, ",\"model\":");
    JsonBuf_String(&b, child ? child->model : job->model);
    JsonBuf_Puts(&b, ",\"effort\":");
    JsonBuf_String(&b, child ? child->effort : job->effort);
    if (resumable)
    {
        JsonBuf_Puts(&b, ",\"session_id\":");
        JsonBuf_String(&b, child->session_id);
    }
    JsonBuf_Puts(&b, ",\"resumable\":");
    JsonBuf_Bool(&b, resumable);
    JsonBuf_Puts(&b, ",\"final_answer\":");
    JsonBuf_String(&b, answer ? answer : "");
    JsonBuf_Putc(&b, '}');
    return JsonBuf_Steal(&b);
}

static bool PublishDelegation(PicoDelegationJob *job, PicoDelegationState state,
                              const char *status, const PicoAgent *child,
                              const char *answer, bool is_error)
{
    pthread_mutex_lock(&job->mu);
    if (DelegationTerminal(job->state))
    {
        pthread_mutex_unlock(&job->mu);
        return false;
    }
    char *result = DelegationResultJson(job, status, child, answer);
    if (!result)
    {
        result = JsonDup("{\"status\":\"error\",\"profile\":\"unknown\",\"resumable\":false,\"final_answer\":\"out of memory\"}");
        state = PICO_DELEGATION_ERROR;
        is_error = true;
    }
    free(job->result);
    job->result = result;
    job->result_is_error = is_error;
    job->state = state;
    pthread_cond_broadcast(&job->cv);
    pthread_mutex_unlock(&job->mu);
    return true;
}

static void RemoveDelegation(PicoAgentManager *manager, PicoDelegationJob *job)
{
    bool removed = false;
    pthread_mutex_lock(&manager->delegation_mu);
    PicoDelegationJob **link = &manager->delegations;
    while (*link)
    {
        if (*link == job)
        {
            *link = job->next;
            job->next = NULL;
            removed = true;
            break;
        }
        link = &(*link)->next;
    }
    pthread_mutex_unlock(&manager->delegation_mu);
    if (removed)
    {
        DelegationRelease(job);
    }
}

static const char *AgentResultText(PicoAgentResult result)
{
    switch (result)
    {
    case PICO_AGENT_RESULT_LIMIT: return "agent or delegation depth limit reached";
    case PICO_AGENT_RESULT_SESSION_IN_USE: return "session is already open";
    case PICO_AGENT_RESULT_SESSION_INVALID: return "session is invalid";
    case PICO_AGENT_RESULT_NO_MEMORY: return "out of memory";
    case PICO_AGENT_RESULT_NOT_FOUND: return "parent agent no longer exists";
    default: return "could not create subagent";
    }
}

static const char *LastAssistantSince(const PicoAgent *agent, int message_start)
{
    for (int i = agent ? agent->message_count - 1 : -1; i >= message_start; i--)
    {
        if (agent->messages[i].role == PICO_ROLE_ASSISTANT &&
            agent->messages[i].source && agent->messages[i].source[0])
        {
            return agent->messages[i].source;
        }
    }
    return "";
}

static bool StartDelegation(PicoAgentManager *manager, PicoDelegationJob *job)
{
    PicoApp *app = manager->app;
    PicoAgent *parent = PicoAgentManager_Find(manager, job->parent_id);
    if (!parent || parent->runtime_generation != job->parent_generation)
    {
        PublishDelegation(job, PICO_DELEGATION_ABANDONED, "cancelled", NULL,
                          "parent runtime is no longer live", true);
        return false;
    }
    const PicoSubagentProfileInfo *profile = PicoSubagentConfig_Find(manager, job->profile);
    if (!profile)
    {
        PublishDelegation(job, PICO_DELEGATION_ERROR, "error", NULL,
                          "unknown subagent profile", true);
        return false;
    }
    char model[128];
    char effort[PICO_EFFORT_LEN];
    if (!PicoSubagentConfig_Resolve(app, parent, profile, model, sizeof(model),
                                    effort, sizeof(effort)))
    {
        PublishDelegation(job, PICO_DELEGATION_ERROR, "error", NULL,
                          "profile model and effort could not be resolved", true);
        return false;
    }
    pthread_mutex_lock(&job->mu);
    snprintf(job->model, sizeof(job->model), "%s", model);
    snprintf(job->effort, sizeof(job->effort), "%s", effort);
    pthread_mutex_unlock(&job->mu);

    PicoSessionHeader header;
    memset(&header, 0, sizeof(header));
    if (job->session_id[0])
    {
        char path[4096];
        if (PicoSession_Resolve(app, parent, job->session_id, false, path, sizeof(path)) != 0 ||
            PicoSession_ReadHeader(path, &header) != 0)
        {
            PublishDelegation(job, PICO_DELEGATION_ERROR, "error", NULL,
                              "subagent session was not found or is invalid", true);
            return false;
        }
        if (header.kind != PICO_AGENT_SUBAGENT || strcmp(header.profile, profile->name) != 0)
        {
            PublishDelegation(job, PICO_DELEGATION_ERROR, "error", NULL,
                              "subagent session profile does not match", true);
            return false;
        }
    }

    const char *tools[PICO_MAX_TOOLS];
    for (int i = 0; i < profile->tool_count; i++)
    {
        tools[i] = profile->tools[i];
    }
    PicoAgentCreateOptions options = {
        .kind = PICO_AGENT_SUBAGENT,
        .parent_id = parent->id,
        .profile = profile->name,
        .purpose = profile->purpose,
        .model = model,
        .effort = effort,
        .tools = profile->restricted_tools ? tools : NULL,
        .tool_count = profile->restricted_tools ? profile->tool_count : 0,
        .session_start = job->session_id[0] ? PICO_SESSION_RESUME :
                         (parent->persistence == PICO_SESSION_DURABLE ? PICO_SESSION_NEW : PICO_SESSION_NONE),
        .session_id = job->session_id[0] ? job->session_id : NULL,
        .select = false,
    };
    PicoAgentId child_id = 0;
    PicoAgentResult created = pico_agent_create(app, &options, &child_id);
    if (created != PICO_AGENT_RESULT_OK)
    {
        PublishDelegation(job, PICO_DELEGATION_ERROR, "error", NULL,
                          AgentResultText(created), true);
        return false;
    }
    PicoAgent *child = PicoAgentManager_Find(manager, child_id);
    if (!child)
    {
        PublishDelegation(job, PICO_DELEGATION_ERROR, "error", NULL,
                          "created subagent was not published", true);
        return false;
    }

    char replayed_model[128];
    snprintf(replayed_model, sizeof(replayed_model), "%s", child->model);

    child->kind = PICO_AGENT_SUBAGENT;
    child->parent_id = parent->id;
    snprintf(child->profile, sizeof(child->profile), "%s", profile->name);
    snprintf(child->purpose, sizeof(child->purpose), "%s", profile->purpose);
    snprintf(child->model, sizeof(child->model), "%s", model);
    snprintf(child->effort, sizeof(child->effort), "%s", effort);
    if (!job->session_id[0])
    {
        snprintf(child->parent_session_id, sizeof(child->parent_session_id), "%s",
                 parent->persistence == PICO_SESSION_DURABLE ? parent->session_id : "");
    }
    PicoSettings_SyncAgent(app, child);
    if (job->session_id[0] && replayed_model[0] && strcmp(replayed_model, model) != 0)
    {
        PicoAgent_RotateCacheKey(child);
    }

    pthread_mutex_lock(&job->mu);
    job->child_id = child_id;
    job->child_message_start = child->message_count;
    job->state = PICO_DELEGATION_RUNNING;
    pthread_mutex_unlock(&job->mu);
    PicoAgent_AddMessage(app, child, PICO_ROLE_USER, job->task);
    PicoSession_LogUser(app, child, job->task, job->task);
    PicoAgent_StartTurn(app, child, job->task);
    return true;
}

static void ProcessDelegationRequests(PicoAgentManager *manager)
{
    for (;;)
    {
        PicoDelegationJob *job = NULL;
        pthread_mutex_lock(&manager->delegation_mu);
        for (PicoDelegationJob *it = manager->delegations; it; it = it->next)
        {
            pthread_mutex_lock(&it->mu);
            if (it->state == PICO_DELEGATION_REQUESTED)
            {
                it->state = PICO_DELEGATION_STARTING;
                job = it;
                pthread_mutex_unlock(&it->mu);
                break;
            }
            pthread_mutex_unlock(&it->mu);
        }
        pthread_mutex_unlock(&manager->delegation_mu);
        if (!job)
        {
            break;
        }
        if (!PicoAgentManager_AcceptsNewWork(manager))
        {
            PublishDelegation(job, PICO_DELEGATION_ERROR, "error", NULL,
                              "reload or workspace transition is pending", true);
            RemoveDelegation(manager, job);
        }
        else if (!StartDelegation(manager, job))
        {
            RemoveDelegation(manager, job);
        }
    }
}

static void CloseDelegationChild(PicoAgentManager *manager, PicoDelegationJob *job,
                                 PicoAgentId child_id)
{
    pthread_mutex_lock(&job->mu);
    job->child_id = 0;
    pthread_mutex_unlock(&job->mu);
    PicoAgentResult closed = pico_agent_close(manager->app, child_id);
    if (closed == PICO_AGENT_RESULT_OK || closed == PICO_AGENT_RESULT_NOT_FOUND)
    {
        RemoveDelegation(manager, job);
        return;
    }
    pthread_mutex_lock(&job->mu);
    job->child_id = child_id;
    pthread_mutex_unlock(&job->mu);
}

static void ProcessDelegationTerminals(PicoAgentManager *manager)
{
    PicoDelegationJob *jobs[PICO_MAX_AGENTS * 2];
    int count = 0;
    pthread_mutex_lock(&manager->delegation_mu);
    for (PicoDelegationJob *it = manager->delegations;
         it && count < (int)(sizeof(jobs) / sizeof(jobs[0])); it = it->next)
    {
        DelegationRetain(it);
        jobs[count++] = it;
    }
    pthread_mutex_unlock(&manager->delegation_mu);

    for (int i = 0; i < count; i++)
    {
        PicoDelegationJob *job = jobs[i];
        pthread_mutex_lock(&job->mu);
        PicoDelegationState state = job->state;
        PicoAgentId child_id = job->child_id;
        int child_message_start = job->child_message_start;
        pthread_mutex_unlock(&job->mu);
        PicoAgent *child = child_id ? PicoAgentManager_Find(manager, child_id) : NULL;

        if ((state == PICO_DELEGATION_CANCELLED || state == PICO_DELEGATION_ABANDONED) && child)
        {
            if (PicoAgent_IsBusy(child))
            {
                PicoAgent_Cancel(child);
            }
            else
            {
                CloseDelegationChild(manager, job, child_id);
            }
        }
        else if (state == PICO_DELEGATION_RUNNING)
        {
            if (!child)
            {
                PublishDelegation(job, PICO_DELEGATION_ERROR, "error", NULL,
                                  "subagent disappeared before completion", true);
                RemoveDelegation(manager, job);
            }
            else if (child->state == PICO_AGENT_ERROR)
            {
                PublishDelegation(job, PICO_DELEGATION_ERROR, "error", child,
                                  child->error ? child->error : "subagent failed", true);
                CloseDelegationChild(manager, job, child_id);
            }
            else if (!PicoAgent_IsBusy(child) && child->state == PICO_AGENT_IDLE)
            {
                PublishDelegation(job, PICO_DELEGATION_DONE, "completed", child,
                                  LastAssistantSince(child, child_message_start), false);
                CloseDelegationChild(manager, job, child_id);
            }
        }
        else if (DelegationTerminal(state))
        {
            if (child)
            {
                if (!PicoAgent_IsBusy(child))
                {
                    CloseDelegationChild(manager, job, child_id);
                }
            }
            else
            {
                RemoveDelegation(manager, job);
            }
        }
        DelegationRelease(job);
    }
}

char *PicoAgentManager_Delegate(PicoAgentContext *ctx, const char *profile,
                                const char *task, const char *session_id,
                                bool *is_error)
{
    if (is_error)
    {
        *is_error = true;
    }
    PicoAgentManager *manager = PicoAgentContext_Manager(ctx);
    if (!manager || !PicoAgentManager_AcceptsNewWork(manager))
    {
        return JsonDup("{\"status\":\"error\",\"profile\":\"unknown\",\"resumable\":false,\"final_answer\":\"reload or workspace transition is pending\"}");
    }
    if (!profile || !profile[0] || strlen(profile) > 64 ||
        !task || !task[0] || strlen(task) > 64 * 1024 ||
        (session_id && strlen(session_id) >= 40))
    {
        return JsonDup("{\"status\":\"error\",\"profile\":\"unknown\",\"resumable\":false,\"final_answer\":\"invalid subagent request\"}");
    }
    PicoDelegationJob *job = (PicoDelegationJob *)calloc(1, sizeof(*job));
    if (!job)
    {
        return JsonDup("{\"status\":\"error\",\"profile\":\"unknown\",\"resumable\":false,\"final_answer\":\"out of memory\"}");
    }
    pthread_mutex_init(&job->mu, NULL);
    pthread_cond_init(&job->cv, NULL);
    job->refs = 2;
    job->state = PICO_DELEGATION_REQUESTED;
    job->parent_id = pico_agent_context_id(ctx);
    job->parent_generation = pico_agent_context_generation(ctx);
    snprintf(job->profile, sizeof(job->profile), "%s", profile);
    snprintf(job->session_id, sizeof(job->session_id), "%s", session_id ? session_id : "");
    job->task = JsonDup(task);
    if (!job->task)
    {
        job->refs = 1;
        DelegationRelease(job);
        return JsonDup("{\"status\":\"error\",\"profile\":\"unknown\",\"resumable\":false,\"final_answer\":\"out of memory\"}");
    }

    if (!PicoAgentContext_LockIfLive(ctx))
    {
        char *result = DelegationResultJson(job, "cancelled", NULL,
                                            "parent cancelled delegation");
        job->refs = 1;
        DelegationRelease(job);
        return result ? result : JsonDup("{\"status\":\"cancelled\",\"profile\":\"unknown\",\"resumable\":false,\"final_answer\":\"parent cancelled delegation\"}");
    }
    pthread_mutex_lock(&manager->lifecycle_mu);
    if (!manager->accepting_work || manager->retained_shutdown)
    {
        pthread_mutex_unlock(&manager->lifecycle_mu);
        PicoAgentContext_UnlockLive(ctx);
        pthread_mutex_lock(&job->mu);
        job->state = PICO_DELEGATION_ERROR;
        job->result = DelegationResultJson(job, "error", NULL,
                                           "reload or workspace transition is pending");
        job->result_is_error = true;
        pthread_mutex_unlock(&job->mu);
        job->refs = 1;
        char *result = JsonDup(job->result);
        DelegationRelease(job);
        return result;
    }
    pthread_mutex_lock(&manager->delegation_mu);
    PicoDelegationJob **tail = &manager->delegations;
    while (*tail) tail = &(*tail)->next;
    *tail = job;
    pthread_mutex_unlock(&manager->delegation_mu);
    pthread_mutex_unlock(&manager->lifecycle_mu);
    PicoAgentContext_UnlockLive(ctx);

    pthread_mutex_lock(&job->mu);
    while (!DelegationTerminal(job->state))
    {
        pthread_cond_wait(&job->cv, &job->mu);
    }
    char *result = JsonDup(job->result ? job->result :
                           "{\"status\":\"error\",\"profile\":\"unknown\",\"resumable\":false,\"final_answer\":\"delegation ended without a result\"}");
    bool failed = job->result_is_error;
    pthread_mutex_unlock(&job->mu);
    if (is_error)
    {
        *is_error = failed;
    }
    DelegationRelease(job);
    return result;
}

void PicoAgentManager_CancelChildDelegation(PicoAgentManager *manager, PicoAgentId child_id)
{
    if (!manager || !child_id)
    {
        return;
    }
    PicoAgent *child = PicoAgentManager_Find(manager, child_id);
    pthread_mutex_lock(&manager->delegation_mu);
    for (PicoDelegationJob *job = manager->delegations; job; job = job->next)
    {
        pthread_mutex_lock(&job->mu);
        if (job->child_id == child_id && !DelegationTerminal(job->state))
        {
            free(job->result);
            job->result = DelegationResultJson(job, "cancelled", child,
                                               child ? LastAssistantSince(child,
                                                                         job->child_message_start) : "");
            job->result_is_error = true;
            job->state = PICO_DELEGATION_CANCELLED;
            pthread_cond_broadcast(&job->cv);
        }
        pthread_mutex_unlock(&job->mu);
    }
    pthread_mutex_unlock(&manager->delegation_mu);
}

void PicoAgentManager_CancelDelegations(PicoAgentManager *manager, PicoAgentId parent_id,
                                        uint64_t runtime_generation)
{
    if (!manager)
    {
        return;
    }
    PicoAgentId children[PICO_MAX_AGENTS];
    int child_count = 0;
    pthread_mutex_lock(&manager->delegation_mu);
    for (PicoDelegationJob *job = manager->delegations; job; job = job->next)
    {
        pthread_mutex_lock(&job->mu);
        bool match = parent_id == 0 ||
                     (job->parent_id == parent_id && job->parent_generation == runtime_generation);
        if (match && !DelegationTerminal(job->state))
        {
            free(job->result);
            job->result = DelegationResultJson(job, "cancelled", NULL,
                                               "parent cancelled delegation");
            job->result_is_error = true;
            job->state = PICO_DELEGATION_CANCELLED;
            if (job->child_id && child_count < PICO_MAX_AGENTS)
            {
                children[child_count++] = job->child_id;
            }
            pthread_cond_broadcast(&job->cv);
        }
        pthread_mutex_unlock(&job->mu);
    }
    pthread_mutex_unlock(&manager->delegation_mu);
    for (int i = 0; i < child_count; i++)
    {
        PicoAgent *child = PicoAgentManager_Find(manager, children[i]);
        if (child)
        {
            PicoAgent_Cancel(child);
        }
    }
}

bool PicoAgentManager_JobReferences(const PicoAgentManager *manager, PicoAgentId id)
{
    if (!manager || !id)
    {
        return false;
    }
    bool found = false;
    pthread_mutex_lock((pthread_mutex_t *)&manager->delegation_mu);
    for (PicoDelegationJob *job = manager->delegations; job && !found; job = job->next)
    {
        pthread_mutex_lock(&job->mu);
        found = job->parent_id == id || job->child_id == id;
        pthread_mutex_unlock(&job->mu);
    }
    pthread_mutex_unlock((pthread_mutex_t *)&manager->delegation_mu);
    return found;
}

static void DropDelegations(PicoAgentManager *manager)
{
    pthread_mutex_lock(&manager->delegation_mu);
    PicoDelegationJob *job = manager->delegations;
    manager->delegations = NULL;
    pthread_mutex_unlock(&manager->delegation_mu);
    while (job)
    {
        PicoDelegationJob *next = job->next;
        job->next = NULL;
        DelegationRelease(job);
        job = next;
    }
}
