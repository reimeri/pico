#include "pico/host.h"
#include "pico/plugin.h"
#include "host_internal.h"
#include "workspace_internal.h"
#include "settings.h"
#include "agent_internal.h"
#include "agent.h"
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
        if (pico_workspace_open(host, other, &second) != PICO_OK || second == 0 || second == first ||
            pico_workspace_count(host) != 2)
        {
            Fail("a second live workspace should succeed");
            pico_host_free(host);
            unlink(alias);
            rmdir(other);
            rmdir(dir);
            return 1;
        }

        /* Test exhausting workspace limit (PICO_MAX_WORKSPACES = 8) */
        char extra_dirs[6][32];
        PicoWorkspaceId extra_ids[6];
        for (int i = 0; i < 6; i++)
        {
            snprintf(extra_dirs[i], sizeof(extra_dirs[i]), "/tmp/pico-ws-ext-%d-XXXXXX", i);
            if (!mkdtemp(extra_dirs[i]))
            {
                Fail("mkdtemp extra");
                return 1;
            }
            if (pico_workspace_open(host, extra_dirs[i], &extra_ids[i]) != PICO_OK)
            {
                Fail("open extra workspace up to limit");
            }
        }
        if (pico_workspace_count(host) != PICO_MAX_WORKSPACES)
        {
            Fail("workspace count should reach PICO_MAX_WORKSPACES");
        }

        char ninth[] = "/tmp/pico-ws-ninth-XXXXXX";
        PicoWorkspaceId ninth_id = 0;
        if (mkdtemp(ninth))
        {
            if (pico_workspace_open(host, ninth, &ninth_id) != PICO_LIMIT || ninth_id != 0)
            {
                Fail("opening ninth workspace should return PICO_LIMIT");
            }
            rmdir(ninth);
        }

        for (int i = 0; i < 6; i++)
        {
            rmdir(extra_dirs[i]);
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
        PicoWorkspace_RegistrationClear(host.workspaces[0]);
        free(host.workspaces[0]);
        return 1;
    }
    PicoWorkspace_RegistrationClear(host.workspaces[0]);
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

static void WriteFileStr(const char *path, const char *content)
{
    FILE *f = fopen(path, "wb");
    if (!f)
    {
        return;
    }
    fputs(content, f);
    fclose(f);
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
    PicoWorkspaceId id = 0;
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
    if (!PicoHost_ChangeWorkspace(host, PicoHost_PrimaryWorkspace(host), ws2))
    {
        Fail("request change workspace");
        FinishLifecycleHost(host, cfg, cache, ws);
        RmRf(ws2);
        return 1;
    }
    pico_host_pump(host);
    ReadFileStr(life, log, sizeof(log));
    if (log[0] != '\0')
    {
        Fail("cd must not shut down the previous workspace");
        FinishLifecycleHost(host, cfg, cache, ws);
        RmRf(ws2);
        return 1;
    }
    if (pico_workspace_count(host) != 2)
    {
        Fail("cd must leave both workspaces open");
        FinishLifecycleHost(host, cfg, cache, ws);
        RmRf(ws2);
        return 1;
    }
    pico_host_free(host);
    host = NULL;
    ReadFileStr(life, log, sizeof(log));
    FinishLifecycleHost(NULL, cfg, cache, ws);
    RmRf(ws2);
    if (strcmp(log, "YYH") != 0)
    {
        fprintf(stderr, "actual log: %s\n", log);
        Fail("workspace shutdown must run cleanly for both workspaces without use-after-free");
        return 1;
    }
    return 0;
}

static int TestCdOpensSelectsAndReusesWorkspace(void)
{
    PicoHost *host = NULL;
    char dirA[] = "/tmp/pico-cd-A-XXXXXX";
    char dirB[] = "/tmp/pico-cd-B-XXXXXX";
    PicoWorkspaceId idA = 0;
    PicoWorkspaceId idB = 0;
    PicoWorkspaceId reused = 0;
    PicoAgentCreateOptions opt;
    PicoAgentId agentA = 0;
    PicoAgent *selected;
    PicoWorkspace *wsA;
    PicoWorkspace *wsB;

    if (!mkdtemp(dirA) || !mkdtemp(dirB))
    {
        Fail("mkdtemp cd open/select");
        return 1;
    }
    if (pico_host_init(&host, NULL, true) != PICO_OK || !host)
    {
        Fail("host init cd open/select");
        rmdir(dirA);
        rmdir(dirB);
        return 1;
    }
    if (pico_workspace_open(host, dirA, &idA) != PICO_OK)
    {
        Fail("open A for cd");
        pico_host_free(host);
        rmdir(dirA);
        rmdir(dirB);
        return 1;
    }
    memset(&opt, 0, sizeof(opt));
    opt.kind = PICO_AGENT_MAIN;
    opt.session_start = PICO_SESSION_NONE;
    opt.select = true;
    if (pico_main_agent_create(host, idA, &opt, &agentA) != PICO_OK)
    {
        Fail("create main agent A for cd");
        pico_host_free(host);
        rmdir(dirA);
        rmdir(dirB);
        return 1;
    }
    host->chat_sel.msg = 9;
    host->chat_follow_bottom = false;
    host->hovered_tool = true;
    if (!PicoHost_ChangeWorkspace(host, PicoHost_FindWorkspace(host, idA), dirB))
    {
        Fail("cd to B");
        pico_host_free(host);
        rmdir(dirA);
        rmdir(dirB);
        return 1;
    }
    wsA = PicoHost_FindWorkspace(host, idA);
    selected = PicoHost_SelectedAgent(host);
    wsB = selected ? selected->workspace : NULL;
    if (!wsA || !wsB || wsA == wsB || pico_workspace_count(host) != 2 ||
        wsA->state != PICO_WORKSPACE_OPEN || !PicoWorkspace_AcceptsNewWork(wsA) ||
        !PicoHost_FindAgent(host, agentA) || selected->id == agentA)
    {
        Fail("cd must open B, select a main agent there, and leave A running");
        pico_host_free(host);
        rmdir(dirA);
        rmdir(dirB);
        return 1;
    }
    if (host->chat_sel.msg != -1 || !host->chat_follow_bottom || host->hovered_tool)
    {
        Fail("cd onto a newly created agent must reset transcript UI state");
        pico_host_free(host);
        rmdir(dirA);
        rmdir(dirB);
        return 1;
    }
    idB = wsB->id;
    if (!PicoHost_ChangeWorkspace(host, wsB, dirA) || pico_workspace_count(host) != 2 ||
        pico_workspace_open(host, dirA, &reused) != PICO_ALREADY_OPEN || reused != idA ||
        PicoHost_SelectedWorkspace(host) != wsA || pico_agent_active(host) != agentA ||
        PicoHost_FindWorkspace(host, idB) != wsB)
    {
        Fail("cd back to A must reuse the open workspace");
        pico_host_free(host);
        rmdir(dirA);
        rmdir(dirB);
        return 1;
    }
    pico_host_free(host);
    rmdir(dirA);
    rmdir(dirB);
    return 0;
}

static int TestReloadTargetsSelectedWorkspace(void)
{
    PicoHost *host = NULL;
    char dirA[] = "/tmp/pico-rl-A-XXXXXX";
    char dirB[] = "/tmp/pico-rl-B-XXXXXX";
    PicoWorkspaceId idA = 0;
    PicoWorkspaceId idB = 0;
    PicoAgentCreateOptions opt;
    PicoAgentId agentA = 0;
    PicoAgentId agentB = 0;
    PicoWorkspace *wsA;
    PicoWorkspace *wsB;

    if (!mkdtemp(dirA) || !mkdtemp(dirB))
    {
        Fail("mkdtemp reload selected");
        return 1;
    }
    if (pico_host_init(&host, NULL, true) != PICO_OK || !host)
    {
        Fail("host init reload selected");
        rmdir(dirA);
        rmdir(dirB);
        return 1;
    }
    if (pico_workspace_open(host, dirA, &idA) != PICO_OK || pico_workspace_open(host, dirB, &idB) != PICO_OK)
    {
        Fail("open workspaces for reload selected");
        pico_host_free(host);
        rmdir(dirA);
        rmdir(dirB);
        return 1;
    }
    memset(&opt, 0, sizeof(opt));
    opt.kind = PICO_AGENT_MAIN;
    opt.session_start = PICO_SESSION_NONE;
    opt.select = true;
    if (pico_main_agent_create(host, idA, &opt, &agentA) != PICO_OK)
    {
        Fail("create agent A for reload selected");
        pico_host_free(host);
        rmdir(dirA);
        rmdir(dirB);
        return 1;
    }
    opt.select = false;
    if (pico_main_agent_create(host, idB, &opt, &agentB) != PICO_OK)
    {
        Fail("create agent B for reload selected");
        pico_host_free(host);
        rmdir(dirA);
        rmdir(dirB);
        return 1;
    }
    if (!pico_agent_select(host, agentA))
    {
        Fail("select agent A for reload");
        pico_host_free(host);
        rmdir(dirA);
        rmdir(dirB);
        return 1;
    }
    PicoHost_RequestReload(host);
    wsA = PicoHost_FindWorkspace(host, idA);
    wsB = PicoHost_FindWorkspace(host, idB);
    if (!wsA || !wsB || wsA->state != PICO_WORKSPACE_RELOADING || PicoWorkspace_AcceptsNewWork(wsA) ||
        wsB->state != PICO_WORKSPACE_OPEN || !PicoWorkspace_AcceptsNewWork(wsB) || !host->reload_queued)
    {
        Fail("reload must target the selected workspace without pausing others");
        pico_host_free(host);
        rmdir(dirA);
        rmdir(dirB);
        return 1;
    }
    pico_host_pump(host);
    if (host->reload_queued || wsA->state != PICO_WORKSPACE_OPEN || !PicoWorkspace_AcceptsNewWork(wsA) ||
        wsB->state != PICO_WORKSPACE_OPEN || !PicoWorkspace_AcceptsNewWork(wsB))
    {
        Fail("selected workspace reload must not block the other workspace");
        pico_host_free(host);
        rmdir(dirA);
        rmdir(dirB);
        return 1;
    }
    pico_host_free(host);
    rmdir(dirA);
    rmdir(dirB);
    return 0;
}

static PicoAgentId g_cd_other_agent;

static void SelectOtherBeforeCd(PicoWorkspace *workspace, const PicoHookEvent *event, void *state)
{
    PicoHost *host = workspace ? workspace->host : NULL;
    (void)event;
    (void)state;
    if (host && g_cd_other_agent)
    {
        pico_agent_select(host, g_cd_other_agent);
    }
}

static bool PrependWorkspaceSubmitHook(PicoWorkspace *ws, PicoWorkspaceHookFn fn)
{
    PicoRegistrationGeneration *reg = ws ? ws->active_registration : NULL;
    if (!reg || reg->hook_count >= PICO_MAX_HOOKS)
    {
        return false;
    }
    memmove(&reg->hooks[1], &reg->hooks[0], (size_t)reg->hook_count * sizeof(reg->hooks[0]));
    memset(&reg->hooks[0], 0, sizeof(reg->hooks[0]));
    reg->hooks[0].hook = PICO_HOOK_BEFORE_SUBMIT;
    reg->hooks[0].workspace_fn = fn;
    reg->hooks[0].workspace = ws;
    reg->hook_count++;
    return true;
}

static int TestHostReloadIgnoresWorkspaceLocalCompileFailure(void)
{
    char cfg[256];
    char cache[256];
    char dirA[] = "/tmp/pico-hrl-A-XXXXXX";
    char dirB[] = "/tmp/pico-hrl-B-XXXXXX";
    char ext_dir[1024];
    char src[2048];
    PicoHost *host = NULL;
    PicoWorkspaceId idA = 0;
    PicoWorkspaceId idB = 0;

    snprintf(cfg, sizeof(cfg), "/tmp/pico-cfg-XXXXXX");
    snprintf(cache, sizeof(cache), "/tmp/pico-cache-XXXXXX");
    if (!mkdtemp(cfg) || !mkdtemp(cache) || !mkdtemp(dirA) || !mkdtemp(dirB))
    {
        Fail("mkdtemp host reload isolation");
        return 1;
    }
    setenv("XDG_CONFIG_HOME", cfg, 1);
    setenv("XDG_CACHE_HOME", cache, 1);
    if (pico_host_init(&host, NULL, false) != PICO_OK || !host)
    {
        Fail("host init host reload isolation");
        unsetenv("XDG_CONFIG_HOME");
        unsetenv("XDG_CACHE_HOME");
        RmRf(cfg);
        RmRf(cache);
        rmdir(dirA);
        rmdir(dirB);
        return 1;
    }
    if (pico_workspace_open(host, dirA, &idA) != PICO_OK || pico_workspace_open(host, dirB, &idB) != PICO_OK)
    {
        Fail("open workspaces host reload isolation");
        pico_host_free(host);
        unsetenv("XDG_CONFIG_HOME");
        unsetenv("XDG_CACHE_HOME");
        RmRf(cfg);
        RmRf(cache);
        rmdir(dirA);
        rmdir(dirB);
        return 1;
    }
    snprintf(ext_dir, sizeof(ext_dir), "%s/.pico/extensions", dirB);
    snprintf(src, sizeof(src), "%s/broken_ws.c", ext_dir);
    if (MkdirParents(ext_dir) != 0 || WriteFile(src, "this is not valid C {\n") != 0)
    {
        Fail("write broken workspace-local extension");
        pico_host_free(host);
        unsetenv("XDG_CONFIG_HOME");
        unsetenv("XDG_CACHE_HOME");
        RmRf(cfg);
        RmRf(cache);
        RmRf(dirA);
        RmRf(dirB);
        return 1;
    }
    if (!PicoPlugins_ReloadHost(host))
    {
        Fail("host reload must succeed when another workspace's local extension fails to compile");
        pico_host_free(host);
        unsetenv("XDG_CONFIG_HOME");
        unsetenv("XDG_CACHE_HOME");
        RmRf(cfg);
        RmRf(cache);
        RmRf(dirA);
        RmRf(dirB);
        return 1;
    }
    if (host->status_warn && strstr(host->status_warn, "broken_ws.c"))
    {
        Fail("host reload must not compile workspace-local sources");
        pico_host_free(host);
        unsetenv("XDG_CONFIG_HOME");
        unsetenv("XDG_CACHE_HOME");
        RmRf(cfg);
        RmRf(cache);
        RmRf(dirA);
        RmRf(dirB);
        return 1;
    }
    pico_host_free(host);
    unsetenv("XDG_CONFIG_HOME");
    unsetenv("XDG_CACHE_HOME");
    RmRf(cfg);
    RmRf(cache);
    RmRf(dirA);
    RmRf(dirB);
    return 0;
}

static int TestCdResolvesAgainstCommandWorkspace(void)
{
    PicoHost *host = NULL;
    char dirA[] = "/tmp/pico-cdrel-A-XXXXXX";
    char dirB[] = "/tmp/pico-cdrel-B-XXXXXX";
    char childA[4096];
    char childB[4096];
    PicoWorkspaceId idA = 0;
    PicoWorkspaceId idB = 0;
    PicoAgentCreateOptions opt;
    PicoAgentId agentA = 0;
    PicoAgentId agentB = 0;
    PicoWorkspace *wsA;
    PicoAgent *selected;
    const char *selected_path;

    if (!mkdtemp(dirA) || !mkdtemp(dirB))
    {
        Fail("mkdtemp cd relative");
        return 1;
    }
    snprintf(childA, sizeof(childA), "%s/child", dirA);
    snprintf(childB, sizeof(childB), "%s/child", dirB);
    if (mkdir(childA, 0700) != 0 || mkdir(childB, 0700) != 0)
    {
        Fail("mkdir cd relative children");
        rmdir(dirA);
        rmdir(dirB);
        return 1;
    }
    if (pico_host_init(&host, NULL, true) != PICO_OK || !host)
    {
        Fail("host init cd relative");
        rmdir(childA);
        rmdir(childB);
        rmdir(dirA);
        rmdir(dirB);
        return 1;
    }
    if (pico_workspace_open(host, dirA, &idA) != PICO_OK || pico_workspace_open(host, dirB, &idB) != PICO_OK)
    {
        Fail("open workspaces cd relative");
        pico_host_free(host);
        rmdir(childA);
        rmdir(childB);
        rmdir(dirA);
        rmdir(dirB);
        return 1;
    }
    memset(&opt, 0, sizeof(opt));
    opt.kind = PICO_AGENT_MAIN;
    opt.session_start = PICO_SESSION_NONE;
    opt.select = true;
    if (pico_main_agent_create(host, idA, &opt, &agentA) != PICO_OK)
    {
        Fail("create agent A cd relative");
        pico_host_free(host);
        rmdir(childA);
        rmdir(childB);
        rmdir(dirA);
        rmdir(dirB);
        return 1;
    }
    opt.select = false;
    if (pico_main_agent_create(host, idB, &opt, &agentB) != PICO_OK || !pico_agent_select(host, agentA))
    {
        Fail("create agent B cd relative");
        pico_host_free(host);
        rmdir(childA);
        rmdir(childB);
        rmdir(dirA);
        rmdir(dirB);
        return 1;
    }
    wsA = PicoHost_FindWorkspace(host, idA);
    g_cd_other_agent = agentB;
    if (!wsA || !PrependWorkspaceSubmitHook(wsA, SelectOtherBeforeCd))
    {
        Fail("prepend cd submit hook");
        pico_host_free(host);
        rmdir(childA);
        rmdir(childB);
        rmdir(dirA);
        rmdir(dirB);
        return 1;
    }
    PicoComposer_SetText(host, "/cd child");
    PicoHost_Submit(host);
    selected = PicoHost_SelectedAgent(host);
    selected_path = PicoWorkspace_Path(selected ? selected->workspace : NULL);
    if (!selected || strcmp(selected_path, childA) != 0)
    {
        Fail("/cd relative path must resolve against the command workspace after a selection change");
        pico_host_free(host);
        rmdir(childA);
        rmdir(childB);
        rmdir(dirA);
        rmdir(dirB);
        return 1;
    }
    pico_host_free(host);
    rmdir(childA);
    rmdir(childB);
    rmdir(dirA);
    rmdir(dirB);
    return 0;
}

static int TestCdRollsBackNewWorkspaceOnAgentLimit(void)
{
    PicoHost *host = NULL;
    char dirA[] = "/tmp/pico-cdlim-A-XXXXXX";
    char dirB[] = "/tmp/pico-cdlim-B-XXXXXX";
    char dirC[] = "/tmp/pico-cdlim-C-XXXXXX";
    PicoWorkspaceId idA = 0;
    PicoWorkspaceId idB = 0;
    PicoAgentCreateOptions opt;
    PicoAgentId idsA[PICO_MAX_AGENTS];
    PicoAgentId idsB[PICO_MAX_AGENTS];
    int i;
    int count_before;

    if (!mkdtemp(dirA) || !mkdtemp(dirB) || !mkdtemp(dirC))
    {
        Fail("mkdtemp cd rollback");
        return 1;
    }
    if (pico_host_init(&host, NULL, true) != PICO_OK || !host)
    {
        Fail("host init cd rollback");
        rmdir(dirA);
        rmdir(dirB);
        rmdir(dirC);
        return 1;
    }
    if (pico_workspace_open(host, dirA, &idA) != PICO_OK || pico_workspace_open(host, dirB, &idB) != PICO_OK)
    {
        Fail("open workspaces cd rollback");
        pico_host_free(host);
        rmdir(dirA);
        rmdir(dirB);
        rmdir(dirC);
        return 1;
    }
    memset(&opt, 0, sizeof(opt));
    opt.kind = PICO_AGENT_MAIN;
    opt.session_start = PICO_SESSION_NONE;
    for (i = 0; i < PICO_MAX_AGENTS; i++)
    {
        if (pico_main_agent_create(host, idA, &opt, &idsA[i]) != PICO_OK ||
            pico_main_agent_create(host, idB, &opt, &idsB[i]) != PICO_OK)
        {
            Fail("fill agents for cd rollback");
            pico_host_free(host);
            rmdir(dirA);
            rmdir(dirB);
            rmdir(dirC);
            return 1;
        }
    }
    count_before = pico_workspace_count(host);
    if (PicoHost_ChangeWorkspace(host, PicoHost_FindWorkspace(host, idA), dirC) ||
        pico_workspace_count(host) != count_before)
    {
        Fail("cd must roll back a newly opened workspace when agent creation fails");
        pico_host_free(host);
        rmdir(dirA);
        rmdir(dirB);
        rmdir(dirC);
        return 1;
    }
    pico_host_free(host);
    rmdir(dirA);
    rmdir(dirB);
    rmdir(dirC);
    return 0;
}

static int TestCdRejectsClosingWorkspace(void)
{
    PicoHost *host = NULL;
    char dirA[] = "/tmp/pico-cdcls-A-XXXXXX";
    char dirB[] = "/tmp/pico-cdcls-B-XXXXXX";
    PicoWorkspaceId idA = 0;
    PicoWorkspaceId idB = 0;
    PicoAgentCreateOptions opt;
    PicoAgentId agentA = 0;
    PicoAgentId agentB = 0;
    PicoWorkspace *wsB;
    PicoAgentId selected_before;

    if (!mkdtemp(dirA) || !mkdtemp(dirB))
    {
        Fail("mkdtemp cd closing");
        return 1;
    }
    if (pico_host_init(&host, NULL, true) != PICO_OK || !host)
    {
        Fail("host init cd closing");
        rmdir(dirA);
        rmdir(dirB);
        return 1;
    }
    if (pico_workspace_open(host, dirA, &idA) != PICO_OK || pico_workspace_open(host, dirB, &idB) != PICO_OK)
    {
        Fail("open workspaces cd closing");
        pico_host_free(host);
        rmdir(dirA);
        rmdir(dirB);
        return 1;
    }
    memset(&opt, 0, sizeof(opt));
    opt.kind = PICO_AGENT_MAIN;
    opt.session_start = PICO_SESSION_NONE;
    opt.select = true;
    if (pico_main_agent_create(host, idA, &opt, &agentA) != PICO_OK)
    {
        Fail("create agent A cd closing");
        pico_host_free(host);
        rmdir(dirA);
        rmdir(dirB);
        return 1;
    }
    opt.select = false;
    if (pico_main_agent_create(host, idB, &opt, &agentB) != PICO_OK)
    {
        Fail("create agent B cd closing");
        pico_host_free(host);
        rmdir(dirA);
        rmdir(dirB);
        return 1;
    }
    wsB = PicoHost_FindWorkspace(host, idB);
    if (!wsB || pico_workspace_request_close(host, idB) != PICO_OK || wsB->state != PICO_WORKSPACE_CLOSING)
    {
        Fail("close B for cd closing");
        pico_host_free(host);
        rmdir(dirA);
        rmdir(dirB);
        return 1;
    }
    selected_before = pico_agent_active(host);
    if (PicoHost_ChangeWorkspace(host, PicoHost_FindWorkspace(host, idA), dirB) ||
        pico_agent_active(host) != selected_before || pico_workspace_count(host) != 2)
    {
        Fail("cd must not select an agent in a closing workspace");
        pico_host_free(host);
        rmdir(dirA);
        rmdir(dirB);
        return 1;
    }
    pico_host_free(host);
    rmdir(dirA);
    rmdir(dirB);
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

    int ext_count = PicoPlugins_Count(host);
    int target_idx = -1;
    for (int i = 0; i < ext_count; i++)
    {
        PicoExtInfo info;
        if (PicoPlugins_Get(host, i, &info) && info.name && strcmp(info.name, "footer") == 0)
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

static int TestReloadInitRollbackPreservesActiveState(void)
{
    char cfg[256];
    char cache[256];
    char ws[256];
    char life[512];
    char log[32];
    PicoHost *host = NULL;

    cfg[0] = cache[0] = ws[0] = '\0';
    if (StartLifecycleHost(&host, cfg, cache, ws, life, 0) != 0)
    {
        FinishLifecycleHost(host, cfg, cache, ws);
        return 1;
    }
    void *host_state = PicoPlugins_HostState(host, "lifecycle");
    void *workspace_state = PicoPlugins_WorkspaceState(PicoHost_PrimaryWorkspace(host), "lifecycle");
    char source[512];
    snprintf(source, sizeof(source), "%s/pico/extensions/lifecycle.c", cfg);
    bool preserved = host_state && workspace_state;
    if (preserved && WriteFile(source, "this is not valid C\n") != 0)
    {
        preserved = false;
    }
    PicoPlugins_Reload(host);
    preserved = preserved && PicoPlugins_HostState(host, "lifecycle") == host_state &&
                PicoPlugins_WorkspaceState(PicoHost_PrimaryWorkspace(host), "lifecycle") == workspace_state;
    if (WriteFile(source, kLifecycleExt) != 0)
    {
        preserved = false;
    }
    setenv("PICO_TEST_FAIL_WORKSPACE", "1", 1);
    PicoPlugins_Reload(host);
    ReadFileStr(life, log, sizeof(log));
    preserved = preserved &&
                PicoPlugins_WorkspaceState(PicoHost_PrimaryWorkspace(host), "lifecycle") == workspace_state &&
                PicoPlugins_HostState(host, "lifecycle") != NULL;
    pico_host_free(host);
    host = NULL;
    FinishLifecycleHost(NULL, cfg, cache, ws);
    if (!preserved)
    {
        Fail("failed reload must preserve the previous initialized extension state");
        return 1;
    }
    return 0;
}

static int TestGenerationRolloutAndDlcloseOnRelease(void)
{
    PicoHost host;
    memset(&host, 0, sizeof(host));
    PicoHost_SetPath(&host, ".");
    PicoWorkspace *ws = PicoHost_PrimaryWorkspace(&host);

    PicoModuleGeneration mod_n;
    memset(&mod_n, 0, sizeof(mod_n));
    mod_n.ext.name = "test_gen_mod";
    mod_n.generation = 1;
    mod_n.desired = true;
    mod_n.ref_count = 1; /* module store */

    ws->workspace_plugin_count = 1;
    snprintf(ws->workspace_plugins[0].name, sizeof(ws->workspace_plugins[0].name), "%s", mod_n.ext.name);
    ws->workspace_plugins[0].module = &mod_n;
    ws->workspace_plugins[0].initialized = true;

    /* Publish registration generation N */
    if (!PicoWorkspace_PublishRegistrationGeneration(ws))
    {
        Fail("publish registration generation N failed");
        free(host.workspaces[0]);
        return 1;
    }

    PicoRegistrationGeneration *gen_n = PicoWorkspace_RegistrationActive(ws);
    if (!gen_n || mod_n.ref_count != 2) /* store + snapshot */
    {
        Fail("gen N should be active and ref_count should be 2");
        free(host.workspaces[0]);
        return 1;
    }

    /* Simulate a running turn worker retaining gen_n */
    PicoWorkspace_RegistrationRetain(gen_n);
    if (gen_n->ref_count != 2)
    {
        Fail("gen_n ref_count should be 2 (workspace active + turn worker)");
        free(host.workspaces[0]);
        return 1;
    }

    /* Now rollout generation N+1 */
    PicoModuleGeneration mod_n1;
    memset(&mod_n1, 0, sizeof(mod_n1));
    mod_n1.ext.name = "test_gen_mod";
    mod_n1.generation = 2;
    mod_n1.desired = true;
    mod_n1.ref_count = 1; /* module store */

    ws->workspace_plugins[0].module = &mod_n1;
    if (!PicoWorkspace_PublishRegistrationGeneration(ws))
    {
        Fail("publish registration generation N+1 failed");
        free(host.workspaces[0]);
        return 1;
    }

    PicoRegistrationGeneration *gen_n1 = PicoWorkspace_RegistrationActive(ws);
    if (!gen_n1 || gen_n1 == gen_n || mod_n1.ref_count != 2)
    {
        Fail("gen N+1 should be active with ref_count 2");
        free(host.workspaces[0]);
        return 1;
    }

    /* Old generation mod_n is no longer desired in store */
    mod_n.desired = false;
    PicoModule_Release(&mod_n); /* release store ref */

    /* mod_n is still referenced by turn worker's gen_n */
    if (mod_n.ref_count != 1)
    {
        Fail("mod_n should still have ref_count 1 from retained gen_n snapshot");
        free(host.workspaces[0]);
        return 1;
    }

    /* Now turn worker completes and releases gen_n */
    PicoWorkspace_RegistrationRelease(gen_n);

    /* mod_n should now have ref_count 0 */
    if (mod_n.ref_count != 0)
    {
        Fail("mod_n should have ref_count 0 after gen_n released");
        free(host.workspaces[0]);
        return 1;
    }

    PicoWorkspace_RegistrationClear(ws);
    free(host.workspaces[0]);
    return 0;
}

static int TestScopedExtensionListingRecords(void)
{
    char cfg[256];
    char cache[256];
    char ws1[256];
    char life[512];
    PicoHost *host = NULL;
    if (StartLifecycleHost(&host, cfg, cache, ws1, life, 0) != 0 || !host)
    {
        Fail("lifecycle host init failed");
        return 1;
    }

    char ws2[256];
    snprintf(ws2, sizeof(ws2), "/tmp/pico_test_ws2_%ld", (long)time(NULL));
    mkdir(ws2, 0755);
    PicoWorkspaceId ws2_id = 0;
    (void)pico_workspace_open(host, ws2, &ws2_id);

    int count = PicoPlugins_Count(host);
    if (count <= 0)
    {
        Fail("plugin count should be positive");
        FinishLifecycleHost(host, cfg, cache, ws1);
        rmdir(ws2);
        return 1;
    }

    bool found_host = false;
    bool found_ws = false;
    for (int i = 0; i < count; i++)
    {
        PicoExtInfo info;
        if (!PicoPlugins_Get(host, i, &info))
        {
            Fail("PicoPlugins_Get failed for valid index");
            FinishLifecycleHost(host, cfg, cache, ws1);
            rmdir(ws2);
            return 1;
        }
        if (info.scope == PICO_EXTENSION_HOST)
        {
            found_host = true;
            if (info.workspace_id != 0)
            {
                Fail("host-scoped plugin record must have workspace_id == 0");
                FinishLifecycleHost(host, cfg, cache, ws1);
                rmdir(ws2);
                return 1;
            }
        }
        else if (info.scope == PICO_EXTENSION_WORKSPACE)
        {
            found_ws = true;
            if (info.workspace_id == 0)
            {
                Fail("workspace-scoped plugin record must have non-zero workspace_id");
                FinishLifecycleHost(host, cfg, cache, ws1);
                rmdir(ws2);
                return 1;
            }
        }
    }

    if (!found_host || !found_ws)
    {
        Fail("scoped listing must report both host and workspace records");
        FinishLifecycleHost(host, cfg, cache, ws1);
        rmdir(ws2);
        return 1;
    }

    FinishLifecycleHost(host, cfg, cache, ws1);
    rmdir(ws2);
    return 0;
}

static int FailingWsInitDummy(PicoWorkspace *ws, void **state_out)
{
    (void)ws;
    (void)state_out;
    return -1;
}

static int SuccessfulHostInitDummy(PicoHost *host, void **state_out)
{
    (void)host;
    static int s_host_state = 42;
    *state_out = &s_host_state;
    return 0;
}

static int TestDualScopeIndependentPublicationRollback(void)
{
    PicoHost host;
    memset(&host, 0, sizeof(host));
    PicoHost_SetPath(&host, ".");
    PicoWorkspace *ws = PicoHost_PrimaryWorkspace(&host);

    PicoModuleGeneration mod;
    memset(&mod, 0, sizeof(mod));
    mod.ext.name = "dual_scope_ext";
    mod.generation = 1;
    mod.desired = true;
    mod.builtin = true;
    mod.ext.host_init = SuccessfulHostInitDummy;
    mod.ext.workspace_init = FailingWsInitDummy;

    bool host_ok = PicoHostExtensions_Activate(&host, &mod);
    bool ws_ok = PicoWorkspaceExtensions_Activate(ws, &mod);

    if (!host_ok || host.host_plugin_count != 1 || !host.host_plugins[0].initialized ||
        PicoHostExtensions_State(&host, "dual_scope_ext") == NULL)
    {
        Fail("host activation must succeed independently of workspace activation");
        free(host.workspaces[0]);
        return 1;
    }
    if (ws_ok || PicoWorkspaceExtensions_State(ws, "dual_scope_ext") != NULL)
    {
        Fail("workspace activation must fail and stay inactive without crashing");
        free(host.workspaces[0]);
        return 1;
    }

    /* Host instance is alive and working */
    if (host.host_plugin_count != 1 || !host.host_plugins[0].initialized)
    {
        Fail("host plugin slot must remain initialized");
        free(host.workspaces[0]);
        return 1;
    }

    PicoHostExtensions_Shutdown(&host);
    PicoWorkspaceExtensions_Shutdown(ws);
    free(host.workspaces[0]);
    return 0;
}

static int StatelessWsInitDummy(PicoWorkspace *ws, void **state_out)
{
    (void)ws;
    (void)state_out;
    return 0;
}

static int TestStatelessExtensionRollbackDoesNotLeakModule(void)
{
    PicoHost host;
    memset(&host, 0, sizeof(host));
    PicoHost_SetPath(&host, ".");
    PicoWorkspace *ws = PicoHost_PrimaryWorkspace(&host);

    /* Setup 2 candidate modules: mod1 is stateless (no shutdown callback), mod2 fails init */
    PicoModuleGeneration mod1;
    memset(&mod1, 0, sizeof(mod1));
    mod1.ext.name = "stateless_ext";
    mod1.generation = 1;
    mod1.desired = true;
    mod1.ext.workspace_init = StatelessWsInitDummy;
    mod1.ext.workspace_shutdown = NULL;
    mod1.ref_count = 1;

    PicoModuleGeneration mod2;
    memset(&mod2, 0, sizeof(mod2));
    mod2.ext.name = "failing_ext";
    mod2.generation = 1;
    mod2.desired = true;
    mod2.ext.workspace_init = FailingWsInitDummy;
    mod2.ref_count = 1;

    host.modules = (PicoModuleGeneration *)calloc(2, sizeof(PicoModuleGeneration));
    host.modules[0] = mod1;
    host.modules[1] = mod2;
    host.module_count = 2;
    host.module_capacity = 2;

    bool reload_ok = PicoWorkspace_Reload(ws);
    if (reload_ok)
    {
        Fail("workspace reload must fail when one module fails init");
        free(host.modules);
        free(host.workspaces[0]);
        return 1;
    }

    /* mod1 was activated during staging, but on rollback must be released! */
    if (host.modules[0].ref_count != 1)
    {
        Fail("stateless extension module must be released on staging rollback even without shutdown callback");
        free(host.modules);
        free(host.workspaces[0]);
        return 1;
    }

    free(host.modules);
    free(host.workspaces[0]);
    return 0;
}

static int TestExtensionToggleWhileBusyQueuesReloadAndKeepsAcceptingWork(void)
{
    PicoHost host;
    memset(&host, 0, sizeof(host));
    PicoHost_SetPath(&host, ".");
    PicoWorkspace *ws = PicoHost_PrimaryWorkspace(&host);

    /* Simulate workspace being busy */
    ws->accepting_work = true;
    ws->count = 1;
    ws->agents[0] = (PicoAgent *)calloc(1, sizeof(PicoAgent));
    ws->agents[0]->workspace = ws;
    ws->agents[0]->state = PICO_AGENT_TOOL_WAIT;

    bool ok = PicoWorkspace_Reload(ws);
    if (ok)
    {
        Fail("reload must not proceed while workspace is busy");
        free(ws->agents[0]);
        free(host.workspaces[0]);
        return 1;
    }

    if (!ws->reload_queued)
    {
        Fail("busy workspace must set reload_queued = true");
        free(ws->agents[0]);
        free(host.workspaces[0]);
        return 1;
    }

    if (!ws->accepting_work)
    {
        Fail("busy workspace must NOT have accepting_work permanently set to false");
        free(ws->agents[0]);
        free(host.workspaces[0]);
        return 1;
    }

    free(ws->agents[0]);
    free(host.workspaces[0]);
    return 0;
}

static int TestMultiWorkspaceInstructionsIsolation(void)
{
    char dirA[] = "/tmp/pico-ws-instA-XXXXXX";
    char dirB[] = "/tmp/pico-ws-instB-XXXXXX";
    if (!mkdtemp(dirA) || !mkdtemp(dirB))
    {
        Fail("mkdtemp instructions test");
        return 1;
    }

    char fileA[4096];
    char fileB[4096];
    snprintf(fileA, sizeof(fileA), "%s/AGENTS.md", dirA);
    snprintf(fileB, sizeof(fileB), "%s/AGENTS.md", dirB);
    FILE *fA = fopen(fileA, "wb");
    if (fA) { fputs("INSTRUCTION_ALPHA", fA); fclose(fA); }
    FILE *fB = fopen(fileB, "wb");
    if (fB) { fputs("INSTRUCTION_BETA", fB); fclose(fB); }

    char picoA[4096];
    char picoB[4096];
    snprintf(picoA, sizeof(picoA), "%s/.pico", dirA);
    snprintf(picoB, sizeof(picoB), "%s/.pico", dirB);
    mkdir(picoA, 0755);
    mkdir(picoB, 0755);

    char sysA[4096];
    char sysB[4096];
    snprintf(sysA, sizeof(sysA), "%s/.pico/SYSTEM.md", dirA);
    snprintf(sysB, sizeof(sysB), "%s/.pico/SYSTEM.md", dirB);
    fA = fopen(sysA, "wb");
    if (fA) { fputs("SYSTEM_ALPHA", fA); fclose(fA); }
    fB = fopen(sysB, "wb");
    if (fB) { fputs("SYSTEM_BETA", fB); fclose(fB); }

    PicoHost *host = NULL;
    if (pico_host_init(&host, NULL, true) != PICO_OK || !host)
    {
        Fail("host init instructions");
        return 1;
    }

    PicoWorkspaceId idA = 0, idB = 0;
    if (pico_workspace_open(host, dirA, &idA) != PICO_OK ||
        pico_workspace_open(host, dirB, &idB) != PICO_OK)
    {
        Fail("open workspaces for instructions");
        pico_host_free(host);
        return 1;
    }

    PicoAgentCreateOptions opt = { .kind = PICO_AGENT_MAIN, .session_start = PICO_SESSION_NONE };
    PicoAgentId agA = 0, agB = 0;
    if (pico_main_agent_create(host, idA, &opt, &agA) != PICO_OK ||
        pico_main_agent_create(host, idB, &opt, &agB) != PICO_OK)
    {
        Fail("create agents for instructions");
        pico_host_free(host);
        return 1;
    }

    PicoAgent *agentA = PicoHost_FindAgent(host, agA);
    PicoAgent *agentB = PicoHost_FindAgent(host, agB);
    char *instA = PicoAgent_BuildInstructions(host, agentA);
    char *instB = PicoAgent_BuildInstructions(host, agentB);

    if (!instA || strstr(instA, "INSTRUCTION_ALPHA") == NULL || strstr(instA, "SYSTEM_ALPHA") == NULL ||
        strstr(instA, "INSTRUCTION_BETA") != NULL || strstr(instA, "SYSTEM_BETA") != NULL)
    {
        Fail("agent A instructions should only contain workspace A instructions");
    }
    if (!instB || strstr(instB, "INSTRUCTION_BETA") == NULL || strstr(instB, "SYSTEM_BETA") == NULL ||
        strstr(instB, "INSTRUCTION_ALPHA") != NULL || strstr(instB, "SYSTEM_ALPHA") != NULL)
    {
        Fail("agent B instructions should only contain workspace B instructions");
    }

    free(instA);
    free(instB);
    pico_host_free(host);

    unlink(fileA);
    unlink(fileB);
    unlink(sysA);
    unlink(sysB);
    rmdir(picoA);
    rmdir(picoB);
    rmdir(dirA);
    rmdir(dirB);
    return 0;
}

static void RunToolA(PicoAgentContext *ctx, const char *args, PicoToolResult *out, void *state)
{
    (void)ctx; (void)args; (void)state;
    out->output = strdup("TOOL_OUTPUT_ALPHA");
    out->is_error = false;
}

static void RunToolB(PicoAgentContext *ctx, const char *args, PicoToolResult *out, void *state)
{
    (void)ctx; (void)args; (void)state;
    out->output = strdup("TOOL_OUTPUT_BETA");
    out->is_error = false;
}

static int TestMultiWorkspaceToolNameIsolation(void)
{
    PicoHost *host = NULL;
    if (pico_host_init(&host, NULL, true) != PICO_OK || !host)
    {
        Fail("host init tool isolation");
        return 1;
    }

    char dirA[] = "/tmp/pico-ws-toolA-XXXXXX";
    char dirB[] = "/tmp/pico-ws-toolB-XXXXXX";
    if (!mkdtemp(dirA) || !mkdtemp(dirB))
    {
        Fail("mkdtemp tool isolation");
        return 1;
    }

    PicoWorkspaceId idA = 0, idB = 0;
    pico_workspace_open(host, dirA, &idA);
    pico_workspace_open(host, dirB, &idB);
    PicoWorkspace *wsA = PicoHost_FindWorkspace(host, idA);
    PicoWorkspace *wsB = PicoHost_FindWorkspace(host, idB);

    PicoHost_BeginRegistration(host, PICO_REG_WORKSPACE, wsA);
    pico_add_tool(wsA, "custom_worker_tool", "desc A", "{}", RunToolA, NULL);
    PicoHost_PublishRegistration(host, NULL);

    PicoHost_BeginRegistration(host, PICO_REG_WORKSPACE, wsB);
    pico_add_tool(wsB, "custom_worker_tool", "desc B", "{}", RunToolB, NULL);
    PicoHost_PublishRegistration(host, NULL);

    int idxA = -1, idxB = -1;
    for (int i = 0; i < wsA->tool_count; i++)
    {
        if (wsA->tools[i].name && strcmp(wsA->tools[i].name, "custom_worker_tool") == 0)
        {
            idxA = i;
            break;
        }
    }
    for (int i = 0; i < wsB->tool_count; i++)
    {
        if (wsB->tools[i].name && strcmp(wsB->tools[i].name, "custom_worker_tool") == 0)
        {
            idxB = i;
            break;
        }
    }

    if (idxA < 0 || idxB < 0)
    {
        Fail("workspace tool registrations must register in both workspaces");
        pico_host_free(host);
        return 1;
    }

    PicoToolResult resA = {0}, resB = {0};
    wsA->tools[idxA].run(NULL, "{}", &resA, wsA->tools[idxA].state);
    wsB->tools[idxB].run(NULL, "{}", &resB, wsB->tools[idxB].state);

    if (!resA.output || strcmp(resA.output, "TOOL_OUTPUT_ALPHA") != 0 ||
        !resB.output || strcmp(resB.output, "TOOL_OUTPUT_BETA") != 0)
    {
        Fail("executing tool with same name in workspace A and B must execute distinct implementations");
    }

    free(resA.output);
    free(resB.output);
    pico_host_free(host);
    rmdir(dirA);
    rmdir(dirB);
    return 0;
}

static int TestMultiWorkspaceMailboxIsolation(void)
{
    PicoHost *host = NULL;
    if (pico_host_init(&host, NULL, true) != PICO_OK || !host)
    {
        Fail("host init mailbox isolation");
        return 1;
    }

    char dirA[] = "/tmp/pico-ws-mbA-XXXXXX";
    char dirB[] = "/tmp/pico-ws-mbB-XXXXXX";
    if (!mkdtemp(dirA) || !mkdtemp(dirB))
    {
        Fail("mkdtemp mailbox isolation");
        return 1;
    }

    PicoWorkspaceId idA = 0, idB = 0;
    pico_workspace_open(host, dirA, &idA);
    pico_workspace_open(host, dirB, &idB);
    PicoWorkspace *wsA = PicoHost_FindWorkspace(host, idA);
    PicoWorkspace *wsB = PicoHost_FindWorkspace(host, idB);

    PicoAgentCreateOptions opt = { .kind = PICO_AGENT_MAIN, .session_start = PICO_SESSION_NONE };
    PicoAgentId a1 = 0, a2 = 0, b1 = 0;
    pico_main_agent_create(host, idA, &opt, &a1);
    pico_main_agent_create(host, idA, &opt, &a2);
    pico_main_agent_create(host, idB, &opt, &b1);

    PicoAgent *agentA1 = PicoHost_FindAgent(host, a1);
    PicoAgent *agentA2 = PicoHost_FindAgent(host, a2);
    PicoAgent *agentB1 = PicoHost_FindAgent(host, b1);

    PicoWorkspace_UiPost(wsA, "status_box", PICO_UI_POST_TEXT, a1, agentA1->runtime_generation, "A1_POST", 7);
    PicoWorkspace_UiPost(wsA, "status_box", PICO_UI_POST_TEXT, a2, agentA2->runtime_generation, "A2_POST", 7);
    PicoWorkspace_UiPost(wsB, "status_box", PICO_UI_POST_TEXT, b1, agentB1->runtime_generation, "B1_POST", 7);

    PicoWorkspace_PumpUiPosts(wsA);
    PicoWorkspace_PumpUiPosts(wsB);

    PicoUiPost p1 = {0}, p2 = {0}, p3 = {0};
    if (!pico_agent_ui_latest(host, a1, "status_box", &p1) || !p1.text || strcmp(p1.text, "A1_POST") != 0 ||
        !pico_agent_ui_latest(host, a2, "status_box", &p2) || !p2.text || strcmp(p2.text, "A2_POST") != 0 ||
        !pico_agent_ui_latest(host, b1, "status_box", &p3) || !p3.text || strcmp(p3.text, "B1_POST") != 0)
    {
        Fail("mailbox posts with the same name across agents and workspaces must remain completely isolated");
    }

    /* Stale/zero ID must not match or fall back to selection */
    PicoUiPost p_invalid = {0};
    if (pico_agent_ui_latest(host, 0, "status_box", &p_invalid) ||
        pico_agent_ui_latest(host, 9999, "status_box", &p_invalid))
    {
        Fail("lookup on zero or stale agent ID must return false");
    }

    pico_agent_ui_clear(host, a1, "status_box");
    PicoUiPost p1_cleared = {0};
    if (pico_agent_ui_latest(host, a1, "status_box", &p1_cleared))
    {
        Fail("clearing agent a1 mailbox should make it not found");
    }
    if (!pico_agent_ui_latest(host, a2, "status_box", &p2) || strcmp(p2.text, "A2_POST") != 0)
    {
        Fail("clearing a1 mailbox must not clear a2 mailbox");
    }
    if (!pico_agent_ui_latest(host, b1, "status_box", &p3) || strcmp(p3.text, "B1_POST") != 0)
    {
        Fail("clearing a1 mailbox must not clear b1 mailbox");
    }

    pico_host_free(host);
    rmdir(dirA);
    rmdir(dirB);
    return 0;
}

static int TestMultiWorkspaceAskOrderingAndRouting(void)
{
    PicoHost *host = NULL;
    if (pico_host_init(&host, NULL, true) != PICO_OK || !host)
    {
        Fail("host init ask ordering");
        return 1;
    }

    char dirA[] = "/tmp/pico-ws-askA-XXXXXX";
    char dirB[] = "/tmp/pico-ws-askB-XXXXXX";
    if (!mkdtemp(dirA) || !mkdtemp(dirB))
    {
        Fail("mkdtemp ask ordering");
        return 1;
    }

    PicoWorkspaceId idA = 0, idB = 0;
    pico_workspace_open(host, dirA, &idA);
    pico_workspace_open(host, dirB, &idB);

    PicoAgentCreateOptions opt = { .kind = PICO_AGENT_MAIN, .session_start = PICO_SESSION_NONE };
    PicoAgentId a1 = 0, b1 = 0;
    pico_main_agent_create(host, idA, &opt, &a1);
    pico_main_agent_create(host, idB, &opt, &b1);

    PicoToolAsk pending;
    if (pico_tool_pending_ask(host, &pending))
    {
        Fail("pending ask should be false when no asks are active");
    }

    if (pico_tool_answer(host, 0, "{}") || pico_tool_answer(host, 9999, "{}"))
    {
        Fail("answering invalid/stale ask ID must return false");
    }

    pico_host_free(host);
    rmdir(dirA);
    rmdir(dirB);
    return 0;
}

static int TestMultiWorkspaceReloadAndCloseIsolation(void)
{
    PicoHost *host = NULL;
    if (pico_host_init(&host, NULL, true) != PICO_OK || !host)
    {
        Fail("host init reload close isolation");
        return 1;
    }

    char dirA[] = "/tmp/pico-ws-rcA-XXXXXX";
    char dirB[] = "/tmp/pico-ws-rcB-XXXXXX";
    if (!mkdtemp(dirA) || !mkdtemp(dirB))
    {
        Fail("mkdtemp reload close isolation");
        return 1;
    }

    PicoWorkspaceId idA = 0, idB = 0;
    pico_workspace_open(host, dirA, &idA);
    pico_workspace_open(host, dirB, &idB);
    PicoWorkspace *wsA = PicoHost_FindWorkspace(host, idA);
    PicoWorkspace *wsB = PicoHost_FindWorkspace(host, idB);

    PicoAgentCreateOptions opt = { .kind = PICO_AGENT_MAIN, .session_start = PICO_SESSION_NONE };
    PicoAgentId a1 = 0, b1 = 0;
    pico_main_agent_create(host, idA, &opt, &a1);
    pico_main_agent_create(host, idB, &opt, &b1);

    /* Request reload on A */
    if (pico_workspace_request_reload(host, idA) != PICO_OK || wsA->state != PICO_WORKSPACE_RELOADING)
    {
        Fail("workspace A should enter RELOADING");
    }
    if (PicoWorkspace_AcceptsNewWork(wsA))
    {
        Fail("workspace A in RELOADING should reject new work");
    }
    if (!PicoWorkspace_AcceptsNewWork(wsB) || wsB->state != PICO_WORKSPACE_OPEN)
    {
        Fail("workspace B should remain OPEN and accepting work while A is reloading");
    }

    /* Request close on A while reloading */
    if (pico_workspace_request_close(host, idA) != PICO_OK || wsA->state != PICO_WORKSPACE_CLOSING)
    {
        Fail("workspace A should enter CLOSING");
    }

    /* Pump host - A is quiescent so it should close and be removed */
    pico_host_pump(host);

    if (PicoHost_FindWorkspace(host, idA) != NULL || pico_workspace_count(host) != 1)
    {
        Fail("workspace A should be destroyed and removed after quiescence");
    }
    if (PicoHost_FindWorkspace(host, idB) == NULL || wsB->state != PICO_WORKSPACE_OPEN)
    {
        Fail("workspace B should continue running normally");
    }

    pico_host_free(host);
    rmdir(dirA);
    rmdir(dirB);
    return 0;
}

static int TestMultiWorkspaceStuckWorkerIsolation(void)
{
    PicoHost *host = NULL;
    if (pico_host_init(&host, NULL, true) != PICO_OK || !host)
    {
        Fail("host init stuck worker isolation");
        return 1;
    }

    char dirA[] = "/tmp/pico-ws-stuckA-XXXXXX";
    char dirB[] = "/tmp/pico-ws-stuckB-XXXXXX";
    if (!mkdtemp(dirA) || !mkdtemp(dirB))
    {
        Fail("mkdtemp stuck worker isolation");
        return 1;
    }

    PicoWorkspaceId idA = 0, idB = 0;
    pico_workspace_open(host, dirA, &idA);
    pico_workspace_open(host, dirB, &idB);
    PicoWorkspace *wsA = PicoHost_FindWorkspace(host, idA);
    PicoWorkspace *wsB = PicoHost_FindWorkspace(host, idB);

    PicoAgentCreateOptions opt = { .kind = PICO_AGENT_MAIN, .session_start = PICO_SESSION_NONE };
    PicoAgentId a1 = 0, b1 = 0;
    pico_main_agent_create(host, idA, &opt, &a1);
    pico_main_agent_create(host, idB, &opt, &b1);

    PicoAgent *agentA = PicoHost_FindAgent(host, a1);

    /* Start turn in workspace A */
    PicoAgent_StartTurn(host, agentA, "turn in A");

    /* Request close on A while turn is active */
    pico_workspace_request_close(host, idA);
    if (wsA->state != PICO_WORKSPACE_CLOSING)
    {
        Fail("workspace A should enter CLOSING");
    }

    /* Pump host while worker is busy */
    pico_host_pump(host);

    if (PicoHost_FindWorkspace(host, idA) == NULL || wsA->state != PICO_WORKSPACE_CLOSING)
    {
        Fail("workspace A with busy turn must remain in CLOSING without being freed prematurely");
    }
    if (PicoHost_FindWorkspace(host, idB) == NULL || wsB->state != PICO_WORKSPACE_OPEN)
    {
        Fail("workspace B must continue operating normally while A is in CLOSING");
    }

    /* Cancel agent A and pump until quiescence */
    pico_agent_cancel(host, a1);
    for (int i = 0; i < 50 && PicoAgent_IsBusy(agentA); i++)
    {
        pico_host_pump(host);
        usleep(5000);
    }

    pico_host_pump(host);

    if (PicoHost_FindWorkspace(host, idA) != NULL || pico_workspace_count(host) != 1)
    {
        Fail("workspace A should cleanly close and be removed after worker finishes");
    }

    pico_host_free(host);
    rmdir(dirA);
    rmdir(dirB);
    return 0;
}

static int TestMultiWorkspaceMainAgentDelegationDrain(void)
{
    PicoHost *host = NULL;
    if (pico_host_init(&host, NULL, true) != PICO_OK || !host)
    {
        Fail("host init delegation drain test");
        return 1;
    }

    char dirA[] = "/tmp/pico-ws-delgA-XXXXXX";
    if (!mkdtemp(dirA))
    {
        Fail("mkdtemp delegation drain");
        return 1;
    }

    PicoWorkspaceId idA = 0;
    pico_workspace_open(host, dirA, &idA);
    PicoWorkspace *wsA = PicoHost_FindWorkspace(host, idA);

    PicoAgentCreateOptions opt = { .kind = PICO_AGENT_MAIN, .session_start = PICO_SESSION_NONE };
    PicoAgentId main1 = 0, main2 = 0;
    pico_main_agent_create(host, idA, &opt, &main1);
    pico_main_agent_create(host, idA, &opt, &main2);

    /* Create a child subagent descended from main1 */
    PicoAgentCreateOptions child_opt = {
        .kind = PICO_AGENT_SUBAGENT,
        .parent_id = main1,
        .session_start = PICO_SESSION_NONE,
    };
    PicoAgentId sub1 = 0;
    if (pico_agent_create(host, &child_opt, &sub1) != PICO_AGENT_RESULT_OK || sub1 == 0)
    {
        Fail("subagent creation under main1 should succeed");
    }

    if (wsA->count != 3)
    {
        Fail("workspace should have 3 agents (main1, main2, sub1)");
    }

    /* Close main1: child tree is cancelled and drained, sub1 and main1 destroyed, main2 survives */
    PicoAgentResult res = pico_agent_close(host, main1);
    if (res != PICO_AGENT_RESULT_OK)
    {
        Fail("closing main1 should cancel/drain subagent and destroy main1");
    }

    if (PicoHost_FindAgent(host, main1) != NULL || PicoHost_FindAgent(host, sub1) != NULL)
    {
        Fail("main1 and sub1 should be destroyed");
    }
    if (PicoHost_FindAgent(host, main2) == NULL || wsA->count != 1)
    {
        Fail("main2 in workspace A should survive unharmed");
    }

    pico_host_free(host);
    rmdir(dirA);
    return 0;
}

static int TestMultiWorkspaceModelAndSettingsIsolation(void)
{
    PicoHost *host = NULL;
    if (pico_host_init(&host, NULL, true) != PICO_OK || !host)
    {
        Fail("host init model isolation test");
        return 1;
    }

    char dirA[] = "/tmp/pico-ws-modA-XXXXXX";
    char dirB[] = "/tmp/pico-ws-modB-XXXXXX";
    if (!mkdtemp(dirA) || !mkdtemp(dirB))
    {
        Fail("mkdtemp model isolation");
        return 1;
    }

    PicoWorkspaceId idA = 0, idB = 0;
    pico_workspace_open(host, dirA, &idA);
    pico_workspace_open(host, dirB, &idB);
    PicoWorkspace *wsA = PicoHost_FindWorkspace(host, idA);
    PicoWorkspace *wsB = PicoHost_FindWorkspace(host, idB);

    PicoModel modelsA[2];
    memset(modelsA, 0, sizeof(modelsA));
    snprintf(modelsA[0].id, sizeof(modelsA[0].id), "model-alpha");
    snprintf(modelsA[0].name, sizeof(modelsA[0].name), "model-alpha");
    snprintf(modelsA[0].default_effort, sizeof(modelsA[0].default_effort), "low");
    snprintf(modelsA[0].effort[0], sizeof(modelsA[0].effort[0]), "low");
    modelsA[0].effort_count = 1;
    snprintf(modelsA[1].id, sizeof(modelsA[1].id), "model-shared");
    snprintf(modelsA[1].name, sizeof(modelsA[1].name), "model-shared");
    wsA->models = modelsA;
    wsA->model_count = 2;

    PicoModel modelsB[2];
    memset(modelsB, 0, sizeof(modelsB));
    snprintf(modelsB[0].id, sizeof(modelsB[0].id), "model-beta");
    snprintf(modelsB[0].name, sizeof(modelsB[0].name), "model-beta");
    snprintf(modelsB[0].default_effort, sizeof(modelsB[0].default_effort), "high");
    snprintf(modelsB[0].effort[0], sizeof(modelsB[0].effort[0]), "high");
    modelsB[0].effort_count = 1;
    snprintf(modelsB[1].id, sizeof(modelsB[1].id), "model-shared");
    snprintf(modelsB[1].name, sizeof(modelsB[1].name), "model-shared");
    wsB->models = modelsB;
    wsB->model_count = 2;

    PicoAgentCreateOptions opt = { .kind = PICO_AGENT_MAIN, .session_start = PICO_SESSION_NONE };
    PicoAgentId a1 = 0, b1 = 0;
    pico_main_agent_create(host, idA, &opt, &a1);
    pico_main_agent_create(host, idB, &opt, &b1);

    PicoAgent *agentA = PicoHost_FindAgent(host, a1);
    PicoAgent *agentB = PicoHost_FindAgent(host, b1);

    PicoSettings_SetModel(agentA, "model-alpha");
    PicoSettings_SetModel(agentB, "model-beta");

    if (strcmp(agentA->model, "model-alpha") != 0 ||
        strcmp(agentB->model, "model-beta") != 0)
    {
        Fail("workspaces must maintain isolated agent model catalogs and assignments");
    }

    /* Model alpha in workspace A must not be visible or settable in workspace B */
    if (PicoSettings_SetModel(agentB, "model-alpha"))
    {
        Fail("workspace B should reject models that only exist in workspace A");
    }

    wsA->models = NULL;
    wsA->model_count = 0;
    wsB->models = NULL;
    wsB->model_count = 0;

    pico_host_free(host);
    rmdir(dirA);
    rmdir(dirB);
    return 0;
}

static int TestMultiWorkspaceFrameCallbacks(void)
{
    PicoHost *host = NULL;
    if (pico_host_init(&host, NULL, true) != PICO_OK || !host)
    {
        Fail("host init frame callbacks");
        return 1;
    }

    char dirA[] = "/tmp/pico-ws-fcA-XXXXXX";
    char dirB[] = "/tmp/pico-ws-fcB-XXXXXX";
    if (!mkdtemp(dirA) || !mkdtemp(dirB))
    {
        Fail("mkdtemp frame callbacks");
        return 1;
    }

    PicoWorkspaceId idA = 0, idB = 0;
    pico_workspace_open(host, dirA, &idA);
    pico_workspace_open(host, dirB, &idB);

    /* Pump host three times */
    pico_host_pump(host);
    pico_host_pump(host);
    pico_host_pump(host);

    pico_host_free(host);
    rmdir(dirA);
    rmdir(dirB);
    return 0;
}

static int TestMultiWorkspaceCloseLastMainAgentAndZeroAgents(void)
{
    PicoHost *host = NULL;
    if (pico_host_init(&host, NULL, true) != PICO_OK || !host)
    {
        Fail("host init zero agent test");
        return 1;
    }

    char dirA[] = "/tmp/pico-ws-zeroA-XXXXXX";
    if (!mkdtemp(dirA))
    {
        Fail("mkdtemp zero agent test");
        return 1;
    }

    PicoWorkspaceId idA = 0;
    pico_workspace_open(host, dirA, &idA);
    PicoWorkspace *wsA = PicoHost_FindWorkspace(host, idA);

    PicoAgentCreateOptions opt = { .kind = PICO_AGENT_MAIN, .session_start = PICO_SESSION_NONE };
    PicoAgentId a1 = 0;
    pico_main_agent_create(host, idA, &opt, &a1);

    if (wsA->count != 1)
    {
        Fail("workspace should have 1 agent");
    }

    /* Closing the only/last main agent in workspace */
    if (pico_agent_close(host, a1) != PICO_AGENT_RESULT_OK)
    {
        Fail("pico_agent_close on last agent should succeed");
    }
    if (wsA->count != 0 || wsA->state != PICO_WORKSPACE_OPEN || pico_workspace_count(host) != 1)
    {
        Fail("closing last main agent should leave workspace open with 0 agents");
    }

    /* Creating a new main agent in the 0-agent workspace succeeds */
    PicoAgentId a2 = 0;
    if (pico_main_agent_create(host, idA, &opt, &a2) != PICO_OK || a2 == 0 || wsA->count != 1)
    {
        Fail("creating new main agent in 0-agent workspace should succeed");
    }

    pico_host_free(host);
    rmdir(dirA);
    return 0;
}

static int TestMultiWorkspaceAgentLimits(void)
{
    PicoHost *host = NULL;
    if (pico_host_init(&host, NULL, true) != PICO_OK || !host)
    {
        Fail("host init limits test");
        return 1;
    }

    char dirA[] = "/tmp/pico-ws-limA-XXXXXX";
    char dirB[] = "/tmp/pico-ws-limB-XXXXXX";
    if (!mkdtemp(dirA) || !mkdtemp(dirB))
    {
        Fail("mkdtemp limits test");
        return 1;
    }

    PicoWorkspaceId idA = 0, idB = 0;
    pico_workspace_open(host, dirA, &idA);
    pico_workspace_open(host, dirB, &idB);

    PicoAgentCreateOptions opt = { .kind = PICO_AGENT_MAIN, .session_start = PICO_SESSION_NONE };

    /* Fill workspace A up to PICO_MAX_AGENTS (16) */
    PicoAgentId idsA[16];
    for (int i = 0; i < 16; i++)
    {
        if (pico_main_agent_create(host, idA, &opt, &idsA[i]) != PICO_OK)
        {
            Fail("create agent in A up to 16");
            pico_host_free(host);
            return 1;
        }
    }
    PicoAgentId overflowA = 0;
    if (pico_main_agent_create(host, idA, &opt, &overflowA) != PICO_LIMIT || overflowA != 0)
    {
        Fail("17th agent in workspace A should return PICO_LIMIT");
    }

    /* Fill workspace B with 16 agents (total agents in host = 32) */
    PicoAgentId idsB[16];
    for (int i = 0; i < 16; i++)
    {
        if (pico_main_agent_create(host, idB, &opt, &idsB[i]) != PICO_OK)
        {
            Fail("create agent in B up to 16");
            pico_host_free(host);
            return 1;
        }
    }
    if (PicoHost_TotalAgentCount(host) != PICO_MAX_TOTAL_AGENTS)
    {
        Fail("total agents in host should reach PICO_MAX_TOTAL_AGENTS (32)");
    }

    /* 33rd agent in host should return PICO_LIMIT */
    PicoAgentId overflowB = 0;
    if (pico_main_agent_create(host, idB, &opt, &overflowB) != PICO_LIMIT || overflowB != 0)
    {
        Fail("exceeding PICO_MAX_TOTAL_AGENTS should return PICO_LIMIT");
    }

    pico_host_free(host);
    rmdir(dirA);
    rmdir(dirB);
    return 0;
}

static int TestMultiWorkspaceStaleIds(void)
{
    PicoHost *host = NULL;
    if (pico_host_init(&host, NULL, true) != PICO_OK || !host)
    {
        Fail("host init stale ids test");
        return 1;
    }

    char dirA[] = "/tmp/pico-ws-staleA-XXXXXX";
    if (!mkdtemp(dirA))
    {
        Fail("mkdtemp stale ids test");
        return 1;
    }

    PicoWorkspaceId idA = 0;
    pico_workspace_open(host, dirA, &idA);

    PicoAgentCreateOptions opt = { .kind = PICO_AGENT_MAIN, .session_start = PICO_SESSION_NONE };
    PicoAgentId a1 = 0;
    pico_main_agent_create(host, idA, &opt, &a1);

    /* Close agent a1 */
    pico_agent_close(host, a1);

    /* Stale agent ID operations should return not found / invalid */
    PicoAgentInfo info;
    if (pico_agent_find(host, a1, &info))
    {
        Fail("stale agent id find should return false");
    }
    if (pico_agent_submit(host, a1, "hello", NULL) != PICO_NOT_FOUND)
    {
        Fail("stale agent submit should return PICO_NOT_FOUND");
    }
    if (pico_agent_cancel(host, a1) != PICO_AGENT_RESULT_NOT_FOUND)
    {
        Fail("stale agent cancel should return PICO_AGENT_RESULT_NOT_FOUND");
    }
    if (pico_agent_close(host, a1) != PICO_AGENT_RESULT_NOT_FOUND)
    {
        Fail("stale agent close should return PICO_AGENT_RESULT_NOT_FOUND");
    }

    /* Creating a new agent allocates a new unique ID != stale a1 */
    PicoAgentId a2 = 0;
    pico_main_agent_create(host, idA, &opt, &a2);
    if (a2 == a1 || a2 == 0)
    {
        Fail("new agent id must be monotonically unique and not reuse stale id");
    }

    pico_host_free(host);
    rmdir(dirA);
    return 0;
}

static int TestMultiWorkspaceFairPumping(void)
{
    PicoHost *host = NULL;
    if (pico_host_init(&host, NULL, true) != PICO_OK || !host)
    {
        Fail("host init fair pumping test");
        return 1;
    }

    char dirA[] = "/tmp/pico-ws-fairA-XXXXXX";
    char dirB[] = "/tmp/pico-ws-fairB-XXXXXX";
    char dirC[] = "/tmp/pico-ws-fairC-XXXXXX";
    if (!mkdtemp(dirA) || !mkdtemp(dirB) || !mkdtemp(dirC))
    {
        Fail("mkdtemp fair pumping test");
        return 1;
    }

    PicoWorkspaceId idA = 0, idB = 0, idC = 0;
    pico_workspace_open(host, dirA, &idA);
    pico_workspace_open(host, dirB, &idB);
    pico_workspace_open(host, dirC, &idC);

    PicoAgentCreateOptions opt = { .kind = PICO_AGENT_MAIN, .session_start = PICO_SESSION_NONE };
    PicoAgentId a1 = 0, b1 = 0, c1 = 0;
    pico_main_agent_create(host, idA, &opt, &a1);
    pico_main_agent_create(host, idB, &opt, &b1);
    pico_main_agent_create(host, idC, &opt, &c1);

    /* Verify fair round-robin pumping across workspaces */
    host->pump_rr_index = 0;
    pico_host_pump(host);
    if (host->pump_rr_index != 1)
    {
        Fail("pump 1 should advance rr index to 1");
    }

    pico_host_pump(host);
    if (host->pump_rr_index != 2)
    {
        Fail("pump 2 should advance rr index to 2");
    }

    pico_host_pump(host);
    if (host->pump_rr_index != 0)
    {
        Fail("pump 3 should wrap rr index to 0");
    }

    pico_host_pump(host);
    if (host->pump_rr_index != 1)
    {
        Fail("pump 4 should advance rr index to 1");
    }

    pico_host_free(host);
    rmdir(dirA);
    rmdir(dirB);
    rmdir(dirC);
    return 0;
}

static int TestMultiWorkspaceDeletedDirectoryIntegrity(void)
{
    PicoHost *host = NULL;
    if (pico_host_init(&host, NULL, true) != PICO_OK || !host)
    {
        Fail("host init deleted dir test");
        return 1;
    }

    char dirA[] = "/tmp/pico-ws-delA-XXXXXX";
    if (!mkdtemp(dirA))
    {
        Fail("mkdtemp deleted dir test");
        return 1;
    }

    PicoWorkspaceId idA = 0;
    pico_workspace_open(host, dirA, &idA);
    PicoWorkspaceInfo info;
    pico_workspace_info(host, 0, &info);

    /* Delete directory from filesystem */
    rmdir(dirA);

    /* Workspace identity and stored canonical path remain intact */
    PicoWorkspaceInfo info_after;
    if (!pico_workspace_info(host, 0, &info_after) || info_after.id != idA ||
        strcmp(info_after.path, info.path) != 0)
    {
        Fail("deleted directory should not change workspace identity or canonical path");
    }

    pico_host_free(host);
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
    if (TestCdOpensSelectsAndReusesWorkspace() != 0)
    {
        return 1;
    }
    if (TestReloadTargetsSelectedWorkspace() != 0)
    {
        return 1;
    }
    if (TestHostReloadIgnoresWorkspaceLocalCompileFailure() != 0)
    {
        return 1;
    }
    if (TestCdResolvesAgainstCommandWorkspace() != 0)
    {
        return 1;
    }
    if (TestCdRollsBackNewWorkspaceOnAgentLimit() != 0)
    {
        return 1;
    }
    if (TestCdRejectsClosingWorkspace() != 0)
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
    if (TestReloadInitRollbackPreservesActiveState() != 0)
    {
        return 1;
    }
    if (TestGenerationRolloutAndDlcloseOnRelease() != 0)
    {
        return 1;
    }
    if (TestScopedExtensionListingRecords() != 0)
    {
        return 1;
    }
    if (TestDualScopeIndependentPublicationRollback() != 0)
    {
        return 1;
    }
    if (TestStatelessExtensionRollbackDoesNotLeakModule() != 0)
    {
        return 1;
    }
    if (TestExtensionToggleWhileBusyQueuesReloadAndKeepsAcceptingWork() != 0)
    {
        return 1;
    }
    if (TestMultiWorkspaceInstructionsIsolation() != 0)
    {
        return 1;
    }
    if (TestMultiWorkspaceToolNameIsolation() != 0)
    {
        return 1;
    }
    if (TestMultiWorkspaceMailboxIsolation() != 0)
    {
        return 1;
    }
    if (TestMultiWorkspaceAskOrderingAndRouting() != 0)
    {
        return 1;
    }
    if (TestMultiWorkspaceReloadAndCloseIsolation() != 0)
    {
        return 1;
    }
    if (TestMultiWorkspaceStuckWorkerIsolation() != 0)
    {
        return 1;
    }
    if (TestMultiWorkspaceMainAgentDelegationDrain() != 0)
    {
        return 1;
    }
    if (TestMultiWorkspaceModelAndSettingsIsolation() != 0)
    {
        return 1;
    }
    if (TestMultiWorkspaceFrameCallbacks() != 0)
    {
        return 1;
    }
    if (TestMultiWorkspaceCloseLastMainAgentAndZeroAgents() != 0)
    {
        return 1;
    }
    if (TestMultiWorkspaceAgentLimits() != 0)
    {
        return 1;
    }
    if (TestMultiWorkspaceStaleIds() != 0)
    {
        return 1;
    }
    if (TestMultiWorkspaceFairPumping() != 0)
    {
        return 1;
    }
    if (TestMultiWorkspaceDeletedDirectoryIntegrity() != 0)
    {
        return 1;
    }
    if (g_failed)
    {
        return 1;
    }
    return 0;
}
