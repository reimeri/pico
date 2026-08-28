#include "pico/host.h"
#include "pico/plugin.h"
#include "host_internal.h"
#include "workspace_internal.h"
#include "settings.h"
#include "agent_internal.h"
#include "clay/clay.h"

#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

void Clay_Raylib_Render(Clay_RenderCommandArray renderCommands, Font *fonts)
{
    (void)renderCommands;
    (void)fonts;
}

_Static_assert((PicoWorkspaceId)0 == 0, "zero is an invalid workspace id");
_Static_assert((PicoAgentId)0 == 0, "zero is an invalid agent id");
_Static_assert(PICO_MAX_WORKSPACES == 8, "PICO_MAX_WORKSPACES");
_Static_assert(PICO_MAX_AGENTS == 16, "PICO_MAX_AGENTS");
_Static_assert(PICO_MAX_TOTAL_AGENTS == 32, "PICO_MAX_TOTAL_AGENTS");

static int g_failed;

static void Fail(const char *msg)
{
    fprintf(stderr, "FAIL: %s\n", msg);
    g_failed = 1;
}

static int TestCanonicalOpenAndDuplicate(void)
{
    char dir[] = "/tmp/pico-ws-XXXXXX";
    char alias[4096];
    PicoHost *host = NULL;
    PicoWorkspaceId first = 0;
    PicoWorkspaceId again = 0;
    PicoWorkspaceId linked = 0;
    PicoWorkspaceInfo info;

    if (!mkdtemp(dir))
    {
        Fail("mkdtemp");
        return 1;
    }
    snprintf(alias, sizeof(alias), "%s-alias", dir);
    if (symlink(dir, alias) != 0)
    {
        Fail("symlink");
        rmdir(dir);
        return 1;
    }
    if (pico_host_init(&host, NULL, true) != PICO_OK || !host)
    {
        Fail("pico_host_init");
        unlink(alias);
        rmdir(dir);
        return 1;
    }
    if (pico_workspace_open(host, dir, &first) != PICO_OK || first == 0)
    {
        Fail("open canonical directory");
        pico_host_free(host);
        unlink(alias);
        rmdir(dir);
        return 1;
    }
    if (pico_workspace_open(host, dir, &again) != PICO_ALREADY_OPEN || again != first)
    {
        Fail("duplicate open should return PICO_ALREADY_OPEN with the same id");
        pico_host_free(host);
        unlink(alias);
        rmdir(dir);
        return 1;
    }
    if (pico_workspace_open(host, alias, &linked) != PICO_ALREADY_OPEN || linked != first)
    {
        Fail("symlink alias should return PICO_ALREADY_OPEN with the same id");
        pico_host_free(host);
        unlink(alias);
        rmdir(dir);
        return 1;
    }
    if (pico_workspace_count(host) != 1 || !pico_workspace_info(host, 0, &info) || info.id != first)
    {
        Fail("workspace info");
        pico_host_free(host);
        unlink(alias);
        rmdir(dir);
        return 1;
    }
    if (info.main_agent_count != 0 || info.total_agent_count != 0)
    {
        Fail("opening a workspace must not create a main agent");
        pico_host_free(host);
        unlink(alias);
        rmdir(dir);
        return 1;
    }
    {
        char other[] = "/tmp/pico-ws-XXXXXX";
        PicoWorkspaceId second = 0;
        if (!mkdtemp(other))
        {
            Fail("mkdtemp second");
            pico_host_free(host);
            unlink(alias);
            rmdir(dir);
            return 1;
        }
        if (pico_workspace_open(host, other, &second) != PICO_LIMIT || second != 0 ||
            pico_workspace_count(host) != 1)
        {
            Fail("a second live workspace must return PICO_LIMIT");
            pico_host_free(host);
            unlink(alias);
            rmdir(other);
            rmdir(dir);
            return 1;
        }
        rmdir(other);
    }
    pico_host_free(host);
    unlink(alias);
    rmdir(dir);
    return 0;
}

static void DummyView(PicoHost *host, void *state)
{
    (void)host;
    (void)state;
}

static int TestSortedViewRegistrationAssignsStateAndRollsBack(void)
{
    PicoHost host;
    char state_old;
    char state_new;

    memset(&host, 0, sizeof(host));
    PicoHost_BeginRegistration(&host, PICO_REG_HOST, NULL);
    pico_host_add_view(&host, PICO_SLOT_SIDEBAR, 10, DummyView);
    PicoHost_PublishRegistration(&host, &state_old);
    PicoHost_BeginRegistration(&host, PICO_REG_HOST, NULL);
    pico_host_add_view(&host, PICO_SLOT_SIDEBAR, 0, DummyView);
    PicoHost_PublishRegistration(&host, &state_new);
    if (host.view_count[PICO_SLOT_SIDEBAR] != 2 || host.views[PICO_SLOT_SIDEBAR][0].z != 0 ||
        host.views[PICO_SLOT_SIDEBAR][0].state != &state_new || host.views[PICO_SLOT_SIDEBAR][1].z != 10 ||
        host.views[PICO_SLOT_SIDEBAR][1].state != &state_old)
    {
        Fail("lower-z view should receive the new state without stealing the old callback");
        return 1;
    }

    PicoHost_BeginRegistration(&host, PICO_REG_HOST, NULL);
    pico_host_add_view(&host, PICO_SLOT_SIDEBAR, -5, DummyView);
    PicoHost_DiscardRegistration(&host);
    if (host.view_count[PICO_SLOT_SIDEBAR] != 2 || host.views[PICO_SLOT_SIDEBAR][0].state != &state_new ||
        host.views[PICO_SLOT_SIDEBAR][1].state != &state_old)
    {
        Fail("failed init must not truncate an older view");
        return 1;
    }
    return 0;
}

static char *DupStr(const char *s)
{
    size_t n = strlen(s) + 1;
    char *out = (char *)malloc(n);
    if (out)
    {
        memcpy(out, s, n);
    }
    return out;
}

static int TestSubmitSettersTakeOwnership(void)
{
    PicoHost host;
    memset(&host, 0, sizeof(host));
    pico_host_set_agent_input(&host, DupStr("one"));
    pico_host_set_agent_input(&host, DupStr("two"));
    pico_host_set_agent_parts(&host, DupStr("[]"));
    pico_host_request_submit_cancel(&host);
    if (!host.submit_cancel || !host.agent_input || strcmp(host.agent_input, "two") != 0 || !host.agent_parts ||
        strcmp(host.agent_parts, "[]") != 0)
    {
        Fail("submit setters should own replacements and record cancel");
        free(host.agent_input);
        free(host.agent_parts);
        return 1;
    }
    pico_host_set_agent_input(&host, NULL);
    pico_host_set_agent_parts(&host, NULL);
    return 0;
}

static int TestWorkspaceBuiltinsRegisterThroughWorkspaceInit(void)
{
    PicoHost host;
    PicoWorkspace *workspace;
    PicoExt shell;
    PicoExt subagent;
    int i;
    bool has_sh = false;
    bool has_subagent = false;

    memset(&host, 0, sizeof(host));
    PicoHost_SetPath(&host, ".");
    workspace = PicoHost_PrimaryWorkspace(&host);
    shell = pico_ext_shell();
    subagent = pico_ext_subagent();
    if (shell.host_init || !shell.workspace_init || subagent.host_init || !subagent.workspace_init || !workspace)
    {
        Fail("shell and subagent must initialize as workspace instances");
        return 1;
    }
    PicoHost_BeginRegistration(&host, PICO_REG_WORKSPACE, workspace);
    if (shell.workspace_init(workspace, NULL) != 0)
    {
        Fail("shell workspace builtin init");
        return 1;
    }
    PicoHost_PublishRegistration(&host, NULL);

    PicoHost_BeginRegistration(&host, PICO_REG_WORKSPACE, workspace);
    if (subagent.workspace_init(workspace, NULL) != 0)
    {
        Fail("subagent workspace builtin init");
        return 1;
    }
    PicoHost_PublishRegistration(&host, NULL);

    for (i = 0; i < workspace->tool_count; i++)
    {
        if (workspace->tools[i].name && strcmp(workspace->tools[i].name, "sh") == 0)
        {
            has_sh = true;
        }
        if (workspace->tools[i].name && strcmp(workspace->tools[i].name, "subagent") == 0)
        {
            has_subagent = true;
        }
    }
    if (!has_sh || !has_subagent)
    {
        Fail("workspace init must register sh and subagent tools");
        free(host.workspaces[0]);
        return 1;
    }
    free(host.workspaces[0]);
    return 0;
}

static void RmRf(const char *path)
{
    DIR *d = opendir(path);
    struct dirent *ent;
    if (!d)
    {
        unlink(path);
        return;
    }
    while ((ent = readdir(d)) != NULL)
    {
        char child[4096];
        if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0)
        {
            continue;
        }
        snprintf(child, sizeof(child), "%s/%s", path, ent->d_name);
        RmRf(child);
    }
    closedir(d);
    rmdir(path);
}

static int MkdirParents(const char *path)
{
    char buf[4096];
    char *p;
    snprintf(buf, sizeof(buf), "%s", path);
    for (p = buf + 1; *p; p++)
    {
        if (*p == '/')
        {
            *p = 0;
            if (mkdir(buf, 0700) != 0 && errno != EEXIST)
            {
                return -1;
            }
            *p = '/';
        }
    }
    if (mkdir(buf, 0700) != 0 && errno != EEXIST)
    {
        return -1;
    }
    return 0;
}

static int WriteFile(const char *path, const char *text)
{
    FILE *f = fopen(path, "wb");
    if (!f)
    {
        return -1;
    }
    if (fputs(text, f) < 0)
    {
        fclose(f);
        return -1;
    }
    return fclose(f) == 0 ? 0 : -1;
}

static void ReadFileStr(const char *path, char *out, size_t cap)
{
    FILE *f;
    size_t n;
    if (!out || cap == 0)
    {
        return;
    }
    out[0] = '\0';
    f = fopen(path, "rb");
    if (!f)
    {
        return;
    }
    n = fread(out, 1, cap - 1, f);
    fclose(f);
    out[n] = '\0';
}

static const char *kLifecycleExt =
    "#include \"pico/plugin.h\"\n"
    "#include <stdio.h>\n"
    "#include <stdlib.h>\n"
    "static FILE *Life(void)\n"
    "{\n"
    "    const char *path = getenv(\"PICO_TEST_LIFE\");\n"
    "    return path ? fopen(path, \"a\") : NULL;\n"
    "}\n"
    "static void HostView(PicoHost *host, void *state)\n"
    "{\n"
    "    (void)host;\n"
    "    (void)state;\n"
    "}\n"
    "static int HostInit(PicoHost *host, void **state_out)\n"
    "{\n"
    "    *state_out = malloc(1);\n"
    "    pico_host_add_view(host, PICO_SLOT_SIDEBAR, 99, HostView);\n"
    "    return 0;\n"
    "}\n"
    "static void HostShutdown(PicoHost *host, void *state)\n"
    "{\n"
    "    FILE *f = Life();\n"
    "    (void)host;\n"
    "    if (f)\n"
    "    {\n"
    "        fputc('H', f);\n"
    "        fclose(f);\n"
    "    }\n"
    "    free(state);\n"
    "}\n"
    "static int WorkspaceInit(PicoWorkspace *workspace, void **state_out)\n"
    "{\n"
    "    (void)workspace;\n"
    "    if (getenv(\"PICO_TEST_FAIL_WORKSPACE\"))\n"
    "    {\n"
    "        return -1;\n"
    "    }\n"
    "    *state_out = malloc(1);\n"
    "    return 0;\n"
    "}\n"
    "static void WorkspaceShutdown(PicoWorkspace *workspace, void *state)\n"
    "{\n"
    "    FILE *f = Life();\n"
    "    if (f)\n"
    "    {\n"
    "        fputc(workspace && pico_workspace_host(workspace) ? 'Y' : 'N', f);\n"
    "        fclose(f);\n"
    "    }\n"
    "    free(state);\n"
    "}\n"
    "PicoExt pico_ext(void)\n"
    "{\n"
    "    return (PicoExt){\n"
    "        .abi = PICO_EXT_ABI,\n"
    "        .name = \"lifecycle\",\n"
    "        .host_init = HostInit,\n"
    "        .host_shutdown = HostShutdown,\n"
    "        .workspace_init = WorkspaceInit,\n"
    "        .workspace_shutdown = WorkspaceShutdown,\n"
    "    };\n"
    "}\n";

static int StartLifecycleHost(PicoHost **host_out, char *cfg, char *cache, char *ws, char *life, int fail_workspace)
{
    char ext_dir[320];
    char src[336];
    PicoWorkspaceId id = 0;

    snprintf(cfg, 256, "/tmp/pico-cfg-XXXXXX");
    snprintf(cache, 256, "/tmp/pico-cache-XXXXXX");
    snprintf(ws, 256, "/tmp/pico-ws-XXXXXX");
    if (!mkdtemp(cfg) || !mkdtemp(cache) || !mkdtemp(ws))
    {
        Fail("mkdtemp lifecycle");
        return -1;
    }
    snprintf(life, 512, "%s/life", cache);
    snprintf(ext_dir, sizeof(ext_dir), "%s/pico/extensions", cfg);
    snprintf(src, sizeof(src), "%s/lifecycle.c", ext_dir);
    if (MkdirParents(ext_dir) != 0 || WriteFile(src, kLifecycleExt) != 0)
    {
        Fail("write lifecycle extension");
        return -1;
    }
    setenv("XDG_CONFIG_HOME", cfg, 1);
    setenv("XDG_CACHE_HOME", cache, 1);
    setenv("PICO_TEST_LIFE", life, 1);
    if (fail_workspace)
    {
        setenv("PICO_TEST_FAIL_WORKSPACE", "1", 1);
    }
    else
    {
        unsetenv("PICO_TEST_FAIL_WORKSPACE");
    }
    if (pico_host_init(host_out, NULL, false) != PICO_OK || !*host_out)
    {
        Fail("pico_host_init lifecycle");
        return -1;
    }
    if (pico_workspace_open(*host_out, ws, &id) != PICO_OK)
    {
        Fail("open lifecycle workspace");
        pico_host_free(*host_out);
        *host_out = NULL;
        return -1;
    }
    PicoPlugins_Load(*host_out);
    return 0;
}

static void FinishLifecycleHost(PicoHost *host, char *cfg, char *cache, char *ws)
{
    if (host)
    {
        pico_host_free(host);
    }
    unsetenv("PICO_TEST_FAIL_WORKSPACE");
    unsetenv("PICO_TEST_LIFE");
    unsetenv("XDG_CONFIG_HOME");
    unsetenv("XDG_CACHE_HOME");
    RmRf(cfg);
    RmRf(cache);
    RmRf(ws);
}

static int SidebarHasLifecycleView(const PicoHost *host)
{
    int i;
    if (!host)
    {
        return 0;
    }
    for (i = 0; i < host->view_count[PICO_SLOT_SIDEBAR]; i++)
    {
        if (host->views[PICO_SLOT_SIDEBAR][i].z == 99 && host->views[PICO_SLOT_SIDEBAR][i].host_render &&
            host->views[PICO_SLOT_SIDEBAR][i].state)
        {
            return 1;
        }
    }
    return 0;
}

static int TestFailedWorkspaceInitKeepsHostSlot(void)
{
    char cfg[256];
    char cache[256];
    char ws[256];
    char life[512];
    char log[8];
    PicoHost *host = NULL;

    cfg[0] = cache[0] = ws[0] = '\0';
    if (StartLifecycleHost(&host, cfg, cache, ws, life, 1) != 0)
    {
        FinishLifecycleHost(host, cfg, cache, ws);
        return 1;
    }
    ReadFileStr(life, log, sizeof(log));
    if (log[0] != '\0' || !SidebarHasLifecycleView(host))
    {
        Fail("failed workspace init must keep the published host instance");
        if (host && host->status_warn)
        {
            fprintf(stderr, "status_warn:\n%s\n", host->status_warn);
        }
        FinishLifecycleHost(host, cfg, cache, ws);
        return 1;
    }
    PicoPlugins_Shutdown(host);
    ReadFileStr(life, log, sizeof(log));
    FinishLifecycleHost(host, cfg, cache, ws);
    if (strcmp(log, "H") != 0)
    {
        Fail("host shutdown should run once when the process tears down the kept host slot");
        return 1;
    }
    return 0;
}

static int TestWorkspaceShutdownSeesOwningWorkspace(void)
{
    char cfg[256];
    char cache[256];
    char ws[256];
    char life[512];
    char log[8];
    PicoHost *host = NULL;

    cfg[0] = cache[0] = ws[0] = '\0';
    if (StartLifecycleHost(&host, cfg, cache, ws, life, 0) != 0)
    {
        FinishLifecycleHost(host, cfg, cache, ws);
        return 1;
    }
    pico_host_free(host);
    host = NULL;
    ReadFileStr(life, log, sizeof(log));
    FinishLifecycleHost(NULL, cfg, cache, ws);
    if (log[0] != 'Y')
    {
        Fail("workspace shutdown must receive the owning workspace");
        return 1;
    }
    return 0;
}

static int TestWorkspaceChangeSeesOwningWorkspace(void)
{
    char cfg[256];
    char cache[256];
    char ws[256];
    char ws2[256];
    char life[512];
    char log[16];
    PicoHost *host = NULL;

    cfg[0] = cache[0] = ws[0] = ws2[0] = '\0';
    if (StartLifecycleHost(&host, cfg, cache, ws, life, 0) != 0)
    {
        FinishLifecycleHost(host, cfg, cache, ws);
        return 1;
    }
    snprintf(ws2, sizeof(ws2), "/tmp/pico-ws2-XXXXXX");
    if (!mkdtemp(ws2))
    {
        Fail("mkdtemp ws2");
        FinishLifecycleHost(host, cfg, cache, ws);
        return 1;
    }
    if (!PicoHost_ChangeWorkspace(host, ws2))
    {
        Fail("request change workspace");
        FinishLifecycleHost(host, cfg, cache, ws);
        RmRf(ws2);
        return 1;
    }
    pico_host_pump(host);
    ReadFileStr(life, log, sizeof(log));
    if (log[0] != 'Y')
    {
        Fail("workspace change must call workspace_shutdown on the old valid workspace");
        FinishLifecycleHost(host, cfg, cache, ws);
        RmRf(ws2);
        return 1;
    }
    pico_host_free(host);
    host = NULL;
    ReadFileStr(life, log, sizeof(log));
    FinishLifecycleHost(NULL, cfg, cache, ws);
    RmRf(ws2);
    if (strcmp(log, "YHYH") != 0)
    {
        fprintf(stderr, "actual log: %s\n", log);
        Fail("workspace shutdown must run cleanly for both workspaces without use-after-free");
        return 1;
    }
    return 0;
}

static int TestModelChangeDoesNotMutateWorkspaceDefault(void)
{
    PicoWorkspace ws;
    PicoAgent agent;
    PicoModel models[2];
    memset(&ws, 0, sizeof(ws));
    memset(&agent, 0, sizeof(agent));
    memset(models, 0, sizeof(models));

    snprintf(models[0].id, sizeof(models[0].id), "original-default-model");
    snprintf(models[1].id, sizeof(models[1].id), "custom-agent-model");
    ws.models = models;
    ws.model_count = 2;
    snprintf(ws.settings.default_model, sizeof(ws.settings.default_model), "original-default-model");
    agent.workspace = &ws;

    PicoSettings_InitAgent(&agent);
    if (strcmp(agent.model_name, "original-default-model") != 0)
    {
        Fail("agent should initialize with workspace default model");
        return 1;
    }

    PicoSettings_SetModel(&agent, "custom-agent-model");
    if (strcmp(agent.model_name, "custom-agent-model") != 0)
    {
        Fail("agent model should update to custom model");
        return 1;
    }
    if (strcmp(ws.settings.default_model, "original-default-model") != 0)
    {
        Fail("PicoSettings_SetModel must not mutate workspace default_model");
        return 1;
    }
    return 0;
}

static int TestWorkspacePluginIsolation(void)
{
    char ws1_dir[] = "/tmp/pico-ws-iso1-XXXXXX";
    char ws2_dir[] = "/tmp/pico-ws-iso2-XXXXXX";
    if (!mkdtemp(ws1_dir) || !mkdtemp(ws2_dir))
    {
        Fail("mkdtemp ws iso");
        return 1;
    }
    PicoHost *host1 = NULL;
    PicoHost *host2 = NULL;
    PicoWorkspaceId id1 = 0;
    PicoWorkspaceId id2 = 0;
    if (pico_host_init(&host1, NULL, true) != PICO_OK || !host1 ||
        pico_host_init(&host2, NULL, true) != PICO_OK || !host2)
    {
        Fail("host_init iso");
        if (host1) pico_host_free(host1);
        if (host2) pico_host_free(host2);
        rmdir(ws1_dir);
        rmdir(ws2_dir);
        return 1;
    }
    if (pico_workspace_open(host1, ws1_dir, &id1) != PICO_OK ||
        pico_workspace_open(host2, ws2_dir, &id2) != PICO_OK)
    {
        Fail("workspace_open iso");
        pico_host_free(host1);
        pico_host_free(host2);
        rmdir(ws1_dir);
        rmdir(ws2_dir);
        return 1;
    }
    PicoPlugins_Load(host1);
    PicoPlugins_Load(host2);
    PicoWorkspace *ws1 = PicoHost_PrimaryWorkspace(host1);
    PicoWorkspace *ws2 = PicoHost_PrimaryWorkspace(host2);
    void *files1 = PicoPlugins_WorkspaceState(ws1, "files");
    void *diff1 = PicoPlugins_WorkspaceState(ws1, "diff");
    void *todo1 = PicoPlugins_WorkspaceState(ws1, "todos");
    void *files2 = PicoPlugins_WorkspaceState(ws2, "files");
    void *diff2 = PicoPlugins_WorkspaceState(ws2, "diff");
    void *todo2 = PicoPlugins_WorkspaceState(ws2, "todos");
    if (!files1 || !diff1 || !todo1 || !files2 || !diff2 || !todo2)
    {
        Fail("workspace plugins must be initialized on both workspaces");
        pico_host_free(host1);
        pico_host_free(host2);
        rmdir(ws1_dir);
        rmdir(ws2_dir);
        return 1;
    }
    if (files1 == files2 || diff1 == diff2 || todo1 == todo2)
    {
        Fail("workspace plugin states must be isolated per-workspace instance");
        pico_host_free(host1);
        pico_host_free(host2);
        rmdir(ws1_dir);
        rmdir(ws2_dir);
        return 1;
    }

    pico_host_free(host1);
    pico_host_free(host2);
    rmdir(ws1_dir);
    rmdir(ws2_dir);
    return 0;
}

static int TestHostPluginIsolation(void)
{
    char cfg1[] = "/tmp/pico-cfg1-XXXXXX";
    char cfg2[] = "/tmp/pico-cfg2-XXXXXX";
    char cache1[] = "/tmp/pico-cache1-XXXXXX";
    char cache2[] = "/tmp/pico-cache2-XXXXXX";
    if (!mkdtemp(cfg1) || !mkdtemp(cfg2) || !mkdtemp(cache1) || !mkdtemp(cache2))
    {
        Fail("mkdtemp host iso");
        return 1;
    }
    setenv("XDG_CONFIG_HOME", cfg1, 1);
    setenv("XDG_CACHE_HOME", cache1, 1);
    PicoHost *host1 = NULL;
    if (pico_host_init(&host1, NULL, true) != PICO_OK || !host1)
    {
        Fail("host1 init");
        return 1;
    }
    PicoPlugins_Load(host1);

    setenv("XDG_CONFIG_HOME", cfg2, 1);
    setenv("XDG_CACHE_HOME", cache2, 1);
    PicoHost *host2 = NULL;
    if (pico_host_init(&host2, NULL, true) != PICO_OK || !host2)
    {
        Fail("host2 init");
        pico_host_free(host1);
        return 1;
    }
    PicoPlugins_Load(host2);

    void *comp1 = PicoPlugins_HostState(host1, "composer");
    void *comp2 = PicoPlugins_HostState(host2, "composer");
    void *chat1 = PicoPlugins_HostState(host1, "chat");
    void *chat2 = PicoPlugins_HostState(host2, "chat");
    if (!comp1 || !comp2 || !chat1 || !chat2 || comp1 == comp2 || chat1 == chat2)
    {
        Fail("host plugins must have distinct per-host instances without global fallback");
        pico_host_free(host1);
        pico_host_free(host2);
        return 1;
    }

    pico_host_free(host1);
    pico_host_free(host2);
    unsetenv("XDG_CONFIG_HOME");
    unsetenv("XDG_CACHE_HOME");
    RmRf(cfg1);
    RmRf(cfg2);
    RmRf(cache1);
    RmRf(cache2);
    return 0;
}

static int TestHostPreferencesPersistence(void)
{
    char cfg[] = "/tmp/pico-pref-cfg-XXXXXX";
    char cache[] = "/tmp/pico-pref-cache-XXXXXX";
    char ws_dir[] = "/tmp/pico-pref-ws-XXXXXX";
    if (!mkdtemp(cfg) || !mkdtemp(cache) || !mkdtemp(ws_dir))
    {
        Fail("mkdtemp pref");
        return 1;
    }
    setenv("XDG_CONFIG_HOME", cfg, 1);
    setenv("XDG_CACHE_HOME", cache, 1);

    PicoHost *host = NULL;
    PicoWorkspaceId id = 0;
    if (pico_host_init(&host, NULL, true) != PICO_OK || !host)
    {
        Fail("pref host init");
        return 1;
    }
    if (pico_workspace_open(host, ws_dir, &id) != PICO_OK)
    {
        Fail("pref ws open");
        pico_host_free(host);
        return 1;
    }
    PicoPlugins_Load(host);

    int ext_count = PicoPlugins_Count();
    int target_idx = -1;
    for (int i = 0; i < ext_count; i++)
    {
        PicoExtInfo info;
        if (PicoPlugins_Get(i, &info) && info.name && strcmp(info.name, "footer") == 0)
        {
            target_idx = i;
            break;
        }
    }
    if (target_idx < 0)
    {
        Fail("find footer extension");
        pico_host_free(host);
        return 1;
    }

    if (!PicoPlugins_SetEnabled(host, target_idx, false))
    {
        Fail("PicoPlugins_SetEnabled to false");
        pico_host_free(host);
        return 1;
    }

    char pref_path[512];
    snprintf(pref_path, sizeof(pref_path), "%s/pico/host_preferences.json", cfg);
    if (access(pref_path, F_OK) != 0)
    {
        Fail("disabling host plugin must write to host_preferences.json");
        pico_host_free(host);
        return 1;
    }

    char ws_settings_path[512];
    snprintf(ws_settings_path, sizeof(ws_settings_path), "%s/.pico/settings.json", ws_dir);
    if (access(ws_settings_path, F_OK) == 0)
    {
        char content[1024];
        ReadFileStr(ws_settings_path, content, sizeof(content));
        if (strstr(content, "disabled_extensions") || strstr(content, "footer"))
        {
            Fail("disabling host plugin must NOT write disabled_extensions to workspace settings.json");
            pico_host_free(host);
            return 1;
        }
    }

    pico_host_free(host);
    unsetenv("XDG_CONFIG_HOME");
    unsetenv("XDG_CACHE_HOME");
    RmRf(cfg);
    RmRf(cache);
    RmRf(ws_dir);
    return 0;
}

static void DummyHostView(PicoHost *h, void *s) { (void)h; (void)s; }
static void DummyWsView(PicoWorkspace *w, PicoAgentId a, void *s) { (void)w; (void)a; (void)s; }
static void DummyHostHook(PicoHost *h, const PicoHookEvent *e, void *s) { (void)h; (void)e; (void)s; }
static void DummyWsHook(PicoWorkspace *w, const PicoHookEvent *e, void *s) { (void)w; (void)e; (void)s; }
static void DummyTool(PicoAgentContext *c, const char *a, PicoToolResult *o, void *s) { (void)c; (void)a; (void)o; (void)s; }
static void DummyHostCmd(PicoHost *h, PicoAgentId a, const char *args, void *s) { (void)h; (void)a; (void)args; (void)s; }
static void DummyWsCmd(PicoWorkspace *w, PicoAgentId a, const char *args, void *s) { (void)w; (void)a; (void)args; (void)s; }
static int DummyHostQuery(PicoHost *h, const char *p, PicoCompleteItem *o, int m, void *s) { (void)h; (void)p; (void)o; (void)m; (void)s; return 0; }
static int DummyWsQuery(PicoWorkspace *w, const char *p, PicoCompleteItem *o, int m, void *s) { (void)w; (void)p; (void)o; (void)m; (void)s; return 0; }
static void DummyAuthLogin(PicoHost *h, PicoAgentId a, const char *args, void *s) { (void)h; (void)a; (void)args; (void)s; }

static int TestScopeEnforcement(void)
{
    PicoHost host;
    memset(&host, 0, sizeof(host));
    PicoHost_SetPath(&host, ".");
    PicoWorkspace *ws = PicoHost_PrimaryWorkspace(&host);

    /* 1. In Host Init scope */
    PicoHost_BeginRegistration(&host, PICO_REG_HOST, NULL);

    /* Workspace registrations must be rejected during host init */
    if (pico_add_tool(ws, "invalid_tool", "desc", "{}", DummyTool, NULL))
    {
        Fail("pico_add_tool must be rejected during host init");
        free(host.workspaces[0]);
        return 1;
    }
    pico_workspace_add_view(ws, PICO_SLOT_SIDEBAR, 0, DummyWsView);
    pico_workspace_add_empty_view(ws, PICO_EMPTY_ABOVE, 0, DummyWsView);
    pico_workspace_add_command(ws, "invalid_cmd", "help", DummyWsCmd);
    pico_workspace_add_completer(ws, '#', false, DummyWsQuery, NULL);
    pico_workspace_add_hook(ws, PICO_HOOK_BEFORE_SUBMIT, DummyWsHook);
    pico_add_tool_before_hook(ws, NULL);
    pico_add_tool_after_hook(ws, NULL);
    pico_add_llm_hook(ws, NULL);
    pico_add_context_hook(ws, NULL);
    pico_add_tool_row_hook(ws, NULL);

    if (ws->tool_count > 0 || ws->view_count[PICO_SLOT_SIDEBAR] > 0 || ws->empty_view_count > 0 ||
        ws->command_count > 0 || ws->completer_count > 0 || ws->hook_count > 0 ||
        ws->tool_before_hook_count > 0 || ws->tool_after_hook_count > 0 || ws->llm_hook_count > 0 ||
        ws->context_hook_count > 0 || ws->tool_row_hook_count > 0 ||
        host.staging.ws_tool_count > 0)
    {
        Fail("workspace registrations during host init must not mutate workspace or staging state");
        free(host.workspaces[0]);
        return 1;
    }
    if (!host.status_warn)
    {
        Fail("workspace registrations during host init must generate warnings");
        free(host.workspaces[0]);
        return 1;
    }
    PicoHost_DiscardRegistration(&host);
    free(host.status_warn);
    host.status_warn = NULL;

    /* 2. In Workspace Init scope */
    PicoHost_BeginRegistration(&host, PICO_REG_WORKSPACE, ws);

    /* Host registrations must be rejected during workspace init */
    pico_host_add_view(&host, PICO_SLOT_SIDEBAR, 0, DummyHostView);
    pico_host_add_command(&host, "invalid_hcmd", "help", DummyHostCmd);
    pico_host_add_completer(&host, '#', false, DummyHostQuery, NULL);
    pico_add_auth(&host, &(PicoAuth){.provider = "test", .login = DummyAuthLogin});
    pico_host_add_hook(&host, PICO_HOOK_AFTER_LAYOUT, DummyHostHook);

    /* Workspace cannot register AFTER_LAYOUT or AFTER_RENDER */
    pico_workspace_add_hook(ws, PICO_HOOK_AFTER_LAYOUT, DummyWsHook);
    pico_workspace_add_hook(ws, PICO_HOOK_AFTER_RENDER, DummyWsHook);

    if (host.view_count[PICO_SLOT_SIDEBAR] > 0 || host.command_count > 0 || host.completer_count > 0 ||
        host.auth_count > 0 || host.hook_count > 0 || host.staging.host_view_count[PICO_SLOT_SIDEBAR] > 0)
    {
        Fail("host registrations during workspace init must not mutate host state");
        free(host.workspaces[0]);
        return 1;
    }
    if (!host.status_warn)
    {
        Fail("host registrations during workspace init must generate warnings");
        free(host.workspaces[0]);
        return 1;
    }
    PicoHost_DiscardRegistration(&host);
    free(host.status_warn);
    host.status_warn = NULL;

    /* 3. Outside of any init (PICO_REG_NONE) */
    pico_host_add_command(&host, "unscoped_hcmd", "help", DummyHostCmd);
    pico_workspace_add_command(ws, "unscoped_wcmd", "help", DummyWsCmd);
    if (host.command_count > 0 || ws->command_count > 0)
    {
        Fail("registrations outside init must not mutate host or workspace");
        free(host.workspaces[0]);
        return 1;
    }

    free(host.workspaces[0]);
    return 0;
}

typedef struct RollbackState {
    bool freed;
} RollbackState;

static int FailingWorkspaceInit(PicoWorkspace *ws, void **state_out)
{
    RollbackState *s = (RollbackState *)calloc(1, sizeof(RollbackState));
    *state_out = s;
    pico_add_tool(ws, "rollback_tool", "desc", "{}", DummyTool, NULL);
    pico_workspace_add_command(ws, "rollback_cmd", "help", DummyWsCmd);
    return -1;
}

static void RollbackWorkspaceShutdown(PicoWorkspace *ws, void *state)
{
    (void)ws;
    RollbackState *s = (RollbackState *)state;
    if (s)
    {
        s->freed = true;
        free(s);
    }
}

static int TestStagingRollbackOnFailedInit(void)
{
    PicoHost host;
    memset(&host, 0, sizeof(host));
    PicoHost_SetPath(&host, ".");
    PicoWorkspace *ws = PicoHost_PrimaryWorkspace(&host);

    int init_tools = ws->tool_count;
    int init_cmds = ws->command_count;

    PicoExt ext = {
        .abi = PICO_EXT_ABI,
        .name = "failing_ext",
        .workspace_init = FailingWorkspaceInit,
        .workspace_shutdown = RollbackWorkspaceShutdown,
    };

    void *state = NULL;
    PicoHost_BeginRegistration(&host, PICO_REG_WORKSPACE, ws);
    int rc = ext.workspace_init(ws, &state);
    if (rc != 0)
    {
        PicoHost_DiscardRegistration(&host);
        if (state && ext.workspace_shutdown)
        {
            ext.workspace_shutdown(ws, state);
        }
    }
    else
    {
        PicoHost_PublishRegistration(&host, state);
    }

    if (ws->tool_count != init_tools || ws->command_count != init_cmds)
    {
        Fail("staged registrations must be rolled back on init failure");
        free(host.workspaces[0]);
        return 1;
    }

    free(host.workspaces[0]);
    return 0;
}

static const char *kWorkspaceLocalWithHostExt =
    "#include \"pico/plugin.h\"\n"
    "#include <stdlib.h>\n"
    "static int HostInit(PicoHost *host, void **state_out)\n"
    "{\n"
    "    (void)host;\n"
    "    (void)state_out;\n"
    "    return 0;\n"
    "}\n"
    "PicoExt pico_ext(void)\n"
    "{\n"
    "    return (PicoExt){\n"
    "        .abi = PICO_EXT_ABI,\n"
    "        .name = \"ws_local_bad\",\n"
    "        .host_init = HostInit,\n"
    "    };\n"
    "}\n";

static int TestWorkspaceLocalExtensionWithHostCallbacksRejected(void)
{
    char cfg[256];
    char cache[256];
    char ws[256];
    char ws_ext_dir[1024];
    char src[2048];
    PicoHost *host = NULL;
    PicoWorkspaceId id = 0;

    snprintf(cfg, sizeof(cfg), "/tmp/pico-cfg-XXXXXX");
    snprintf(cache, sizeof(cache), "/tmp/pico-cache-XXXXXX");
    snprintf(ws, sizeof(ws), "/tmp/pico-ws-XXXXXX");
    if (!mkdtemp(cfg) || !mkdtemp(cache) || !mkdtemp(ws))
    {
        Fail("mkdtemp ws_local");
        return 1;
    }
    snprintf(ws_ext_dir, sizeof(ws_ext_dir), "%s/.pico/extensions", ws);
    snprintf(src, sizeof(src), "%s/bad.c", ws_ext_dir);
    if (MkdirParents(ws_ext_dir) != 0 || WriteFile(src, kWorkspaceLocalWithHostExt) != 0)
    {
        Fail("write ws_local extension");
        return 1;
    }
    setenv("XDG_CONFIG_HOME", cfg, 1);
    setenv("XDG_CACHE_HOME", cache, 1);
    if (pico_host_init(&host, NULL, false) != PICO_OK || !host)
    {
        Fail("pico_host_init ws_local");
        return 1;
    }
    if (pico_workspace_open(host, ws, &id) != PICO_OK)
    {
        Fail("open ws_local workspace");
        pico_host_free(host);
        return 1;
    }
    PicoPlugins_Load(host);

    if (!host->status_warn || !strstr(host->status_warn, "workspace-local extension cannot have host callbacks"))
    {
        Fail("workspace-local extension with host callbacks must be rejected with warning");
        pico_host_free(host);
        unsetenv("XDG_CONFIG_HOME");
        unsetenv("XDG_CACHE_HOME");
        RmRf(cfg);
        RmRf(cache);
        RmRf(ws);
        return 1;
    }

    pico_host_free(host);
    unsetenv("XDG_CONFIG_HOME");
    unsetenv("XDG_CACHE_HOME");
    RmRf(cfg);
    RmRf(cache);
    RmRf(ws);
    return 0;
}

int main(void)
{
    if (TestCanonicalOpenAndDuplicate() != 0)
    {
        return 1;
    }
    if (TestSortedViewRegistrationAssignsStateAndRollsBack() != 0)
    {
        return 1;
    }
    if (TestSubmitSettersTakeOwnership() != 0)
    {
        return 1;
    }
    if (TestWorkspaceBuiltinsRegisterThroughWorkspaceInit() != 0)
    {
        return 1;
    }
    if (TestFailedWorkspaceInitKeepsHostSlot() != 0)
    {
        return 1;
    }
    if (TestWorkspaceShutdownSeesOwningWorkspace() != 0)
    {
        return 1;
    }
    if (TestWorkspaceChangeSeesOwningWorkspace() != 0)
    {
        return 1;
    }
    if (TestModelChangeDoesNotMutateWorkspaceDefault() != 0)
    {
        return 1;
    }
    if (TestWorkspacePluginIsolation() != 0)
    {
        return 1;
    }
    if (TestHostPluginIsolation() != 0)
    {
        return 1;
    }
    if (TestHostPreferencesPersistence() != 0)
    {
        return 1;
    }
    if (TestScopeEnforcement() != 0)
    {
        return 1;
    }
    if (TestStagingRollbackOnFailedInit() != 0)
    {
        return 1;
    }
    if (TestWorkspaceLocalExtensionWithHostCallbacksRejected() != 0)
    {
        return 1;
    }
    if (g_failed)
    {
        return 1;
    }
    return 0;
}
