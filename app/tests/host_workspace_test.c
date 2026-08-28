#include "pico/host.h"
#include "host_internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

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

int main(void)
{
    if (TestCanonicalOpenAndDuplicate() != 0)
    {
        return 1;
    }
    if (g_failed)
    {
        return 1;
    }
    return 0;
}
