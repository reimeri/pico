#define _POSIX_C_SOURCE 200809L

#include "subagent_config.h"

#include "agent_manager.h"
#include "json.h"
#include "settings.h"

#include <ctype.h>
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

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
    if (JsonParse(&doc, source, len) != 0)
    {
        ProfileWarning(app, path, "profile must be a JSON object");
        free(source);
        return false;
    }
    if (!JsonIsObject(&doc, 0))
    {
        ProfileWarning(app, path, "profile must be a JSON object");
        JsonFree(&doc);
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
    else if (effort && model &&
             !PicoSettings_EffortAllowed(PicoSettings_FindModelConst(app, model), effort))
    {
        error = "effort is not supported by the configured model";
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

void PicoSubagentConfig_Load(PicoAgentManager *manager)
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

    memset(manager->profiles, 0, sizeof(manager->profiles));
    memcpy(manager->profiles, loaded, (size_t)count * sizeof(loaded[0]));
    manager->profile_count = count;
}

const PicoSubagentProfileInfo *PicoSubagentConfig_Find(const PicoAgentManager *manager,
                                                       const char *name)
{
    for (int i = 0; manager && name && i < manager->profile_count; i++)
    {
        if (strcmp(manager->profiles[i].name, name) == 0)
        {
            return &manager->profiles[i];
        }
    }
    return NULL;
}

bool PicoSubagentConfig_Resolve(const PicoApp *app, const PicoAgent *parent,
                                const PicoSubagentProfileInfo *profile,
                                char *model, size_t model_cap,
                                char *effort, size_t effort_cap)
{
    if (!app || !parent || !profile || !model || model_cap == 0 || !effort || effort_cap == 0)
    {
        return false;
    }
    const char *resolved_model = profile->has_model ? profile->model : parent->model;
    const PicoModel *catalog = PicoSettings_FindModelConst(app, resolved_model);
    if (!catalog)
    {
        return false;
    }
    const char *resolved_effort = NULL;
    if (profile->has_effort)
    {
        resolved_effort = profile->effort;
    }
    else if (strcmp(resolved_model, parent->model) == 0)
    {
        resolved_effort = parent->effort;
    }
    else if (PicoSettings_EffortAllowed(catalog, catalog->default_effort))
    {
        resolved_effort = catalog->default_effort;
    }
    else if (catalog->effort_count > 0)
    {
        resolved_effort = catalog->effort[0];
    }
    else
    {
        resolved_effort = "none";
    }
    if (!PicoSettings_EffortAllowed(catalog, resolved_effort) &&
        !(catalog->effort_count == 0 && strcmp(resolved_effort, "none") == 0))
    {
        return false;
    }
    snprintf(model, model_cap, "%s", resolved_model);
    snprintf(effort, effort_cap, "%s", resolved_effort);
    return true;
}
