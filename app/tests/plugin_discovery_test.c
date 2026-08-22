#define _POSIX_C_SOURCE 200809L

#include "pico/plugin.h"
#include "agent_manager.h"
#include "path.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static int g_clear_count;
static double g_time;

static PicoExt EmptyBuiltin(void)
{
    return (PicoExt){.abi = PICO_EXT_ABI, .name = "builtin"};
}

PicoExt pico_ext_chat(void) { return EmptyBuiltin(); }
PicoExt pico_ext_composer(void) { return EmptyBuiltin(); }
PicoExt pico_ext_footer(void) { return EmptyBuiltin(); }
PicoExt pico_ext_overlay(void) { return EmptyBuiltin(); }
PicoExt pico_ext_ask_user(void) { return EmptyBuiltin(); }
PicoExt pico_ext_todo(void) { return EmptyBuiltin(); }
PicoExt pico_ext_shell(void) { return EmptyBuiltin(); }
PicoExt pico_ext_subagent(void) { return EmptyBuiltin(); }
PicoExt pico_ext_commands(void) { return EmptyBuiltin(); }
PicoExt pico_ext_files(void) { return EmptyBuiltin(); }
PicoExt pico_ext_openai(void) { return EmptyBuiltin(); }
PicoExt pico_ext_extensions(void) { return EmptyBuiltin(); }
PicoExt pico_ext_prompt(void) { return EmptyBuiltin(); }
PicoExt pico_ext_sidebar(void) { return EmptyBuiltin(); }

bool PicoApp_ProcessRetired(void) { return false; }
void pico_clear_registrations(PicoApp *app) { (void)app; g_clear_count++; }
void pico_status_warn(PicoApp *app, const char *message) { (void)app; (void)message; }
double GetTime(void) { return g_time; }
void PicoAgentManager_SetAcceptingWork(PicoAgentManager *manager, bool accepting)
{ (void)manager; (void)accepting; }
bool PicoAgentManager_BlocksReload(const PicoAgentManager *manager) { (void)manager; return false; }
void PicoAgentManager_PrepareReload(PicoAgentManager *manager) { (void)manager; }
void PicoAgentManager_LoadProfiles(PicoAgentManager *manager) { (void)manager; }
void PicoAgentManager_RevalidateToolPolicies(PicoAgentManager *manager) { (void)manager; }
void PicoAgentManager_NotifySessions(PicoAgentManager *manager) { (void)manager; }
void PicoAgentManager_ReplayToolDetails(PicoAgentManager *manager) { (void)manager; }

static void MakeDirs(const char *path)
{
    char copy[4096];
    snprintf(copy, sizeof(copy), "%s", path);
    for (char *p = copy + 1; *p; p++)
    {
        if (*p == '/')
        {
            *p = '\0';
            (void)mkdir(copy, 0700);
            *p = '/';
        }
    }
    (void)mkdir(copy, 0700);
}

int main(void)
{
    char root[] = "/tmp/pico-plugin-global-XXXXXX";
    if (!mkdtemp(root) || setenv("XDG_CONFIG_HOME", root, 1) != 0)
    {
        return 1;
    }
    char workspace[4096];
    char workspace_ext[4096];
    char source[4096];
    if (!PicoPath_Format(workspace, sizeof(workspace), "%s/workspace", root) ||
        !PicoPath_Format(workspace_ext, sizeof(workspace_ext), "%s/.pico/extensions", workspace) ||
        !PicoPath_Format(source, sizeof(source), "%s/ignored.c", workspace_ext))
    {
        return 1;
    }
    MakeDirs(workspace_ext);
    FILE *file = fopen(source, "wb");
    if (!file)
    {
        return 1;
    }
    fputs("this is intentionally invalid C", file);
    fclose(file);

    PicoApp app;
    memset(&app, 0, sizeof(app));
    snprintf(app.workspace, sizeof(app.workspace), "%s", workspace);
    PicoPlugins_Load(&app);
    int initial_count = PicoPlugins_Count();
    int clears_after_load = g_clear_count;
    g_time = 1.0;
    PicoPlugins_Poll(&app);
    bool ok = initial_count == 14 && PicoPlugins_Count() == initial_count &&
              g_clear_count == clears_after_load && !app.status_warn;
    PicoPlugins_Shutdown(&app);
    unlink(source);
    rmdir(workspace_ext);
    char pico_dir[4096];
    PicoPath_Format(pico_dir, sizeof(pico_dir), "%s/.pico", workspace);
    rmdir(pico_dir);
    rmdir(workspace);
    return ok ? 0 : 1;
}
