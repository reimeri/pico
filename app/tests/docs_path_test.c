#include "docs_path.h"

#include <stdio.h>
#include <string.h>

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
    return 0;
}
