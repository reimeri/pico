#define _POSIX_C_SOURCE 200809L

#include "agent_manager.h"
#include "agent.h"
#include "json.h"
#include "session.h"
#include "settings.h"

#include <curl/curl.h>
#include <ctype.h>
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

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
    if (curl_global_init(CURL_GLOBAL_DEFAULT) != CURLE_OK)
    {
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
        if (!PicoSettings_EffortAllowed(model, options->effort))
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
    if (manager->count == 1 || PicoAgent_IsBusy(agent) || PicoAgent_RetiredReferences(manager, id))
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
    for (int i = 0; i < manager->count; i++)
    {
        PicoAgent_Pump(manager->app, manager->agents[i]);
    }
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
    for (int i = 0; i < manager->count; i++)
    {
        if (PicoAgent_BlocksReload(manager->agents[i]))
        {
            return true;
        }
    }
    return false;
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
    struct timespec deadline;
    clock_gettime(CLOCK_REALTIME, &deadline);
    deadline.tv_sec += 1;
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
        manager->retained_shutdown = true;
        return false;
    }
    if (manager->curl_initialized)
    {
        curl_global_cleanup();
    }
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

static bool ValidProfileName(const char *name)
{
    if (!name || !isalnum((unsigned char)name[0]))
    {
        return false;
    }
    for (const char *p = name + 1; *p; p++)
    {
        if (!isalnum((unsigned char)*p) && *p != '.' && *p != '_' && *p != '-')
        {
            return false;
        }
    }
    return strlen(name) <= 64;
}

static bool ToolRegistered(const PicoApp *app, const char *name)
{
    for (int i = 0; app && i < app->tool_count; i++)
    {
        if (app->tools[i].name && strcmp(app->tools[i].name, name) == 0)
        {
            return true;
        }
    }
    return false;
}

static void ProfileWarning(PicoApp *app, const char *path, const char *reason)
{
    char line[4608];
    snprintf(line, sizeof(line), "%s: %s", path, reason);
    pico_status_warn(app, line);
}

static bool ParseProfile(PicoApp *app, const char *path, const char *name,
                         PicoSubagentProfileInfo *out)
{
    size_t len = 0;
    char *source = Pico_ReadFile(path, &len);
    if (!source)
    {
        ProfileWarning(app, path, "could not read profile");
        return false;
    }
    if (!JsonValidUtf8(source, len))
    {
        ProfileWarning(app, path, "profile is not valid UTF-8");
        free(source);
        return false;
    }
    JsonStripComments(source, len);
    JsonDoc doc;
    if (JsonParse(&doc, source, len) != 0 || !JsonIsObject(&doc, 0))
    {
        ProfileWarning(app, path, "profile must be a JSON object");
        free(source);
        return false;
    }
    memset(out, 0, sizeof(*out));
    snprintf(out->name, sizeof(out->name), "%s", name);
    char *purpose = JsonObjStr(&doc, 0, "purpose");
    char *description = JsonObjStr(&doc, 0, "description");
    char *model = JsonObjStr(&doc, 0, "model");
    char *effort = JsonObjStr(&doc, 0, "effort");
    const char *error = NULL;
    if (!purpose || !purpose[0] || strlen(purpose) > 1024)
    {
        error = "purpose must be a non-empty string of at most 1024 bytes";
    }
    else if (JsonObjGet(&doc, 0, "description") >= 0 && !description)
    {
        error = "description must be a string";
    }
    else if (description && strlen(description) > 256)
    {
        error = "description exceeds 256 bytes";
    }
    else if (JsonObjGet(&doc, 0, "model") >= 0 && !model)
    {
        error = "model must be a string";
    }
    else if (model && (!model[0] || !PicoSettings_FindModelConst(app, model)))
    {
        error = "model is not in the model catalog";
    }
    else if (JsonObjGet(&doc, 0, "effort") >= 0 && !effort)
    {
        error = "effort must be a string";
    }
    else if (effort)
    {
        const PicoModel *resolved = model ? PicoSettings_FindModelConst(app, model) : NULL;
        if (resolved && !PicoSettings_EffortAllowed(resolved, effort))
        {
            error = "effort is not supported by the configured model";
        }
    }

    int tools_tok = JsonObjGet(&doc, 0, "tools");
    if (!error && tools_tok >= 0)
    {
        if (!JsonIsArray(&doc, tools_tok) || JsonArrayLen(&doc, tools_tok) > PICO_MAX_TOOLS)
        {
            error = "tools must be an array within the tool limit";
        }
        else
        {
            out->restricted_tools = true;
            int count = JsonArrayLen(&doc, tools_tok);
            for (int i = 0; i < count && !error; i++)
            {
                char *tool = JsonStrDup(&doc, JsonArrayAt(&doc, tools_tok, i));
                if (!tool || !tool[0] || strlen(tool) >= sizeof(out->tools[0]))
                {
                    error = "tool names must be non-empty strings shorter than 128 bytes";
                }
                else if (!ToolRegistered(app, tool))
                {
                    error = "tools contains an unknown tool name";
                }
                for (int j = 0; tool && !error && j < i; j++)
                {
                    if (strcmp(out->tools[j], tool) == 0)
                    {
                        error = "tools contains a duplicate name";
                    }
                }
                if (!error)
                {
                    snprintf(out->tools[out->tool_count++], sizeof(out->tools[0]), "%s", tool);
                }
                free(tool);
            }
        }
    }

    for (int i = 0; i < JsonObjLen(&doc, 0); i++)
    {
        int key_tok = -1;
        int value_tok = -1;
        if (!JsonObjPair(&doc, 0, i, &key_tok, &value_tok))
        {
            continue;
        }
        char *key = JsonStrDup(&doc, key_tok);
        if (key && strcmp(key, "purpose") != 0 && strcmp(key, "description") != 0 &&
            strcmp(key, "model") != 0 && strcmp(key, "effort") != 0 && strcmp(key, "tools") != 0)
        {
            char reason[256];
            snprintf(reason, sizeof(reason), "unknown profile key `%s`", key);
            ProfileWarning(app, path, reason);
        }
        free(key);
        (void)value_tok;
    }

    if (!error)
    {
        snprintf(out->purpose, sizeof(out->purpose), "%s", purpose);
        snprintf(out->description, sizeof(out->description), "%s", description ? description : "");
        if (model)
        {
            out->has_model = true;
            snprintf(out->model, sizeof(out->model), "%s", model);
        }
        if (effort)
        {
            out->has_effort = true;
            snprintf(out->effort, sizeof(out->effort), "%s", effort);
        }
    }
    else
    {
        ProfileWarning(app, path, error);
    }
    free(purpose);
    free(description);
    free(model);
    free(effort);
    JsonFree(&doc);
    free(source);
    return error == NULL;
}

void PicoAgentManager_LoadProfiles(PicoAgentManager *manager)
{
    if (!manager || !manager->app)
    {
        return;
    }
    char config[4096];
    char dir[4096];
    Pico_ConfigDir(config, sizeof(config));
    snprintf(dir, sizeof(dir), "%s/subagents", config);
    Pico_MkdirP(dir);

    PicoSubagentProfileInfo loaded[PICO_MAX_SUBAGENT_PROFILES];
    int count = 0;
    DIR *directory = opendir(dir);
    if (directory)
    {
        struct dirent *entry;
        while ((entry = readdir(directory)) && count < PICO_MAX_SUBAGENT_PROFILES)
        {
            size_t name_len = strlen(entry->d_name);
            if (entry->d_name[0] == '.' || name_len <= 5 ||
                strcmp(entry->d_name + name_len - 5, ".json") != 0)
            {
                continue;
            }
            char profile_name[65];
            size_t stem_len = name_len - 5;
            if (stem_len >= sizeof(profile_name))
            {
                continue;
            }
            memcpy(profile_name, entry->d_name, stem_len);
            profile_name[stem_len] = '\0';
            char path[4096];
            if ((size_t)snprintf(path, sizeof(path), "%s/%s", dir, entry->d_name) >= sizeof(path))
            {
                continue;
            }
            struct stat st;
            if (lstat(path, &st) != 0 || !S_ISREG(st.st_mode))
            {
                continue;
            }
            if (!ValidProfileName(profile_name))
            {
                ProfileWarning(manager->app, path, "invalid profile filename");
                continue;
            }
            if (ParseProfile(manager->app, path, profile_name, &loaded[count]))
            {
                count++;
            }
        }
        closedir(directory);
    }
    memcpy(manager->profiles, loaded, (size_t)count * sizeof(loaded[0]));
    manager->profile_count = count;
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

PicoAgentResult PicoAgentManager_ResumeActive(PicoApp *app, const char *id, bool allow_prefix)
{
    PicoAgent *old = PicoApp_ActiveAgent(app);
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
