#define _POSIX_C_SOURCE 200809L

#include "workspace_internal.h"
#include "agent.h"
#include "json.h"
#include "session.h"
#include "settings.h"
#include "subagent_config.h"
#include "host_internal.h"

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
    char *call_id;
    char *result;
    bool result_is_error;
} PicoDelegationJob;

static void ProcessDelegationRequests(PicoWorkspace *workspace);
static void ProcessDelegationTerminals(PicoWorkspace *workspace);
static void DropDelegations(PicoWorkspace *workspace);
static void LinkDelegationToolRows(PicoWorkspace *workspace);

static int FindIndex(const PicoWorkspace *workspace, PicoAgentId id)
{
    if (!workspace || !id)
    {
        return -1;
    }
    for (int i = 0; i < workspace->count; i++)
    {
        if (workspace->agents[i] && workspace->agents[i]->id == id)
        {
            return i;
        }
    }
    return -1;
}

PicoAgent *PicoWorkspace_FindAgent(PicoWorkspace *workspace, PicoAgentId id)
{
    int index = FindIndex(workspace, id);
    return index >= 0 ? workspace->agents[index] : NULL;
}

const PicoAgent *PicoWorkspace_FindAgentConst(const PicoWorkspace *workspace, PicoAgentId id)
{
    int index = FindIndex(workspace, id);
    return index >= 0 ? workspace->agents[index] : NULL;
}

static void SyncSelectedAgent(PicoHost *host, PicoAgentId id)
{
    PicoAgent *agent;
    if (!host)
    {
        return;
    }
    host->selected_agent_id = id;
    agent = PicoHost_FindAgent(host, id);
    if (agent)
    {
        PicoSession_SetUnseenComplete(host, agent, false);
    }
}

static bool IsUnusedPendingDraft(const PicoAgent *agent)
{
    return agent && agent->kind == PICO_AGENT_MAIN && agent->persistence == PICO_SESSION_DURABLE &&
           !agent->session_id[0] && !agent->session_path[0] && agent->message_count == 0 &&
           !PicoAgent_IsBusy(agent);
}

static void SelectAgentAndDiscardDraft(PicoHost *host, PicoAgentId id)
{
    PicoAgentId previous;
    PicoAgent *old;
    if (!host)
    {
        return;
    }
    previous = host->selected_agent_id;
    SyncSelectedAgent(host, id);
    if (!previous || previous == id)
    {
        return;
    }
    old = PicoHost_FindAgent(host, previous);
    if (IsUnusedPendingDraft(old))
    {
        (void)pico_agent_close(host, previous);
    }
}

static PicoUiMailbox *FindMailbox(PicoWorkspace *workspace, PicoAgentId agent_id, uint64_t generation, const char *name)
{
    int i;
    if (!workspace || !name || !name[0] || !agent_id)
    {
        return NULL;
    }
    for (i = 0; i < workspace->ui_mailbox_count; i++)
    {
        PicoUiMailbox *box = &workspace->ui_mailboxes[i];
        if (box->agent_id == agent_id && box->generation == generation && strcmp(box->name, name) == 0)
        {
            return box;
        }
    }
    return NULL;
}

static void FreeMailbox(PicoUiMailbox *box)
{
    if (!box)
    {
        return;
    }
    free(box->text);
    free(box->pub_text);
    memset(box, 0, sizeof(*box));
}

static void RemoveMailboxAt(PicoWorkspace *workspace, int index)
{
    if (!workspace || index < 0 || index >= workspace->ui_mailbox_count)
    {
        return;
    }
    FreeMailbox(&workspace->ui_mailboxes[index]);
    workspace->ui_mailbox_count--;
    if (index < workspace->ui_mailbox_count)
    {
        workspace->ui_mailboxes[index] = workspace->ui_mailboxes[workspace->ui_mailbox_count];
        memset(&workspace->ui_mailboxes[workspace->ui_mailbox_count], 0, sizeof(workspace->ui_mailboxes[0]));
    }
}

static bool MailboxAppend(PicoUiMailbox *box, const char *s, size_t n)
{
    size_t room;
    char *next;
    if (!box || !s || n == 0)
    {
        return true;
    }
    room = PICO_UI_POST_TEXT_MAX > box->text_len ? PICO_UI_POST_TEXT_MAX - box->text_len : 0;
    if (room == 0)
    {
        return true;
    }
    if (n > room)
    {
        n = room;
    }
    next = (char *)realloc(box->text, box->text_len + n + 1);
    if (!next)
    {
        return false;
    }
    memcpy(next + box->text_len, s, n);
    box->text_len += n;
    next[box->text_len] = '\0';
    box->text = next;
    return true;
}

void PicoWorkspace_UiPost(PicoWorkspace *workspace, const char *name, PicoUiPostKind kind,
                          PicoAgentId agent_id, uint64_t generation, const char *text, size_t n)
{
    PicoUiMailbox *box;
    if (!workspace || !name || !name[0] || !generation || !agent_id ||
        (kind != PICO_UI_POST_TEXT && kind != PICO_UI_POST_STATUS))
    {
        return;
    }
    if (kind == PICO_UI_POST_TEXT && (!text || n == 0))
    {
        return;
    }
    if (!text)
    {
        text = "";
        n = 0;
    }
    pthread_mutex_lock(&workspace->ui_post_mu);
    {
        const PicoAgent *agent = PicoWorkspace_FindAgentConst(workspace, agent_id);
        if (!agent || agent->runtime_generation != generation)
        {
            pthread_mutex_unlock(&workspace->ui_post_mu);
            return;
        }
    }
    box = FindMailbox(workspace, agent_id, generation, name);
    if (!box)
    {
        if (workspace->ui_mailbox_count >= PICO_MAX_UI_POSTS)
        {
            pthread_mutex_unlock(&workspace->ui_post_mu);
            return;
        }
        box = &workspace->ui_mailboxes[workspace->ui_mailbox_count];
        memset(box, 0, sizeof(*box));
        snprintf(box->name, sizeof(box->name), "%s", name);
        box->agent_id = agent_id;
        box->generation = generation;
        workspace->ui_mailbox_count++;
    }
    if (kind == PICO_UI_POST_STATUS)
    {
        size_t copy = n < PICO_UI_POST_STATUS_MAX ? n : PICO_UI_POST_STATUS_MAX;
        memcpy(box->status, text, copy);
        box->status[copy] = '\0';
        box->dirty = true;
    }
    else if (MailboxAppend(box, text, n))
    {
        box->dirty = true;
    }
    pthread_mutex_unlock(&workspace->ui_post_mu);
}

void PicoWorkspace_PumpUiPosts(PicoWorkspace *workspace)
{
    int i;
    if (!workspace)
    {
        return;
    }
    pthread_mutex_lock(&workspace->ui_post_mu);
    for (i = 0; i < workspace->ui_mailbox_count;)
    {
        PicoUiMailbox *box = &workspace->ui_mailboxes[i];
        const PicoAgent *agent;
        bool live;
        if (!box->dirty)
        {
            i++;
            continue;
        }
        agent = PicoWorkspace_FindAgentConst(workspace, box->agent_id);
        live = agent && agent->runtime_generation == box->generation;
        if (!live)
        {
            box->dirty = false;
            if (!box->published)
            {
                RemoveMailboxAt(workspace, i);
                continue;
            }
            i++;
            continue;
        }
        box->pub_agent_id = box->agent_id;
        box->pub_generation = box->generation;
        memcpy(box->pub_status, box->status, sizeof(box->pub_status));
        free(box->pub_text);
        box->pub_text = NULL;
        if (box->text && box->text_len)
        {
            box->pub_text = (char *)malloc(box->text_len + 1);
            if (box->pub_text)
            {
                memcpy(box->pub_text, box->text, box->text_len + 1);
            }
        }
        box->published = true;
        box->dirty = false;
        i++;
    }
    pthread_mutex_unlock(&workspace->ui_post_mu);
}

bool PicoWorkspace_UiLatest(const PicoWorkspace *workspace, PicoAgentId agent_id, const char *name, PicoUiPost *out)
{
    bool found = false;
    if (!out)
    {
        return false;
    }
    memset(out, 0, sizeof(*out));
    if (!workspace || !name || !name[0] || agent_id == 0)
    {
        return false;
    }
    pthread_mutex_lock((pthread_mutex_t *)&workspace->ui_post_mu);
    for (int i = 0; i < workspace->ui_mailbox_count; i++)
    {
        const PicoUiMailbox *box = &workspace->ui_mailboxes[i];
        if (box->published && box->pub_agent_id == agent_id && strcmp(box->name, name) == 0)
        {
            out->agent_id = box->pub_agent_id;
            out->generation = box->pub_generation;
            out->status = box->pub_status;
            out->text = box->pub_text ? box->pub_text : "";
            found = true;
            break;
        }
    }
    pthread_mutex_unlock((pthread_mutex_t *)&workspace->ui_post_mu);
    return found;
}

void PicoWorkspace_UiClear(PicoWorkspace *workspace, PicoAgentId agent_id, const char *name)
{
    if (!workspace || !name || !name[0] || agent_id == 0)
    {
        return;
    }
    pthread_mutex_lock(&workspace->ui_post_mu);
    for (int i = 0; i < workspace->ui_mailbox_count;)
    {
        PicoUiMailbox *box = &workspace->ui_mailboxes[i];
        if (strcmp(box->name, name) == 0 && (box->agent_id == agent_id || box->pub_agent_id == agent_id))
        {
            RemoveMailboxAt(workspace, i);
            continue;
        }
        i++;
    }
    pthread_mutex_unlock(&workspace->ui_post_mu);
}

void PicoWorkspace_DropAgentMailboxes(PicoWorkspace *workspace, PicoAgentId agent_id, uint64_t generation)
{
    if (!workspace || !agent_id)
    {
        return;
    }
    pthread_mutex_lock(&workspace->ui_post_mu);
    for (int i = 0; i < workspace->ui_mailbox_count;)
    {
        PicoUiMailbox *box = &workspace->ui_mailboxes[i];
        bool match = (box->agent_id == agent_id || box->pub_agent_id == agent_id);
        if (match && (generation == 0 || box->generation == generation || box->pub_generation == generation))
        {
            RemoveMailboxAt(workspace, i);
            continue;
        }
        i++;
    }
    pthread_mutex_unlock(&workspace->ui_post_mu);
}

static void FreeUiPosts(PicoWorkspace *workspace)
{
    int i;
    if (!workspace)
    {
        return;
    }
    pthread_mutex_lock(&workspace->ui_post_mu);
    for (i = 0; i < workspace->ui_mailbox_count; i++)
    {
        FreeMailbox(&workspace->ui_mailboxes[i]);
    }
    workspace->ui_mailbox_count = 0;
    pthread_mutex_unlock(&workspace->ui_post_mu);
}

static void FreeAgentFieldsOnCreateFailure(PicoAgent *agent)
{
    (void)PicoAgent_Destroy(agent);
}

static bool CopyToolPolicy(PicoHost *app, PicoAgent *agent,
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
        PicoWorkspace *ws = agent ? agent->workspace : PicoHost_SelectedWorkspace(app);
        for (int t = 0; ws && t < ws->tool_count; t++)
        {
            if (ws->tools[t].name && strcmp(ws->tools[t].name, tools[i]) == 0)
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

static PicoResult ConfigureAgent(PicoHost *app, PicoAgent *agent,
                                 const PicoAgentCreateOptions *options)
{
    if (!agent || !options)
    {
        return PICO_INVALID;
    }
    PicoWorkspace *workspace = agent->workspace;
    if (options->kind != PICO_AGENT_MAIN && options->kind != PICO_AGENT_SUBAGENT)
    {
        return PICO_INVALID;
    }
    if (options->kind == PICO_AGENT_MAIN && options->parent_id)
    {
        return PICO_INVALID;
    }
    agent->kind = options->kind;
    agent->parent_id = options->parent_id;
    if (options->parent_id)
    {
        PicoAgent *parent = PicoHost_FindAgent(app, options->parent_id);
        if (!parent)
        {
            return PICO_NOT_FOUND;
        }
        if (parent->workspace != workspace)
        {
            return PICO_INVALID;
        }
        agent->depth = parent->depth + 1;
        if (agent->depth > PICO_MAX_DELEGATION_DEPTH)
        {
            return PICO_LIMIT;
        }
    }
    else if (options->kind == PICO_AGENT_SUBAGENT)
    {
        return PICO_INVALID;
    }
    if ((options->profile && strlen(options->profile) > 64) ||
        (options->purpose && strlen(options->purpose) > 1024))
    {
        return PICO_INVALID;
    }
    snprintf(agent->profile, sizeof(agent->profile), "%s", options->profile ? options->profile : "");
    snprintf(agent->purpose, sizeof(agent->purpose), "%s", options->purpose ? options->purpose : "");
    if (options->model && options->model[0])
    {
        const PicoModel *model = PicoSettings_FindModelConst(workspace, options->model);
        if (!model)
        {
            return PICO_INVALID;
        }
        snprintf(agent->model, sizeof(agent->model), "%s", model->id);
        agent->effort[0] = '\0';
        PicoSettings_SyncAgent(agent);
    }
    if (options->effort && options->effort[0])
    {
        const PicoModel *model = PicoSettings_ActiveModelConst(agent);
        if (!PicoSettings_EffortAllowed(model, options->effort) &&
            !(model && model->effort_count == 0 && strcmp(options->effort, "none") == 0))
        {
            return PICO_INVALID;
        }
        snprintf(agent->effort, sizeof(agent->effort), "%s", options->effort);
    }
    if (!CopyToolPolicy(app, agent, options->tools, options->tool_count))
    {
        return options->tools ? PICO_INVALID : PICO_NO_MEMORY;
    }
    return PICO_OK;
}

static void PublishAgent(PicoWorkspace *workspace, PicoAgent *agent, bool select)
{
    PicoHost *host = workspace ? workspace->host : NULL;
    workspace->agents[workspace->count++] = agent;
    if (host && (!host->selected_agent_id || select))
    {
        SelectAgentAndDiscardDraft(host, agent->id);
    }
}

PicoResult PicoWorkspace_CreateAgent(PicoWorkspace *workspace, const PicoAgentCreateOptions *options,
                                     PicoAgentId *out)
{
    if (out)
    {
        *out = 0;
    }
    if (!workspace || !options)
    {
        return PICO_INVALID;
    }
    PicoHost *app = workspace->host;
    if (workspace->state != PICO_WORKSPACE_OPEN && workspace->state != PICO_WORKSPACE_RELOADING)
    {
        return PICO_BUSY;
    }
    PicoAgent_ReapRetired(workspace);
    if (workspace->count >= PICO_MAX_AGENTS || PicoHost_TotalAgentCount(app) >= PICO_MAX_TOTAL_AGENTS)
    {
        return PICO_LIMIT;
    }
    PicoAgent *agent = PicoAgent_Create(app, workspace);
    if (!agent)
    {
        return PICO_NO_MEMORY;
    }
    PicoResult result = ConfigureAgent(app, agent, options);
    if (result != PICO_OK)
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
        if (!options->session_id || PicoSession_Resolve(workspace, options->session_id, false,
                                                       path, sizeof(path)) != 0)
        {
            FreeAgentFieldsOnCreateFailure(agent);
            return PICO_SESSION_INVALID;
        }
        if (!PicoWorkspace_ReserveSession(workspace, agent->id, path))
        {
            FreeAgentFieldsOnCreateFailure(agent);
            return PICO_SESSION_IN_USE;
        }
        agent->persistence = PICO_SESSION_DURABLE;
        if (PicoSession_Replay(app, agent, path, false) != 0)
        {
            PicoWorkspace_ReleaseSessions(workspace, agent->id);
            FreeAgentFieldsOnCreateFailure(agent);
            return PICO_SESSION_INVALID;
        }
    }
    else
    {
        agent->persistence = PICO_SESSION_DURABLE;
    }

    PublishAgent(workspace, agent, options->select);
    if (options->session_start == PICO_SESSION_RESUME)
    {
        PicoSession_AppendInterrupted(app, agent);
    }
    pico_run_hooks(app, PICO_HOOK_ON_SESSION_RESET, agent->id);
    if (out)
    {
        *out = agent->id;
    }
    return PICO_OK;
}

int pico_agent_count(const PicoHost *app)
{
    return PicoHost_TotalAgentCount(app);
}

bool pico_agent_info(const PicoHost *app, int index, PicoAgentInfo *out)
{
    if (!app || !out || index < 0)
    {
        return false;
    }
    int cur = 0;
    for (int w = 0; w < app->workspace_count; w++)
    {
        const PicoWorkspace *workspace = app->workspaces[w];
        if (!workspace)
        {
            continue;
        }
        if (index < cur + workspace->count)
        {
            PicoAgent_CopyInfo(workspace->agents[index - cur], out);
            return true;
        }
        cur += workspace->count;
    }
    return false;
}

bool pico_agent_find(const PicoHost *app, PicoAgentId id, PicoAgentInfo *out)
{
    const PicoAgent *agent = PicoHost_FindAgentConst(app, id);
    if (!agent || !out)
    {
        return false;
    }
    PicoAgent_CopyInfo(agent, out);
    return true;
}

PicoAgentId pico_agent_active(const PicoHost *app)
{
    return app ? app->selected_agent_id : 0;
}

bool pico_agent_select(PicoHost *app, PicoAgentId id)
{
    PicoAgent *agent = app ? PicoHost_FindAgent(app, id) : NULL;
    if (!app || !agent)
    {
        return false;
    }
    PicoSession_SetUnseenComplete(app, agent, false);
    if (app->selected_agent_id == id)
    {
        return true;
    }
    SelectAgentAndDiscardDraft(app, id);
    PicoChatSel_Clear(app);
    memset(&app->chat_scrollbar, 0, sizeof(app->chat_scrollbar));
    app->chat_follow_bottom = true;
    app->chat_overflow = true;
    app->hovered_tool = false;
    return true;
}

PicoResult pico_agent_close(PicoHost *app, PicoAgentId id)
{
    PicoAgent *agent = PicoHost_FindAgent(app, id);
    if (!agent)
    {
        return PICO_NOT_FOUND;
    }
    PicoWorkspace *workspace = agent->workspace;
    if (!workspace)
    {
        return PICO_INVALID;
    }
    PicoAgent_ReapRetired(workspace);
    int index = FindIndex(workspace, id);
    if (index < 0)
    {
        return PICO_NOT_FOUND;
    }
    /* Cancel delegation tree for this agent */
    PicoWorkspace_CancelDelegations(workspace, id, 0);
    for (int i = 0; i < workspace->count; i++)
    {
        PicoAgent *child = workspace->agents[i];
        if (child && child->parent_id == id)
        {
            PicoAgent_Cancel(child);
        }
    }

    bool has_busy_children = false;
    for (int i = 0; i < workspace->count; i++)
    {
        PicoAgent *child = workspace->agents[i];
        if (child && child->parent_id == id)
        {
            if (PicoAgent_IsBusy(child) || PicoAgent_RetiredReferences(workspace, child->id) ||
                PicoWorkspace_JobReferences(workspace, child->id))
            {
                has_busy_children = true;
                break;
            }
        }
    }
    if (has_busy_children || PicoAgent_IsBusy(agent) || PicoAgent_RetiredReferences(workspace, id) ||
        PicoWorkspace_JobReferences(workspace, id))
    {
        return PICO_BUSY;
    }

    /* Destroy any idle child agents first */
    for (int i = workspace->count - 1; i >= 0; i--)
    {
        PicoAgent *child = workspace->agents[i];
        if (child && child->parent_id == id)
        {
            PicoAgentId cid = child->id;
            PicoSession_DrainPersist(app, child);
            for (int k = i + 1; k < workspace->count; k++)
            {
                workspace->agents[k - 1] = workspace->agents[k];
            }
            workspace->agents[--workspace->count] = NULL;
            PicoWorkspace_ReleaseSessions(workspace, cid);
            PicoWorkspace_DropAgentMailboxes(workspace, cid, 0);
            PicoWorkspace_RunHooks(workspace, PICO_HOOK_ON_AGENT_DESTROY, cid);
            PicoAgent_Destroy(child);
        }
    }

    PicoSession_DrainPersist(app, agent);
    index = FindIndex(workspace, id);
    if (index < 0)
    {
        return PICO_NOT_FOUND;
    }

    for (int i = index + 1; i < workspace->count; i++)
    {
        workspace->agents[i - 1] = workspace->agents[i];
    }
    workspace->agents[--workspace->count] = NULL;

    if (app && app->selected_agent_id == id)
    {
        PicoAgentId next = 0;
        if (workspace->count > 0)
        {
            next = workspace->agents[index < workspace->count ? index : workspace->count - 1]->id;
        }
        else
        {
            for (int w = 0; w < app->workspace_count; w++)
            {
                PicoWorkspace *ws = app->workspaces[w];
                if (ws && ws->count > 0 && ws->agents[0])
                {
                    next = ws->agents[0]->id;
                    break;
                }
            }
        }
        SyncSelectedAgent(app, next);
        PicoChatSel_Clear(app);
        app->chat_follow_bottom = true;
    }

    PicoWorkspace_ReleaseSessions(workspace, id);
    PicoWorkspace_DropAgentMailboxes(workspace, id, 0);
    PicoWorkspace_RunHooks(workspace, PICO_HOOK_ON_AGENT_DESTROY, id);
    if (!PicoAgent_Destroy(agent))
    {
        return PICO_BUSY;
    }
    return PICO_OK;
}

PicoResult pico_agent_cancel(PicoHost *app, PicoAgentId id)
{
    PicoAgent *agent = PicoHost_FindAgent(app, id);
    if (!agent)
    {
        return PICO_NOT_FOUND;
    }
    PicoAgent_Cancel(agent);
    return PICO_OK;
}

PicoResult pico_agent_force_cancel(PicoHost *app, PicoAgentId id)
{
    PicoAgent *agent = PicoHost_FindAgent(app, id);
    if (!agent)
    {
        return PICO_NOT_FOUND;
    }
    PicoAgent_ForceCancel(app, agent);
    if (agent->workspace)
    {
        PicoWorkspace_DropAgentMailboxes(agent->workspace, agent->id, 0);
    }
    return PICO_OK;
}

void PicoWorkspace_Pump(PicoWorkspace *workspace)
{
    if (!workspace)
    {
        return;
    }
    PicoWorkspace_PumpUiPosts(workspace);
    PicoAgent_ReapRetired(workspace);
    ProcessDelegationRequests(workspace);
    int budget = 256;
    for (int i = 0; i < workspace->count && budget > 0; i++)
    {
        PicoAgent_PumpBounded(workspace->host, workspace->agents[i], &budget);
    }
    LinkDelegationToolRows(workspace);
    ProcessDelegationTerminals(workspace);
    for (int i = 0; i < workspace->count; i++)
    {
        PicoSettings_ReconcileIdleAgent(workspace->agents[i]);
    }
}

bool PicoWorkspace_IsQuiescent(const PicoWorkspace *workspace)
{
    if (!workspace)
    {
        return true;
    }
    if (workspace->retired_count > 0)
    {
        return false;
    }
    pthread_mutex_lock((pthread_mutex_t *)&workspace->delegation_mu);
    bool delegating = (workspace->delegations != NULL);
    pthread_mutex_unlock((pthread_mutex_t *)&workspace->delegation_mu);
    if (delegating)
    {
        return false;
    }
    for (int i = 0; i < workspace->count; i++)
    {
        if (PicoAgent_IsBusy(workspace->agents[i]))
        {
            return false;
        }
    }
    return true;
}

bool PicoWorkspace_AcceptsNewWork(const PicoWorkspace *workspace)
{
    if (!workspace)
    {
        return true;
    }
    pthread_mutex_lock((pthread_mutex_t *)&workspace->lifecycle_mu);
    bool accepting = workspace->accepting_work && !workspace->retained_shutdown;
    pthread_mutex_unlock((pthread_mutex_t *)&workspace->lifecycle_mu);
    return accepting;
}

void PicoWorkspace_SetAcceptingWork(PicoWorkspace *workspace, bool accepting)
{
    if (!workspace)
    {
        return;
    }
    pthread_mutex_lock(&workspace->lifecycle_mu);
    workspace->accepting_work = accepting && !workspace->retained_shutdown;
    pthread_mutex_unlock(&workspace->lifecycle_mu);
}

bool PicoWorkspace_BlocksReload(const PicoWorkspace *workspace)
{
    if (!workspace)
    {
        return false;
    }
    if (workspace->retired_count > 0)
    {
        return true;
    }
    pthread_mutex_lock((pthread_mutex_t *)&workspace->delegation_mu);
    bool delegating = workspace->delegations != NULL;
    pthread_mutex_unlock((pthread_mutex_t *)&workspace->delegation_mu);
    if (delegating)
    {
        return true;
    }
    for (int i = 0; i < workspace->count; i++)
    {
        if (PicoAgent_BlocksReload(workspace->agents[i]))
        {
            return true;
        }
    }
    return false;
}

void PicoWorkspace_PrepareReload(PicoWorkspace *workspace)
{
    if (!workspace || PicoWorkspace_BlocksReload(workspace))
    {
        return;
    }
    for (int i = 0; i < workspace->count; i++)
    {
        PicoAgent_PrepareReload(workspace->agents[i]);
    }
}

void PicoWorkspace_RevalidateToolPolicies(PicoWorkspace *workspace)
{
    if (!workspace)
    {
        return;
    }
    for (int i = 0; i < workspace->count; i++)
    {
        PicoAgent *agent = workspace->agents[i];
        if (!PicoAgent_RevalidateToolPolicy(workspace->host, agent))
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
            pico_status_warn(workspace->host, line);
        }
    }
}

void PicoWorkspace_NotifySessions(PicoWorkspace *workspace)
{
    for (int i = 0; workspace && i < workspace->count; i++)
    {
        pico_run_hooks(workspace->host, PICO_HOOK_ON_SESSION_RESET, workspace->agents[i]->id);
    }
}

bool PicoWorkspace_QuiesceBefore(PicoWorkspace *workspace, const struct timespec *deadline)
{
    if (!workspace)
    {
        return true;
    }
    if (!deadline || workspace->retained_shutdown)
    {
        return false;
    }
    PicoWorkspace_SetAcceptingWork(workspace, false);
    PicoWorkspace_CancelDelegations(workspace, 0, 0);
    for (int i = 0; i < workspace->count; i++)
    {
        PicoAgent_Cancel(workspace->agents[i]);
    }
    bool clean = true;
    for (int i = 0; i < workspace->count; i++)
    {
        if (!PicoSession_DrainPersistBefore(workspace->host, workspace->agents[i], deadline))
        {
            clean = false;
        }
        pico_run_hooks(workspace->host, PICO_HOOK_ON_AGENT_DESTROY, workspace->agents[i]->id);
        if (!PicoAgent_DestroyBefore(workspace->agents[i], deadline))
        {
            clean = false;
        }
        workspace->agents[i] = NULL;
    }
    workspace->count = 0;
    if (!PicoAgent_ShutdownRetired(workspace, deadline))
    {
        clean = false;
    }
    if (!clean)
    {
        pthread_mutex_lock(&workspace->lifecycle_mu);
        workspace->retained_shutdown = true;
        workspace->accepting_work = false;
        pthread_mutex_unlock(&workspace->lifecycle_mu);
        return false;
    }
    return true;
}

bool PicoWorkspace_Quiesce(PicoWorkspace *workspace)
{
    struct timespec deadline;
    clock_gettime(CLOCK_REALTIME, &deadline);
    deadline.tv_sec += 1;
    return PicoWorkspace_QuiesceBefore(workspace, &deadline);
}

void PicoWorkspace_Free(PicoWorkspace *workspace)
{
    if (!workspace)
    {
        return;
    }
    DropDelegations(workspace);
    for (int i = 0; i < workspace->snapshot_count; i++)
    {
        PicoMessages_Free(workspace->snapshots[i].messages, workspace->snapshots[i].message_count);
        memset(&workspace->snapshots[i], 0, sizeof(workspace->snapshots[i]));
    }
    free(workspace->snapshots);
    workspace->snapshots = NULL;
    workspace->snapshot_count = 0;
    workspace->snapshot_capacity = 0;
    FreeUiPosts(workspace);
    free(workspace->models);
    workspace->models = NULL;
    workspace->model_count = 0;
    pthread_mutex_destroy(&workspace->settings_mu);
    pthread_mutex_destroy(&workspace->ui_post_mu);
    pthread_mutex_destroy(&workspace->lifecycle_mu);
    pthread_mutex_destroy(&workspace->delegation_mu);
    free(workspace);
}

bool PicoWorkspace_Destroy(PicoWorkspace *workspace)
{
    if (!PicoWorkspace_Quiesce(workspace))
    {
        return false;
    }
    PicoWorkspace_Free(workspace);
    return true;
}

bool PicoWorkspace_ReserveSession(PicoWorkspace *workspace, PicoAgentId owner,
                                  const char *path)
{
    if (!workspace || !owner || !path || !path[0] ||
        workspace->reservation_count >= (int)(sizeof(workspace->reservations) / sizeof(workspace->reservations[0])))
    {
        return false;
    }
    for (int i = 0; i < workspace->reservation_count; i++)
    {
        if (strcmp(workspace->reservations[i].path, path) == 0)
        {
            return workspace->reservations[i].owner == owner;
        }
    }
    PicoSessionReservation *reservation = &workspace->reservations[workspace->reservation_count++];
    reservation->owner = owner;
    snprintf(reservation->path, sizeof(reservation->path), "%s", path);
    return true;
}

void PicoWorkspace_ReleaseSessions(PicoWorkspace *workspace, PicoAgentId owner)
{
    if (!workspace)
    {
        return;
    }
    for (int i = 0; i < workspace->reservation_count;)
    {
        if (workspace->reservations[i].owner != owner)
        {
            i++;
            continue;
        }
        workspace->reservations[i] = workspace->reservations[--workspace->reservation_count];
    }
}

bool PicoWorkspace_SessionReserved(const PicoWorkspace *workspace, const char *path,
                                   PicoAgentId except_owner)
{
    for (int i = 0; workspace && path && i < workspace->reservation_count; i++)
    {
        if (workspace->reservations[i].owner != except_owner &&
            strcmp(workspace->reservations[i].path, path) == 0)
        {
            return true;
        }
    }
    return false;
}

/* An ask surfaces only while the session it belongs to is open: the owner must
   be the selected agent or, for hidden delegated children, a transitive
   descendant of it. */
static bool AskSurfacedForSelection(const PicoHost *app, const PicoAgent *owner)
{
    for (const PicoAgent *agent = owner; app && agent;)
    {
        if (agent->id == app->selected_agent_id)
        {
            return true;
        }
        agent = agent->parent_id ? PicoHost_FindAgentConst(app, agent->parent_id) : NULL;
    }
    return false;
}

bool pico_tool_pending_ask(const PicoHost *app, PicoToolAsk *out)
{
    if (!app || !out || app->selected_agent_id == 0)
    {
        return false;
    }
    bool found = false;
    PicoToolAsk oldest = {0};
    for (int w = 0; w < app->workspace_count; w++)
    {
        PicoWorkspace *workspace = app->workspaces[w];
        if (!workspace)
        {
            continue;
        }
        for (int i = 0; i < workspace->count; i++)
        {
            PicoToolAsk ask;
            if (PicoAgent_PendingAsk(workspace->agents[i], &ask) &&
                AskSurfacedForSelection(app, workspace->agents[i]) &&
                (!found || ask.id < oldest.id))
            {
                oldest = ask;
                found = true;
            }
        }
    }
    if (found)
    {
        *out = oldest;
    }
    return found;
}

bool pico_tool_answer(PicoHost *app, uint64_t id, const char *answer_json)
{
    if (!app || !id)
    {
        return false;
    }
    for (int w = 0; w < app->workspace_count; w++)
    {
        PicoWorkspace *workspace = app->workspaces[w];
        if (!workspace)
        {
            continue;
        }
        for (int i = 0; i < workspace->count; i++)
        {
            if (PicoAgent_AnswerAsk(workspace->agents[i], id, answer_json))
            {
                return true;
            }
        }
    }
    return false;
}

int pico_agent_message_count(const PicoHost *app, PicoAgentId id)
{
    const PicoAgent *agent = PicoHost_FindAgentConst(app, id);
    return agent ? agent->message_count : 0;
}

const PicoMessage *pico_agent_message(const PicoHost *app, PicoAgentId id, int index)
{
    const PicoAgent *agent = PicoHost_FindAgentConst(app, id);
    return agent && index >= 0 && index < agent->message_count ? &agent->messages[index] : NULL;
}

void PicoWorkspace_ReplayToolDetails(PicoWorkspace *workspace)
{
    for (int i = 0; workspace && i < workspace->count; i++)
    {
        PicoSession_ReplayToolDetails(workspace->host, workspace->agents[i]);
    }
}

void PicoWorkspace_LoadProfiles(PicoWorkspace *workspace)
{
    PicoSubagentConfig_Load(workspace);
}

int pico_subagent_profile_count(const PicoHost *app)
{
    const PicoWorkspace *workspace = PicoHost_PrimaryWorkspaceConst(app);
    return workspace ? workspace->profile_count : 0;
}

bool pico_subagent_profile_info(const PicoHost *app, int index,
                                PicoSubagentProfileInfo *out)
{
    const PicoWorkspace *workspace = PicoHost_PrimaryWorkspaceConst(app);
    if (!workspace || !out || index < 0 || index >= workspace->profile_count)
    {
        return false;
    }
    *out = workspace->profiles[index];
    return true;
}

PicoResult PicoWorkspace_Resume(PicoHost *app, PicoAgentId agent_id, const char *id,
                                     bool allow_prefix)
{
    PicoAgent *old = PicoHost_FindAgent(app, agent_id);
    PicoWorkspace *workspace;
    PicoAgentId old_id;
    bool was_selected;
    char path[4096];
    PicoAgent *replacement;
    int index;

    if (!old)
    {
        return PICO_NOT_FOUND;
    }
    if (PicoAgent_IsBusy(old))
    {
        return PICO_BUSY;
    }
    workspace = old->workspace;
    if (!workspace)
    {
        return PICO_INVALID;
    }
    if (PicoSession_Resolve(workspace, id, allow_prefix, path, sizeof(path)) != 0)
    {
        return PICO_SESSION_INVALID;
    }
    if (old->session_path[0] && strcmp(old->session_path, path) == 0)
    {
        return PICO_OK;
    }
    if (PicoWorkspace_SessionReserved(workspace, path, old->id))
    {
        return PICO_SESSION_IN_USE;
    }
    replacement = PicoAgent_Create(app, workspace);
    if (!replacement)
    {
        return PICO_NO_MEMORY;
    }
    replacement->persistence = PICO_SESSION_DURABLE;
    if (!PicoWorkspace_ReserveSession(workspace, replacement->id, path))
    {
        PicoAgent_Destroy(replacement);
        return PICO_SESSION_IN_USE;
    }
    if (PicoSession_Replay(app, replacement, path, false) != 0)
    {
        PicoWorkspace_ReleaseSessions(workspace, replacement->id);
        PicoAgent_Destroy(replacement);
        return PICO_SESSION_INVALID;
    }
    index = FindIndex(workspace, old->id);
    if (index < 0)
    {
        PicoWorkspace_ReleaseSessions(workspace, replacement->id);
        PicoAgent_Destroy(replacement);
        return PICO_NOT_FOUND;
    }
    old_id = old->id;
    was_selected = app && app->selected_agent_id == old_id;
    if (!PicoAgent_Destroy(old))
    {
        PicoWorkspace_ReleaseSessions(workspace, replacement->id);
        PicoAgent_Destroy(replacement);
        return PICO_BUSY;
    }
    workspace->agents[index] = replacement;
    PicoWorkspace_ReleaseSessions(workspace, old_id);
    if (was_selected)
    {
        SyncSelectedAgent(app, replacement->id);
        PicoChatSel_Clear(app);
        app->chat_follow_bottom = true;
    }
    pico_run_hooks(app, PICO_HOOK_ON_AGENT_DESTROY, old_id);
    PicoSession_AppendInterrupted(app, replacement);
    pico_run_hooks(app, PICO_HOOK_ON_SESSION_RESET, replacement->id);
    return PICO_OK;
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
    free(job->call_id);
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

static void RemoveDelegation(PicoWorkspace *workspace, PicoDelegationJob *job)
{
    bool removed = false;
    pthread_mutex_lock(&workspace->delegation_mu);
    PicoDelegationJob **link = &workspace->delegations;
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
    pthread_mutex_unlock(&workspace->delegation_mu);
    if (removed)
    {
        DelegationRelease(job);
    }
}

static const char *AgentResultText(PicoResult result)
{
    switch (result)
    {
    case PICO_LIMIT: return "agent or delegation depth limit reached";
    case PICO_SESSION_IN_USE: return "session is already open";
    case PICO_SESSION_INVALID: return "session is invalid";
    case PICO_NO_MEMORY: return "out of memory";
    case PICO_NOT_FOUND: return "parent agent no longer exists";
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

static bool IsSubagentLine(const PicoTraceLine *line)
{
    return line && line->is_tool && line->tool_name && strcmp(line->tool_name, "subagent") == 0;
}

static PicoTraceLine *FindPendingSubagentLine(PicoAgent *parent)
{
    if (!parent)
    {
        return NULL;
    }
    for (int i = parent->message_count - 1; i >= 0; i--)
    {
        PicoMessage *msg = &parent->messages[i];
        for (int t = msg->trace_count - 1; t >= 0; t--)
        {
            PicoTraceLine *line = &msg->trace[t];
            if (IsSubagentLine(line) && !line->tool_output && line->child_id == 0)
            {
                return line;
            }
        }
    }
    return NULL;
}

static PicoTraceLine *FindSubagentLine(PicoAgent *parent, PicoAgentId child_id,
                                       const char *call_id)
{
    if (!parent)
    {
        return NULL;
    }
    if (child_id)
    {
        for (int i = parent->message_count - 1; i >= 0; i--)
        {
            PicoMessage *msg = &parent->messages[i];
            for (int t = msg->trace_count - 1; t >= 0; t--)
            {
                PicoTraceLine *line = &msg->trace[t];
                if (IsSubagentLine(line) && line->child_id == child_id)
                {
                    return line;
                }
            }
        }
    }
    if (call_id && call_id[0])
    {
        for (int i = parent->message_count - 1; i >= 0; i--)
        {
            PicoMessage *msg = &parent->messages[i];
            for (int t = msg->trace_count - 1; t >= 0; t--)
            {
                PicoTraceLine *line = &msg->trace[t];
                if (IsSubagentLine(line) && !line->child_id && !line->tool_output &&
                    line->tool_call_id && strcmp(line->tool_call_id, call_id) == 0)
                {
                    return line;
                }
            }
        }
    }
    return NULL;
}

static void StampSubagentLine(PicoTraceLine *line, PicoAgentId child_id, const char *session_id)
{
    if (!line)
    {
        return;
    }
    if (child_id)
    {
        line->child_id = child_id;
    }
    if (session_id && session_id[0])
    {
        snprintf(line->child_session_id, sizeof(line->child_session_id), "%s", session_id);
    }
}

static void CopySessionIdFromOutput(const char *output, char *out, size_t cap)
{
    if (!output || !out || cap == 0)
    {
        return;
    }
    JsonDoc doc;
    if (JsonParse(&doc, output, strlen(output)) != 0 || !JsonIsObject(&doc, 0))
    {
        return;
    }
    char *id = JsonObjStr(&doc, 0, "session_id");
    if (id && id[0])
    {
        snprintf(out, cap, "%s", id);
    }
    free(id);
    JsonFree(&doc);
}

static PicoSubagentSnapshot *FindSnapshot(PicoWorkspace *workspace, PicoAgentId child_id,
                                          const char *session_id)
{
    if (!workspace)
    {
        return NULL;
    }
    if (child_id)
    {
        for (int i = 0; i < workspace->snapshot_count; i++)
        {
            if (workspace->snapshots[i].child_id == child_id)
            {
                return &workspace->snapshots[i];
            }
        }
    }
    if (session_id && session_id[0])
    {
        for (int i = 0; i < workspace->snapshot_count; i++)
        {
            if (workspace->snapshots[i].session_id[0] &&
                strcmp(workspace->snapshots[i].session_id, session_id) == 0)
            {
                return &workspace->snapshots[i];
            }
        }
    }
    return NULL;
}

static PicoSubagentSnapshot *AllocSnapshot(PicoWorkspace *workspace)
{
    if (!workspace)
    {
        return NULL;
    }
    if (workspace->snapshot_count >= workspace->snapshot_capacity)
    {
        int next_capacity = workspace->snapshot_capacity == 0 ? 16 : workspace->snapshot_capacity * 2;
        PicoSubagentSnapshot *next = (PicoSubagentSnapshot *)realloc(
            workspace->snapshots, (size_t)next_capacity * sizeof(PicoSubagentSnapshot));
        if (!next)
        {
            return NULL;
        }
        memset(&next[workspace->snapshot_capacity], 0,
               (size_t)(next_capacity - workspace->snapshot_capacity) * sizeof(PicoSubagentSnapshot));
        workspace->snapshots = next;
        workspace->snapshot_capacity = next_capacity;
    }
    PicoSubagentSnapshot *slot = &workspace->snapshots[workspace->snapshot_count++];
    memset(slot, 0, sizeof(*slot));
    return slot;
}

static void SnapshotChild(PicoWorkspace *workspace, const PicoAgent *child)
{
    if (!workspace || !child)
    {
        return;
    }
    PicoSubagentSnapshot *slot = FindSnapshot(workspace, child->id, child->session_id);
    if (!slot)
    {
        slot = AllocSnapshot(workspace);
    }
    if (!slot)
    {
        return;
    }
    PicoMessages_Free(slot->messages, slot->message_count);
    slot->messages = NULL;
    slot->message_count = 0;
    slot->child_id = child->id;
    slot->parent_id = child->parent_id;
    snprintf(slot->session_id, sizeof(slot->session_id), "%s", child->session_id);
    snprintf(slot->profile, sizeof(slot->profile), "%s", child->profile);
    snprintf(slot->purpose, sizeof(slot->purpose), "%s", child->purpose);
    snprintf(slot->model, sizeof(slot->model), "%s", child->model);
    snprintf(slot->effort, sizeof(slot->effort), "%s", child->effort);
    (void)PicoMessages_Copy(child->messages, child->message_count, &slot->messages,
                            &slot->message_count);
}

static void FillInspectFromAgent(PicoSubagentInspect *out, const PicoAgent *child)
{
    out->live_id = child->id;
    out->live = true;
    out->state = child->state;
    snprintf(out->session_id, sizeof(out->session_id), "%s", child->session_id);
    snprintf(out->profile, sizeof(out->profile), "%s", child->profile);
    snprintf(out->purpose, sizeof(out->purpose), "%s", child->purpose);
    snprintf(out->model, sizeof(out->model), "%s", child->model);
    snprintf(out->effort, sizeof(out->effort), "%s", child->effort);
    snprintf(out->activity, sizeof(out->activity), "%s", child->activity);
    out->messages = child->messages;
    out->message_count = child->message_count;
}

static void FillInspectFromSnapshot(PicoSubagentInspect *out, const PicoSubagentSnapshot *slot)
{
    out->live_id = 0;
    out->live = false;
    out->state = PICO_AGENT_IDLE;
    snprintf(out->session_id, sizeof(out->session_id), "%s", slot->session_id);
    snprintf(out->profile, sizeof(out->profile), "%s", slot->profile);
    snprintf(out->purpose, sizeof(out->purpose), "%s", slot->purpose);
    snprintf(out->model, sizeof(out->model), "%s", slot->model);
    snprintf(out->effort, sizeof(out->effort), "%s", slot->effort);
    out->activity[0] = '\0';
    out->messages = slot->messages;
    out->message_count = slot->message_count;
}

static bool LoadSnapshotFromSession(PicoWorkspace *workspace, const char *session_id)
{
    if (!workspace || !workspace->host || !session_id || !session_id[0] ||
        FindSnapshot(workspace, 0, session_id))
    {
        return FindSnapshot(workspace, 0, session_id) != NULL;
    }
    PicoMessage *messages = NULL;
    int count = 0;
    if (PicoSession_LoadTranscript(workspace, session_id, &messages, &count) != 0)
    {
        return false;
    }
    PicoMessages_PrepareDocs(messages, count);
    PicoSubagentSnapshot *slot = AllocSnapshot(workspace);
    if (!slot)
    {
        PicoMessages_Free(messages, count);
        return false;
    }
    slot->messages = messages;
    slot->message_count = count;
    snprintf(slot->session_id, sizeof(slot->session_id), "%s", session_id);
    PicoSessionHeader header;
    char path[4096];
    if (PicoSession_Resolve(workspace, session_id, false, path, sizeof(path)) == 0 &&
        PicoSession_ReadHeader(path, &header) == 0)
    {
        snprintf(slot->profile, sizeof(slot->profile), "%s", header.profile);
        snprintf(slot->purpose, sizeof(slot->purpose), "%s", header.initial_purpose);
        snprintf(slot->model, sizeof(slot->model), "%s", header.model);
    }
    return true;
}

static void LinkDelegationToolRows(PicoWorkspace *workspace)
{
    PicoDelegationJob *jobs[PICO_MAX_AGENTS * 2];
    int count = 0;
    pthread_mutex_lock(&workspace->delegation_mu);
    for (PicoDelegationJob *it = workspace->delegations;
         it && count < (int)(sizeof(jobs) / sizeof(jobs[0])); it = it->next)
    {
        jobs[count++] = it;
    }
    pthread_mutex_unlock(&workspace->delegation_mu);
    for (int i = 0; i < count; i++)
    {
        PicoDelegationJob *job = jobs[i];
        pthread_mutex_lock(&job->mu);
        PicoAgentId parent_id = job->parent_id;
        PicoAgentId child_id = job->child_id;
        char session_id[40];
        snprintf(session_id, sizeof(session_id), "%s", job->session_id);
        pthread_mutex_unlock(&job->mu);
        if (!child_id)
        {
            continue;
        }
        PicoAgent *parent = PicoWorkspace_FindAgent(workspace, parent_id);
        PicoAgent *child = PicoWorkspace_FindAgent(workspace, child_id);
        const char *sid = session_id[0] ? session_id :
                          (child && child->session_id[0] ? child->session_id : NULL);
        PicoTraceLine *line = FindSubagentLine(parent, child_id, job->call_id);
        if (!line)
        {
            line = FindPendingSubagentLine(parent);
        }
        StampSubagentLine(line, child_id, sid);
    }
}

bool PicoWorkspace_InspectSubagent(PicoHost *host, const PicoTraceLine *line,
                                   PicoSubagentInspect *out)
{
    if (out)
    {
        memset(out, 0, sizeof(*out));
    }
    if (!host || !line || !out)
    {
        return false;
    }
    PicoWorkspace *workspace = PicoHost_SelectedWorkspace(host);
    if (!workspace)
    {
        return false;
    }
    char session_id[40];
    snprintf(session_id, sizeof(session_id), "%s", line->child_session_id);
    if (!session_id[0])
    {
        CopySessionIdFromOutput(line->tool_output, session_id, sizeof(session_id));
    }
    if (line->child_id)
    {
        PicoAgent *child = PicoWorkspace_FindAgent(workspace, line->child_id);
        if (!child)
        {
            child = PicoHost_FindAgent(host, line->child_id);
        }
        if (child)
        {
            FillInspectFromAgent(out, child);
            return true;
        }
    }
    PicoSubagentSnapshot *slot = FindSnapshot(workspace, line->child_id, session_id);
    if (!slot && session_id[0] && LoadSnapshotFromSession(workspace, session_id))
    {
        slot = FindSnapshot(workspace, 0, session_id);
    }
    if (!slot)
    {
        return false;
    }
    FillInspectFromSnapshot(out, slot);
    return true;
}

static char *UnknownProfileError(const PicoWorkspace *workspace)
{
    JsonBuf b;
    JsonBuf_Init(&b);
    JsonBuf_Puts(&b, "unknown subagent profile; available: ");
    if (!workspace || workspace->profile_count <= 0)
    {
        JsonBuf_Puts(&b, "(none)");
        return JsonBuf_Steal(&b);
    }
    for (int i = 0; i < workspace->profile_count; i++)
    {
        if (i > 0)
        {
            JsonBuf_Puts(&b, ", ");
        }
        JsonBuf_Puts(&b, workspace->profiles[i].name);
    }
    return JsonBuf_Steal(&b);
}

static bool StartDelegation(PicoWorkspace *workspace, PicoDelegationJob *job)
{
    PicoHost *app = workspace ? workspace->host : NULL;
    PicoAgent *parent = PicoWorkspace_FindAgent(workspace, job->parent_id);
    if (!parent || parent->runtime_generation != job->parent_generation)
    {
        PublishDelegation(job, PICO_DELEGATION_ABANDONED, "cancelled", NULL,
                          "parent runtime is no longer live", true);
        return false;
    }
    const PicoSubagentProfileInfo *profile = PicoSubagentConfig_Find(workspace, job->profile);
    if (!profile)
    {
        char *message = UnknownProfileError(workspace);
        PublishDelegation(job, PICO_DELEGATION_ERROR, "error", NULL,
                          message ? message : "unknown subagent profile", true);
        free(message);
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
        if (PicoSession_Resolve(workspace, job->session_id, false, path, sizeof(path)) != 0 ||
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
    PicoResult created = PicoWorkspace_CreateAgent(workspace, &options, &child_id);
    if (created != PICO_OK)
    {
        PublishDelegation(job, PICO_DELEGATION_ERROR, "error", NULL,
                          AgentResultText(created), true);
        return false;
    }
    PicoAgent *child = PicoWorkspace_FindAgent(workspace, child_id);
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
    PicoSettings_SyncAgent(child);
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
    PicoSession_LogUser(app, child, job->task, job->task, NULL);
    PicoAgent_StartTurn(app, child, job->task);
    return true;
}

static void ProcessDelegationRequests(PicoWorkspace *workspace)
{
    PicoDelegationJob *job = NULL;
    pthread_mutex_lock(&workspace->delegation_mu);
    for (PicoDelegationJob *it = workspace->delegations; it; it = it->next)
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
    pthread_mutex_unlock(&workspace->delegation_mu);
    if (!job)
    {
        return;
    }
    if (!PicoWorkspace_AcceptsNewWork(workspace))
    {
        PublishDelegation(job, PICO_DELEGATION_ERROR, "error", NULL,
                          "reload or workspace transition is pending", true);
        RemoveDelegation(workspace, job);
    }
    else if (!StartDelegation(workspace, job))
    {
        RemoveDelegation(workspace, job);
    }
}

static void CloseDelegationChild(PicoWorkspace *workspace, PicoDelegationJob *job,
                                 PicoAgentId child_id)
{
    PicoAgent *child = child_id ? PicoWorkspace_FindAgent(workspace, child_id) : NULL;
    PicoAgent *parent = PicoWorkspace_FindAgent(workspace, job->parent_id);
    if (child)
    {
        PicoTraceLine *line = FindSubagentLine(parent, child_id, job->call_id);
        StampSubagentLine(line, child_id, child->session_id[0] ? child->session_id : NULL);
        SnapshotChild(workspace, child);
    }
    pthread_mutex_lock(&job->mu);
    job->child_id = 0;
    pthread_mutex_unlock(&job->mu);
    PicoResult closed = pico_agent_close(workspace->host, child_id);
    if (closed == PICO_OK || closed == PICO_NOT_FOUND)
    {
        RemoveDelegation(workspace, job);
        return;
    }
    pthread_mutex_lock(&job->mu);
    job->child_id = child_id;
    pthread_mutex_unlock(&job->mu);
}

static void ProcessDelegationTerminals(PicoWorkspace *workspace)
{
    PicoDelegationJob *jobs[PICO_MAX_AGENTS * 2];
    int count = 0;
    pthread_mutex_lock(&workspace->delegation_mu);
    for (PicoDelegationJob *it = workspace->delegations;
         it && count < (int)(sizeof(jobs) / sizeof(jobs[0])); it = it->next)
    {
        DelegationRetain(it);
        jobs[count++] = it;
    }
    pthread_mutex_unlock(&workspace->delegation_mu);

    for (int i = 0; i < count; i++)
    {
        PicoDelegationJob *job = jobs[i];
        pthread_mutex_lock(&job->mu);
        PicoDelegationState state = job->state;
        PicoAgentId child_id = job->child_id;
        int child_message_start = job->child_message_start;
        pthread_mutex_unlock(&job->mu);
        PicoAgent *child = child_id ? PicoWorkspace_FindAgent(workspace, child_id) : NULL;

        if ((state == PICO_DELEGATION_CANCELLED || state == PICO_DELEGATION_ABANDONED) && child)
        {
            if (PicoAgent_IsBusy(child))
            {
                PicoAgent_Cancel(child);
            }
            else
            {
                CloseDelegationChild(workspace, job, child_id);
            }
        }
        else if (state == PICO_DELEGATION_RUNNING)
        {
            if (!child)
            {
                PublishDelegation(job, PICO_DELEGATION_ERROR, "error", NULL,
                                  "subagent disappeared before completion", true);
                RemoveDelegation(workspace, job);
            }
            else if (child->state == PICO_AGENT_ERROR)
            {
                PicoSession_DrainPersist(workspace->host, child);
                PublishDelegation(job, PICO_DELEGATION_ERROR, "error", child,
                                  child->error ? child->error : "subagent failed", true);
                CloseDelegationChild(workspace, job, child_id);
            }
            else if (!PicoAgent_IsBusy(child) && child->state == PICO_AGENT_IDLE)
            {
                PicoSession_DrainPersist(workspace->host, child);
                PublishDelegation(job, PICO_DELEGATION_DONE, "completed", child,
                                  LastAssistantSince(child, child_message_start), false);
                CloseDelegationChild(workspace, job, child_id);
            }
        }
        else if (DelegationTerminal(state))
        {
            if (child)
            {
                if (!PicoAgent_IsBusy(child))
                {
                    CloseDelegationChild(workspace, job, child_id);
                }
            }
            else
            {
                RemoveDelegation(workspace, job);
            }
        }
        DelegationRelease(job);
    }
}

char *PicoWorkspace_Delegate(PicoAgentContext *ctx, const char *profile,
                             const char *task, const char *session_id,
                             bool *is_error)
{
    if (is_error)
    {
        *is_error = true;
    }
    PicoWorkspace *workspace = PicoAgentContext_Workspace(ctx);
    if (!workspace || !PicoWorkspace_AcceptsNewWork(workspace))
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
    const char *call_id = PicoAgentContext_ToolCallId(ctx);
    job->call_id = call_id && call_id[0] ? JsonDup(call_id) : NULL;
    if (!job->task || (call_id && call_id[0] && !job->call_id))
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
    pthread_mutex_lock(&workspace->lifecycle_mu);
    if (!workspace->accepting_work || workspace->retained_shutdown)
    {
        pthread_mutex_unlock(&workspace->lifecycle_mu);
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
    pthread_mutex_lock(&workspace->delegation_mu);
    PicoDelegationJob **tail = &workspace->delegations;
    while (*tail) tail = &(*tail)->next;
    *tail = job;
    pthread_mutex_unlock(&workspace->delegation_mu);
    pthread_mutex_unlock(&workspace->lifecycle_mu);
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

void PicoWorkspace_CancelChildDelegation(PicoWorkspace *workspace, PicoAgentId child_id)
{
    if (!workspace || !child_id)
    {
        return;
    }
    PicoAgent *child = PicoWorkspace_FindAgent(workspace, child_id);
    pthread_mutex_lock(&workspace->delegation_mu);
    for (PicoDelegationJob *job = workspace->delegations; job; job = job->next)
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
    pthread_mutex_unlock(&workspace->delegation_mu);
}

void PicoWorkspace_CancelDelegations(PicoWorkspace *workspace, PicoAgentId parent_id,
                                     uint64_t runtime_generation)
{
    if (!workspace)
    {
        return;
    }
    PicoAgentId children[PICO_MAX_AGENTS];
    int child_count = 0;
    pthread_mutex_lock(&workspace->delegation_mu);
    for (PicoDelegationJob *job = workspace->delegations; job; job = job->next)
    {
        pthread_mutex_lock(&job->mu);
        bool match = parent_id == 0 ||
                     (job->parent_id == parent_id &&
                      (runtime_generation == 0 || job->parent_generation == runtime_generation));
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
    pthread_mutex_unlock(&workspace->delegation_mu);
    for (int i = 0; i < child_count; i++)
    {
        PicoAgent *child = PicoWorkspace_FindAgent(workspace, children[i]);
        if (child)
        {
            PicoAgent_Cancel(child);
        }
    }
}

bool PicoWorkspace_JobReferences(const PicoWorkspace *workspace, PicoAgentId id)
{
    if (!workspace || !id)
    {
        return false;
    }
    bool found = false;
    pthread_mutex_lock((pthread_mutex_t *)&workspace->delegation_mu);
    for (PicoDelegationJob *job = workspace->delegations; job && !found; job = job->next)
    {
        pthread_mutex_lock(&job->mu);
        found = job->parent_id == id || job->child_id == id;
        pthread_mutex_unlock(&job->mu);
    }
    pthread_mutex_unlock((pthread_mutex_t *)&workspace->delegation_mu);
    return found;
}

static void DropDelegations(PicoWorkspace *workspace)
{
    pthread_mutex_lock(&workspace->delegation_mu);
    PicoDelegationJob *job = workspace->delegations;
    workspace->delegations = NULL;
    pthread_mutex_unlock(&workspace->delegation_mu);
    while (job)
    {
        PicoDelegationJob *next = job->next;
        job->next = NULL;
        DelegationRelease(job);
        job = next;
    }
}
