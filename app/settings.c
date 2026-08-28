#define _POSIX_C_SOURCE 200809L

#include "settings.h"
#include "docs_path.h"
#include "json.h"
#include "overlay.h"
#include "path.h"
#include "session.h"
#include "theme_internal.h"
#include "host_internal.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <time.h>
#include <errno.h>

static const char *HomeDir(void)
{
    const char *home = getenv("HOME");
    return home && home[0] ? home : ".";
}

bool Pico_ConfigDir(char *out, size_t cap)
{
    const char *xdg = getenv("XDG_CONFIG_HOME");
    if (xdg && xdg[0])
    {
        return PicoPath_Format(out, cap, "%s/pico", xdg);
    }
    return PicoPath_Format(out, cap, "%s/.config/pico", HomeDir());
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

static bool ParseFontScale(const char *s, double *out)
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
    if (*end != '\0' || !isfinite(v) || v < 0.5 || v > 3.0)
    {
        return false;
    }
    *out = v;
    return true;
}

#define PICO_CHAT_WIDTH_DEFAULT 75
#define PICO_CHAT_WIDTH_MIN 40
#define PICO_CHAT_WIDTH_MAX 200

static bool ParseChatWidth(const char *s, int *out)
{
    if (!s || !s[0] || !out)
    {
        return false;
    }
    char *end = NULL;
    long v = strtol(s, &end, 10);
    if (end == s)
    {
        return false;
    }
    while (*end == ' ' || *end == '\t')
    {
        end++;
    }
    if (*end != '\0')
    {
        return false;
    }
    if (v == 0)
    {
        *out = 0;
        return true;
    }
    if (v < PICO_CHAT_WIDTH_MIN || v > PICO_CHAT_WIDTH_MAX)
    {
        return false;
    }
    *out = (int)v;
    return true;
}

float Pico_ClampChatWidth(float available, float text_max)
{
    if (!(text_max > 0.0f))
    {
        return available;
    }
    return available < text_max ? available : text_max;
}

static float ChatChWidth(void)
{
    Clay_TextElementConfig config = {
        .fontId = FONT_REGULAR,
        .fontSize = CHAT_BODY_FONT_SIZE,
    };
    Clay_StringSlice slice = {.length = 1, .chars = "0", .baseChars = "0"};
    float width = Pico_MeasureTextUtf8(slice, &config, NULL).width;
    if (width > 0.0f)
    {
        return width;
    }
    return Pico_FontPx(CHAT_BODY_FONT_SIZE) * 0.5f;
}

float Pico_ChatTextMaxPx(const PicoHost *app)
{
    if (!app || app->settings.chat_width <= 0)
    {
        return 0.0f;
    }
    return (float)app->settings.chat_width * ChatChWidth();
}

float Pico_ChatColumnMaxPx(const PicoHost *app)
{
    float text_max = Pico_ChatTextMaxPx(app);
    if (!(text_max > 0.0f))
    {
        return 0.0f;
    }
    return text_max + CHAT_WRAP_CHROME;
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

static void ParseModel(const JsonDoc *doc, int obj, PicoModel *m)
{
    memset(m, 0, sizeof(*m));
    if (!JsonIsObject(doc, obj))
    {
        return;
    }
    char *id = JsonObjStr(doc, obj, "id");
    char *name = JsonObjStr(doc, obj, "name");
    char *provider = JsonObjStr(doc, obj, "provider");
    char *base_url = JsonObjStr(doc, obj, "base_url");
    char *selected = JsonObjStr(doc, obj, "selected_effort");
    CopyField(m->id, sizeof(m->id), id);
    if (name && name[0])
    {
        CopyField(m->name, sizeof(m->name), name);
    }
    else
    {
        CopyField(m->name, sizeof(m->name), m->id);
    }
    CopyField(m->provider, sizeof(m->provider), provider);
    if (!m->provider[0])
    {
        snprintf(m->provider, sizeof(m->provider), "%s", "openai");
    }
    CopyField(m->base_url, sizeof(m->base_url), base_url);
    m->context_limit = JsonObjInt(doc, obj, "context_limit", 0);
    int vis = JsonObjGet(doc, obj, "vision");
    m->vision = vis >= 0 && (JsonEq(doc, vis, "true") || JsonEq(doc, vis, "1"));
    CopyField(m->default_effort, sizeof(m->default_effort), selected);
    int arr = JsonObjGet(doc, obj, "effort");
    int n = JsonIsArray(doc, arr) ? JsonArrayLen(doc, arr) : 0;
    for (int i = 0; i < n && m->effort_count < PICO_MAX_EFFORTS; i++)
    {
        char *level = JsonStrDup(doc, JsonArrayAt(doc, arr, i));
        if (level && level[0])
        {
            snprintf(m->effort[m->effort_count], PICO_EFFORT_LEN, "%s", level);
            m->effort_count++;
        }
        free(level);
    }
    free(id);
    free(name);
    free(provider);
    free(base_url);
    free(selected);
}

static void ReplaceModels(PicoModel **models, int *count, const JsonDoc *doc, int obj)
{
    int arr = JsonObjGet(doc, obj, "models");
    if (!JsonIsArray(doc, arr))
    {
        return;
    }
    int n = JsonArrayLen(doc, arr);
    PicoModel *list = n > 0 ? (PicoModel *)calloc((size_t)n, sizeof(PicoModel)) : NULL;
    int got = 0;
    for (int i = 0; list && i < n; i++)
    {
        PicoModel m;
        ParseModel(doc, JsonArrayAt(doc, arr, i), &m);
        if (!m.id[0])
        {
            continue;
        }
        list[got++] = m;
    }
    free(*models);
    *models = list;
    *count = got;
}

static void ApplyObject(PicoSettings *s, const JsonDoc *doc, int obj)
{
    if (!JsonIsObject(doc, obj))
    {
        return;
    }
    char *model = JsonObjStr(doc, obj, "model");
    CopyField(s->model, sizeof(s->model), model);
    int limit = JsonObjInt(doc, obj, "context_limit", 0);
    if (limit > 0)
    {
        s->context_limit = limit;
    }
    ApplyCompactAt(s, doc, obj);
    int resume = JsonObjGet(doc, obj, "resume_last");
    if (resume >= 0)
    {
        s->resume_last = JsonEq(doc, resume, "true") || JsonEq(doc, resume, "1");
    }
    char *font_scale = JsonObjRaw(doc, obj, "font_scale");
    if (font_scale)
    {
        double scale;
        if (ParseFontScale(font_scale, &scale))
        {
            s->font_scale = scale;
        }
        free(font_scale);
    }
    char *chat_width = JsonObjRaw(doc, obj, "chat_width");
    if (chat_width)
    {
        int width;
        if (ParseChatWidth(chat_width, &width))
        {
            s->chat_width = width;
        }
        free(chat_width);
    }
    free(model);
}

static bool DisabledNameListed(const PicoSettings *s, const char *name)
{
    if (!s || !name || !name[0])
    {
        return false;
    }
    for (int i = 0; i < s->disabled_extension_count; i++)
    {
        if (strcmp(s->disabled_extensions[i], name) == 0)
        {
            return true;
        }
    }
    return false;
}

static void ApplyDisabledExtensions(PicoSettings *s, const JsonDoc *doc, int obj)
{
    s->disabled_extension_count = 0;
    int arr = JsonObjGet(doc, obj, "disabled_extensions");
    if (!JsonIsArray(doc, arr))
    {
        return;
    }
    int n = JsonArrayLen(doc, arr);
    for (int i = 0; i < n && s->disabled_extension_count < PICO_MAX_DISABLED_EXTENSIONS; i++)
    {
        char *name = JsonStrDup(doc, JsonArrayAt(doc, arr, i));
        if (name && name[0] && strcmp(name, "extensions") != 0 && !DisabledNameListed(s, name))
        {
            snprintf(s->disabled_extensions[s->disabled_extension_count], PICO_DISABLED_EXT_NAME, "%s",
                     name);
            s->disabled_extension_count++;
        }
        free(name);
    }
}

static void LoadFile(PicoSettings *s, PicoModel **models, int *model_count, const char *path,
                     bool load_disabled)
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
        ReplaceModels(models, model_count, &doc, 0);
        if (load_disabled)
        {
            ApplyDisabledExtensions(s, &doc, 0);
        }
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

static bool UserSettingsPath(char *out, size_t cap)
{
    char dir[4096];
    return Pico_ConfigDir(dir, sizeof(dir)) &&
           PicoPath_Format(out, cap, "%s/settings.json", dir);
}

static bool WorkspaceSettingsPath(const PicoWorkspace *workspace, char *out, size_t cap)
{
    if (workspace && workspace->path[0])
    {
        return PicoPath_Format(out, cap, "%s/.pico/settings.json", workspace->path);
    }
    if (out && cap > 0)
    {
        out[0] = '\0';
    }
    return true;
}

static bool SettingsFileExists(const char *path)
{
    struct stat st;
    return path && path[0] && stat(path, &st) == 0 && S_ISREG(st.st_mode);
}

static bool FileHasModels(const char *path)
{
    size_t len = 0;
    char *src = Pico_ReadFile(path, &len);
    if (!src)
    {
        return false;
    }
    JsonStripComments(src, len);
    JsonDoc doc;
    bool has = false;
    if (JsonParse(&doc, src, len) == 0)
    {
        has = JsonIsArray(&doc, JsonObjGet(&doc, 0, "models"));
        JsonFree(&doc);
    }
    free(src);
    return has;
}

PicoModel *PicoSettings_FindModel(PicoHost *app, const char *id)
{
    if (!app || !id || !id[0])
    {
        return NULL;
    }
    for (int i = 0; i < app->model_count; i++)
    {
        if (strcmp(app->models[i].id, id) == 0)
        {
            return &app->models[i];
        }
    }
    return NULL;
}

bool PicoSettings_EffortAllowed(const PicoModel *model, const char *effort)
{
    if (!model || !effort || !effort[0])
    {
        return false;
    }
    for (int i = 0; i < model->effort_count; i++)
    {
        if (strcmp(model->effort[i], effort) == 0)
        {
            return true;
        }
    }
    return false;
}

const PicoModel *PicoSettings_FindModelConst(const PicoHost *app, const char *id)
{
    if (!app || !id || !id[0])
    {
        return NULL;
    }
    for (int i = 0; i < app->model_count; i++)
    {
        if (strcmp(app->models[i].id, id) == 0)
        {
            return &app->models[i];
        }
    }
    return NULL;
}

PicoModel *PicoSettings_ActiveModel(PicoHost *app, const PicoAgent *agent)
{
    return agent ? PicoSettings_FindModel(app, agent->model) : NULL;
}

const PicoModel *PicoSettings_ActiveModelConst(const PicoHost *app, const PicoAgent *agent)
{
    return agent ? PicoSettings_FindModelConst(app, agent->model) : NULL;
}

const char *PicoSettings_ActiveEffort(const PicoAgent *agent)
{
    return agent && agent->effort[0] ? agent->effort : "none";
}

void PicoSettings_SyncAgent(const PicoHost *app, PicoAgent *agent)
{
    if (!app || !agent)
    {
        return;
    }
    const PicoModel *m = PicoSettings_ActiveModelConst(app, agent);
    snprintf(agent->model_name, sizeof(agent->model_name), "%s",
             m && m->name[0] ? m->name : agent->model);
    if (m && m->context_limit > 0)
    {
        agent->context_limit = m->context_limit;
    }
    else
    {
        agent->context_limit = app->settings.context_limit;
    }
    if (!m || !PicoSettings_EffortAllowed(m, agent->effort))
    {
        const char *effort = m && PicoSettings_EffortAllowed(m, m->default_effort)
                                 ? m->default_effort
                                 : (m && m->effort_count > 0 ? m->effort[0] : "none");
        snprintf(agent->effort, sizeof(agent->effort), "%s", effort);
    }
}

void PicoSettings_InitAgent(const PicoHost *app, PicoAgent *agent)
{
    if (!app || !agent)
    {
        return;
    }
    snprintf(agent->model, sizeof(agent->model), "%s", app->settings.model);
    agent->compact_enabled = app->settings.compact_enabled;
    agent->compact_ratio = app->settings.compact_ratio;
    agent->effort[0] = '\0';
    PicoSettings_SyncAgent(app, agent);
}

static PicoModel *FindCatalog(PicoHost *app, const char *q)
{
    if (!app || !q || !q[0])
    {
        return NULL;
    }
    for (int i = 0; i < app->model_count; i++)
    {
        if (strcasecmp(app->models[i].id, q) == 0 || strcasecmp(app->models[i].name, q) == 0)
        {
            return &app->models[i];
        }
    }
    return NULL;
}

bool PicoSettings_SetModel(PicoHost *app, PicoAgent *agent, const char *id_or_name)
{
    if (!app || !agent)
    {
        return false;
    }
    PicoModel *m = FindCatalog(app, id_or_name);
    if (!m)
    {
        char line[256];
        snprintf(line, sizeof(line), "Unknown model `%s`. Try `/model` for the catalog.",
                 id_or_name ? id_or_name : "");
        PicoOverlay_Notify(app, line);
        return false;
    }
    snprintf(agent->model, sizeof(agent->model), "%s", m->id);
    snprintf(app->settings.model, sizeof(app->settings.model), "%s", m->id);
    agent->effort[0] = '\0';
    PicoSettings_SyncAgent(app, agent);
    PicoSettings_SaveSelection(app, agent, true, false);
    PicoSession_LogModelChange(app, agent, agent->model, PicoSettings_ActiveEffort(agent));
    char line[256];
    snprintf(line, sizeof(line), "Model `%s` · effort `%s`", m->name[0] ? m->name : m->id,
             PicoSettings_ActiveEffort(agent));
    PicoOverlay_Notify(app, line);
    return true;
}

bool PicoSettings_SetEffort(PicoHost *app, PicoAgent *agent, const char *level)
{
    if (!app || !agent)
    {
        return false;
    }
    PicoModel *m = PicoSettings_ActiveModel(app, agent);
    if (!m)
    {
        PicoOverlay_Notify(app, "No model in the catalog. Add one in settings.json.");
        return false;
    }
    if (!level || !level[0])
    {
        return false;
    }
    if (m->effort_count > 0 && !PicoSettings_EffortAllowed(m, level))
    {
        char line[256];
        snprintf(line, sizeof(line), "`%s` is not in this model's effort list.", level);
        PicoOverlay_Notify(app, line);
        return false;
    }
    snprintf(agent->effort, sizeof(agent->effort), "%s", level);
    PicoSettings_SaveSelection(app, agent, false, true);
    PicoSession_LogModelChange(app, agent, agent->model, PicoSettings_ActiveEffort(agent));
    char line[256];
    snprintf(line, sizeof(line), "Effort `%s` for `%s`", agent->effort, m->name[0] ? m->name : m->id);
    PicoOverlay_Notify(app, line);
    return true;
}

static void EnsureDefaultCatalog(PicoHost *app)
{
    if (app->model_count > 0)
    {
        return;
    }
    PicoModel *list = (PicoModel *)calloc(1, sizeof(PicoModel));
    if (!list)
    {
        return;
    }
    snprintf(list[0].id, sizeof(list[0].id), "%s", app->settings.model);
    snprintf(list[0].name, sizeof(list[0].name), "%s", app->settings.model);
    snprintf(list[0].provider, sizeof(list[0].provider), "%s", "openai");
    list[0].context_limit = app->settings.context_limit;
    snprintf(list[0].default_effort, sizeof(list[0].default_effort), "%s", "none");
    app->models = list;
    app->model_count = 1;
}

void PicoSettings_Load(PicoHost *app)
{
    PicoSettings *s = &app->settings;
    memset(s, 0, sizeof(*s));
    snprintf(s->model, sizeof(s->model), "gpt-5.6-sol");
    s->context_limit = 128000;
    s->compact_enabled = true;
    s->compact_ratio = 0.9;
    s->font_scale = 1.0;
    s->chat_width = PICO_CHAT_WIDTH_DEFAULT;

    char dir[4096];
    if (Pico_ConfigDir(dir, sizeof(dir)))
    {
        Pico_MkdirP(dir);
    }

    PicoModel *catalog = NULL;
    int catalog_n = 0;

    char path[4096];
    if (UserSettingsPath(path, sizeof(path)))
    {
        LoadFile(s, &catalog, &catalog_n, path, true);
    }

    if (WorkspaceSettingsPath(PicoHost_PrimaryWorkspaceConst(app), path, sizeof(path)) && path[0])
    {
        LoadFile(s, &catalog, &catalog_n, path, false);
    }

    CopyField(s->model, sizeof(s->model), FirstEnv("PICO_MODEL", "OPENAI_MODEL"));
    const char *limit = getenv("PICO_CONTEXT_LIMIT");
    if (limit && limit[0])
    {
        int n = atoi(limit);
        if (n > 0)
        {
            s->context_limit = n;
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
    const char *font_scale = getenv("PICO_FONT_SCALE");
    if (font_scale && font_scale[0])
    {
        double scale;
        if (ParseFontScale(font_scale, &scale))
        {
            s->font_scale = scale;
        }
    }

    free(app->models);
    app->models = catalog;
    app->model_count = catalog_n;
    EnsureDefaultCatalog(app);

    const char *effort = getenv("PICO_EFFORT");
    if (effort && effort[0])
    {
        PicoModel *m = PicoSettings_FindModel(app, app->settings.model);
        if (m)
        {
            snprintf(m->default_effort, sizeof(m->default_effort), "%s", effort);
        }
    }
    Pico_SetFontScale((float)s->font_scale);
}

static bool WriteFile(const char *path, const char *data, size_t len)
{
    if (!path || !path[0] || !data)
    {
        return false;
    }
    FILE *f = fopen(path, "wb");
    if (!f)
    {
        return false;
    }
    size_t n = fwrite(data, 1, len, f);
    fclose(f);
    return n == len;
}

static char *JsonQuoted(const char *s)
{
    JsonBuf b;
    JsonBuf_Init(&b);
    JsonBuf_String(&b, s ? s : "");
    return JsonBuf_Steal(&b);
}

static char *Splice(const char *src, size_t len, int at, int del, const char *ins)
{
    if (!src || at < 0 || del < 0 || (size_t)at + (size_t)del > len)
    {
        return NULL;
    }
    size_t ins_len = ins ? strlen(ins) : 0;
    char *out = (char *)malloc(len - (size_t)del + ins_len + 1);
    if (!out)
    {
        return NULL;
    }
    memcpy(out, src, (size_t)at);
    if (ins_len)
    {
        memcpy(out + at, ins, ins_len);
    }
    memcpy(out + at + ins_len, src + at + del, len - (size_t)at - (size_t)del);
    out[len - (size_t)del + ins_len] = '\0';
    return out;
}

static int ObjectClose(const char *src, const JsonDoc *doc, int obj)
{
    int end = JsonTokEnd(doc, obj);
    if (end <= 0)
    {
        return -1;
    }
    int pos = end - 1;
    while (pos > 0 && (src[pos] == ' ' || src[pos] == '\n' || src[pos] == '\r' || src[pos] == '\t'))
    {
        pos--;
    }
    return src[pos] == '}' ? pos : -1;
}

static bool ObjectIsEmpty(const char *src, const JsonDoc *doc, int obj, int close_at)
{
    int start = JsonTokStart(doc, obj);
    if (start < 0 || close_at <= start)
    {
        return true;
    }
    for (int i = start + 1; i < close_at; i++)
    {
        char c = src[i];
        if (c != ' ' && c != '\n' && c != '\r' && c != '\t')
        {
            return false;
        }
    }
    return true;
}

static bool PatchStringTok(char **src, size_t *len, const JsonDoc *doc, int tok, const char *value)
{
    int start = JsonTokStart(doc, tok);
    int end = JsonTokEnd(doc, tok);
    if (start < 0 || end < start)
    {
        return false;
    }
    char *quoted = JsonQuoted(value);
    if (!quoted || quoted[0] != '"')
    {
        free(quoted);
        return false;
    }
    size_t qn = strlen(quoted);
    char *inner = quoted + 1;
    size_t inner_len = qn >= 2 ? qn - 2 : 0;
    inner[inner_len] = '\0';
    char *next = Splice(*src, *len, start, end - start, inner);
    free(quoted);
    if (!next)
    {
        return false;
    }
    free(*src);
    *src = next;
    *len = strlen(next);
    return true;
}

static bool InsertObjectKey(char **src, size_t *len, const JsonDoc *doc, int obj, const char *key,
                            const char *json_value)
{
    int close_at = ObjectClose(*src, doc, obj);
    if (close_at < 0)
    {
        return false;
    }
    JsonBuf b;
    JsonBuf_Init(&b);
    if (!ObjectIsEmpty(*src, doc, obj, close_at))
    {
        JsonBuf_Puts(&b, ",");
    }
    JsonBuf_Puts(&b, "\n  ");
    JsonBuf_String(&b, key);
    JsonBuf_Puts(&b, ": ");
    JsonBuf_Puts(&b, json_value);
    char *ins = JsonBuf_Steal(&b);
    char *next = Splice(*src, *len, close_at, 0, ins);
    free(ins);
    if (!next)
    {
        return false;
    }
    free(*src);
    *src = next;
    *len = strlen(next);
    return true;
}

static bool PatchRootString(const char *path, const char *key, const char *value)
{
    size_t len = 0;
    char *src = Pico_ReadFile(path, &len);
    if (!src)
    {
        JsonBuf b;
        JsonBuf_Init(&b);
        JsonBuf_Puts(&b, "{\n  ");
        JsonBuf_String(&b, key);
        JsonBuf_Puts(&b, ": ");
        JsonBuf_String(&b, value);
        JsonBuf_Puts(&b, "\n}\n");
        char *out = JsonBuf_Steal(&b);
        bool ok = WriteFile(path, out, out ? strlen(out) : 0);
        free(out);
        return ok;
    }
    char *stripped = (char *)malloc(len + 1);
    if (!stripped)
    {
        free(src);
        return false;
    }
    memcpy(stripped, src, len + 1);
    JsonStripComments(stripped, len);
    JsonDoc doc;
    if (JsonParse(&doc, stripped, len) != 0)
    {
        free(stripped);
        free(src);
        return false;
    }
    int tok = JsonObjGet(&doc, 0, key);
    bool ok = false;
    if (tok >= 0)
    {
        ok = PatchStringTok(&src, &len, &doc, tok, value);
    }
    else
    {
        char *quoted = JsonQuoted(value);
        ok = InsertObjectKey(&src, &len, &doc, 0, key, quoted);
        free(quoted);
    }
    JsonFree(&doc);
    free(stripped);
    if (ok)
    {
        ok = WriteFile(path, src, len);
    }
    free(src);
    return ok;
}

static bool PatchTokSpan(char **src, size_t *len, const JsonDoc *doc, int tok, const char *json_value)
{
    int start = JsonTokStart(doc, tok);
    int end = JsonTokEnd(doc, tok);
    if (start < 0 || end < start || !json_value)
    {
        return false;
    }
    char *next = Splice(*src, *len, start, end - start, json_value);
    if (!next)
    {
        return false;
    }
    free(*src);
    *src = next;
    *len = strlen(next);
    return true;
}

static bool PatchRootJson(const char *path, const char *key, const char *json_value)
{
    if (!path || !key || !json_value)
    {
        return false;
    }
    size_t len = 0;
    char *src = Pico_ReadFile(path, &len);
    if (!src)
    {
        JsonBuf b;
        JsonBuf_Init(&b);
        JsonBuf_Puts(&b, "{\n  ");
        JsonBuf_String(&b, key);
        JsonBuf_Puts(&b, ": ");
        JsonBuf_Puts(&b, json_value);
        JsonBuf_Puts(&b, "\n}\n");
        char *out = JsonBuf_Steal(&b);
        bool ok = WriteFile(path, out, out ? strlen(out) : 0);
        free(out);
        return ok;
    }
    char *stripped = (char *)malloc(len + 1);
    if (!stripped)
    {
        free(src);
        return false;
    }
    memcpy(stripped, src, len + 1);
    JsonStripComments(stripped, len);
    JsonDoc doc;
    if (JsonParse(&doc, stripped, len) != 0)
    {
        free(stripped);
        free(src);
        return false;
    }
    int tok = JsonObjGet(&doc, 0, key);
    bool ok = false;
    if (tok >= 0)
    {
        ok = PatchTokSpan(&src, &len, &doc, tok, json_value);
    }
    else
    {
        ok = InsertObjectKey(&src, &len, &doc, 0, key, json_value);
    }
    JsonFree(&doc);
    free(stripped);
    if (ok)
    {
        ok = WriteFile(path, src, len);
    }
    free(src);
    return ok;
}

static char *DisabledExtensionsJson(const PicoSettings *s)
{
    JsonBuf b;
    JsonBuf_Init(&b);
    JsonBuf_Putc(&b, '[');
    for (int i = 0; i < s->disabled_extension_count; i++)
    {
        if (i)
        {
            JsonBuf_Putc(&b, ',');
        }
        JsonBuf_String(&b, s->disabled_extensions[i]);
    }
    JsonBuf_Putc(&b, ']');
    return JsonBuf_Steal(&b);
}

static bool SaveDisabledExtensions(PicoHost *app)
{
    char path[4096];
    if (!UserSettingsPath(path, sizeof(path)))
    {
        return false;
    }
    char dir[4096];
    if (Pico_ConfigDir(dir, sizeof(dir)))
    {
        Pico_MkdirP(dir);
    }
    char *json = DisabledExtensionsJson(&app->settings);
    bool ok = json && PatchRootJson(path, "disabled_extensions", json);
    free(json);
    return ok;
}

bool PicoSettings_SetExtensionDisabled(PicoHost *app, const char *name, bool disabled)
{
    if (!app || !name || !name[0] || strcmp(name, "extensions") == 0)
    {
        return false;
    }
    PicoSettings *s = &app->settings;
    int found = -1;
    for (int i = 0; i < s->disabled_extension_count; i++)
    {
        if (strcmp(s->disabled_extensions[i], name) == 0)
        {
            found = i;
            break;
        }
    }
    if (disabled)
    {
        if (found >= 0)
        {
            return SaveDisabledExtensions(app);
        }
        if (s->disabled_extension_count >= PICO_MAX_DISABLED_EXTENSIONS)
        {
            return false;
        }
        snprintf(s->disabled_extensions[s->disabled_extension_count], PICO_DISABLED_EXT_NAME, "%s", name);
        s->disabled_extension_count++;
    }
    else if (found >= 0)
    {
        for (int i = found; i < s->disabled_extension_count - 1; i++)
        {
            memcpy(s->disabled_extensions[i], s->disabled_extensions[i + 1], PICO_DISABLED_EXT_NAME);
        }
        s->disabled_extension_count--;
        s->disabled_extensions[s->disabled_extension_count][0] = '\0';
    }
    return SaveDisabledExtensions(app);
}

static void WriteModelValue(JsonBuf *b, const PicoModel *m, const char *selected_effort)
{
    JsonBuf_Puts(b, "{\"name\":");
    JsonBuf_String(b, m->name);
    JsonBuf_Puts(b, ",\"id\":");
    JsonBuf_String(b, m->id);
    if (m->provider[0])
    {
        JsonBuf_Puts(b, ",\"provider\":");
        JsonBuf_String(b, m->provider);
    }
    if (m->base_url[0])
    {
        JsonBuf_Puts(b, ",\"base_url\":");
        JsonBuf_String(b, m->base_url);
    }
    JsonBuf_Puts(b, ",\"context_limit\":");
    JsonBuf_Int(b, m->context_limit);
    JsonBuf_Puts(b, ",\"effort\":[");
    for (int i = 0; i < m->effort_count; i++)
    {
        if (i)
        {
            JsonBuf_Putc(b, ',');
        }
        JsonBuf_String(b, m->effort[i]);
    }
    JsonBuf_Puts(b, "],\"selected_effort\":");
    JsonBuf_String(b, selected_effort && selected_effort[0] ? selected_effort :
                      (m->default_effort[0] ? m->default_effort : "none"));
    JsonBuf_Putc(b, '}');
}

static bool PatchSelectedEffort(const char *path, PicoHost *app, const PicoAgent *agent)
{
    PicoModel *active = PicoSettings_ActiveModel(app, agent);
    if (!active)
    {
        return false;
    }
    size_t len = 0;
    char *src = Pico_ReadFile(path, &len);
    if (!src)
    {
        JsonBuf b;
        JsonBuf_Init(&b);
        JsonBuf_Puts(&b, "{\n  \"model\":");
        JsonBuf_String(&b, agent->model);
        JsonBuf_Puts(&b, ",\n  \"models\":[");
        WriteModelValue(&b, active, agent->effort);
        JsonBuf_Puts(&b, "]\n}\n");
        char *out = JsonBuf_Steal(&b);
        bool ok = WriteFile(path, out, out ? strlen(out) : 0);
        free(out);
        return ok;
    }
    char *stripped = (char *)malloc(len + 1);
    if (!stripped)
    {
        free(src);
        return false;
    }
    memcpy(stripped, src, len + 1);
    JsonStripComments(stripped, len);
    JsonDoc doc;
    if (JsonParse(&doc, stripped, len) != 0)
    {
        free(stripped);
        free(src);
        return false;
    }
    int arr = JsonObjGet(&doc, 0, "models");
    bool ok = false;
    if (!JsonIsArray(&doc, arr))
    {
        JsonBuf b;
        JsonBuf_Init(&b);
        JsonBuf_Putc(&b, '[');
        for (int i = 0; i < app->model_count; i++)
        {
            if (i)
            {
                JsonBuf_Putc(&b, ',');
            }
            WriteModelValue(&b, &app->models[i],
                            strcmp(app->models[i].id, agent->model) == 0 ? agent->effort : NULL);
        }
        JsonBuf_Putc(&b, ']');
        char *val = JsonBuf_Steal(&b);
        ok = InsertObjectKey(&src, &len, &doc, 0, "models", val);
        free(val);
    }
    else
    {
        int n = JsonArrayLen(&doc, arr);
        int found = -1;
        for (int i = 0; i < n; i++)
        {
            int item = JsonArrayAt(&doc, arr, i);
            char *id = JsonObjStr(&doc, item, "id");
            if (id && strcmp(id, active->id) == 0)
            {
                found = item;
                free(id);
                break;
            }
            free(id);
        }
        if (found >= 0)
        {
            int tok = JsonObjGet(&doc, found, "selected_effort");
            if (tok >= 0)
            {
                ok = PatchStringTok(&src, &len, &doc, tok, agent->effort);
            }
            else
            {
                char *quoted = JsonQuoted(agent->effort);
                ok = InsertObjectKey(&src, &len, &doc, found, "selected_effort", quoted);
                free(quoted);
            }
        }
    }
    JsonFree(&doc);
    free(stripped);
    if (ok)
    {
        ok = WriteFile(path, src, len);
    }
    free(src);
    return ok;
}

bool PicoSettings_SaveSelection(PicoHost *app, const PicoAgent *agent, bool save_model, bool save_effort)
{
    if (!app || !agent)
    {
        return false;
    }
    char user[4096];
    char workspace[4096];
    if (!UserSettingsPath(user, sizeof(user)) ||
        !WorkspaceSettingsPath(PicoAgent_Workspace(agent), workspace, sizeof(workspace)))
    {
        return false;
    }
    bool ok = true;
    if (save_model)
    {
        const char *path = SettingsFileExists(workspace) ? workspace : user;
        ok = PatchRootString(path, "model", agent->model) && ok;
    }
    if (save_effort)
    {
        const char *path = FileHasModels(workspace) ? workspace : (FileHasModels(user) ? user : NULL);
        if (!path)
        {
            path = SettingsFileExists(workspace) ? workspace : user;
        }
        ok = PatchSelectedEffort(path, app, agent) && ok;
    }
    return ok;
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

static bool AppendFileSpan(JsonBuf *b, const char *path, PicoPromptSpan *spans, int *span_count,
                           int max_spans, PicoPromptSource source)
{
    size_t before = b->len;
    AppendFile(b, path);
    if (b->len <= before || !spans || !span_count || *span_count >= max_spans)
    {
        return b->len > before;
    }
    size_t start = before ? before + 2 : 0;
    if (start >= b->len)
    {
        return true;
    }
    spans[*span_count].source = source;
    spans[*span_count].start = start;
    spans[*span_count].length = b->len - start;
    (*span_count)++;
    return true;
}

static void PushSpan(PicoPromptSpan *spans, int *span_count, int max_spans, PicoPromptSource source,
                     size_t start, size_t length)
{
    if (!spans || !span_count || *span_count >= max_spans || length == 0)
    {
        return;
    }
    spans[*span_count].source = source;
    spans[*span_count].start = start;
    spans[*span_count].length = length;
    (*span_count)++;
}

static void AppendDocsHint(JsonBuf *b)
{
    char docs[4096];
    if (!Pico_DocsFile("README", docs, sizeof(docs)))
    {
        return;
    }
    if (b->len)
    {
        JsonBuf_Puts(b, "\n\n");
    }
    JsonBuf_Puts(b, "If the user asks about Pico or wants to extend it, read ");
    JsonBuf_Puts(b, docs);
    JsonBuf_Puts(b, ". That file is shipped with Pico, not in the workspace. ");
    JsonBuf_Puts(b, "Topic pages are in the same directory as that README. ");
    char examples[4096];
    if (Pico_DocsJoin(Pico_DocsAppDir(), "examples", examples, sizeof(examples)))
    {
        JsonBuf_Puts(b, "Example sources and subagent profiles are in ");
        JsonBuf_Puts(b, examples);
        JsonBuf_Puts(b, ". ");
    }
    char builtins[4096];
    if (Pico_DocsJoin(Pico_DocsAppDir(), "builtins", builtins, sizeof(builtins)))
    {
        JsonBuf_Puts(b, "Reference builtin sources (sh, openai, hyper; compiled into Pico) are in ");
        JsonBuf_Puts(b, builtins);
        JsonBuf_Puts(b, ". ");
    }
    JsonBuf_Puts(b, "Resolve relative paths in those pages from the file that contains them.");
}

static bool FileHasContent(const char *path)
{
    struct stat st;
    return path && path[0] && stat(path, &st) == 0 && S_ISREG(st.st_mode) && st.st_size > 0;
}

static bool PushContext(const char **labels, int max, int *n, const char *path, const char *label)
{
    if (*n >= max || !FileHasContent(path))
    {
        return false;
    }
    labels[*n] = label;
    (*n)++;
    return true;
}

int PicoSettings_LoadedContext(const PicoWorkspace *workspace, const char **labels, int max)
{
    if (!labels || max <= 0)
    {
        return 0;
    }
    int n = 0;
    char path[4096];
    char config[4096];
    const char *root = PicoWorkspace_Path(workspace);
    if (Pico_ConfigDir(config, sizeof(config)) &&
        PicoPath_Format(path, sizeof(path), "%s/SYSTEM.md", config))
    {
        PushContext(labels, max, &n, path, "SYSTEM.md");
    }
    if (root[0])
    {
        if (PicoPath_Format(path, sizeof(path), "%s/.pico/SYSTEM.md", root))
        {
            PushContext(labels, max, &n, path, ".pico/SYSTEM.md");
        }
        if (PicoPath_Format(path, sizeof(path), "%s/AGENTS.md", root))
        {
            PushContext(labels, max, &n, path, "AGENTS.md");
        }
    }
    return n;
}

char *PicoSettings_LoadSystemPromptSpans(const PicoWorkspace *workspace, PicoPromptSpan *spans, int *span_count)
{
    if (span_count)
    {
        *span_count = 0;
    }
    JsonBuf b;
    JsonBuf_Init(&b);
    const char *root = PicoWorkspace_Path(workspace);
    char path[4096];
    char config[4096];
    if (Pico_ConfigDir(config, sizeof(config)) &&
        PicoPath_Format(path, sizeof(path), "%s/SYSTEM.md", config))
    {
        AppendFileSpan(&b, path, spans, span_count, PICO_PROMPT_SPAN_MAX, PICO_PROMPT_SOURCE_BASE);
    }
    if (root[0] && PicoPath_Format(path, sizeof(path), "%s/.pico/SYSTEM.md", root))
    {
        AppendFileSpan(&b, path, spans, span_count, PICO_PROMPT_SPAN_MAX,
                       PICO_PROMPT_SOURCE_WORKSPACE_SYSTEM);
    }
    if (!b.len)
    {
        JsonBuf_Puts(&b,
                     "You are coding assistant, working inside Pico agent harness. The user's workspace is the current "
                     "working directory. Use the sh tool to run shell commands when that helps. "
                     "Prefer concise answers.");
        PushSpan(spans, span_count, PICO_PROMPT_SPAN_MAX, PICO_PROMPT_SOURCE_BASE, 0, b.len);
    }
    if (root[0] && PicoPath_Format(path, sizeof(path), "%s/AGENTS.md", root))
    {
        AppendFileSpan(&b, path, spans, span_count, PICO_PROMPT_SPAN_MAX, PICO_PROMPT_SOURCE_AGENTS);
    }
    size_t before_docs = b.len;
    AppendDocsHint(&b);
    if (b.len > before_docs)
    {
        size_t start = before_docs ? before_docs + 2 : 0;
        PushSpan(spans, span_count, PICO_PROMPT_SPAN_MAX, PICO_PROMPT_SOURCE_BASE, start, b.len - start);
    }
    return JsonBuf_Steal(&b);
}

char *PicoSettings_LoadSystemPrompt(const PicoWorkspace *workspace)
{
    return PicoSettings_LoadSystemPromptSpans(workspace, NULL, NULL);
}
