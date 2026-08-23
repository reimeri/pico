#include "docs_path.h"
#include "path.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>

static char g_app_dir[4096];

static int Fold(int c)
{
    return (c >= 'A' && c <= 'Z') ? c - 'A' + 'a' : c;
}

static bool FoldEq(const char *a, const char *b)
{
    if (!a || !b)
    {
        return false;
    }
    while (*a && *b)
    {
        if (Fold((unsigned char)*a) != Fold((unsigned char)*b))
        {
            return false;
        }
        a++;
        b++;
    }
    return *a == *b;
}

void Pico_DocsSetAppDir(const char *dir)
{
    if (!dir || !dir[0])
    {
        g_app_dir[0] = '\0';
        return;
    }
    snprintf(g_app_dir, sizeof(g_app_dir), "%s", dir);
}

const char *Pico_DocsAppDir(void)
{
    return g_app_dir;
}

bool Pico_DocsRelPath(const char *topic, char *out, size_t cap)
{
    char name[64];
    size_t n = 0;

    if (!out || cap == 0)
    {
        return false;
    }
    out[0] = '\0';
    while (topic && *topic && isspace((unsigned char)*topic))
    {
        topic++;
    }
    if (!topic || !topic[0])
    {
        return PicoPath_Format(out, cap, "docs/extend/README.md");
    }
    for (; topic[n] && n + 1 < sizeof(name); n++)
    {
        unsigned char c = (unsigned char)topic[n];
        if (!(isalnum(c) || c == '_' || c == '-'))
        {
            break;
        }
        name[n] = (char)Fold(c);
    }
    name[n] = '\0';
    if (!name[0])
    {
        return false;
    }
    if (FoldEq(name, "readme") || FoldEq(name, "index"))
    {
        return PicoPath_Format(out, cap, "docs/extend/README.md");
    }
    if (FoldEq(name, "subagents"))
    {
        return PicoPath_Format(out, cap, "docs/subagents.md");
    }
    return PicoPath_Format(out, cap, "docs/extend/%s.md", name);
}

bool Pico_DocsJoin(const char *app_dir, const char *rel, char *out, size_t cap)
{
    char base[4096];
    size_t n;

    if (!out || cap == 0)
    {
        return false;
    }
    out[0] = '\0';
    if (!app_dir || !app_dir[0] || !rel || !rel[0])
    {
        return false;
    }
    if (!PicoPath_Format(base, sizeof(base), "%s", app_dir))
    {
        return false;
    }
    n = strlen(base);
    while (n > 1 && base[n - 1] == '/')
    {
        base[--n] = '\0';
    }
    while (*rel == '/')
    {
        rel++;
    }
    if (!rel[0])
    {
        return false;
    }
    return PicoPath_Format(out, cap, "%s/%s", base, rel);
}

bool Pico_DocsFile(const char *topic, char *out, size_t cap)
{
    char rel[256];
    if (!Pico_DocsRelPath(topic, rel, sizeof(rel)))
    {
        return false;
    }
    return Pico_DocsJoin(g_app_dir, rel, out, cap);
}
