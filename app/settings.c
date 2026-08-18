#include "settings.h"
#include "json.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <errno.h>

static const char *HomeDir(void)
{
    const char *home = getenv("HOME");
    return home && home[0] ? home : ".";
}

void Pico_ConfigDir(char *out, size_t cap)
{
    const char *xdg = getenv("XDG_CONFIG_HOME");
    if (xdg && xdg[0])
    {
        snprintf(out, cap, "%s/pico", xdg);
    }
    else
    {
        snprintf(out, cap, "%s/.config/pico", HomeDir());
    }
}

static int MkdirP(const char *path)
{
    char buf[4096];
    snprintf(buf, sizeof(buf), "%s", path);
    for (char *p = buf + 1; *p; p++)
    {
        if (*p == '/')
        {
            *p = 0;
            mkdir(buf, 0755);
            *p = '/';
        }
    }
    if (mkdir(buf, 0755) != 0 && errno != EEXIST)
    {
        return -1;
    }
    return 0;
}

static void CopyField(char *dst, size_t cap, const char *src)
{
    if (!src || !src[0])
    {
        return;
    }
    snprintf(dst, cap, "%s", src);
}

static void ApplyObject(PicoSettings *s, const JsonDoc *doc, int obj)
{
    if (!JsonIsObject(doc, obj))
    {
        return;
    }
    char *api_key = JsonObjStr(doc, obj, "api_key");
    char *base_url = JsonObjStr(doc, obj, "base_url");
    char *model = JsonObjStr(doc, obj, "model");
    CopyField(s->api_key, sizeof(s->api_key), api_key);
    CopyField(s->base_url, sizeof(s->base_url), base_url);
    CopyField(s->model, sizeof(s->model), model);
    int limit = JsonObjInt(doc, obj, "context_limit", 0);
    if (limit > 0)
    {
        s->context_limit = limit;
    }
    int rs = JsonObjGet(doc, obj, "reasoning_summary");
    if (rs >= 0)
    {
        s->reasoning_summary = JsonEq(doc, rs, "true") || JsonEq(doc, rs, "1");
    }
    free(api_key);
    free(base_url);
    free(model);
}

static void LoadFile(PicoSettings *s, const char *path)
{
    size_t len = 0;
    char *src = Pico_ReadFile(path, &len);
    if (!src)
    {
        return;
    }
    JsonDoc doc;
    if (JsonParse(&doc, src, len) == 0)
    {
        ApplyObject(s, &doc, 0);
        JsonFree(&doc);
    }
    free(src);
}

static const char *FirstEnv(const char *a, const char *b)
{
    const char *v = getenv(a);
    if (v && v[0])
    {
        return v;
    }
    v = getenv(b);
    if (v && v[0])
    {
        return v;
    }
    return NULL;
}

void PicoSettings_Load(PicoApp *app)
{
    PicoSettings *s = &app->settings;
    memset(s, 0, sizeof(*s));
    snprintf(s->base_url, sizeof(s->base_url), "https://api.openai.com/v1");
    snprintf(s->model, sizeof(s->model), "gpt-4o");
    s->context_limit = 128000;
    s->reasoning_summary = true;

    char dir[4096];
    Pico_ConfigDir(dir, sizeof(dir));
    MkdirP(dir);

    char path[4096];
    snprintf(path, sizeof(path), "%s/settings.json", dir);
    LoadFile(s, path);

    if (app->workspace[0])
    {
        snprintf(path, sizeof(path), "%s/.pico/settings.json", app->workspace);
        LoadFile(s, path);
    }

    CopyField(s->api_key, sizeof(s->api_key), FirstEnv("PICO_API_KEY", "OPENAI_API_KEY"));
    CopyField(s->base_url, sizeof(s->base_url), FirstEnv("PICO_BASE_URL", "OPENAI_BASE_URL"));
    CopyField(s->model, sizeof(s->model), FirstEnv("PICO_MODEL", "OPENAI_MODEL"));
    const char *rs = getenv("PICO_REASONING_SUMMARY");
    if (rs && rs[0])
    {
        s->reasoning_summary = !(rs[0] == '0' || rs[0] == 'f' || rs[0] == 'F' || rs[0] == 'n' || rs[0] == 'N');
    }
    const char *limit = getenv("PICO_CONTEXT_LIMIT");
    if (limit && limit[0])
    {
        int n = atoi(limit);
        if (n > 0)
        {
            s->context_limit = n;
        }
    }

    app->model_name = s->model;
    app->tokens_limit = s->context_limit;
}

static void AppendFile(JsonBuf *b, const char *path)
{
    size_t len = 0;
    char *src = Pico_ReadFile(path, &len);
    if (!src || !src[0])
    {
        free(src);
        return;
    }
    if (b->len)
    {
        JsonBuf_Puts(b, "\n\n");
    }
    JsonBuf_Append(b, src, len);
    free(src);
}

char *PicoSettings_LoadSystemPrompt(const PicoApp *app)
{
    JsonBuf b;
    JsonBuf_Init(&b);
    char path[4096];
    Pico_ConfigDir(path, sizeof(path));
    size_t n = strlen(path);
    snprintf(path + n, sizeof(path) - n, "/SYSTEM.md");
    AppendFile(&b, path);
    if (app->workspace[0])
    {
        snprintf(path, sizeof(path), "%s/.pico/SYSTEM.md", app->workspace);
        AppendFile(&b, path);
    }
    if (!b.len)
    {
        JsonBuf_Puts(&b,
                     "You are Pico, a small coding assistant. The user's workspace is the current "
                     "working directory. Use the sh tool to run shell commands when that helps. "
                     "Prefer concise answers.");
    }
    return JsonBuf_Steal(&b);
}
