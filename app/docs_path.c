#include "docs_path.h"
#include "path.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#ifndef PICO_INSTALL_DATADIR
#define PICO_INSTALL_DATADIR ""
#endif

static char g_app_dir[4096];

static bool IsDirectory(const char *path)
{
    struct stat st;
    return path && path[0] && stat(path, &st) == 0 && S_ISDIR(st.st_mode);
}

static bool HasRuntimeData(const char *dir)
{
    char path[4096];
    return dir && dir[0] &&
           PicoPath_Format(path, sizeof(path), "%s/resources", dir) &&
           IsDirectory(path);
}

static void SetDataDir(const char *dir)
{
    if (!dir || !dir[0])
    {
        g_app_dir[0] = '\0';
        return;
    }
    snprintf(g_app_dir, sizeof(g_app_dir), "%s", dir);
    size_t n = strlen(g_app_dir);
    while (n > 1 && g_app_dir[n - 1] == '/')
    {
        g_app_dir[--n] = '\0';
    }
}

void Pico_PathsInit(const char *application_dir)
{
    char candidate[4096];
    const char *override = getenv("PICO_DATA_DIR");
    if (override && override[0])
    {
        SetDataDir(override);
        return;
    }
    if (HasRuntimeData(application_dir))
    {
        SetDataDir(application_dir);
        return;
    }
    if (application_dir && application_dir[0] &&
        PicoPath_Format(candidate, sizeof(candidate), "%s/../share/pico", application_dir) &&
        HasRuntimeData(candidate))
    {
        SetDataDir(candidate);
        return;
    }
    if (HasRuntimeData(PICO_INSTALL_DATADIR))
    {
        SetDataDir(PICO_INSTALL_DATADIR);
        return;
    }
    SetDataDir(application_dir);
}

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
    SetDataDir(dir);
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

bool Pico_DataPath(const char *relative, char *out, size_t cap)
{
    return Pico_DocsJoin(g_app_dir, relative, out, cap);
}

bool Pico_SdkIncludeDir(char *out, size_t cap)
{
    return Pico_DataPath("sdk/include", out, cap);
}

bool Pico_DocsFile(const char *topic, char *out, size_t cap)
{
    char rel[256];
    if (!Pico_DocsRelPath(topic, rel, sizeof(rel)))
    {
        return false;
    }
    return Pico_DataPath(rel, out, cap);
}
