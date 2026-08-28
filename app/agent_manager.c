#define _POSIX_C_SOURCE 200809L

#include "agent_manager.h"
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

static void ProcessDelegationRequests(PicoAgentManager *manager);
static void ProcessDelegationTerminals(PicoAgentManager *manager);
static void DropDelegations(PicoAgentManager *manager);
static void LinkDelegationToolRows(PicoAgentManager *manager);

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

static PicoUiMailbox *FindMailbox(PicoAgentManager *manager, const char *name)
{
    int i;
    if (!manager || !name || !name[0])
    {
        return NULL;
    }
    for (i = 0; i < manager->ui_mailbox_count; i++)
    {
        if (strcmp(manager->ui_mailboxes[i].name, name) == 0)
        {
            return &manager->ui_mailboxes[i];
        }
    }
    return NULL;
}

static const PicoUiMailbox *FindMailboxConst(const PicoAgentManager *manager, const char *name)
{
    return FindMailbox((PicoAgentManager *)manager, name);
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

static void RemoveMailboxAt(PicoAgentManager *manager, int index)
{
    if (!manager || index < 0 || index >= manager->ui_mailbox_count)
    {
        return;
    }
    FreeMailbox(&manager->ui_mailboxes[index]);
    manager->ui_mailbox_count--;
    if (index < manager->ui_mailbox_count)
    {
        manager->ui_mailboxes[index] = manager->ui_mailboxes[manager->ui_mailbox_count];
        memset(&manager->ui_mailboxes[manager->ui_mailbox_count], 0, sizeof(manager->ui_mailboxes[0]));
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

void PicoAgentManager_UiPost(PicoAgentManager *manager, const char *name, PicoUiPostKind kind,
                             PicoAgentId agent_id, uint64_t generation, const char *text, size_t n)
{
    PicoUiMailbox *box;
    if (!manager || !name || !name[0] || !generation ||
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
    pthread_mutex_lock(&manager->ui_post_mu);
    {
        const PicoAgent *agent = PicoAgentManager_FindConst(manager, agent_id);
        if (!agent || agent->runtime_generation != generation)
        {
            pthread_mutex_unlock(&manager->ui_post_mu);
            return;
        }
    }
    box = FindMailbox(manager, name);
    if (!box)
    {
        if (manager->ui_mailbox_count >= PICO_MAX_UI_POSTS)
        {
            pthread_mutex_unlock(&manager->ui_post_mu);
            return;
        }
        box = &manager->ui_mailboxes[manager->ui_mailbox_count];
        memset(box, 0, sizeof(*box));
        snprintf(box->name, sizeof(box->name), "%s", name);
        manager->ui_mailbox_count++;
    }
    if (box->generation != 0 && box->agent_id == agent_id && generation < box->generation)
    {
        pthread_mutex_unlock(&manager->ui_post_mu);
        return;
    }
    if (box->generation != 0 &&
        (box->agent_id != agent_id || generation != box->generation))
    {
        free(box->text);
        box->text = NULL;
        box->text_len = 0;
        box->status[0] = '\0';
    }
    box->agent_id = agent_id;
    box->generation = generation;
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
    pthread_mutex_unlock(&manager->ui_post_mu);
}

void PicoAgentManager_PumpUiPosts(PicoAgentManager *manager)
{
    int i;
    if (!manager)
    {
        return;
    }
    pthread_mutex_lock(&manager->ui_post_mu);
    for (i = 0; i < manager->ui_mailbox_count;)
    {
        PicoUiMailbox *box = &manager->ui_mailboxes[i];
        const PicoAgent *agent;
        bool live;
        if (!box->dirty)
        {
            i++;
            continue;
        }
        agent = PicoAgentManager_FindConst(manager, box->agent_id);
        live = agent && agent->runtime_generation == box->generation;
        if (!live)
        {
            box->dirty = false;
            if (!box->published)
            {
                RemoveMailboxAt(manager, i);
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
    pthread_mutex_unlock(&manager->ui_post_mu);
}

bool PicoAgentManager_UiLatest(const PicoAgentManager *manager, const char *name, PicoUiPost *out)
{
    const PicoUiMailbox *box;
    bool found = false;
    if (!out)
    {
        return false;
    }
    memset(out, 0, sizeof(*out));
    if (!manager)
    {
        return false;
    }
    pthread_mutex_lock((pthread_mutex_t *)&manager->ui_post_mu);
    box = FindMailboxConst(manager, name);
    if (box && box->published)
    {
        out->agent_id = box->pub_agent_id;
        out->generation = box->pub_generation;
        out->status = box->pub_status;
        out->text = box->pub_text ? box->pub_text : "";
        found = true;
    }
    pthread_mutex_unlock((pthread_mutex_t *)&manager->ui_post_mu);
    return found;
}

void PicoAgentManager_UiClear(PicoAgentManager *manager, const char *name)
{
    int i;
    if (!manager || !name || !name[0])
    {
        return;
    }
    pthread_mutex_lock(&manager->ui_post_mu);
    for (i = 0; i < manager->ui_mailbox_count; i++)
    {
        if (strcmp(manager->ui_mailboxes[i].name, name) == 0)
        {
            RemoveMailboxAt(manager, i);
            break;
        }
    }
    pthread_mutex_unlock(&manager->ui_post_mu);
}

static void FreeUiPosts(PicoAgentManager *manager)
{
    int i;
    if (!manager)
    {
        return;
    }
    pthread_mutex_lock(&manager->ui_post_mu);
    for (i = 0; i < manager->ui_mailbox_count; i++)
    {
        FreeMailbox(&manager->ui_mailboxes[i]);
    }
    manager->ui_mailbox_count = 0;
    pthread_mutex_unlock(&manager->ui_post_mu);
}

static PicoHost *MgrHost(const PicoAgentManager *manager)
{
    return manager && manager->workspace ? manager->workspace->host : NULL;
}

PicoAgentManager *PicoAgentManager_Create(PicoWorkspace *workspace)
{
    PicoAgentManager *manager = (PicoAgentManager *)calloc(1, sizeof(*manager));
    if (!manager)
    {
        return NULL;
    }
    manager->workspace = workspace;
    pthread_mutex_init(&manager->delegation_mu, NULL);
    pthread_mutex_init(&manager->lifecycle_mu, NULL);
    pthread_mutex_init(&manager->ui_post_mu, NULL);
    manager->accepting_work = true;
    return manager;
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

static PicoAgentResult ConfigureAgent(PicoHost *app, PicoAgent *agent,
                                      const PicoAgentCreateOptions *options)
{
    if (!agent || !options)
    {
        return PICO_AGENT_RESULT_INVALID;
    }
    if (options->kind != PICO_AGENT_MAIN && options->kind != PICO_AGENT_SUBAGENT)
    {
        return PICO_AGENT_RESULT_INVALID;
    }
    agent->kind = options->kind;
    agent->parent_id = options->parent_id;
    if (options->parent_id)
    {
        PicoAgent *parent = PicoAgentManager_Find(app->agents, options->parent_id);
        if (!parent)
        {
            return PICO_AGENT_RESULT_NOT_FOUND;
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

static void PublishAgent(PicoAgentManager *manager, PicoAgent *agent, bool select)
{
    manager->agents[manager->count++] = agent;
    if (!manager->active_id || select)
    {
        manager->active_id = agent->id;
    }
}

bool PicoAgentManager_AdoptInitial(PicoAgentManager *manager, PicoAgent *agent)
{
    if (!manager || !agent || manager->count != 0 || PicoAgent_IsBusy(agent))
    {
        return false;
    }
    PicoAgent_RebindHost(MgrHost(manager), agent, manager);
    if (agent->manager != manager)
    {
        return false;
    }
    agent->kind = PICO_AGENT_MAIN;
    agent->parent_id = 0;
    agent->depth = 0;
    agent->profile[0] = '\0';
    agent->purpose[0] = '\0';
    agent->parent_session_id[0] = '\0';
    agent->persistence = PICO_SESSION_DURABLE;
    agent->tool_policy_valid = true;
    PublishAgent(manager, agent, true);
    return true;
}

PicoAgentResult pico_agent_create(PicoHost *app, const PicoAgentCreateOptions *options,
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
        if (!options->session_id || PicoSession_Resolve(app, options->session_id, false,
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

int pico_agent_count(const PicoHost *app)
{
    return app && app->agents ? app->agents->count : 0;
}

bool pico_agent_info(const PicoHost *app, int index, PicoAgentInfo *out)
{
    if (!app || !app->agents || !out || index < 0 || index >= app->agents->count)
    {
        return false;
    }
    PicoAgent_CopyInfo(app->agents->agents[index], out);
    return true;
}

bool pico_agent_find(const PicoHost *app, PicoAgentId id, PicoAgentInfo *out)
{
    const PicoAgent *agent = app && app->agents ? PicoAgentManager_FindConst(app->agents, id) : NULL;
    if (!agent || !out)
    {
        return false;
    }
    PicoAgent_CopyInfo(agent, out);
    return true;
}

PicoAgentId pico_agent_active(const PicoHost *app)
{
    return app && app->agents ? app->agents->active_id : 0;
}

bool pico_agent_select(PicoHost *app, PicoAgentId id)
{
    if (!app || !app->agents || !PicoAgentManager_Find(app->agents, id))
    {
        return false;
    }
    if (app->agents->active_id == id)
    {
        return true;
    }
    app->agents->active_id = id;
    PicoChatSel_Clear(app);
    memset(&app->chat_scrollbar, 0, sizeof(app->chat_scrollbar));
    app->chat_follow_bottom = true;
    app->chat_overflow = true;
    app->hovered_tool = false;
    return true;
}

PicoAgentResult pico_agent_close(PicoHost *app, PicoAgentId id)
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
    if (manager->count == 1 || PicoAgent_IsBusy(agent) || PicoAgent_RetiredReferences(manager, id) ||
        PicoAgentManager_JobReferences(manager, id))
    {
        return PICO_AGENT_RESULT_BUSY;
    }
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
        manager->active_id = manager->agents[index < manager->count ? index : manager->count - 1]->id;
        PicoChatSel_Clear(app);
        app->chat_follow_bottom = true;
    }
    PicoAgentManager_ReleaseSessions(manager, id);
    pico_run_hooks(app, PICO_HOOK_ON_AGENT_DESTROY, id);
    return PICO_AGENT_RESULT_OK;
}

PicoAgentResult pico_agent_cancel(PicoHost *app, PicoAgentId id)
{
    PicoAgent *agent = app && app->agents ? PicoAgentManager_Find(app->agents, id) : NULL;
    if (!agent)
    {
        return PICO_AGENT_RESULT_NOT_FOUND;
    }
    PicoAgent_Cancel(agent);
    return PICO_AGENT_RESULT_OK;
}

PicoAgentResult pico_agent_force_cancel(PicoHost *app, PicoAgentId id)
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
    PicoAgentManager_PumpUiPosts(manager);
    PicoAgent_ReapRetired(manager);
    ProcessDelegationRequests(manager);
    for (int i = 0; i < manager->count; i++)
    {
        PicoAgent_Pump(MgrHost(manager), manager->agents[i]);
    }
    LinkDelegationToolRows(manager);
    ProcessDelegationTerminals(manager);
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
        if (!PicoAgent_RevalidateToolPolicy(MgrHost(manager), agent))
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
            pico_status_warn(MgrHost(manager), line);
        }
    }
}

void PicoAgentManager_NotifySessions(PicoAgentManager *manager)
{
    for (int i = 0; manager && i < manager->count; i++)
    {
        pico_run_hooks(MgrHost(manager), PICO_HOOK_ON_SESSION_RESET, manager->agents[i]->id);
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
        pico_run_hooks(MgrHost(manager), PICO_HOOK_ON_AGENT_DESTROY, manager->agents[i]->id);
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
        return false;
    }
    DropDelegations(manager);
    for (int i = 0; i < manager->snapshot_count; i++)
    {
        PicoMessages_Free(manager->snapshots[i].messages, manager->snapshots[i].message_count);
        memset(&manager->snapshots[i], 0, sizeof(manager->snapshots[i]));
    }
    free(manager->snapshots);
    manager->snapshots = NULL;
    manager->snapshot_count = 0;
    manager->snapshot_capacity = 0;
    FreeUiPosts(manager);
    pthread_mutex_destroy(&manager->ui_post_mu);
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

bool pico_tool_pending_ask(const PicoHost *app, PicoToolAsk *out)
{
    if (!app || !app->agents || !out)
    {
        return false;
    }
    bool found = false;
    PicoToolAsk oldest = {0};
    for (int i = 0; i < app->agents->count; i++)
    {
        PicoToolAsk ask;
        if (PicoAgent_PendingAsk(app->agents->agents[i], &ask) &&
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

bool pico_tool_answer(PicoHost *app, uint64_t id, const char *answer_json)
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

int pico_agent_message_count(const PicoHost *app, PicoAgentId id)
{
    const PicoAgent *agent = app && app->agents ? PicoAgentManager_FindConst(app->agents, id) : NULL;
    return agent ? agent->message_count : 0;
}

const PicoMessage *pico_agent_message(const PicoHost *app, PicoAgentId id, int index)
{
    const PicoAgent *agent = app && app->agents ? PicoAgentManager_FindConst(app->agents, id) : NULL;
    return agent && index >= 0 && index < agent->message_count ? &agent->messages[index] : NULL;
}

void PicoAgentManager_ReplayToolDetails(PicoAgentManager *manager)
{
    for (int i = 0; manager && i < manager->count; i++)
    {
        PicoSession_ReplayToolDetails(MgrHost(manager), manager->agents[i]);
    }
}

void PicoAgentManager_LoadProfiles(PicoAgentManager *manager)
{
    PicoSubagentConfig_Load(manager);
}

int pico_subagent_profile_count(const PicoHost *app)
{
    return app && app->agents ? app->agents->profile_count : 0;
}

bool pico_subagent_profile_info(const PicoHost *app, int index,
                                PicoSubagentProfileInfo *out)
{
    if (!app || !app->agents || !out || index < 0 || index >= app->agents->profile_count)
    {
        return false;
    }
    *out = app->agents->profiles[index];
    return true;
}

PicoAgentResult PicoAgentManager_ResumeActive(PicoHost *app, const char *id, bool allow_prefix)
{
    PicoAgent *old = PicoHost_ActiveAgent(app);
    if (!old || PicoAgent_IsBusy(old))
    {
        return PICO_AGENT_RESULT_BUSY;
    }
    char path[4096];
    if (PicoSession_Resolve(app, id, allow_prefix, path, sizeof(path)) != 0)
    {
        return PICO_AGENT_RESULT_SESSION_INVALID;
    }
    if (old->session_path[0] && strcmp(old->session_path, path) == 0)
    {
        return PICO_AGENT_RESULT_OK;
    }
    if (PicoAgentManager_SessionReserved(app->agents, path, old->id))
    {
        return PICO_AGENT_RESULT_SESSION_IN_USE;
    }
    PicoAgent *replacement = PicoAgent_Create(app);
    if (!replacement)
    {
        return PICO_AGENT_RESULT_NO_MEMORY;
    }
    replacement->persistence = PICO_SESSION_DURABLE;
    if (!PicoAgentManager_ReserveSession(app->agents, replacement->id, path))
    {
        PicoAgent_Destroy(replacement);
        return PICO_AGENT_RESULT_SESSION_IN_USE;
    }
    if (PicoSession_Replay(app, replacement, path, false) != 0)
    {
        PicoAgentManager_ReleaseSessions(app->agents, replacement->id);
        PicoAgent_Destroy(replacement);
        return PICO_AGENT_RESULT_SESSION_INVALID;
    }
    int index = FindIndex(app->agents, old->id);
    PicoAgentId old_id = old->id;
    if (!PicoAgent_Destroy(old))
    {
        PicoAgentManager_ReleaseSessions(app->agents, replacement->id);
        PicoAgent_Destroy(replacement);
        return PICO_AGENT_RESULT_BUSY;
    }
    app->agents->agents[index] = replacement;
    app->agents->active_id = replacement->id;
    PicoAgentManager_ReleaseSessions(app->agents, old_id);
    pico_run_hooks(app, PICO_HOOK_ON_AGENT_DESTROY, old_id);
    PicoSession_AppendInterrupted(app, replacement);
    pico_run_hooks(app, PICO_HOOK_ON_SESSION_RESET, replacement->id);
    PicoChatSel_Clear(app);
    app->chat_follow_bottom = true;
    return PICO_AGENT_RESULT_OK;
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

static PicoSubagentSnapshot *FindSnapshot(PicoAgentManager *manager, PicoAgentId child_id,
                                          const char *session_id)
{
    if (!manager)
    {
        return NULL;
    }
    if (child_id)
    {
        for (int i = 0; i < manager->snapshot_count; i++)
        {
            if (manager->snapshots[i].child_id == child_id)
            {
                return &manager->snapshots[i];
            }
        }
    }
    if (session_id && session_id[0])
    {
        for (int i = 0; i < manager->snapshot_count; i++)
        {
            if (manager->snapshots[i].session_id[0] &&
                strcmp(manager->snapshots[i].session_id, session_id) == 0)
            {
                return &manager->snapshots[i];
            }
        }
    }
    return NULL;
}

static PicoSubagentSnapshot *AllocSnapshot(PicoAgentManager *manager)
{
    if (!manager)
    {
        return NULL;
    }
    if (manager->snapshot_count >= manager->snapshot_capacity)
    {
        int next_capacity = manager->snapshot_capacity == 0 ? 16 : manager->snapshot_capacity * 2;
        PicoSubagentSnapshot *next = (PicoSubagentSnapshot *)realloc(
            manager->snapshots, (size_t)next_capacity * sizeof(PicoSubagentSnapshot));
        if (!next)
        {
            return NULL;
        }
        memset(&next[manager->snapshot_capacity], 0,
               (size_t)(next_capacity - manager->snapshot_capacity) * sizeof(PicoSubagentSnapshot));
        manager->snapshots = next;
        manager->snapshot_capacity = next_capacity;
    }
    PicoSubagentSnapshot *slot = &manager->snapshots[manager->snapshot_count++];
    memset(slot, 0, sizeof(*slot));
    return slot;
}

static void SnapshotChild(PicoAgentManager *manager, const PicoAgent *child)
{
    if (!manager || !child)
    {
        return;
    }
    PicoSubagentSnapshot *slot = FindSnapshot(manager, child->id, child->session_id);
    if (!slot)
    {
        slot = AllocSnapshot(manager);
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

static bool LoadSnapshotFromSession(PicoAgentManager *manager, const char *session_id)
{
    if (!manager || !MgrHost(manager) || !session_id || !session_id[0] ||
        FindSnapshot(manager, 0, session_id))
    {
        return FindSnapshot(manager, 0, session_id) != NULL;
    }
    PicoMessage *messages = NULL;
    int count = 0;
    if (PicoSession_LoadTranscript(MgrHost(manager), session_id, &messages, &count) != 0)
    {
        return false;
    }
    PicoMessages_PrepareDocs(messages, count);
    PicoSubagentSnapshot *slot = AllocSnapshot(manager);
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
    if (PicoSession_Resolve(MgrHost(manager), session_id, false, path, sizeof(path)) == 0 &&
        PicoSession_ReadHeader(path, &header) == 0)
    {
        snprintf(slot->profile, sizeof(slot->profile), "%s", header.profile);
        snprintf(slot->purpose, sizeof(slot->purpose), "%s", header.initial_purpose);
        snprintf(slot->model, sizeof(slot->model), "%s", header.model);
    }
    return true;
}

static void LinkDelegationToolRows(PicoAgentManager *manager)
{
    PicoDelegationJob *jobs[PICO_MAX_AGENTS * 2];
    int count = 0;
    pthread_mutex_lock(&manager->delegation_mu);
    for (PicoDelegationJob *it = manager->delegations;
         it && count < (int)(sizeof(jobs) / sizeof(jobs[0])); it = it->next)
    {
        jobs[count++] = it;
    }
    pthread_mutex_unlock(&manager->delegation_mu);
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
        PicoAgent *parent = PicoAgentManager_Find(manager, parent_id);
        PicoAgent *child = PicoAgentManager_Find(manager, child_id);
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

bool PicoAgentManager_InspectSubagent(PicoHost *app, const PicoTraceLine *line,
                                      PicoSubagentInspect *out)
{
    if (out)
    {
        memset(out, 0, sizeof(*out));
    }
    if (!app || !app->agents || !line || !out)
    {
        return false;
    }
    PicoAgentManager *manager = app->agents;
    char session_id[40];
    snprintf(session_id, sizeof(session_id), "%s", line->child_session_id);
    if (!session_id[0])
    {
        CopySessionIdFromOutput(line->tool_output, session_id, sizeof(session_id));
    }
    if (line->child_id)
    {
        PicoAgent *child = PicoAgentManager_Find(manager, line->child_id);
        if (child)
        {
            FillInspectFromAgent(out, child);
            return true;
        }
    }
    PicoSubagentSnapshot *slot = FindSnapshot(manager, line->child_id, session_id);
    if (!slot && session_id[0] && LoadSnapshotFromSession(manager, session_id))
    {
        slot = FindSnapshot(manager, 0, session_id);
    }
    if (!slot)
    {
        return false;
    }
    FillInspectFromSnapshot(out, slot);
    return true;
}

static char *UnknownProfileError(const PicoAgentManager *manager)
{
    JsonBuf b;
    JsonBuf_Init(&b);
    JsonBuf_Puts(&b, "unknown subagent profile; available: ");
    if (!manager || manager->profile_count <= 0)
    {
        JsonBuf_Puts(&b, "(none)");
        return JsonBuf_Steal(&b);
    }
    for (int i = 0; i < manager->profile_count; i++)
    {
        if (i > 0)
        {
            JsonBuf_Puts(&b, ", ");
        }
        JsonBuf_Puts(&b, manager->profiles[i].name);
    }
    return JsonBuf_Steal(&b);
}

static bool StartDelegation(PicoAgentManager *manager, PicoDelegationJob *job)
{
    PicoHost *app = MgrHost(manager);
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
        char *message = UnknownProfileError(manager);
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
        if (PicoSession_Resolve(app, job->session_id, false, path, sizeof(path)) != 0 ||
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
    PicoSession_LogUser(app, child, job->task, job->task, NULL);
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
    PicoAgent *child = child_id ? PicoAgentManager_Find(manager, child_id) : NULL;
    PicoAgent *parent = PicoAgentManager_Find(manager, job->parent_id);
    if (child)
    {
        PicoTraceLine *line = FindSubagentLine(parent, child_id, job->call_id);
        StampSubagentLine(line, child_id, child->session_id[0] ? child->session_id : NULL);
        SnapshotChild(manager, child);
    }
    pthread_mutex_lock(&job->mu);
    job->child_id = 0;
    pthread_mutex_unlock(&job->mu);
    PicoAgentResult closed = pico_agent_close(MgrHost(manager), child_id);
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
