#define _POSIX_C_SOURCE 200809L

#include "pico/plugin.h"

#include "config.h"
#include "host_internal.h"
#include "json.h"
#include "path.h"
#include "skill_load.h"
#include "skills.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Agent Skills (agentskills.io) as a builtin extension.
 *
 * Discovery: <config>/pico/skills/<name>/SKILL.md (global) and
 * <workspace>/.pico/skills/<name>/SKILL.md (workspace, shadows global).
 * Progressive disclosure: the LLM hook lists name+description in the
 * instructions; the use_skill tool loads the full body into the transcript.
 * The catalog is rescanned on every prompt build so skills dropped onto disk
 * take effect without a reload, mirroring how SYSTEM.md is re-read per turn. */

typedef struct SkillsState {
    PicoWorkspace *workspace;
    pthread_mutex_t lock; /* catalog: tool runs on the worker, rescans on main */
    PicoSkillCatalog catalog;
} SkillsState;

static void SkillsWarn(const char *path, const char *message, void *ctx)
{
    PicoHost *app = (PicoHost *)ctx;
    if (!app)
    {
        return;
    }
    char line[1024];
    snprintf(line, sizeof(line), "skill %s: %s", path, message);
    pico_status_warn(app, line);
}

static void SkillsRescan(SkillsState *s)
{
    char config[1024];
    char global_dir[1200];
    const char *global = NULL;
    if (Pico_ConfigDir(config, sizeof(config)) &&
        PicoPath_Format(global_dir, sizeof(global_dir), "%s/skills", config))
    {
        global = global_dir;
    }
    char ws_dir[4600];
    const char *ws_path = PicoWorkspace_Path(s->workspace);
    const char *workspace = NULL;
    if (ws_path[0] && PicoPath_Format(ws_dir, sizeof(ws_dir), "%s/.pico/skills", ws_path))
    {
        workspace = ws_dir;
    }
    PicoHost *app = pico_workspace_host(s->workspace);
    pthread_mutex_lock(&s->lock);
    PicoSkillCatalog_Scan(&s->catalog, global, workspace, SkillsWarn, app);
    pthread_mutex_unlock(&s->lock);
}

/* ------------------------------------------------------------- */
/* use_skill tool                                                  */
/* ------------------------------------------------------------- */

static const char kUseSkillParams[] =
    "{\"type\":\"object\","
    "\"properties\":{\"name\":{\"type\":\"string\","
    "\"description\":\"Name of the skill to load, from the available skills list\"}},"
    "\"required\":[\"name\"]}";

static void UseSkillRun(PicoAgentContext *ctx, const char *args_json, PicoToolResult *out, void *state)
{
    (void)ctx;
    SkillsState *s = (SkillsState *)state;
    if (!out)
    {
        return;
    }
    memset(out, 0, sizeof(*out));
    char *name = NULL;
    if (args_json)
    {
        JsonDoc doc;
        if (JsonParse(&doc, args_json, strlen(args_json)) == 0)
        {
            name = JsonObjStr(&doc, 0, "name");
            JsonFree(&doc);
        }
    }
    if (!name || !name[0])
    {
        out->is_error = true;
        out->output = JsonDup("use_skill: missing 'name' argument");
        free(name);
        return;
    }

    pthread_mutex_lock(&s->lock);
    const PicoSkill *skill = PicoSkillCatalog_Find(&s->catalog, name);
    if (!skill)
    {
        JsonBuf b;
        JsonBuf_Init(&b);
        JsonBuf_Puts(&b, "use_skill: unknown skill '");
        JsonBuf_Puts(&b, name);
        JsonBuf_Puts(&b, "'.");
        if (s->catalog.count > 0)
        {
            JsonBuf_Puts(&b, " Available skills:");
            for (int i = 0; i < s->catalog.count; i++)
            {
                JsonBuf_Puts(&b, i == 0 ? " " : ", ");
                JsonBuf_Puts(&b, s->catalog.skills[i].name);
            }
        }
        else
        {
            JsonBuf_Puts(&b, " No skills are loaded in this workspace.");
        }
        pthread_mutex_unlock(&s->lock);
        out->is_error = true;
        out->output = JsonBuf_Steal(&b);
        free(name);
        return;
    }
    size_t body_len = 0;
    char *body = PicoSkill_ReadBody(skill, &body_len);
    char *dir = JsonDup(skill->dir ? skill->dir : "");
    pthread_mutex_unlock(&s->lock);
    free(name);

    if (!body || !dir)
    {
        out->is_error = true;
        out->output = JsonDup("use_skill: could not read the skill's SKILL.md");
        free(body);
        free(dir);
        return;
    }
    JsonBuf b;
    JsonBuf_Init(&b);
    JsonBuf_Puts(&b, "Skill loaded. Its base directory is ");
    JsonBuf_Puts(&b, dir);
    JsonBuf_Puts(&b,
                 "\nRelative file references in the instructions below (scripts/, references/, "
                 "assets/) resolve from that directory.\n\n");
    JsonBuf_Puts(&b, body);
    free(body);
    free(dir);
    out->output = JsonBuf_Steal(&b);
}

/* ------------------------------------------------------------- */
/* Prompt metadata (progressive disclosure level 1)                */
/* ------------------------------------------------------------- */

static void SkillsLlmHook(PicoWorkspace *workspace, PicoAgentId agent_id, PicoLlmEvent *event,
                          void *state)
{
    (void)workspace;
    (void)agent_id;
    SkillsState *s = (SkillsState *)state;
    if (!s || !event || event->compact)
    {
        return;
    }
    bool offered = false;
    for (int i = 0; event->include_tools && i < event->tool_count; i++)
    {
        if (event->tools[i].name && strcmp(event->tools[i].name, "use_skill") == 0 &&
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
    SkillsRescan(s);
    pthread_mutex_lock(&s->lock);
    if (s->catalog.count == 0)
    {
        pthread_mutex_unlock(&s->lock);
        return;
    }
    JsonBuf b;
    JsonBuf_Init(&b);
    JsonBuf_Puts(&b, "## Skills\n");
    JsonBuf_Puts(&b,
                 "The following skills are available. When the task matches a skill's "
                 "description, call the use_skill tool with its name to load the skill's full "
                 "instructions before proceeding.\n");
    for (int i = 0; i < s->catalog.count; i++)
    {
        JsonBuf_Puts(&b, "\n- ");
        JsonBuf_Puts(&b, s->catalog.skills[i].name);
        JsonBuf_Puts(&b, ": ");
        /* Descriptions may contain newlines (block scalars); flatten to one line. */
        for (const char *d = s->catalog.skills[i].description; d && *d; d++)
        {
            JsonBuf_Putc(&b, (*d == '\n' || *d == '\r') ? ' ' : *d);
        }
    }
    pthread_mutex_unlock(&s->lock);
    event->extra_instructions = JsonBuf_Steal(&b);
}

/* ------------------------------------------------------------- */
/* /skill command                                                  */
/* ------------------------------------------------------------- */

/* Rewrites the submission instead of canceling it: the model receives the
 * skill body as the user message while the chat row keeps showing the typed
 * "/skill <name>". */
static void SkillCommand(PicoWorkspace *workspace, PicoAgentId agent_id, const char *args,
                         void *state)
{
    (void)agent_id;
    SkillsState *s = (SkillsState *)state;
    PicoHost *app = pico_workspace_host(workspace);
    if (!s || !app)
    {
        pico_host_request_submit_cancel(app);
        return;
    }
    const char *name = args ? args : "";
    while (*name == ' ' || *name == '\t')
    {
        name++;
    }
    size_t name_len = 0;
    while (name[name_len] && name[name_len] != ' ' && name[name_len] != '\t')
    {
        name_len++;
    }
    if (name_len == 0)
    {
        pico_status_warn(app, "Usage: /skill <name>");
        pico_host_request_submit_cancel(app);
        return;
    }
    char name_buf[128];
    if (name_len >= sizeof(name_buf))
    {
        pico_status_warn(app, "Unknown skill name.");
        pico_host_request_submit_cancel(app);
        return;
    }
    memcpy(name_buf, name, name_len);
    name_buf[name_len] = '\0';
    const char *note = name + name_len;
    while (*note == ' ' || *note == '\t')
    {
        note++;
    }

    SkillsRescan(s); /* pick up skills added since the last turn */
    pthread_mutex_lock(&s->lock);
    const PicoSkill *skill = PicoSkillCatalog_Find(&s->catalog, name_buf);
    if (!skill)
    {
        pthread_mutex_unlock(&s->lock);
        char msg[256];
        snprintf(msg, sizeof(msg), "Unknown skill '%s' (see the Skills card).", name_buf);
        pico_status_warn(app, msg);
        pico_host_request_submit_cancel(app);
        return;
    }
    size_t body_len = 0;
    char *body = PicoSkill_ReadBody(skill, &body_len);
    char *dir = JsonDup(skill->dir ? skill->dir : "");
    pthread_mutex_unlock(&s->lock);
    if (!body || !dir)
    {
        pico_status_warn(app, "Could not read the skill's SKILL.md.");
        pico_host_request_submit_cancel(app);
        free(body);
        free(dir);
        return;
    }

    JsonBuf b;
    JsonBuf_Init(&b);
    JsonBuf_Puts(&b, "Use the \"");
    JsonBuf_Puts(&b, name_buf);
    JsonBuf_Puts(&b, "\" skill to help with this request. Its base directory is ");
    JsonBuf_Puts(&b, dir);
    JsonBuf_Puts(&b,
                 "; relative file references (scripts/, references/, assets/) in the instructions "
                 "below resolve from that directory.\n\n");
    JsonBuf_Puts(&b, body);
    if (note[0])
    {
        JsonBuf_Puts(&b, "\n\nRequest: ");
        JsonBuf_Puts(&b, note);
    }
    free(body);
    free(dir);
    pico_host_set_agent_input(app, JsonBuf_Steal(&b));
    /* No pico_host_request_submit_cancel: the rewritten message submits. */
}

/* ------------------------------------------------------------- */
/* Builtin plumbing                                                */
/* ------------------------------------------------------------- */

int PicoSkills_List(PicoWorkspace *workspace, PicoSkillInfo *out, int max)
{
    SkillsState *s = (SkillsState *)PicoPlugins_WorkspaceState(workspace, "skills");
    if (!s)
    {
        return 0;
    }
    pthread_mutex_lock(&s->lock);
    int n = 0;
    while (n < s->catalog.count && n < max)
    {
        out[n].name = s->catalog.skills[n].name;
        out[n].description = s->catalog.skills[n].description;
        n++;
    }
    pthread_mutex_unlock(&s->lock);
    return n;
}

static int SkillsWorkspaceInit(PicoWorkspace *workspace, void **state_out)
{
    SkillsState *s = (SkillsState *)calloc(1, sizeof(SkillsState));
    if (!s)
    {
        return 1;
    }
    if (!state_out)
    {
        free(s);
        return 1;
    }
    s->workspace = workspace;
    pthread_mutex_init(&s->lock, NULL);
    *state_out = s;
    SkillsRescan(s);
    pico_add_tool(workspace, "use_skill",
                  "Load the full instructions of an available skill. Call this when the user's "
                  "task matches a skill's description; the result includes the skill's base "
                  "directory for resolving its scripts and other files.",
                  kUseSkillParams, UseSkillRun, NULL);
    pico_add_llm_hook(workspace, SkillsLlmHook);
    pico_workspace_add_command(workspace, "skill", "Load a skill into the conversation",
                               SkillCommand);
    return 0;
}

static void SkillsWorkspaceShutdown(PicoWorkspace *workspace, void *state)
{
    (void)workspace;
    SkillsState *s = (SkillsState *)state;
    if (!s)
    {
        return;
    }
    PicoSkillCatalog_Clear(&s->catalog);
    pthread_mutex_destroy(&s->lock);
    free(s);
}

PicoExt pico_ext_skills(void)
{
    return (PicoExt){
        .abi = PICO_EXT_ABI,
        .name = "skills",
        .description = "Agent Skills loading",
        .workspace_init = SkillsWorkspaceInit,
        .workspace_shutdown = SkillsWorkspaceShutdown,
    };
}
