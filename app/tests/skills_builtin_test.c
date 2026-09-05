#define _POSIX_C_SOURCE 200809L

/* Integration test for the skills builtin: stubs the host surface the builtin
 * uses, runs workspace_init against a fake workspace on disk, and exercises
 * the use_skill tool, the LLM hook metadata injection, and the card listing. */

#include "builtins/skills.h"
#include "host_internal.h"
#include "json.h"
#include "pico/plugin.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static int g_failed;

static void Check(bool ok, const char *message)
{
    if (!ok)
    {
        fprintf(stderr, "FAIL: %s\n", message);
        g_failed = 1;
    }
}

/* ---- host stubs ------------------------------------------------------- */

static char g_config_dir[1024];
static PicoToolFn g_tool_run;
static const char *g_tool_name;
static PicoLlmHookFn g_llm_hook;
static void *g_skills_state;

bool Pico_ConfigDir(char *out, size_t cap)
{
    int n = snprintf(out, cap, "%s", g_config_dir);
    return n > 0 && (size_t)n < cap;
}

bool pico_add_tool(PicoWorkspace *workspace, const char *name, const char *description,
                   const char *params_json, PicoToolFn run, PicoToolApplyFn apply)
{
    (void)workspace;
    (void)description;
    (void)params_json;
    (void)apply;
    g_tool_name = name;
    g_tool_run = run;
    return true;
}

void pico_add_llm_hook(PicoWorkspace *workspace, PicoLlmHookFn fn)
{
    (void)workspace;
    g_llm_hook = fn;
}

void pico_status_warn(PicoHost *host, const char *msg)
{
    (void)host;
    fprintf(stderr, "  status warn: %s\n", msg);
}

PicoHost *pico_workspace_host(PicoWorkspace *workspace)
{
    return workspace ? workspace->host : NULL;
}

static PicoWorkspaceCmdFn g_skill_cmd;
static char *g_agent_input;
static bool g_submit_canceled;

void pico_workspace_add_command(PicoWorkspace *workspace, const char *name, const char *help,
                                PicoWorkspaceCmdFn run)
{
    (void)workspace;
    (void)help;
    if (name && strcmp(name, "skill") == 0)
    {
        g_skill_cmd = run;
    }
}

void pico_host_set_agent_input(PicoHost *host, char *text)
{
    (void)host;
    free(g_agent_input);
    g_agent_input = text;
}

void pico_host_request_submit_cancel(PicoHost *host)
{
    (void)host;
    g_submit_canceled = true;
}

void *PicoPlugins_WorkspaceState(const PicoWorkspace *workspace, const char *name)
{
    (void)workspace;
    if (name && strcmp(name, "skills") == 0)
    {
        return g_skills_state;
    }
    return NULL;
}

/* ---- helpers ---------------------------------------------------------- */

static void WriteSkill(const char *root, const char *dir, const char *body)
{
    char path[2048];
    snprintf(path, sizeof(path), "%s/%s", root, dir);
    if (mkdir(path, 0755) != 0)
    {
        fprintf(stderr, "mkdir %s failed\n", path);
        g_failed = 1;
    }
    char file[2048];
    snprintf(file, sizeof(file), "%s/%s/SKILL.md", root, dir);
    FILE *f = fopen(file, "wb");
    if (!f)
    {
        fprintf(stderr, "write %s failed\n", file);
        g_failed = 1;
        return;
    }
    fputs(body, f);
    fclose(f);
}

int main(void)
{
    char tmp[256];
    snprintf(tmp, sizeof(tmp), "/tmp/pico-skills-builtin-test-XXXXXX");
    if (!mkdtemp(tmp))
    {
        fprintf(stderr, "mkdtemp failed\n");
        return 1;
    }
    snprintf(g_config_dir, sizeof(g_config_dir), "%s/config", tmp);
    char global_skills[1200];
    snprintf(global_skills, sizeof(global_skills), "%s/skills", g_config_dir);
    char home_dir[1200];
    snprintf(home_dir, sizeof(home_dir), "%s/home", tmp);
    char home_agents[1400];
    snprintf(home_agents, sizeof(home_agents), "%s/.agents", home_dir);
    char home_skills[1600];
    snprintf(home_skills, sizeof(home_skills), "%s/skills", home_agents);
    char ws_path[1024];
    snprintf(ws_path, sizeof(ws_path), "%s/workspace", tmp);
    char ws_pico[1200];
    snprintf(ws_pico, sizeof(ws_pico), "%s/.pico", ws_path);
    char ws_skills[1400];
    snprintf(ws_skills, sizeof(ws_skills), "%s/skills", ws_pico);
    char ws_agents[1400];
    snprintf(ws_agents, sizeof(ws_agents), "%s/.agents", ws_path);
    char ws_agent_skills[1600];
    snprintf(ws_agent_skills, sizeof(ws_agent_skills), "%s/skills", ws_agents);
    if (mkdir(g_config_dir, 0755) != 0 || mkdir(global_skills, 0755) != 0 ||
        mkdir(home_dir, 0755) != 0 || mkdir(home_agents, 0755) != 0 ||
        mkdir(home_skills, 0755) != 0 || mkdir(ws_path, 0755) != 0 || mkdir(ws_pico, 0755) != 0 ||
        mkdir(ws_skills, 0755) != 0 || mkdir(ws_agents, 0755) != 0 ||
        mkdir(ws_agent_skills, 0755) != 0)
    {
        fprintf(stderr, "setup mkdir failed\n");
        return 1;
    }
    if (setenv("HOME", home_dir, 1) != 0)
    {
        fprintf(stderr, "setenv HOME failed\n");
        return 1;
    }
    WriteSkill(home_skills, "agents-global",
               "---\nname: agents-global\ndescription: A home agents skill.\n---\nhome agents body\n");
    WriteSkill(global_skills, "global-skill",
               "---\nname: global-skill\ndescription: A global skill.\n---\nglobal body\n");
    WriteSkill(ws_agent_skills, "agents-ws",
               "---\nname: agents-ws\ndescription: A workspace agents skill.\n---\n"
               "Do the agents thing.\n");
    WriteSkill(ws_agent_skills, "ws-skill",
               "---\nname: ws-skill\ndescription: Shadowed agents workspace skill.\n---\n"
               "Do the agents thing.\n");
    WriteSkill(ws_skills, "ws-skill", "---\nname: ws-skill\ndescription: A workspace skill.\n---\n"
                                       "Do the workspace thing.\n");

    PicoWorkspace ws;
    memset(&ws, 0, sizeof(ws));
    ws.host = (PicoHost *)&ws; /* opaque non-NULL stand-in */
    snprintf(ws.path, sizeof(ws.path), "%s", ws_path);

    PicoExt ext = pico_ext_skills();
    Check(ext.workspace_init != NULL, "skills ext has workspace_init");
    void *state = NULL;
    Check(ext.workspace_init(&ws, &state) == 0 && state != NULL, "skills workspace_init succeeds");
    g_skills_state = state;

    /* Tool registered as use_skill. */
    Check(g_tool_run != NULL, "use_skill tool registered");
    Check(g_tool_name && strcmp(g_tool_name, "use_skill") == 0, "tool is named use_skill");

    /* LLM hook lists loaded skills with their descriptions. */
    Check(g_llm_hook != NULL, "llm hook registered");
    PicoTool skill_tool = {.name = "use_skill"};
    PicoLlmEvent ev;
    memset(&ev, 0, sizeof(ev));
    ev.include_tools = true;
    ev.tools = &skill_tool;
    ev.tool_count = 1;
    g_llm_hook(&ws, 1, &ev, state);
    Check(ev.extra_instructions != NULL, "hook injects extra instructions");
    if (ev.extra_instructions)
    {
        Check(strstr(ev.extra_instructions, "agents-global") != NULL,
              "metadata lists home agents skill");
        Check(strstr(ev.extra_instructions, "global-skill") != NULL, "metadata lists global skill");
        Check(strstr(ev.extra_instructions, "agents-ws") != NULL,
              "metadata lists workspace agents skill");
        Check(strstr(ev.extra_instructions, "ws-skill") != NULL, "metadata lists workspace skill");
        Check(strstr(ev.extra_instructions, "A workspace skill.") != NULL,
              "metadata includes descriptions");
        Check(strstr(ev.extra_instructions, "Shadowed agents workspace skill.") == NULL,
              "pico workspace skill shadows agents workspace skill");
        free(ev.extra_instructions);
    }

    /* Guidance must match the executable tool catalog. */
    PicoTool shell_tool = {.name = "sh"};
    memset(&ev, 0, sizeof(ev));
    ev.include_tools = true;
    ev.tools = &shell_tool;
    ev.tool_count = 1;
    g_llm_hook(&ws, 1, &ev, state);
    Check(!ev.extra_instructions, "allowlisted agent without use_skill gets no skill guidance");
    free(ev.extra_instructions);

    bool excluded = true;
    ev.tools = &skill_tool;
    ev.exclude = &excluded;
    ev.extra_instructions = NULL;
    g_llm_hook(&ws, 1, &ev, state);
    Check(!ev.extra_instructions, "excluded use_skill gets no skill guidance");
    free(ev.extra_instructions);

    ev.include_tools = false;
    ev.exclude = NULL;
    ev.extra_instructions = NULL;
    g_llm_hook(&ws, 1, &ev, state);
    Check(!ev.extra_instructions, "tool-free prompt gets no skill guidance");
    free(ev.extra_instructions);

    /* Card listing. */
    PicoSkillInfo infos[8];
    int n = PicoSkills_List(&ws, infos, 8);
    Check(n == 4, "card lists skills from each discovery directory");
    Check(n == 4 && strcmp(infos[0].name, "agents-global") == 0 &&
              strcmp(infos[1].name, "agents-ws") == 0 &&
              strcmp(infos[2].name, "global-skill") == 0 && strcmp(infos[3].name, "ws-skill") == 0,
          "card listing is sorted by name");
    Check(n == 4 && strcmp(infos[3].description, "A workspace skill.") == 0,
          "card listing carries descriptions");

    /* Tool returns the body and base directory for a known skill. */
    PicoToolResult result;
    memset(&result, 0, sizeof(result));
    g_tool_run(NULL, "{\"name\":\"ws-skill\"}", &result, state);
    Check(!result.is_error, "loading a known skill succeeds");
    Check(result.output && strstr(result.output, "Do the workspace thing.") != NULL,
          "tool result contains the skill body");
    Check(result.output && strstr(result.output, ws_skills) != NULL,
          "tool result contains the base directory");
    free(result.output);
    free(result.details_json);

    /* Unknown skills are an error that names the available skills. */
    memset(&result, 0, sizeof(result));
    g_tool_run(NULL, "{\"name\":\"nope\"}", &result, state);
    Check(result.is_error, "unknown skill is an error");
    Check(result.output && strstr(result.output, "ws-skill") != NULL,
          "unknown-skill error lists available skills");
    free(result.output);
    free(result.details_json);

    /* A skill added mid-session shows up after the hook's rescan. */
    WriteSkill(ws_skills, "late-skill", "---\nname: late-skill\ndescription: Added later.\n---\n");
    memset(&ev, 0, sizeof(ev));
    ev.include_tools = true;
    ev.tools = &skill_tool;
    ev.tool_count = 1;
    g_llm_hook(&ws, 1, &ev, state);
    Check(ev.extra_instructions && strstr(ev.extra_instructions, "late-skill") != NULL,
          "rescan picks up skills added after init");
    free(ev.extra_instructions);

    /* /skill rewrites the submission to carry the body and keeps submitting. */
    Check(g_skill_cmd != NULL, "skill command registered");
    g_submit_canceled = false;
    g_skill_cmd(&ws, 1, "ws-skill", state);
    Check(!g_submit_canceled, "known /skill keeps the submission alive");
    Check(g_agent_input && strstr(g_agent_input, "Do the workspace thing.") != NULL,
          "/skill submission contains the skill body");
    Check(g_agent_input && strstr(g_agent_input, ws_skills) != NULL,
          "/skill submission contains the base directory");
    free(g_agent_input);
    g_agent_input = NULL;

    g_submit_canceled = false;
    g_skill_cmd(&ws, 1, "nope", state);
    Check(g_submit_canceled && !g_agent_input, "unknown /skill cancels the submission");

    g_submit_canceled = false;
    g_skill_cmd(&ws, 1, "", state);
    Check(g_submit_canceled && !g_agent_input, "bare /skill cancels the submission");

    ext.workspace_shutdown(&ws, state);

    char cmd[2048];
    snprintf(cmd, sizeof(cmd), "rm -rf '%s'", tmp);
    if (system(cmd) != 0)
    {
        fprintf(stderr, "cleanup failed\n");
        g_failed = 1;
    }

    if (g_failed)
    {
        return 1;
    }
    printf("skills builtin: all checks passed\n");
    return 0;
}
