#define _POSIX_C_SOURCE 200809L

#include "settings.h"
#include "agent.h"
#include "docs_path.h"
#include "json.h"
#include "overlay.h"
#include "path.h"
#include "posix_io.h"
#include "session.h"
#include "theme_internal.h"
#include "host_internal.h"
#include "workspace_internal.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <math.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

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
    if (!app || app->preferences.chat_width <= 0)
    {
        return 0.0f;
    }
    return (float)app->preferences.chat_width * ChatChWidth();
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

static void ApplyCompactAtWorkspace(PicoWorkspaceSettings *s, const JsonDoc *doc, int obj)
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

static bool DupOptionalModelField(const JsonDoc *doc, int obj, const char *key, char **out)
{
    int tok = JsonObjGet(doc, obj, key);
    *out = NULL;
    if (tok < 0 || JsonIsNull(doc, tok))
    {
        return true;
    }
    *out = JsonStrDup(doc, tok);
    return *out != NULL;
}

static bool ParseModel(const JsonDoc *doc, int obj, PicoModel *m)
{
    char *id = NULL;
    char *name = NULL;
    char *provider = NULL;
    char *base_url = NULL;
    char *selected = NULL;
    bool ok;
    memset(m, 0, sizeof(*m));
    if (!JsonIsObject(doc, obj))
    {
        return true;
    }
    ok = DupOptionalModelField(doc, obj, "id", &id) &&
         DupOptionalModelField(doc, obj, "name", &name) &&
         DupOptionalModelField(doc, obj, "provider", &provider) &&
         DupOptionalModelField(doc, obj, "base_url", &base_url) &&
         DupOptionalModelField(doc, obj, "selected_effort", &selected);
    if (!ok)
    {
        free(id);
        free(name);
        free(provider);
        free(base_url);
        free(selected);
        return false;
    }
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
        int item = JsonArrayAt(doc, arr, i);
        if (JsonIsNull(doc, item))
        {
            continue;
        }
        char *level = JsonStrDup(doc, item);
        if (!level)
        {
            free(id);
            free(name);
            free(provider);
            free(base_url);
            free(selected);
            return false;
        }
        if (level[0])
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
    return true;
}

static bool ReplaceModels(PicoModel **models, int *count, const JsonDoc *doc, int obj)
{
    int arr = JsonObjGet(doc, obj, "models");
    if (!JsonIsArray(doc, arr))
    {
        return true;
    }
    int n = JsonArrayLen(doc, arr);
    PicoModel *list = n > 0 ? (PicoModel *)calloc((size_t)n, sizeof(PicoModel)) : NULL;
    int got = 0;
    if (n > 0 && !list)
    {
        return false;
    }
    for (int i = 0; i < n; i++)
    {
        PicoModel m;
        if (!ParseModel(doc, JsonArrayAt(doc, arr, i), &m))
        {
            free(list);
            return false;
        }
        if (!m.id[0])
        {
            continue;
        }
        list[got++] = m;
    }
    free(*models);
    *models = list;
    *count = got;
    return true;
}

static void ApplyWorkspaceObject(PicoWorkspaceSettings *s, const JsonDoc *doc, int obj)
{
    if (!JsonIsObject(doc, obj))
    {
        return;
    }
    char *model = JsonObjStr(doc, obj, "model");
    CopyField(s->default_model, sizeof(s->default_model), model);
    int limit = JsonObjInt(doc, obj, "context_limit", 0);
    if (limit > 0)
    {
        s->context_limit_fallback = limit;
    }
    ApplyCompactAtWorkspace(s, doc, obj);
    int resume = JsonObjGet(doc, obj, "resume_last");
    if (resume >= 0)
    {
        s->resume_last = JsonEq(doc, resume, "true") || JsonEq(doc, resume, "1");
    }
    free(model);
}

static void ApplyPreferencesObject(PicoHostPreferences *p, const JsonDoc *doc, int obj)
{
    if (!JsonIsObject(doc, obj))
    {
        return;
    }
    char *font_scale = JsonObjRaw(doc, obj, "font_scale");
    if (font_scale)
    {
        double scale;
        if (ParseFontScale(font_scale, &scale))
        {
            p->font_scale = scale;
        }
        free(font_scale);
    }
    char *chat_width = JsonObjRaw(doc, obj, "chat_width");
    if (chat_width)
    {
        int width;
        if (ParseChatWidth(chat_width, &width))
        {
            p->chat_width = width;
        }
        free(chat_width);
    }
}

static bool DisabledNameListed(const char list[PICO_MAX_DISABLED_EXTENSIONS][PICO_DISABLED_EXT_NAME], int count,
                               const char *name)
{
    if (!name || !name[0])
    {
        return false;
    }
    for (int i = 0; i < count; i++)
    {
        if (strcmp(list[i], name) == 0)
        {
            return true;
        }
    }
    return false;
}

static bool ReservedExtensionName(const char *name)
{
    return name && name[0] && (strcmp(name, "extensions") == 0 || strcmp(name, "settings") == 0);
}

static void ApplyDisabledHostExtensions(PicoHostPreferences *p, const JsonDoc *doc, int obj)
{
    p->disabled_host_extension_count = 0;
    int arr = JsonObjGet(doc, obj, "disabled_host_extensions");
    if (!JsonIsArray(doc, arr))
    {
        return;
    }
    int n = JsonArrayLen(doc, arr);
    for (int i = 0; i < n && p->disabled_host_extension_count < PICO_MAX_DISABLED_EXTENSIONS; i++)
    {
        char *name = JsonStrDup(doc, JsonArrayAt(doc, arr, i));
        if (name && name[0] && !ReservedExtensionName(name) &&
            !DisabledNameListed(p->disabled_host_extensions, p->disabled_host_extension_count, name))
        {
            snprintf(p->disabled_host_extensions[p->disabled_host_extension_count], PICO_DISABLED_EXT_NAME, "%s",
                     name);
            p->disabled_host_extension_count++;
        }
        free(name);
    }
}

static void ApplyDisabledExtensions(PicoWorkspaceSettings *s, const JsonDoc *doc, int obj)
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
        if (name && name[0] && !ReservedExtensionName(name) &&
            !DisabledNameListed(s->disabled_extensions, s->disabled_extension_count, name))
        {
            snprintf(s->disabled_extensions[s->disabled_extension_count], PICO_DISABLED_EXT_NAME, "%s",
                     name);
            s->disabled_extension_count++;
        }
        free(name);
    }
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

static bool SettingsFileExists(const char *path)
{
    struct stat st;
    return path && path[0] && stat(path, &st) == 0 && S_ISREG(st.st_mode);
}

static bool UserSettingsPath(char *out, size_t cap)
{
    char dir[4096];
    return Pico_ConfigDir(dir, sizeof(dir)) &&
           PicoPath_Format(out, cap, "%s/settings.json", dir);
}

static bool CreateFileIfAbsent(const char *path, const char *data, size_t len)
{
    struct stat st;
    if (!path || !path[0] || !data)
    {
        return false;
    }
    if (lstat(path, &st) == 0)
    {
        return true;
    }
    if (errno != ENOENT)
    {
        return false;
    }

    char tmp[4096];
    if (snprintf(tmp, sizeof(tmp), "%s.tmp.XXXXXX", path) >= (int)sizeof(tmp))
    {
        return false;
    }
    int fd = mkstemp(tmp);
    if (fd < 0)
    {
        return false;
    }

    bool ok = PicoIO_WriteAll(fd, data, len);
    if (ok && fsync(fd) != 0)
    {
        ok = false;
    }
    if (close(fd) != 0)
    {
        ok = false;
    }
    if (!ok)
    {
        unlink(tmp);
        return false;
    }

    if (link(tmp, path) != 0)
    {
        int saved_errno = errno;
        unlink(tmp);
        return saved_errno == EEXIST;
    }
    bool removed = unlink(tmp) == 0;

    char dir[4096];
    snprintf(dir, sizeof(dir), "%s", path);
    char *slash = strrchr(dir, '/');
    if (slash)
    {
        *slash = '\0';
    }
    else
    {
        snprintf(dir, sizeof(dir), ".");
    }
    int dfd = open(dir, O_RDONLY | O_DIRECTORY);
    if (dfd < 0)
    {
        return false;
    }
    bool synced = fsync(dfd) == 0;
    if (close(dfd) != 0)
    {
        synced = false;
    }
    return removed && synced;
}

static void EnsureUserSettingsFile(void)
{
    char source[4096];
    char destination[4096];
    if (!Pico_DataPath("examples/settings.json", source, sizeof(source)) ||
        !UserSettingsPath(destination, sizeof(destination)))
    {
        return;
    }
    size_t len = 0;
    char *data = Pico_ReadFile(source, &len);
    if (!data)
    {
        return;
    }
    (void)CreateFileIfAbsent(destination, data, len);
    free(data);
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

void PicoHostPreferences_Load(PicoHost *host)
{
    if (!host)
    {
        return;
    }
    PicoHostPreferences *p = &host->preferences;
    memset(p, 0, sizeof(*p));
    p->font_scale = 1.0;
    p->chat_width = PICO_CHAT_WIDTH_DEFAULT;

    char dir[4096];
    if (Pico_ConfigDir(dir, sizeof(dir)))
    {
        Pico_MkdirP(dir);
        EnsureUserSettingsFile();
    }

    char path[4096];
    if (UserSettingsPath(path, sizeof(path)))
    {
        size_t len = 0;
        char *src = Pico_ReadFile(path, &len);
        if (src)
        {
            JsonStripComments(src, len);
            JsonDoc doc;
            if (JsonParse(&doc, src, len) == 0)
            {
                ApplyPreferencesObject(p, &doc, 0);
                ApplyDisabledHostExtensions(p, &doc, 0);
                JsonFree(&doc);
            }
            free(src);
        }
    }

    const char *font_scale = getenv("PICO_FONT_SCALE");
    if (font_scale && font_scale[0])
    {
        double scale;
        if (ParseFontScale(font_scale, &scale))
        {
            p->font_scale = scale;
        }
    }
    const char *chat_width = getenv("PICO_CHAT_WIDTH");
    if (chat_width && chat_width[0])
    {
        int width;
        if (ParseChatWidth(chat_width, &width))
        {
            p->chat_width = width;
        }
    }

    Pico_SetFontScale((float)p->font_scale);
}

static bool EnsureDefaultCatalog(const PicoWorkspaceSettings *settings, PicoModel **catalog, int *count)
{
    PicoModel *list;
    if (!settings || !catalog || !count || *count > 0)
    {
        return true;
    }
    list = (PicoModel *)calloc(1, sizeof(PicoModel));
    if (!list)
    {
        return false;
    }
    snprintf(list[0].id, sizeof(list[0].id), "%s", settings->default_model);
    snprintf(list[0].name, sizeof(list[0].name), "%s", settings->default_model);
    snprintf(list[0].provider, sizeof(list[0].provider), "%s", "openai");
    list[0].context_limit = settings->context_limit_fallback;
    snprintf(list[0].default_effort, sizeof(list[0].default_effort), "%s", "none");
    free(*catalog);
    *catalog = list;
    *count = 1;
    return true;
}

bool PicoWorkspaceSettings_Load(PicoWorkspace *workspace)
{
    PicoWorkspaceSettings next;
    PicoModel *catalog = NULL;
    int catalog_n = 0;
    char path[4096];
    const char *limit;
    const char *resume;
    const char *compact;
    const char *effort;
    if (!workspace)
    {
        return false;
    }
    memset(&next, 0, sizeof(next));
    snprintf(next.default_model, sizeof(next.default_model), "gpt-5.6-sol");
    next.context_limit_fallback = 128000;
    next.compact_enabled = true;
    next.compact_ratio = 0.9;

    if (UserSettingsPath(path, sizeof(path)))
    {
        size_t len = 0;
        char *src = Pico_ReadFile(path, &len);
        if (src)
        {
            JsonStripComments(src, len);
            JsonDoc doc;
            if (JsonParse(&doc, src, len) == 0)
            {
                ApplyWorkspaceObject(&next, &doc, 0);
                if (!ReplaceModels(&catalog, &catalog_n, &doc, 0))
                {
                    JsonFree(&doc);
                    free(src);
                    free(catalog);
                    return false;
                }
                ApplyDisabledExtensions(&next, &doc, 0);
                JsonFree(&doc);
            }
            free(src);
        }
    }

    if (WorkspaceSettingsPath(workspace, path, sizeof(path)) && path[0])
    {
        size_t len = 0;
        char *src = Pico_ReadFile(path, &len);
        if (src)
        {
            JsonStripComments(src, len);
            JsonDoc doc;
            if (JsonParse(&doc, src, len) == 0)
            {
                ApplyWorkspaceObject(&next, &doc, 0);
                if (!ReplaceModels(&catalog, &catalog_n, &doc, 0))
                {
                    JsonFree(&doc);
                    free(src);
                    free(catalog);
                    return false;
                }
                ApplyDisabledExtensions(&next, &doc, 0);
                JsonFree(&doc);
            }
            free(src);
        }
    }

    CopyField(next.default_model, sizeof(next.default_model), FirstEnv("PICO_MODEL", "OPENAI_MODEL"));
    limit = getenv("PICO_CONTEXT_LIMIT");
    if (limit && limit[0])
    {
        int n = atoi(limit);
        if (n > 0)
        {
            next.context_limit_fallback = n;
        }
    }
    resume = getenv("PICO_RESUME_LAST");
    if (resume && resume[0])
    {
        next.resume_last = !(resume[0] == '0' || resume[0] == 'f' || resume[0] == 'F' || resume[0] == 'n' ||
                             resume[0] == 'N');
    }
    compact = getenv("PICO_COMPACT_AT");
    if (compact && compact[0])
    {
        if (IsOffWord(compact))
        {
            next.compact_enabled = false;
        }
        else
        {
            double ratio;
            if (ParseRatio(compact, &ratio))
            {
                next.compact_enabled = true;
                next.compact_ratio = ratio;
            }
        }
    }
    if (!EnsureDefaultCatalog(&next, &catalog, &catalog_n))
    {
        free(catalog);
        return false;
    }
    effort = getenv("PICO_EFFORT");
    if (effort && effort[0])
    {
        for (int i = 0; i < catalog_n; i++)
        {
            if (strcmp(catalog[i].id, next.default_model) == 0)
            {
                snprintf(catalog[i].default_effort, sizeof(catalog[i].default_effort), "%s", effort);
                break;
            }
        }
    }
    free(workspace->models);
    workspace->settings = next;
    workspace->models = catalog;
    workspace->model_count = catalog_n;
    return true;
}

PicoModel *PicoSettings_FindModel(PicoWorkspace *workspace, const char *id)
{
    if (!workspace || !id || !id[0])
    {
        return NULL;
    }
    for (int i = 0; i < workspace->model_count; i++)
    {
        if (strcmp(workspace->models[i].id, id) == 0)
        {
            return &workspace->models[i];
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

const PicoModel *PicoSettings_FindModelConst(const PicoWorkspace *workspace, const char *id)
{
    return PicoSettings_FindModel((PicoWorkspace *)workspace, id);
}

PicoModel *PicoSettings_ActiveModel(const PicoAgent *agent)
{
    if (!agent)
    {
        return NULL;
    }
    if (agent->has_running_model)
    {
        return (PicoModel *)&agent->running_model;
    }
    return agent->workspace ? PicoSettings_FindModel(agent->workspace, agent->model) : NULL;
}

const PicoModel *PicoSettings_ActiveModelConst(const PicoAgent *agent)
{
    return PicoSettings_ActiveModel(agent);
}

const char *PicoSettings_ActiveEffort(const PicoAgent *agent)
{
    return agent && agent->effort[0] ? agent->effort : "none";
}

void PicoSettings_SyncAgent(PicoAgent *agent)
{
    if (!agent)
    {
        return;
    }
    const PicoModel *m = PicoSettings_ActiveModelConst(agent);
    snprintf(agent->model_name, sizeof(agent->model_name), "%s",
             m && m->name[0] ? m->name : agent->model);
    if (m && m->context_limit > 0)
    {
        agent->context_limit = m->context_limit;
    }
    else if (agent->workspace)
    {
        agent->context_limit = agent->workspace->settings.context_limit_fallback;
    }
    else
    {
        agent->context_limit = 128000;
    }
    if (!m || !PicoSettings_EffortAllowed(m, agent->effort))
    {
        const char *effort = m && PicoSettings_EffortAllowed(m, m->default_effort)
                                 ? m->default_effort
                                 : (m && m->effort_count > 0 ? m->effort[0] : "none");
        snprintf(agent->effort, sizeof(agent->effort), "%s", effort);
    }
}

void PicoSettings_InitAgent(PicoAgent *agent)
{
    if (!agent || !agent->workspace)
    {
        return;
    }
    snprintf(agent->model, sizeof(agent->model), "%s", agent->workspace->settings.default_model);
    agent->compact_enabled = agent->workspace->settings.compact_enabled;
    agent->compact_ratio = agent->workspace->settings.compact_ratio;
    agent->effort[0] = '\0';
    agent->has_running_model = false;
    PicoSettings_SyncAgent(agent);
}

void PicoSettings_ReconcileIdleAgent(PicoAgent *agent)
{
    if (!agent || !agent->workspace || PicoAgent_IsBusy(agent))
    {
        return;
    }
    agent->has_running_model = false;
    if (!PicoSettings_FindModel(agent->workspace, agent->model))
    {
        snprintf(agent->model, sizeof(agent->model), "%s", agent->workspace->settings.default_model);
        agent->effort[0] = '\0';
    }
    PicoSettings_SyncAgent(agent);
}

static PicoModel *FindCatalog(PicoWorkspace *workspace, const char *q)
{
    if (!workspace || !q || !q[0])
    {
        return NULL;
    }
    for (int i = 0; i < workspace->model_count; i++)
    {
        if (strcasecmp(workspace->models[i].id, q) == 0 || strcasecmp(workspace->models[i].name, q) == 0)
        {
            return &workspace->models[i];
        }
    }
    return NULL;
}

bool PicoSettings_SetModel(PicoAgent *agent, const char *id_or_name)
{
    if (!agent || !agent->workspace)
    {
        return false;
    }
    PicoWorkspace *workspace = agent->workspace;
    PicoHost *host = workspace->host;
    PicoModel *m = FindCatalog(workspace, id_or_name);
    if (!m)
    {
        char line[256];
        snprintf(line, sizeof(line), "Unknown model `%s`. Try `/model` for the catalog.",
                 id_or_name ? id_or_name : "");
        PicoOverlay_Notify(host, line);
        return false;
    }
    snprintf(agent->model, sizeof(agent->model), "%s", m->id);
    agent->effort[0] = '\0';
    PicoSettings_SyncAgent(agent);
    PicoSession_LogModelChange(host, agent, agent->model, PicoSettings_ActiveEffort(agent));
    if (agent->session_id[0])
    {
        PicoCatalog_SetSessionModel(PicoWorkspace_Path(workspace), agent->session_id, agent->model,
                                    PicoSettings_ActiveEffort(agent));
    }
    char line[256];
    snprintf(line, sizeof(line), "Model `%s` · effort `%s`", m->name[0] ? m->name : m->id,
             PicoSettings_ActiveEffort(agent));
    PicoOverlay_Notify(host, line);
    return true;
}

bool PicoSettings_SetEffort(PicoAgent *agent, const char *level)
{
    if (!agent || !agent->workspace)
    {
        return false;
    }
    PicoWorkspace *workspace = agent->workspace;
    PicoHost *host = workspace->host;
    PicoModel *m = PicoSettings_ActiveModel(agent);
    if (!m)
    {
        PicoOverlay_Notify(host, "No model in the catalog. Add one in settings.json.");
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
        PicoOverlay_Notify(host, line);
        return false;
    }
    snprintf(agent->effort, sizeof(agent->effort), "%s", level);
    PicoSession_LogModelChange(host, agent, agent->model, PicoSettings_ActiveEffort(agent));
    if (agent->session_id[0])
    {
        PicoCatalog_SetSessionModel(PicoWorkspace_Path(workspace), agent->session_id, agent->model,
                                    PicoSettings_ActiveEffort(agent));
    }
    char line[256];
    snprintf(line, sizeof(line), "Effort `%s` for `%s`", agent->effort, m->name[0] ? m->name : m->id);
    PicoOverlay_Notify(host, line);
    return true;
}

static bool AtomicWriteFile(const char *path, const char *data, size_t len, mode_t mode)
{
    if (!path || !path[0] || !data)
    {
        return false;
    }
    char dir[4096];
    snprintf(dir, sizeof(dir), "%s", path);
    char *slash = strrchr(dir, '/');
    if (slash)
    {
        *slash = '\0';
    }
    else
    {
        snprintf(dir, sizeof(dir), ".");
    }
    char tmp[4096];
    if (snprintf(tmp, sizeof(tmp), "%s.tmp.XXXXXX", path) >= (int)sizeof(tmp))
    {
        return false;
    }
    int fd = mkstemp(tmp);
    if (fd < 0)
    {
        return false;
    }
    bool ok = fchmod(fd, mode) == 0 && PicoIO_WriteAll(fd, data, len);
    if (ok && fsync(fd) != 0)
    {
        ok = false;
    }
    if (close(fd) != 0)
    {
        ok = false;
    }
    if (!ok || rename(tmp, path) != 0)
    {
        unlink(tmp);
        return false;
    }
    int dfd = open(dir, O_RDONLY | O_DIRECTORY);
    if (dfd < 0)
    {
        return false;
    }
    if (fsync(dfd) != 0)
    {
        close(dfd);
        return false;
    }
    if (close(dfd) != 0)
    {
        return false;
    }
    return true;
}

static bool WriteFile(const char *path, const char *data, size_t len)
{
    mode_t mode = 0600;
    struct stat st;
    if (path && stat(path, &st) == 0 && S_ISREG(st.st_mode))
    {
        mode = st.st_mode & 0777;
    }
    return AtomicWriteFile(path, data, len, mode);
}

static int SettingsLockAcquire(const char *settings_path)
{
    char lock_path[4102];
    if (!settings_path ||
        (size_t)snprintf(lock_path, sizeof(lock_path), "%s.lock", settings_path) >= sizeof(lock_path))
    {
        return -1;
    }
    int fd = open(lock_path, O_RDWR | O_CREAT | O_CLOEXEC, 0600);
    if (fd < 0)
    {
        return -1;
    }
    struct flock lock;
    memset(&lock, 0, sizeof(lock));
    lock.l_type = F_WRLCK;
    lock.l_whence = SEEK_SET;
    while (fcntl(fd, F_SETLKW, &lock) != 0)
    {
        if (errno == EINTR)
        {
            continue;
        }
        close(fd);
        return -1;
    }
    return fd;
}

static void SettingsLockRelease(int fd)
{
    if (fd >= 0)
    {
        close(fd);
    }
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

static bool PatchRootStringUnlocked(const char *path, const char *key, const char *value)
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

static bool PatchRootString(const char *path, const char *key, const char *value)
{
    int lock_fd = SettingsLockAcquire(path);
    if (lock_fd < 0)
    {
        return false;
    }
    bool ok = PatchRootStringUnlocked(path, key, value);
    SettingsLockRelease(lock_fd);
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

static bool PatchRootJsonUnlocked(const char *path, const char *key, const char *json_value)
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

static bool PatchRootJson(const char *path, const char *key, const char *json_value)
{
    int lock_fd = SettingsLockAcquire(path);
    if (lock_fd < 0)
    {
        return false;
    }
    bool ok = PatchRootJsonUnlocked(path, key, json_value);
    SettingsLockRelease(lock_fd);
    return ok;
}

static char *DisabledHostExtensionsJson(const PicoHostPreferences *p)
{
    JsonBuf b;
    JsonBuf_Init(&b);
    JsonBuf_Putc(&b, '[');
    for (int i = 0; i < p->disabled_host_extension_count; i++)
    {
        if (i)
        {
            JsonBuf_Putc(&b, ',');
        }
        JsonBuf_String(&b, p->disabled_host_extension_count ? p->disabled_host_extensions[i] : "");
    }
    JsonBuf_Putc(&b, ']');
    return JsonBuf_Steal(&b);
}

static bool SaveDisabledHostExtensions(PicoHost *host)
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
    char *json = DisabledHostExtensionsJson(&host->preferences);
    bool ok = json && PatchRootJson(path, "disabled_host_extensions", json);
    free(json);
    return ok;
}

bool PicoHost_SetExtensionDisabled(PicoHost *host, const char *name, bool disabled)
{
    if (!host || !name || !name[0] || ReservedExtensionName(name))
    {
        return false;
    }
    pthread_mutex_lock(&host->settings_mu);
    PicoHostPreferences *p = &host->preferences;
    int found = -1;
    for (int i = 0; i < p->disabled_host_extension_count; i++)
    {
        if (strcmp(p->disabled_host_extensions[i], name) == 0)
        {
            found = i;
            break;
        }
    }
    bool ok = false;
    if (disabled)
    {
        if (found >= 0)
        {
            ok = SaveDisabledHostExtensions(host);
            pthread_mutex_unlock(&host->settings_mu);
            return ok;
        }
        if (p->disabled_host_extension_count >= PICO_MAX_DISABLED_EXTENSIONS)
        {
            pthread_mutex_unlock(&host->settings_mu);
            return false;
        }
        snprintf(p->disabled_host_extensions[p->disabled_host_extension_count], PICO_DISABLED_EXT_NAME, "%s", name);
        p->disabled_host_extension_count++;
    }
    else if (found >= 0)
    {
        for (int i = found; i < p->disabled_host_extension_count - 1; i++)
        {
            memcpy(p->disabled_host_extensions[i], p->disabled_host_extensions[i + 1], PICO_DISABLED_EXT_NAME);
        }
        p->disabled_host_extension_count--;
        p->disabled_host_extensions[p->disabled_host_extension_count][0] = '\0';
    }
    ok = SaveDisabledHostExtensions(host);
    pthread_mutex_unlock(&host->settings_mu);
    return ok;
}

static char *DisabledExtensionsJson(const PicoWorkspaceSettings *s)
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

static bool SaveDisabledExtensions(PicoWorkspace *workspace)
{
    char path[4096];
    if (!WorkspaceSettingsPath(workspace, path, sizeof(path)) || !path[0])
    {
        return false;
    }
    char dir[4096];
    if (PicoPath_Format(dir, sizeof(dir), "%s/.pico", workspace->path))
    {
        Pico_MkdirP(dir);
    }
    char *json = DisabledExtensionsJson(&workspace->settings);
    bool ok = json && PatchRootJson(path, "disabled_extensions", json);
    free(json);
    return ok;
}

bool PicoWorkspace_SetExtensionDisabled(PicoWorkspace *workspace, const char *name, bool disabled)
{
    if (!workspace || !name || !name[0] || ReservedExtensionName(name))
    {
        return false;
    }
    pthread_mutex_lock(&workspace->settings_mu);
    PicoWorkspaceSettings *s = &workspace->settings;
    int found = -1;
    for (int i = 0; i < s->disabled_extension_count; i++)
    {
        if (strcmp(s->disabled_extensions[i], name) == 0)
        {
            found = i;
            break;
        }
    }
    bool ok = false;
    if (disabled)
    {
        if (found >= 0)
        {
            ok = SaveDisabledExtensions(workspace);
            pthread_mutex_unlock(&workspace->settings_mu);
            return ok;
        }
        if (s->disabled_extension_count >= PICO_MAX_DISABLED_EXTENSIONS)
        {
            pthread_mutex_unlock(&workspace->settings_mu);
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
    ok = SaveDisabledExtensions(workspace);
    pthread_mutex_unlock(&workspace->settings_mu);
    return ok;
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
    JsonBuf_Puts(b, ",\"vision\":");
    JsonBuf_Bool(b, m->vision);
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

static bool PatchSelectedEffortUnlocked(const char *path, PicoWorkspace *workspace, const PicoAgent *agent)
{
    PicoModel *active = PicoSettings_ActiveModel(agent);
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
        for (int i = 0; i < workspace->model_count; i++)
        {
            if (i)
            {
                JsonBuf_Putc(&b, ',');
            }
            WriteModelValue(&b, &workspace->models[i],
                            strcmp(workspace->models[i].id, agent->model) == 0 ? agent->effort : NULL);
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

static bool PatchSelectedEffort(const char *path, PicoWorkspace *workspace, const PicoAgent *agent)
{
    int lock_fd = SettingsLockAcquire(path);
    if (lock_fd < 0)
    {
        return false;
    }
    bool ok = PatchSelectedEffortUnlocked(path, workspace, agent);
    SettingsLockRelease(lock_fd);
    return ok;
}

bool PicoSettings_SaveSelection(const PicoAgent *agent, bool save_model, bool save_effort)
{
    if (!agent || !agent->workspace)
    {
        return false;
    }
    PicoWorkspace *workspace = agent->workspace;
    PicoHost *host = workspace->host;
    char user[4096];
    char ws_path[4096];
    if (!UserSettingsPath(user, sizeof(user)) ||
        !WorkspaceSettingsPath(workspace, ws_path, sizeof(ws_path)))
    {
        return false;
    }
    bool ok = true;
    if (save_model)
    {
        const char *path = SettingsFileExists(ws_path) ? ws_path : user;
        pthread_mutex_t *mu = (path == ws_path) ? &workspace->settings_mu : (host ? &host->settings_mu : NULL);
        if (mu) pthread_mutex_lock(mu);
        ok = PatchRootString(path, "model", agent->model) && ok;
        if (mu) pthread_mutex_unlock(mu);
    }
    if (save_effort)
    {
        const char *path = FileHasModels(ws_path) ? ws_path : (FileHasModels(user) ? user : NULL);
        if (!path)
        {
            path = SettingsFileExists(ws_path) ? ws_path : user;
        }
        pthread_mutex_t *mu = (path == ws_path) ? &workspace->settings_mu : (host ? &host->settings_mu : NULL);
        if (mu) pthread_mutex_lock(mu);
        ok = PatchSelectedEffort(path, workspace, agent) && ok;
        if (mu) pthread_mutex_unlock(mu);
    }
    return ok;
}

void PicoSettings_InitUserDraft(PicoUserSettingsDraft *draft)
{
    if (!draft)
    {
        return;
    }
    memset(draft, 0, sizeof(*draft));
    snprintf(draft->default_model, sizeof(draft->default_model), "gpt-5.6-sol");
    draft->context_limit_fallback = 128000;
    draft->compact_enabled = true;
    draft->compact_ratio = 0.9;
    draft->font_scale = 1.0;
    draft->chat_width = PICO_CHAT_WIDTH_DEFAULT;
}

void PicoSettings_FreeUserDraft(PicoUserSettingsDraft *draft)
{
    if (!draft)
    {
        return;
    }
    free(draft->models);
    free(draft->source_model_ids);
    draft->models = NULL;
    draft->source_model_ids = NULL;
    draft->model_count = 0;
}

static bool SeedUserDraftCatalog(PicoUserSettingsDraft *draft)
{
    PicoModel *list;
    const char *id;
    if (!draft)
    {
        return false;
    }
    list = (PicoModel *)calloc(1, sizeof(PicoModel));
    if (!list)
    {
        return false;
    }
    id = draft->default_model[0] ? draft->default_model : "gpt-5.6-sol";
    snprintf(list[0].id, sizeof(list[0].id), "%s", id);
    snprintf(list[0].name, sizeof(list[0].name), "%s", id);
    snprintf(list[0].provider, sizeof(list[0].provider), "%s", "openai");
    list[0].context_limit = draft->context_limit_fallback;
    snprintf(list[0].default_effort, sizeof(list[0].default_effort), "%s", "none");
    draft->models = list;
    draft->model_count = 1;
    if (!draft->default_model[0])
    {
        snprintf(draft->default_model, sizeof(draft->default_model), "%s", id);
    }
    return true;
}

bool PicoSettings_LoadUserDraft(PicoUserSettingsDraft *draft)
{
    PicoUserSettingsDraft next;
    PicoWorkspaceSettings workspace;
    PicoHostPreferences preferences;
    char path[4096];
    size_t len = 0;
    char *src;
    if (!draft)
    {
        return false;
    }
    PicoSettings_InitUserDraft(&next);
    memset(&workspace, 0, sizeof(workspace));
    snprintf(workspace.default_model, sizeof(workspace.default_model), "%s", next.default_model);
    workspace.context_limit_fallback = next.context_limit_fallback;
    workspace.compact_enabled = next.compact_enabled;
    workspace.compact_ratio = next.compact_ratio;
    workspace.resume_last = next.resume_last;
    memset(&preferences, 0, sizeof(preferences));
    preferences.font_scale = next.font_scale;
    preferences.chat_width = next.chat_width;
    if (UserSettingsPath(path, sizeof(path)))
    {
        src = Pico_ReadFile(path, &len);
        if (src)
        {
            JsonDoc doc;
            JsonStripComments(src, len);
            if (JsonParse(&doc, src, len) != 0)
            {
                free(src);
                return false;
            }
            ApplyWorkspaceObject(&workspace, &doc, 0);
            ApplyPreferencesObject(&preferences, &doc, 0);
            if (!ReplaceModels(&next.models, &next.model_count, &doc, 0))
            {
                JsonFree(&doc);
                free(src);
                PicoSettings_FreeUserDraft(&next);
                return false;
            }
            JsonFree(&doc);
            free(src);
        }
    }
    snprintf(next.default_model, sizeof(next.default_model), "%s", workspace.default_model);
    next.context_limit_fallback = workspace.context_limit_fallback;
    next.compact_enabled = workspace.compact_enabled;
    next.compact_ratio = workspace.compact_ratio;
    next.resume_last = workspace.resume_last;
    next.font_scale = preferences.font_scale;
    next.chat_width = preferences.chat_width;
    if (next.model_count == 0 && !SeedUserDraftCatalog(&next))
    {
        return false;
    }
    next.source_model_ids = calloc((size_t)next.model_count, sizeof(*next.source_model_ids));
    if (!next.source_model_ids)
    {
        PicoSettings_FreeUserDraft(&next);
        return false;
    }
    for (int i = 0; i < next.model_count; i++)
    {
        snprintf(next.source_model_ids[i], sizeof(next.source_model_ids[i]), "%s", next.models[i].id);
    }
    PicoSettings_FreeUserDraft(draft);
    *draft = next;
    return true;
}

bool PicoSettings_ParseModelContextLimit(const char *text, int *out)
{
    char *end = NULL;
    long value;
    if (!text || !text[0] || !out)
    {
        return false;
    }
    errno = 0;
    value = strtol(text, &end, 10);
    if (end == text || *end != '\0' || errno == ERANGE || value < 0 || value > INT_MAX)
    {
        return false;
    }
    *out = (int)value;
    return true;
}

const char *PicoSettings_ValidateUserDraft(const PicoUserSettingsDraft *draft)
{
    int i;
    int j;
    bool default_found = false;
    if (!draft)
    {
        return "Missing settings.";
    }
    if (draft->model_count <= 0 || !draft->models)
    {
        return "Add at least one model.";
    }
    if (draft->model_count > PICO_SETTINGS_MODEL_MAX)
    {
        return "Model catalog has more than 64 entries.";
    }
    if (!draft->default_model[0])
    {
        return "Default model is required.";
    }
    if (draft->context_limit_fallback <= 0)
    {
        return "Fallback context limit must be a positive integer.";
    }
    if (draft->compact_enabled &&
        (!isfinite(draft->compact_ratio) || draft->compact_ratio < 0.0 || draft->compact_ratio > 1.0))
    {
        return "Compact at must be between 0 and 1, or off.";
    }
    if (!isfinite(draft->font_scale) || draft->font_scale < 0.5 || draft->font_scale > 3.0)
    {
        return "Font scale must be between 0.5 and 3.0.";
    }
    if (draft->chat_width != 0 &&
        (draft->chat_width < PICO_CHAT_WIDTH_MIN || draft->chat_width > PICO_CHAT_WIDTH_MAX))
    {
        return "Chat width must be 0 or between 40 and 200.";
    }
    for (i = 0; i < draft->model_count; i++)
    {
        const PicoModel *m = &draft->models[i];
        if (!m->id[0])
        {
            return "Each model needs an id.";
        }
        if (!m->provider[0])
        {
            return "Each model needs a provider.";
        }
        if (m->context_limit < 0)
        {
            return "Model context limits must be zero or positive integers.";
        }
        for (j = 0; j < i; j++)
        {
            if (strcmp(draft->models[j].id, m->id) == 0)
            {
                return "Model ids must be unique.";
            }
        }
        if (m->effort_count > 0 && m->default_effort[0] &&
            !PicoSettings_EffortAllowed(m, m->default_effort))
        {
            return "Selected effort must be in the model's effort list.";
        }
        if (strcmp(m->id, draft->default_model) == 0)
        {
            default_found = true;
        }
    }
    if (!default_found)
    {
        return "Default model must be in the catalog.";
    }
    return NULL;
}

static bool ModelsEqual(const PicoModel *a, const PicoModel *b)
{
    int i;
    if (!a || !b || strcmp(a->id, b->id) != 0 || strcmp(a->name, b->name) != 0 ||
        strcmp(a->provider, b->provider) != 0 || strcmp(a->base_url, b->base_url) != 0 ||
        a->context_limit != b->context_limit || a->vision != b->vision ||
        a->effort_count != b->effort_count || strcmp(a->default_effort, b->default_effort) != 0)
    {
        return false;
    }
    for (i = 0; i < a->effort_count; i++)
    {
        if (strcmp(a->effort[i], b->effort[i]) != 0)
        {
            return false;
        }
    }
    return true;
}

static bool CatalogMatchesSource(const char *src, size_t len, const PicoUserSettingsDraft *draft,
                                 bool *matches)
{
    char *stripped;
    JsonDoc doc;
    PicoModel *models = NULL;
    int count = 0;
    bool equal;
    int i;
    if (!src || !draft || !matches)
    {
        return false;
    }
    *matches = false;
    stripped = (char *)malloc(len + 1);
    if (!stripped)
    {
        return false;
    }
    memcpy(stripped, src, len + 1);
    JsonStripComments(stripped, len);
    if (JsonParse(&doc, stripped, len) != 0)
    {
        free(stripped);
        return false;
    }
    if (!ReplaceModels(&models, &count, &doc, 0))
    {
        JsonFree(&doc);
        free(stripped);
        return false;
    }
    equal = count == draft->model_count;
    for (i = 0; equal && i < count; i++)
    {
        equal = ModelsEqual(&models[i], &draft->models[i]);
    }
    JsonFree(&doc);
    free(models);
    free(stripped);
    *matches = equal;
    return true;
}

static bool PatchObjectValue(char **src, size_t *len, const char *key, const char *json_value)
{
    char *stripped;
    JsonDoc doc;
    int tok;
    bool ok;
    if (!src || !*src || !len || !key || !json_value)
    {
        return false;
    }
    stripped = (char *)malloc(*len + 1);
    if (!stripped)
    {
        return false;
    }
    memcpy(stripped, *src, *len + 1);
    JsonStripComments(stripped, *len);
    if (JsonParse(&doc, stripped, *len) != 0 || !JsonIsObject(&doc, 0))
    {
        free(stripped);
        return false;
    }
    tok = JsonObjGet(&doc, 0, key);
    if (tok >= 0)
    {
        int start = JsonTokStart(&doc, tok);
        int end = JsonTokEnd(&doc, tok);
        if (start > 0 && end >= start && (*src)[start - 1] == '"' && (*src)[end] == '"')
        {
            start--;
            end++;
        }
        char *next = start >= 0 && end >= start ? Splice(*src, *len, start, end - start, json_value) : NULL;
        ok = next != NULL;
        if (next)
        {
            free(*src);
            *src = next;
            *len = strlen(next);
        }
    }
    else
    {
        ok = InsertObjectKey(src, len, &doc, 0, key, json_value);
    }
    JsonFree(&doc);
    free(stripped);
    return ok;
}

static char *EffortsJson(const PicoModel *model)
{
    JsonBuf b;
    int i;
    JsonBuf_Init(&b);
    JsonBuf_Putc(&b, '[');
    for (i = 0; model && i < model->effort_count; i++)
    {
        if (i)
        {
            JsonBuf_Putc(&b, ',');
        }
        JsonBuf_String(&b, model->effort[i]);
    }
    JsonBuf_Putc(&b, ']');
    return JsonBuf_Steal(&b);
}

static char *PatchExistingModel(const char *src, size_t len, const PicoModel *model)
{
    char context[32];
    char *out;
    char *name = NULL;
    char *id = NULL;
    char *provider = NULL;
    char *base_url = NULL;
    char *efforts = NULL;
    char *selected = NULL;
    bool ok;
    out = (char *)malloc(len + 1);
    if (!out)
    {
        return NULL;
    }
    memcpy(out, src, len);
    out[len] = '\0';
    name = JsonQuoted(model->name);
    id = JsonQuoted(model->id);
    provider = JsonQuoted(model->provider);
    base_url = JsonQuoted(model->base_url);
    efforts = EffortsJson(model);
    selected = JsonQuoted(model->default_effort[0] ? model->default_effort : "none");
    snprintf(context, sizeof(context), "%d", model->context_limit);
    ok = name && id && provider && base_url && efforts && selected &&
         PatchObjectValue(&out, &len, "name", name) &&
         PatchObjectValue(&out, &len, "id", id) &&
         PatchObjectValue(&out, &len, "provider", provider) &&
         PatchObjectValue(&out, &len, "base_url", base_url) &&
         PatchObjectValue(&out, &len, "context_limit", context) &&
         PatchObjectValue(&out, &len, "vision", model->vision ? "true" : "false") &&
         PatchObjectValue(&out, &len, "effort", efforts) &&
         PatchObjectValue(&out, &len, "selected_effort", selected);
    free(name);
    free(id);
    free(provider);
    free(base_url);
    free(efforts);
    free(selected);
    if (!ok)
    {
        free(out);
        return NULL;
    }
    return out;
}

static int FindModelObjectIndex(const JsonDoc *doc, int arr, const char *id)
{
    int n = JsonIsArray(doc, arr) ? JsonArrayLen(doc, arr) : 0;
    int i;
    for (i = 0; i < n; i++)
    {
        int obj = JsonArrayAt(doc, arr, i);
        char *found = JsonObjStr(doc, obj, "id");
        bool match = found && strcmp(found, id) == 0;
        free(found);
        if (match)
        {
            return i;
        }
    }
    return -1;
}

static void AppendTriviaWithoutComma(JsonBuf *b, const char *src, int start, int end)
{
    bool line_comment = false;
    bool block_comment = false;
    bool removed = false;
    int i;
    for (i = start; src && i >= 0 && i < end; i++)
    {
        char c = src[i];
        char next = i + 1 < end ? src[i + 1] : '\0';
        if (line_comment)
        {
            JsonBuf_Putc(b, c);
            if (c == '\n')
            {
                line_comment = false;
            }
            continue;
        }
        if (block_comment)
        {
            JsonBuf_Putc(b, c);
            if (c == '*' && next == '/')
            {
                JsonBuf_Putc(b, next);
                i++;
                block_comment = false;
            }
            continue;
        }
        if (c == '/' && next == '/')
        {
            JsonBuf_Putc(b, c);
            JsonBuf_Putc(b, next);
            i++;
            line_comment = true;
            continue;
        }
        if (c == '/' && next == '*')
        {
            JsonBuf_Putc(b, c);
            JsonBuf_Putc(b, next);
            i++;
            block_comment = true;
            continue;
        }
        if (c == ',' && !removed)
        {
            removed = true;
            continue;
        }
        JsonBuf_Putc(b, c);
    }
}

static char *ModelsJsonPreserving(const char *src, size_t len, const PicoUserSettingsDraft *draft)
{
    char *stripped = NULL;
    JsonDoc doc;
    int arr = -1;
    bool parsed = false;
    JsonBuf b;
    int i;
    stripped = src ? (char *)malloc(len + 1) : NULL;
    if (stripped)
    {
        memcpy(stripped, src, len + 1);
        JsonStripComments(stripped, len);
        parsed = JsonParse(&doc, stripped, len) == 0;
        if (parsed)
        {
            arr = JsonObjGet(&doc, 0, "models");
        }
    }
    JsonBuf_Init(&b);
    JsonBuf_Putc(&b, '[');
    for (i = 0; i < draft->model_count; i++)
    {
        const PicoModel *model = &draft->models[i];
        char *object = NULL;
        const char *source_id = draft->source_model_ids && draft->source_model_ids[i][0]
                                    ? draft->source_model_ids[i] : model->id;
        int source_index = parsed ? FindModelObjectIndex(&doc, arr, source_id) : -1;
        if (source_index < 0 && parsed && strcmp(source_id, model->id) != 0)
        {
            source_index = FindModelObjectIndex(&doc, arr, model->id);
        }
        int tok = source_index >= 0 ? JsonArrayAt(&doc, arr, source_index) : -1;
        if (tok >= 0)
        {
            int start = JsonTokStart(&doc, tok);
            int end = JsonTokEnd(&doc, tok);
            if (start >= 0 && end >= start)
            {
                object = PatchExistingModel(src + start, (size_t)(end - start), model);
            }
        }
        if (tok >= 0 && !object)
        {
            JsonBuf_Free(&b);
            if (parsed)
            {
                JsonFree(&doc);
            }
            free(stripped);
            return NULL;
        }
        if (i)
        {
            JsonBuf_Putc(&b, ',');
        }
        if (tok >= 0)
        {
            int trivia_start = source_index > 0 ? JsonTokEnd(&doc, JsonArrayAt(&doc, arr, source_index - 1))
                                               : JsonTokStart(&doc, arr) + 1;
            AppendTriviaWithoutComma(&b, src, trivia_start, JsonTokStart(&doc, tok));
        }
        else
        {
            JsonBuf_Puts(&b, "\n    ");
        }
        if (object)
        {
            JsonBuf_Puts(&b, object);
        }
        else
        {
            WriteModelValue(&b, model, NULL);
        }
        free(object);
    }
    if (parsed && JsonIsArray(&doc, arr) && JsonArrayLen(&doc, arr) > 0)
    {
        int last = JsonArrayAt(&doc, arr, JsonArrayLen(&doc, arr) - 1);
        AppendTriviaWithoutComma(&b, src, JsonTokEnd(&doc, last), JsonTokEnd(&doc, arr) - 1);
    }
    else if (draft->model_count > 0)
    {
        JsonBuf_Puts(&b, "\n  ");
    }
    JsonBuf_Putc(&b, ']');
    if (parsed)
    {
        JsonFree(&doc);
    }
    free(stripped);
    return JsonBuf_Steal(&b);
}

static bool ValidJsonc(const char *src, size_t len)
{
    char *stripped;
    bool valid;
    if (!src)
    {
        return false;
    }
    stripped = (char *)malloc(len + 1);
    if (!stripped)
    {
        return false;
    }
    memcpy(stripped, src, len + 1);
    JsonStripComments(stripped, len);
    valid = JsonValidSyntax(stripped, len);
    free(stripped);
    return valid;
}

bool PicoSettings_SaveUserDraft(PicoHost *host, const PicoUserSettingsDraft *draft)
{
    char path[4096];
    char dir[4096];
    char context_buf[32];
    char compact_buf[32];
    char font_buf[32];
    char width_buf[32];
    char *src = NULL;
    char *models = NULL;
    char *model = NULL;
    size_t len = 0;
    int lock_fd;
    bool catalog_matches;
    bool ok = false;
    const char *err = PicoSettings_ValidateUserDraft(draft);
    if (err || !UserSettingsPath(path, sizeof(path)))
    {
        return false;
    }
    if (Pico_ConfigDir(dir, sizeof(dir)))
    {
        Pico_MkdirP(dir);
    }
    EnsureUserSettingsFile();
    snprintf(context_buf, sizeof(context_buf), "%d", draft->context_limit_fallback);
    if (draft->compact_enabled)
    {
        snprintf(compact_buf, sizeof(compact_buf), "%.6g", draft->compact_ratio);
    }
    else
    {
        snprintf(compact_buf, sizeof(compact_buf), "%s", "null");
    }
    snprintf(font_buf, sizeof(font_buf), "%.6g", draft->font_scale);
    snprintf(width_buf, sizeof(width_buf), "%d", draft->chat_width);
    model = JsonQuoted(draft->default_model);
    if (!model)
    {
        return false;
    }
    if (host)
    {
        pthread_mutex_lock(&host->settings_mu);
    }
    lock_fd = SettingsLockAcquire(path);
    if (lock_fd < 0)
    {
        goto done;
    }
    src = Pico_ReadFile(path, &len);
    if (!src)
    {
        src = JsonDup("{}\n");
        len = src ? strlen(src) : 0;
    }
    if (!src)
    {
        goto unlock;
    }
    if (!CatalogMatchesSource(src, len, draft, &catalog_matches))
    {
        goto unlock;
    }
    if (!catalog_matches)
    {
        models = ModelsJsonPreserving(src, len, draft);
        if (!models || !PatchObjectValue(&src, &len, "models", models))
        {
            goto unlock;
        }
    }
    if (!PatchObjectValue(&src, &len, "model", model) ||
        !PatchObjectValue(&src, &len, "context_limit", context_buf) ||
        !PatchObjectValue(&src, &len, "compact_at", compact_buf) ||
        !PatchObjectValue(&src, &len, "resume_last", draft->resume_last ? "true" : "false") ||
        !PatchObjectValue(&src, &len, "font_scale", font_buf) ||
        !PatchObjectValue(&src, &len, "chat_width", width_buf))
    {
        goto unlock;
    }
    if (!ValidJsonc(src, len))
    {
        goto unlock;
    }
    ok = WriteFile(path, src, len);
unlock:
    SettingsLockRelease(lock_fd);
done:
    if (host)
    {
        pthread_mutex_unlock(&host->settings_mu);
    }
    free(src);
    free(models);
    free(model);
    return ok;
}

bool PicoSettings_ApplyUserDraft(PicoHost *host)
{
    bool all_applied = true;
    int i;
    if (!host)
    {
        return false;
    }
    PicoHostPreferences_Load(host);
    for (i = 0; i < host->workspace_count; i++)
    {
        PicoWorkspace *workspace = host->workspaces[i];
        PicoModel snapshots[PICO_MAX_AGENTS];
        bool snapshot_valid[PICO_MAX_AGENTS];
        int j;
        memset(snapshot_valid, 0, sizeof(snapshot_valid));
        if (!workspace)
        {
            continue;
        }
        for (j = 0; j < workspace->count; j++)
        {
            PicoAgent *agent = workspace->agents[j];
            const PicoModel *model;
            if (!agent || !PicoAgent_IsBusy(agent) || agent->has_running_model)
            {
                continue;
            }
            model = PicoSettings_ActiveModelConst(agent);
            if (model)
            {
                snapshots[j] = *model;
                snapshot_valid[j] = true;
            }
        }
        if (!PicoWorkspaceSettings_Load(workspace))
        {
            all_applied = false;
            continue;
        }
        for (j = 0; j < workspace->count; j++)
        {
            PicoAgent *agent = workspace->agents[j];
            if (agent && snapshot_valid[j])
            {
                agent->running_model = snapshots[j];
                agent->has_running_model = true;
            }
            PicoSettings_ReconcileIdleAgent(agent);
        }
    }
    return all_applied;
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

