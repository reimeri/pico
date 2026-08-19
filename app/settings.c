#define _POSIX_C_SOURCE 200809L

#include "settings.h"
#include "json.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
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

void Pico_MkdirP(const char *path)
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
        return;
    }
}

void Pico_RandomHex(char *out, size_t cap)
{
    if (!out || cap < 2)
    {
        return;
    }
    size_t n = (cap - 1) / 2;
    unsigned char raw[32];
    if (n > sizeof(raw))
    {
        n = sizeof(raw);
    }
    int got = 0;
    FILE *f = fopen("/dev/urandom", "rb");
    if (f)
    {
        got = fread(raw, 1, n, f) == n;
        fclose(f);
    }
    if (!got)
    {
        snprintf(out, cap, "%016lx%016lx", (unsigned long)time(NULL), (unsigned long)(size_t)out);
        out[cap - 1] = '\0';
        return;
    }
    static const char kHex[] = "0123456789abcdef";
    for (size_t i = 0; i < n; i++)
    {
        out[i * 2] = kHex[raw[i] >> 4];
        out[i * 2 + 1] = kHex[raw[i] & 0xf];
    }
    out[n * 2] = '\0';
}

void Pico_IsoTime(char *out, size_t cap, bool filename)
{
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    struct tm tm;
    gmtime_r(&ts.tv_sec, &tm);
    int ms = (int)(ts.tv_nsec / 1000000);
    if (filename)
    {
        snprintf(out, cap, "%04d-%02d-%02dT%02d-%02d-%02d-%03dZ", tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
                 tm.tm_hour, tm.tm_min, tm.tm_sec, ms);
    }
    else
    {
        snprintf(out, cap, "%04d-%02d-%02dT%02d:%02d:%02d.%03dZ", tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
                 tm.tm_hour, tm.tm_min, tm.tm_sec, ms);
    }
}

static bool ParseRatio(const char *s, double *out)
{
    if (!s || !s[0] || !out)
    {
        return false;
    }
    char *end = NULL;
    double v = strtod(s, &end);
    if (end == s)
    {
        return false;
    }
    while (*end == ' ' || *end == '\t')
    {
        end++;
    }
    if (*end != '\0' || v < 0.0 || v > 1.0)
    {
        return false;
    }
    *out = v;
    return true;
}

static bool IsOffWord(const char *s)
{
    return s && (strcmp(s, "null") == 0 || strcmp(s, "off") == 0 || strcmp(s, "false") == 0 ||
                 strcmp(s, "none") == 0);
}

static void ApplyCompactAt(PicoSettings *s, const JsonDoc *doc, int obj)
{
    int tok = JsonObjGet(doc, obj, "compact_at");
    if (tok < 0)
    {
        return;
    }
    if (JsonEq(doc, tok, "null"))
    {
        s->compact_enabled = false;
        return;
    }
    char *raw = JsonStrDup(doc, tok);
    if (!raw)
    {
        return;
    }
    if (IsOffWord(raw))
    {
        s->compact_enabled = false;
        free(raw);
        return;
    }
    double ratio;
    if (ParseRatio(raw, &ratio))
    {
        s->compact_enabled = true;
        s->compact_ratio = ratio;
    }
    free(raw);
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
        s->context_limit_set = true;
    }
    ApplyCompactAt(s, doc, obj);
    int resume = JsonObjGet(doc, obj, "resume_last");
    if (resume >= 0)
    {
        s->resume_last = JsonEq(doc, resume, "true") || JsonEq(doc, resume, "1");
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
    JsonStripComments(src, len);
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
    s->compact_enabled = true;
    s->compact_ratio = 0.9;

    char dir[4096];
    Pico_ConfigDir(dir, sizeof(dir));
    Pico_MkdirP(dir);

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
            s->context_limit_set = true;
        }
    }
    const char *resume = getenv("PICO_RESUME_LAST");
    if (resume && resume[0])
    {
        s->resume_last = !(resume[0] == '0' || resume[0] == 'f' || resume[0] == 'F' || resume[0] == 'n' ||
                           resume[0] == 'N');
    }
    const char *compact = getenv("PICO_COMPACT_AT");
    if (compact && compact[0])
    {
        if (IsOffWord(compact))
        {
            s->compact_enabled = false;
        }
        else
        {
            double ratio;
            if (ParseRatio(compact, &ratio))
            {
                s->compact_enabled = true;
                s->compact_ratio = ratio;
            }
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
