#define _POSIX_C_SOURCE 200809L

#include "docs_path.h"
#include "path.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static int Fail(const char *message)
{
    fprintf(stderr, "docs path: %s\n", message);
    return 1;
}

static int ExpectRel(const char *topic, const char *want)
{
    char rel[256];
    if (!Pico_DocsRelPath(topic, rel, sizeof(rel)))
    {
        fprintf(stderr, "docs path: RelPath rejected topic '%s'\n", topic ? topic : "(null)");
        return 1;
    }
    if (strcmp(rel, want) != 0)
    {
        fprintf(stderr, "docs path: topic '%s' mapped to '%s', expected '%s'\n", topic ? topic : "(null)", rel, want);
        return 1;
    }
    return 0;
}

int main(void)
{
    if (ExpectRel("", "docs/extend/README.md") || ExpectRel(NULL, "docs/extend/README.md") ||
        ExpectRel("README", "docs/extend/README.md") || ExpectRel("index", "docs/extend/README.md") ||
        ExpectRel("  readme", "docs/extend/README.md"))
    {
        return 1;
    }
    if (ExpectRel("subagents", "docs/subagents.md") || ExpectRel("anatomy", "docs/extend/anatomy.md") ||
        ExpectRel("host", "docs/extend/host.md") || ExpectRel("workspace", "docs/extend/workspace.md"))
    {
        return 1;
    }

    char rel[8];
    if (Pico_DocsRelPath("!!!", rel, sizeof(rel)))
    {
        return Fail("non-topic characters were accepted");
    }

    char path[4096];
    if (!Pico_DocsJoin("/opt/pico/", "docs/extend/README.md", path, sizeof(path)) ||
        strcmp(path, "/opt/pico/docs/extend/README.md") != 0)
    {
        return Fail("trailing-slash join did not yield /opt/pico/docs/extend/README.md");
    }
    if (!Pico_DocsJoin("/opt/pico", "docs/extend/README.md", path, sizeof(path)) ||
        strcmp(path, "/opt/pico/docs/extend/README.md") != 0)
    {
        return Fail("no-slash join did not yield /opt/pico/docs/extend/README.md");
    }

    Pico_DocsSetAppDir(NULL);
    if (Pico_DocsFile("README", path, sizeof(path)))
    {
        return Fail("docs file resolved without an application directory");
    }
    Pico_DocsSetAppDir("/opt/pico");
    if (!Pico_DocsFile("README", path, sizeof(path)) || strcmp(path, "/opt/pico/docs/extend/README.md") != 0)
    {
        return Fail("docs file did not join stored application directory");
    }

    char root[] = "/tmp/pico-paths-XXXXXX";
    char portable[4096];
    char portable_resources[4096];
    char bin[4096];
    char share[4096];
    char data[4096];
    char data_resources[4096];
    if (!mkdtemp(root))
    {
        return Fail("could not create path fixture");
    }
    char prefix[4096];
    if (!PicoPath_Format(portable, sizeof(portable), "%s/portable", root) ||
        !PicoPath_Format(portable_resources, sizeof(portable_resources), "%s/resources", portable) ||
        !PicoPath_Format(prefix, sizeof(prefix), "%s/prefix", root) ||
        !PicoPath_Format(bin, sizeof(bin), "%s/bin", prefix) ||
        !PicoPath_Format(share, sizeof(share), "%s/share", prefix) ||
        !PicoPath_Format(data, sizeof(data), "%s/pico", share) ||
        !PicoPath_Format(data_resources, sizeof(data_resources), "%s/resources", data) ||
        mkdir(portable, 0700) != 0 || mkdir(portable_resources, 0700) != 0 ||
        mkdir(prefix, 0700) != 0 || mkdir(bin, 0700) != 0 || mkdir(share, 0700) != 0 ||
        mkdir(data, 0700) != 0 || mkdir(data_resources, 0700) != 0)
    {
        return Fail("could not populate path fixture");
    }

    Pico_PathsInit(portable);
    if (!Pico_DataPath("resources/logo.png", path, sizeof(path)))
    {
        return Fail("portable data path was unavailable");
    }
    char want[4096];
    PicoPath_Format(want, sizeof(want), "%s/resources/logo.png", portable);
    if (strcmp(path, want) != 0)
    {
        return Fail("portable data directory was not preferred");
    }

    Pico_PathsInit(bin);
    PicoPath_Format(want, sizeof(want), "%s/../share/pico/sdk/include", bin);
    if (!Pico_SdkIncludeDir(path, sizeof(path)) || strcmp(path, want) != 0)
    {
        return Fail("prefix-relative SDK directory was not resolved");
    }

    setenv("PICO_DATA_DIR", "/override/pico", 1);
    Pico_PathsInit(portable);
    unsetenv("PICO_DATA_DIR");
    if (!Pico_DataPath("docs/subagents.md", path, sizeof(path)) ||
        strcmp(path, "/override/pico/docs/subagents.md") != 0)
    {
        return Fail("PICO_DATA_DIR did not override discovered data");
    }

    rmdir(data_resources);
    rmdir(data);
    rmdir(share);
    rmdir(bin);
    rmdir(prefix);
    rmdir(portable_resources);
    rmdir(portable);
    rmdir(root);
    return 0;
}
