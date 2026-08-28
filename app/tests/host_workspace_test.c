#include "pico/host.h"
#include "pico/plugin.h"
#include "host_internal.h"
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
    if (shell.workspace_init(workspace, NULL) != 0 || subagent.workspace_init(workspace, NULL) != 0)
    {
        Fail("workspace builtin init");
        return 1;
    }
    for (i = 0; i < host.tool_count; i++)
    {
        if (host.tools[i].name && strcmp(host.tools[i].name, "sh") == 0)
        {
            has_sh = true;
        }
        if (host.tools[i].name && strcmp(host.tools[i].name, "subagent") == 0)
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
    "        fputc(workspace ? 'Y' : 'N', f);\n"
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
    if (g_failed)
    {
        return 1;
    }
    return 0;
}
