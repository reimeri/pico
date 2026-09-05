#include "pico/plugin.h"
#include "scrollbar.h"
#include "host_internal.h"
#include "overlay.h"
#include "settings.h"

#include "clay/clay.h"
#include "docs_path.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum {
    FOCUS_NONE = 0,
    FOCUS_CONTEXT_LIMIT,
    FOCUS_COMPACT_AT,
    FOCUS_FONT_SCALE,
    FOCUS_CHAT_WIDTH,
    FOCUS_MODEL_ID,
    FOCUS_MODEL_NAME,
    FOCUS_MODEL_PROVIDER,
    FOCUS_MODEL_BASE_URL,
    FOCUS_MODEL_CONTEXT,
    FOCUS_CUSTOM_EFFORT,
};

typedef struct SettingsState {
    PicoHost *host;
    bool open;
    bool overflow;
    PicoScrollbar scrollbar;
    PicoScrollbar model_dropdown_scrollbar;
    PicoUserSettingsDraft draft;
    bool *expanded;
    char context_limit[32];
    char compact_at[32];
    char font_scale[32];
    char chat_width[32];
    char model_contexts[PICO_SETTINGS_MODEL_MAX][32];
    char custom_effort[PICO_EFFORT_LEN];
    char error[256];
    int focus_kind;
    int focus_model;
    bool model_dropdown;
    int model_dropdown_selected;
    bool model_dropdown_click_block;
    bool model_dropdown_ensure_visible;
    Texture2D trash_icon;
    Texture2D trash_icon_disabled;
    bool trash_icon_tried;
} SettingsState;

static __thread SettingsState *s_active_settings_state = NULL;
static char s_caret[520];

#define g_host (s_active_settings_state->host)
#define g_open (s_active_settings_state->open)
#define g_overflow (s_active_settings_state->overflow)
#define g_scrollbar (s_active_settings_state->scrollbar)
#define g_draft (s_active_settings_state->draft)
#define g_error (s_active_settings_state->error)

static PicoModel *ModelAt(SettingsState *s, int index);
static void FlushModelContext(SettingsState *s);
static const char *kEffortPresets[] = {"none", "low", "medium", "high", "xhigh"};
static const int kEffortPresetCount = 5;

#define SETTINGS_ROW_BEGIN                                                                                             \
    CLAY_AUTO_ID({.layout = {.layoutDirection = CLAY_TOP_TO_BOTTOM,                                                    \
                             .childGap = 4,                                                                            \
                             .padding = {.bottom = 12},                                                                \
                             .sizing = {.width = CLAY_SIZING_GROW(0)}}})                                               \
    {

static Clay_String CStr(const char *s)
{
    if (!s)
    {
        s = "";
    }
    return (Clay_String){.length = (int32_t)strlen(s), .chars = s};
}

static Clay_String CaretText(const char *s)
{
    snprintf(s_caret, sizeof(s_caret), "%s|", s ? s : "");
    return CStr(s_caret);
}

static bool CompactOff(const char *s)
{
    return !s || !s[0] || strcmp(s, "null") == 0 || strcmp(s, "off") == 0 || strcmp(s, "false") == 0 ||
           strcmp(s, "none") == 0;
}

static void ClearFocus(SettingsState *s)
{
    if (!s)
    {
        return;
    }
    s->focus_kind = FOCUS_NONE;
    s->focus_model = -1;
    s->model_dropdown = false;
    s->custom_effort[0] = '\0';
}

static bool ResetExpanded(SettingsState *s)
{
    free(s->expanded);
    s->expanded = NULL;
    if (s->draft.model_count <= 0)
    {
        return true;
    }
    s->expanded = (bool *)calloc((size_t)s->draft.model_count, sizeof(bool));
    if (!s->expanded)
    {
        return false;
    }
    if (s->draft.model_count == 1)
    {
        s->expanded[0] = true;
    }
    return true;
}

static void SyncFieldsFromDraft(SettingsState *s)
{
    int i;
    if (!s)
    {
        return;
    }
    snprintf(s->context_limit, sizeof(s->context_limit), "%d", s->draft.context_limit_fallback);
    if (s->draft.compact_enabled)
    {
        snprintf(s->compact_at, sizeof(s->compact_at), "%g", s->draft.compact_ratio);
    }
    else
    {
        snprintf(s->compact_at, sizeof(s->compact_at), "%s", "off");
    }
    snprintf(s->font_scale, sizeof(s->font_scale), "%g", s->draft.font_scale);
    snprintf(s->chat_width, sizeof(s->chat_width), "%d", s->draft.chat_width);
    memset(s->model_contexts, 0, sizeof(s->model_contexts));
    for (i = 0; i < s->draft.model_count && i < PICO_SETTINGS_MODEL_MAX; i++)
    {
        snprintf(s->model_contexts[i], sizeof(s->model_contexts[i]), "%d", s->draft.models[i].context_limit);
    }
}

static bool LoadDraft(SettingsState *s)
{
    if (!s || !PicoSettings_LoadUserDraft(&s->draft) ||
        s->draft.model_count > PICO_SETTINGS_MODEL_MAX || !ResetExpanded(s))
    {
        return false;
    }
    SyncFieldsFromDraft(s);
    ClearFocus(s);
    s->error[0] = '\0';
    return true;
}

static void DiscardDraft(SettingsState *s)
{
    if (!s)
    {
        return;
    }
    PicoSettings_FreeUserDraft(&s->draft);
    free(s->expanded);
    s->expanded = NULL;
    ClearFocus(s);
    s->error[0] = '\0';
    s->overflow = false;
    memset(&s->scrollbar, 0, sizeof(s->scrollbar));
    memset(&s->model_dropdown_scrollbar, 0, sizeof(s->model_dropdown_scrollbar));
}

static bool Claim(void)
{
    if (g_open)
    {
        return true;
    }
    if (!g_host || !pico_ui_modal_push(g_host, "settings"))
    {
        return false;
    }
    g_open = true;
    if (!LoadDraft(s_active_settings_state))
    {
        (void)pico_ui_modal_pop(g_host, "settings");
        g_open = false;
        DiscardDraft(s_active_settings_state);
        PicoOverlay_Notify(g_host, "Could not load settings.json.");
        return false;
    }
    return true;
}

static bool Unclaim(void)
{
    if (!g_open)
    {
        return true;
    }
    if (g_host && !pico_ui_modal_pop(g_host, "settings"))
    {
        return false;
    }
    g_open = false;
    DiscardDraft(s_active_settings_state);
    return true;
}

static bool SelectHost(PicoHost *host)
{
    s_active_settings_state = host ? (SettingsState *)PicoPlugins_HostState(host, "settings") : NULL;
    return s_active_settings_state != NULL;
}

void PicoSettingsUi_Close(PicoHost *host)
{
    if (!SelectHost(host))
    {
        return;
    }
    (void)Unclaim();
}

void PicoSettingsUi_Open(PicoHost *host)
{
    if (!SelectHost(host))
    {
        return;
    }
    PicoPrompt_Close(host);
    PicoExts_Close(host);
    SelectHost(host);
    Claim();
}

bool PicoSettingsUi_IsOpen(const PicoHost *host)
{
    SettingsState *s = host ? (SettingsState *)PicoPlugins_HostState(host, "settings") : NULL;
    return s && s->open;
}

static bool ParseFieldsIntoDraft(SettingsState *s)
{
    char *end = NULL;
    long limit;
    long width;
    double scale;
    double ratio;
    int i;
    if (!s)
    {
        return false;
    }
    FlushModelContext(s);
    limit = strtol(s->context_limit, &end, 10);
    if (end == s->context_limit || *end != '\0' || limit <= 0 || limit > 2147483647L)
    {
        snprintf(s->error, sizeof(s->error), "%s", "Fallback context limit must be a positive integer.");
        return false;
    }
    s->draft.context_limit_fallback = (int)limit;
    if (CompactOff(s->compact_at))
    {
        s->draft.compact_enabled = false;
    }
    else
    {
        ratio = strtod(s->compact_at, &end);
        if (end == s->compact_at || *end != '\0')
        {
            snprintf(s->error, sizeof(s->error), "%s", "Compact at must be between 0 and 1, or off.");
            return false;
        }
        s->draft.compact_enabled = true;
        s->draft.compact_ratio = ratio;
    }
    scale = strtod(s->font_scale, &end);
    if (end == s->font_scale || *end != '\0')
    {
        snprintf(s->error, sizeof(s->error), "%s", "Font scale must be between 0.5 and 3.0.");
        return false;
    }
    s->draft.font_scale = scale;
    width = strtol(s->chat_width, &end, 10);
    if (end == s->chat_width || *end != '\0' || width < 0 || width > 2147483647L)
    {
        snprintf(s->error, sizeof(s->error), "%s", "Chat width must be 0 or between 40 and 200.");
        return false;
    }
    s->draft.chat_width = (int)width;
    if (s->draft.model_count > PICO_SETTINGS_MODEL_MAX)
    {
        snprintf(s->error, sizeof(s->error), "%s", "Model catalog has more than 64 entries.");
        return false;
    }
    for (i = 0; i < s->draft.model_count; i++)
    {
        int model_limit;
        PicoModel *m = &s->draft.models[i];
        if (!PicoSettings_ParseModelContextLimit(s->model_contexts[i], &model_limit))
        {
            snprintf(s->error, sizeof(s->error), "%s", "Model context limits must be zero or positive integers.");
            return false;
        }
        m->context_limit = model_limit;
        if (!m->name[0] && m->id[0])
        {
            snprintf(m->name, sizeof(m->name), "%s", m->id);
        }
        if (!m->default_effort[0])
        {
            snprintf(m->default_effort, sizeof(m->default_effort), "%s",
                     m->effort_count > 0 ? m->effort[0] : "none");
        }
    }
    return true;
}

static bool ApplyDraft(SettingsState *s)
{
    const char *err;
    if (!ParseFieldsIntoDraft(s))
    {
        return false;
    }
    err = PicoSettings_ValidateUserDraft(&s->draft);
    if (err)
    {
        snprintf(s->error, sizeof(s->error), "%s", err);
        return false;
    }
    if (!PicoSettings_SaveUserDraft(s->host, &s->draft))
    {
        snprintf(s->error, sizeof(s->error), "%s", "Could not write settings.json.");
        return false;
    }
    if (!PicoSettings_ApplyUserDraft(s->host))
    {
        snprintf(s->error, sizeof(s->error), "%s", "Settings saved, but could not reload them in memory.");
        return false;
    }
    PicoOverlay_Notify(s->host, "Settings saved.");
    return true;
}

static PicoModel *ModelAt(SettingsState *s, int index)
{
    if (!s || index < 0 || index >= s->draft.model_count || !s->draft.models)
    {
        return NULL;
    }
    return &s->draft.models[index];
}

static int FindEffort(const PicoModel *m, const char *level)
{
    int i;
    if (!m || !level || !level[0])
    {
        return -1;
    }
    for (i = 0; i < m->effort_count; i++)
    {
        if (strcmp(m->effort[i], level) == 0)
        {
            return i;
        }
    }
    return -1;
}

static void ToggleEffort(PicoModel *m, const char *level)
{
    int found;
    int i;
    if (!m || !level || !level[0])
    {
        return;
    }
    found = FindEffort(m, level);
    if (found >= 0)
    {
        for (i = found; i < m->effort_count - 1; i++)
        {
            memcpy(m->effort[i], m->effort[i + 1], PICO_EFFORT_LEN);
        }
        m->effort_count--;
        m->effort[m->effort_count][0] = '\0';
        if (strcmp(m->default_effort, level) == 0)
        {
            snprintf(m->default_effort, sizeof(m->default_effort), "%s",
                     m->effort_count > 0 ? m->effort[0] : "none");
        }
        return;
    }
    if (m->effort_count >= PICO_MAX_EFFORTS)
    {
        return;
    }
    snprintf(m->effort[m->effort_count], PICO_EFFORT_LEN, "%s", level);
    m->effort_count++;
    if (!m->default_effort[0])
    {
        snprintf(m->default_effort, sizeof(m->default_effort), "%s", level);
    }
}

static bool AddCustomEffort(SettingsState *s, int model_index)
{
    PicoModel *m = ModelAt(s, model_index);
    if (!m || !s->custom_effort[0])
    {
        return false;
    }
    if (FindEffort(m, s->custom_effort) < 0)
    {
        ToggleEffort(m, s->custom_effort);
    }
    s->custom_effort[0] = '\0';
    return true;
}

static bool AddModel(SettingsState *s)
{
    PicoModel *models;
    char (*source_ids)[128];
    bool *expanded;
    PicoModel *m;
    int n;
    if (!s || !s->expanded || s->draft.model_count >= PICO_SETTINGS_MODEL_MAX)
    {
        snprintf(s->error, sizeof(s->error), "%s", "Model catalog is full.");
        return false;
    }
    n = s->draft.model_count + 1;
    models = (PicoModel *)malloc((size_t)n * sizeof(PicoModel));
    source_ids = calloc((size_t)n, sizeof(*source_ids));
    expanded = (bool *)malloc((size_t)n * sizeof(bool));
    if (!models || !source_ids || !expanded)
    {
        free(models);
        free(source_ids);
        free(expanded);
        snprintf(s->error, sizeof(s->error), "%s", "Out of memory.");
        return false;
    }
    if (s->draft.model_count > 0)
    {
        memcpy(models, s->draft.models, (size_t)s->draft.model_count * sizeof(PicoModel));
        if (s->draft.source_model_ids)
        {
            memcpy(source_ids, s->draft.source_model_ids,
                   (size_t)s->draft.model_count * sizeof(*source_ids));
        }
        memcpy(expanded, s->expanded, (size_t)s->draft.model_count * sizeof(bool));
    }
    free(s->draft.models);
    free(s->draft.source_model_ids);
    free(s->expanded);
    s->draft.models = models;
    s->draft.source_model_ids = source_ids;
    s->expanded = expanded;
    m = &s->draft.models[s->draft.model_count];
    memset(m, 0, sizeof(*m));
    snprintf(m->provider, sizeof(m->provider), "%s", "openai");
    snprintf(m->default_effort, sizeof(m->default_effort), "%s", "none");
    memset(s->expanded, 0, (size_t)s->draft.model_count * sizeof(bool));
    s->expanded[s->draft.model_count] = true;
    s->draft.model_count = n;
    snprintf(s->model_contexts[n - 1], sizeof(s->model_contexts[n - 1]), "%d", m->context_limit);
    s->focus_kind = FOCUS_MODEL_ID;
    s->focus_model = n - 1;
    s->error[0] = '\0';
    return true;
}

static void RemoveModel(SettingsState *s, int index)
{
    int i;
    char removed[128];
    if (!s || !s->expanded || index < 0 || index >= s->draft.model_count || s->draft.model_count <= 1)
    {
        if (s && s->draft.model_count <= 1)
        {
            snprintf(s->error, sizeof(s->error), "%s", "Keep at least one model.");
        }
        return;
    }
    snprintf(removed, sizeof(removed), "%s", s->draft.models[index].id);
    for (i = index; i < s->draft.model_count - 1; i++)
    {
        s->draft.models[i] = s->draft.models[i + 1];
        if (s->draft.source_model_ids)
        {
            memcpy(s->draft.source_model_ids[i], s->draft.source_model_ids[i + 1],
                   sizeof(s->draft.source_model_ids[i]));
        }
        s->expanded[i] = s->expanded[i + 1];
        memcpy(s->model_contexts[i], s->model_contexts[i + 1], sizeof(s->model_contexts[i]));
    }
    s->draft.model_count--;
    if (strcmp(s->draft.default_model, removed) == 0)
    {
        snprintf(s->draft.default_model, sizeof(s->draft.default_model), "%s", s->draft.models[0].id);
    }
    if (s->focus_model == index)
    {
        ClearFocus(s);
    }
    else if (s->focus_model > index)
    {
        s->focus_model--;
    }
    s->error[0] = '\0';
}

static char *FocusBuf(SettingsState *s, size_t *cap)
{
    PicoModel *m;
    if (!s || !cap)
    {
        return NULL;
    }
    switch (s->focus_kind)
    {
    case FOCUS_CONTEXT_LIMIT:
        *cap = sizeof(s->context_limit);
        return s->context_limit;
    case FOCUS_COMPACT_AT:
        *cap = sizeof(s->compact_at);
        return s->compact_at;
    case FOCUS_FONT_SCALE:
        *cap = sizeof(s->font_scale);
        return s->font_scale;
    case FOCUS_CHAT_WIDTH:
        *cap = sizeof(s->chat_width);
        return s->chat_width;
    case FOCUS_CUSTOM_EFFORT:
        *cap = sizeof(s->custom_effort);
        return s->custom_effort;
    case FOCUS_MODEL_CONTEXT:
        if (s->focus_model < 0 || s->focus_model >= PICO_SETTINGS_MODEL_MAX)
        {
            return NULL;
        }
        *cap = sizeof(s->model_contexts[s->focus_model]);
        return s->model_contexts[s->focus_model];
    default:
        break;
    }
    m = ModelAt(s, s->focus_model);
    if (!m)
    {
        return NULL;
    }
    switch (s->focus_kind)
    {
    case FOCUS_MODEL_ID:
        *cap = sizeof(m->id);
        return m->id;
    case FOCUS_MODEL_NAME:
        *cap = sizeof(m->name);
        return m->name;
    case FOCUS_MODEL_PROVIDER:
        *cap = sizeof(m->provider);
        return m->provider;
    case FOCUS_MODEL_BASE_URL:
        *cap = sizeof(m->base_url);
        return m->base_url;
    default:
        break;
    }
    return NULL;
}

static void FlushModelContext(SettingsState *s)
{
    PicoModel *m;
    int limit;
    if (!s || s->focus_kind != FOCUS_MODEL_CONTEXT)
    {
        return;
    }
    m = ModelAt(s, s->focus_model);
    if (!m || s->focus_model < 0 || s->focus_model >= PICO_SETTINGS_MODEL_MAX)
    {
        return;
    }
    if (PicoSettings_ParseModelContextLimit(s->model_contexts[s->focus_model], &limit))
    {
        m->context_limit = limit;
    }
}

static void SetFocus(SettingsState *s, int kind, int model)
{
    PicoModel *m;
    if (!s)
    {
        return;
    }
    FlushModelContext(s);
    s->focus_kind = kind;
    s->focus_model = model;
    s->model_dropdown = false;
    if (kind != FOCUS_CUSTOM_EFFORT)
    {
        s->custom_effort[0] = '\0';
    }
    if (kind == FOCUS_MODEL_CONTEXT)
    {
        m = ModelAt(s, model);
        if (m && model >= 0 && model < PICO_SETTINGS_MODEL_MAX)
        {
            snprintf(s->model_contexts[model], sizeof(s->model_contexts[model]), "%d", m->context_limit);
        }
    }
}

static void InsertAscii(char *buf, size_t cap, int cp)
{
    size_t n;
    if (!buf || cp < 32 || cp > 126)
    {
        return;
    }
    n = strlen(buf);
    if (n + 1 >= cap)
    {
        return;
    }
    buf[n] = (char)cp;
    buf[n + 1] = '\0';
}

static void Backspace(char *buf)
{
    size_t n;
    if (!buf)
    {
        return;
    }
    n = strlen(buf);
    if (n == 0)
    {
        return;
    }
    buf[n - 1] = '\0';
}

static bool OverId(Clay_String id)
{
    return Clay_PointerOver(Clay_GetElementId(id));
}

static void RenderLabel(const char *label)
{
    CLAY_TEXT(CStr(label), CLAY_TEXT_CONFIG({.fontId = FONT_REGULAR,
                                            .fontSize = PICO_FONT_CAPTION,
                                            .textColor = COLOR_MUTED,
                                            .wrapMode = CLAY_TEXT_WRAP_NONE}));
}

static void RenderField(Clay_ElementId id, const char *value, const char *placeholder, bool focused)
{
    bool hover = Clay_PointerOver(id);
    Clay_Color bg = focused ? (Clay_Color){54, 54, 66, 255} : (hover ? COLOR_CODE_BG : COLOR_COMPOSER_BG);
    CLAY(id, {.layout = {.padding = {8, 8, 6, 6}, .sizing = {.width = CLAY_SIZING_GROW(0)}},
              .backgroundColor = bg,
              .cornerRadius = CLAY_CORNER_RADIUS(6)})
    {
        if (focused)
        {
            CLAY_TEXT(CaretText(value), CLAY_TEXT_CONFIG({.fontId = FONT_REGULAR,
                                                         .fontSize = PICO_FONT_UI,
                                                         .textColor = COLOR_TEXT,
                                                         .wrapMode = CLAY_TEXT_WRAP_NONE}));
        }
        else if (value && value[0])
        {
            CLAY_TEXT(CStr(value), CLAY_TEXT_CONFIG({.fontId = FONT_REGULAR,
                                                    .fontSize = PICO_FONT_UI,
                                                    .textColor = COLOR_TEXT,
                                                    .wrapMode = CLAY_TEXT_WRAP_NONE}));
        }
        else
        {
            CLAY_TEXT(CStr(placeholder ? placeholder : ""),
                      CLAY_TEXT_CONFIG({.fontId = FONT_REGULAR,
                                        .fontSize = PICO_FONT_UI,
                                        .textColor = COLOR_MUTED,
                                        .wrapMode = CLAY_TEXT_WRAP_NONE}));
        }
    }
}

static void RenderButton(Clay_ElementId id, const char *label, bool enabled, bool primary)
{
    bool hover = enabled && Clay_PointerOver(id);
    Clay_Color bg = primary ? (Clay_Color){74, 104, 180, 255} : COLOR_FOOTER_BG;
    Clay_Color text = enabled ? COLOR_TEXT : COLOR_MUTED;
    if (!enabled)
    {
        bg = (Clay_Color){38, 38, 44, 255};
    }
    else if (hover)
    {
        bg = primary ? (Clay_Color){92, 126, 210, 255} : COLOR_CODE_BG;
    }
    CLAY(id, {.layout = {.padding = {14, 14, 8, 8}}, .backgroundColor = bg, .cornerRadius = CLAY_CORNER_RADIUS(6)})
    {
        CLAY_TEXT(CStr(label), CLAY_TEXT_CONFIG({.fontId = FONT_BOLD,
                                                .fontSize = PICO_FONT_UI,
                                                .textColor = text,
                                                .wrapMode = CLAY_TEXT_WRAP_NONE}));
    }
}

static void EnsureTrashIcon(SettingsState *s)
{
    char path[4096];
    Image image;
    Image disabled_image;
    if (!s || s->trash_icon_tried || !IsWindowReady())
    {
        return;
    }
    s->trash_icon_tried = true;
    if (!Pico_DataPath("resources/trash.png", path, sizeof(path)))
    {
        snprintf(path, sizeof(path), "%s", "resources/trash.png");
    }
    image = LoadImage(path);
    if (!image.data)
    {
        return;
    }
    disabled_image = ImageCopy(image);
    ImageColorTint(&image, (Color){(unsigned char)COLOR_TEXT.r, (unsigned char)COLOR_TEXT.g,
                                   (unsigned char)COLOR_TEXT.b, (unsigned char)COLOR_TEXT.a});
    s->trash_icon = LoadTextureFromImage(image);
    UnloadImage(image);
    if (disabled_image.data)
    {
        ImageColorTint(&disabled_image, (Color){(unsigned char)COLOR_MUTED.r, (unsigned char)COLOR_MUTED.g,
                                                (unsigned char)COLOR_MUTED.b, (unsigned char)COLOR_MUTED.a});
        s->trash_icon_disabled = LoadTextureFromImage(disabled_image);
        UnloadImage(disabled_image);
    }
    if (s->trash_icon.id != 0)
    {
        SetTextureFilter(s->trash_icon, TEXTURE_FILTER_BILINEAR);
    }
    if (s->trash_icon_disabled.id != 0)
    {
        SetTextureFilter(s->trash_icon_disabled, TEXTURE_FILTER_BILINEAR);
    }
}

static void RenderTrashButton(SettingsState *s, Clay_ElementId id, bool enabled)
{
    float icon_size = Pico_FontPx(18);
    float button_size = icon_size + 12.0f;
    bool hover = enabled && Clay_PointerOver(id);
    Texture2D *icon = s ? (enabled ? &s->trash_icon : &s->trash_icon_disabled) : NULL;
    CLAY(id, {.layout = {.childAlignment = {.x = CLAY_ALIGN_X_CENTER, .y = CLAY_ALIGN_Y_CENTER},
                         .sizing = {.width = CLAY_SIZING_FIXED(button_size),
                                    .height = CLAY_SIZING_FIXED(button_size)}},
              .backgroundColor = hover ? COLOR_CODE_BG : (Clay_Color){0, 0, 0, 0},
              .cornerRadius = CLAY_CORNER_RADIUS(6)})
    {
        if (icon && icon->id != 0)
        {
            CLAY_AUTO_ID({.layout = {.sizing = {.width = CLAY_SIZING_FIXED(icon_size),
                                                .height = CLAY_SIZING_FIXED(icon_size)}},
                          .image = {.imageData = icon}})
            {
            }
        }
        else
        {
            CLAY_TEXT(CLAY_STRING("x"), CLAY_TEXT_CONFIG({.fontId = FONT_BOLD,
                                                          .fontSize = PICO_FONT_UI,
                                                          .textColor = enabled ? COLOR_TEXT : COLOR_MUTED,
                                                          .wrapMode = CLAY_TEXT_WRAP_NONE}));
        }
    }
}

static void RenderChip(Clay_ElementId id, const char *label, bool on)
{
    bool hover = Clay_PointerOver(id);
    Clay_Color bg = on ? (Clay_Color){62, 78, 124, 255} : (hover ? COLOR_CODE_BG : COLOR_FOOTER_BG);
    CLAY(id, {.layout = {.padding = {8, 8, 4, 4}}, .backgroundColor = bg, .cornerRadius = CLAY_CORNER_RADIUS(6)})
    {
        CLAY_TEXT(CStr(label), CLAY_TEXT_CONFIG({.fontId = FONT_REGULAR,
                                                .fontSize = PICO_FONT_CAPTION,
                                                .textColor = COLOR_TEXT,
                                                .wrapMode = CLAY_TEXT_WRAP_NONE}));
    }
}

static void RenderToggle(Clay_ElementId id, const char *label, bool on)
{
    bool hover = Clay_PointerOver(id);
    CLAY(id, {.layout = {.layoutDirection = CLAY_LEFT_TO_RIGHT,
                         .childGap = 8,
                         .childAlignment = {.y = CLAY_ALIGN_Y_CENTER},
                         .padding = {8, 8, 6, 6},
                         .sizing = {.width = CLAY_SIZING_GROW(0)}},
              .backgroundColor = hover ? COLOR_CODE_BG : COLOR_COMPOSER_BG,
              .cornerRadius = CLAY_CORNER_RADIUS(6)})
    {
        CLAY_TEXT(CStr(label), CLAY_TEXT_CONFIG({.fontId = FONT_REGULAR,
                                                .fontSize = PICO_FONT_UI,
                                                .textColor = COLOR_TEXT,
                                                .wrapMode = CLAY_TEXT_WRAP_NONE}));
        CLAY_AUTO_ID({.layout = {.sizing = {.width = CLAY_SIZING_GROW(0)}}}) {}
        CLAY_AUTO_ID({.layout = {.sizing = {.width = CLAY_SIZING_FIXED(10), .height = CLAY_SIZING_FIXED(10)}},
                      .backgroundColor = on ? COLOR_STATUS_ON : COLOR_STATUS_OFF,
                      .cornerRadius = CLAY_CORNER_RADIUS(5)})
        {
        }
    }
}

static void CloseModelDropdown(SettingsState *s)
{
    if (!s)
    {
        return;
    }
    s->model_dropdown = false;
    s->model_dropdown_selected = 0;
    s->model_dropdown_ensure_visible = false;
}

static void OpenModelDropdown(SettingsState *s)
{
    int i;
    if (!s)
    {
        return;
    }
    if (s->model_dropdown)
    {
        CloseModelDropdown(s);
        return;
    }
    if (s->draft.model_count <= 0)
    {
        return;
    }
    SetFocus(s, FOCUS_NONE, -1);
    s->model_dropdown = true;
    s->model_dropdown_selected = 0;
    s->model_dropdown_ensure_visible = true;
    for (i = 0; i < s->draft.model_count; i++)
    {
        if (strcmp(s->draft.default_model, s->draft.models[i].id) == 0)
        {
            s->model_dropdown_selected = i;
            break;
        }
    }
}

static bool AcceptDefaultModel(SettingsState *s)
{
    PicoModel *m;
    if (!s || !s->model_dropdown)
    {
        return false;
    }
    m = ModelAt(s, s->model_dropdown_selected);
    if (!m || !m->id[0])
    {
        return false;
    }
    snprintf(s->draft.default_model, sizeof(s->draft.default_model), "%s", m->id);
    CloseModelDropdown(s);
    return true;
}

static int HoveredDefaultModel(const SettingsState *s)
{
    int i;
    if (!s || !s->model_dropdown)
    {
        return -1;
    }
    for (i = 0; i < s->draft.model_count; i++)
    {
        if (Clay_PointerOver(CLAY_IDI("SettingsDefaultItem", i)))
        {
            return i;
        }
    }
    return -1;
}

static void SelectHoveredDefaultModel(SettingsState *s)
{
    int hovered;
    Vector2 delta;
    if (!s || !s->model_dropdown)
    {
        return;
    }
    delta = GetMouseDelta();
    if (delta.x == 0.0f && delta.y == 0.0f)
    {
        return;
    }
    hovered = HoveredDefaultModel(s);
    if (hovered >= 0)
    {
        s->model_dropdown_selected = hovered;
    }
}

static void RenderDefaultModelMenu(SettingsState *s)
{
    const float row_gap = 2.0f;
    float content_h;
    float menu_h;
    float row_h;
    bool scroll;
    int i;
    if (!s || s->draft.model_count <= 0)
    {
        return;
    }
    SelectHoveredDefaultModel(s);
    row_h = Pico_FontPx(PICO_FONT_UI) + 8.0f;
    content_h = 12.0f + (float)s->draft.model_count * row_h +
                (float)(s->draft.model_count - 1) * row_gap;
    scroll = content_h > 240.0f;
    menu_h = scroll ? 240.0f : content_h;

    CLAY(CLAY_ID("SettingsDefaultMenu"),
         {.floating = {.attachTo = CLAY_ATTACH_TO_PARENT,
                       .zIndex = 41,
                       .attachPoints = {.element = CLAY_ATTACH_POINT_LEFT_TOP,
                                        .parent = CLAY_ATTACH_POINT_LEFT_BOTTOM},
                       .offset = {.y = 6}},
          .layout = {.layoutDirection = CLAY_TOP_TO_BOTTOM,
                     .padding = {6, 6, 6, 6},
                     .childGap = 2,
                     .sizing = {.width = CLAY_SIZING_GROW(0),
                                .height = scroll ? CLAY_SIZING_FIXED(menu_h) : CLAY_SIZING_FIT(0)}},
          .backgroundColor = COLOR_CONTENT_BG,
          .cornerRadius = CLAY_CORNER_RADIUS(6)})
    {
        CLAY(CLAY_ID("SettingsDefaultMenuRow"),
             {.layout = {.layoutDirection = CLAY_LEFT_TO_RIGHT,
                         .childGap = SCROLLBAR_GAP,
                         .sizing = {.width = CLAY_SIZING_GROW(0), .height = CLAY_SIZING_GROW(0)}}})
        {
            CLAY(CLAY_ID("SettingsDefaultMenuScroll"),
                 {.layout = {.layoutDirection = CLAY_TOP_TO_BOTTOM,
                             .childGap = 2,
                             .sizing = {.width = CLAY_SIZING_GROW(0),
                                        .height = scroll ? CLAY_SIZING_GROW(0) : CLAY_SIZING_FIT(0)}},
                  .clip = {.vertical = scroll,
                           .horizontal = true,
                           .childOffset = scroll ? Clay_GetScrollOffset() : (Clay_Vector2){0}}})
            {
                for (i = 0; i < s->draft.model_count; i++)
                {
                    const PicoModel *m = &s->draft.models[i];
                    const char *label = m->name[0] ? m->name : (m->id[0] ? m->id : "(unnamed)");
                    Clay_Color bg = i == s->model_dropdown_selected ? COLOR_CODE_BG : COLOR_CONTENT_BG;
                    CLAY(CLAY_IDI("SettingsDefaultItem", i),
                         {.layout = {.layoutDirection = CLAY_LEFT_TO_RIGHT,
                                     .childGap = 8,
                                     .padding = {8, 8, 4, 4},
                                     .sizing = {.width = CLAY_SIZING_GROW(0)}},
                          .backgroundColor = bg,
                          .cornerRadius = CLAY_CORNER_RADIUS(4)})
                    {
                        CLAY_TEXT(CStr(label), CLAY_TEXT_CONFIG({.fontId = FONT_MONO,
                                                                .fontSize = PICO_FONT_UI,
                                                                .textColor = COLOR_TEXT,
                                                                .wrapMode = CLAY_TEXT_WRAP_NONE}));
                        if (m->provider[0])
                        {
                            CLAY_AUTO_ID({.layout = {.sizing = {.width = CLAY_SIZING_GROW(0)}}}) {}
                            CLAY_TEXT(CStr(m->provider), CLAY_TEXT_CONFIG({.fontId = FONT_REGULAR,
                                                                         .fontSize = PICO_FONT_CAPTION,
                                                                         .textColor = COLOR_MUTED,
                                                                         .wrapMode = CLAY_TEXT_WRAP_NONE}));
                        }
                    }
                }
            }
            if (scroll)
            {
                PicoScrollbar_Render(CLAY_STRING("SettingsDefaultMenuScroll"),
                                     CLAY_STRING("SettingsDefaultMenuScrollTrack"),
                                     CLAY_STRING("SettingsDefaultMenuScrollHandle"));
            }
        }
    }
}

static void RenderDefaultModelPicker(SettingsState *s)
{
    Clay_ElementId id = CLAY_ID("SettingsDefaultModel");
    bool hover = Clay_PointerOver(id);
    Clay_Color bg = s->model_dropdown ? (Clay_Color){54, 54, 66, 255}
                                      : (hover ? COLOR_CODE_BG : COLOR_COMPOSER_BG);
    CLAY(id, {.layout = {.padding = {8, 8, 6, 6}, .sizing = {.width = CLAY_SIZING_GROW(0)}},
              .backgroundColor = bg,
              .cornerRadius = CLAY_CORNER_RADIUS(6)})
    {
        CLAY_TEXT(CStr(s->draft.default_model[0] ? s->draft.default_model : "model id"),
                  CLAY_TEXT_CONFIG({.fontId = FONT_REGULAR,
                                    .fontSize = PICO_FONT_UI,
                                    .textColor = s->draft.default_model[0] ? COLOR_TEXT : COLOR_MUTED,
                                    .wrapMode = CLAY_TEXT_WRAP_NONE}));
        if (s->model_dropdown)
        {
            RenderDefaultModelMenu(s);
        }
    }
}

static void RenderGeneral(SettingsState *s)
{
    CLAY_TEXT(CLAY_STRING("General"),
              CLAY_TEXT_CONFIG({.fontId = FONT_BOLD, .fontSize = PICO_FONT_UI, .textColor = COLOR_TEXT}));
    SETTINGS_ROW_BEGIN
        RenderLabel("Default model");
        RenderDefaultModelPicker(s);
    }
    SETTINGS_ROW_BEGIN
        RenderLabel("Fallback context limit");
        RenderField(CLAY_ID("SettingsContextLimit"), s->context_limit, "128000",
                    s->focus_kind == FOCUS_CONTEXT_LIMIT);
    }
    SETTINGS_ROW_BEGIN
        RenderLabel("Compact at (0–1, or off)");
        RenderField(CLAY_ID("SettingsCompactAt"), s->compact_at, "0.9 or off", s->focus_kind == FOCUS_COMPACT_AT);
    }
    SETTINGS_ROW_BEGIN
        RenderToggle(CLAY_ID("SettingsResumeLast"), "Resume last session", s->draft.resume_last);
    }
    SETTINGS_ROW_BEGIN
        RenderLabel("Font scale (0.5–3.0)");
        RenderField(CLAY_ID("SettingsFontScale"), s->font_scale, "1.0", s->focus_kind == FOCUS_FONT_SCALE);
    }
    SETTINGS_ROW_BEGIN
        RenderLabel("Chat width (0 = no cap, else 40–200)");
        RenderField(CLAY_ID("SettingsChatWidth"), s->chat_width, "90", s->focus_kind == FOCUS_CHAT_WIDTH);
    }
}

static void RenderModelEditor(SettingsState *s, int index, PicoModel *m)
{
    const PicoWorkspace *ws;
    int i;
    const char *context_value = (index >= 0 && index < PICO_SETTINGS_MODEL_MAX) ? s->model_contexts[index] : "";
    bool context_focus = s->focus_kind == FOCUS_MODEL_CONTEXT && s->focus_model == index;
    SETTINGS_ROW_BEGIN
        RenderLabel("Id");
        RenderField(CLAY_IDI("SettingsModelId", index), m->id, "id",
                    s->focus_kind == FOCUS_MODEL_ID && s->focus_model == index);
    }
    SETTINGS_ROW_BEGIN
        RenderLabel("Name");
        RenderField(CLAY_IDI("SettingsModelName", index), m->name, "display name",
                    s->focus_kind == FOCUS_MODEL_NAME && s->focus_model == index);
    }
    SETTINGS_ROW_BEGIN
        RenderLabel("Provider");
        RenderField(CLAY_IDI("SettingsModelProvider", index), m->provider, "openai",
                    s->focus_kind == FOCUS_MODEL_PROVIDER && s->focus_model == index);
        ws = PicoHost_SelectedWorkspaceConst(s->host);
        if (ws && ws->provider_count > 0)
        {
            CLAY_AUTO_ID({.layout = {.layoutDirection = CLAY_LEFT_TO_RIGHT,
                                     .childGap = 6,
                                     .sizing = {.width = CLAY_SIZING_GROW(0)}}})
            {
                for (i = 0; i < ws->provider_count; i++)
                {
                    const char *name = ws->providers[i].name ? ws->providers[i].name : "";
                    if (!name[0])
                    {
                        continue;
                    }
                    RenderChip(CLAY_IDI("SettingsProvChip", index * 16 + i), name, strcmp(m->provider, name) == 0);
                }
            }
        }
    }
    SETTINGS_ROW_BEGIN
        RenderLabel("Base URL (optional; hyper/xAI reject non-canonical URLs)");
        RenderField(CLAY_IDI("SettingsModelBaseUrl", index), m->base_url, "https://…",
                    s->focus_kind == FOCUS_MODEL_BASE_URL && s->focus_model == index);
    }
    SETTINGS_ROW_BEGIN
        RenderLabel("Context limit (0 = fallback)");
        RenderField(CLAY_IDI("SettingsModelContext", index), context_value, "0", context_focus);
    }
    SETTINGS_ROW_BEGIN
        RenderToggle(CLAY_IDI("SettingsModelVision", index), "Vision", m->vision);
    }
    SETTINGS_ROW_BEGIN
        RenderLabel("Effort");
        CLAY_AUTO_ID({.layout = {.layoutDirection = CLAY_LEFT_TO_RIGHT,
                                 .childGap = 6,
                                 .sizing = {.width = CLAY_SIZING_GROW(0)}}})
        {
            for (i = 0; i < kEffortPresetCount; i++)
            {
                RenderChip(CLAY_IDI("SettingsEffortPreset", index * 16 + i), kEffortPresets[i],
                           FindEffort(m, kEffortPresets[i]) >= 0);
            }
        }
        if (m->effort_count > 0)
        {
            CLAY_AUTO_ID({.layout = {.layoutDirection = CLAY_LEFT_TO_RIGHT,
                                     .childGap = 6,
                                     .sizing = {.width = CLAY_SIZING_GROW(0)}}})
            {
                for (i = 0; i < m->effort_count; i++)
                {
                    bool extra = true;
                    int p;
                    for (p = 0; p < kEffortPresetCount; p++)
                    {
                        if (strcmp(m->effort[i], kEffortPresets[p]) == 0)
                        {
                            extra = false;
                            break;
                        }
                    }
                    if (extra)
                    {
                        RenderChip(CLAY_IDI("SettingsEffortExtra", index * 16 + i), m->effort[i], true);
                    }
                }
            }
        }
    }
    SETTINGS_ROW_BEGIN
        RenderLabel("Add custom effort");
        RenderField(CLAY_IDI("SettingsCustomEffort", index), s->custom_effort, "xhigh",
                    s->focus_kind == FOCUS_CUSTOM_EFFORT && s->focus_model == index);
        RenderButton(CLAY_IDI("SettingsAddEffort", index), "Add effort", s->custom_effort[0] != '\0', false);
    }
    SETTINGS_ROW_BEGIN
        RenderLabel("Selected effort");
        CLAY_AUTO_ID({.layout = {.layoutDirection = CLAY_LEFT_TO_RIGHT,
                                 .childGap = 6,
                                 .sizing = {.width = CLAY_SIZING_GROW(0)}}})
        {
            if (m->effort_count <= 0)
            {
                RenderChip(CLAY_IDI("SettingsSelectedEffort", index * 16), "none",
                           strcmp(m->default_effort, "none") == 0 || !m->default_effort[0]);
            }
            for (i = 0; i < m->effort_count; i++)
            {
                RenderChip(CLAY_IDI("SettingsSelectedEffort", index * 16 + i), m->effort[i],
                           strcmp(m->default_effort, m->effort[i]) == 0);
            }
        }
    }
}

static void RenderModelRow(SettingsState *s, int index)
{
    PicoModel *m = ModelAt(s, index);
    bool expanded = s->expanded && s->expanded[index];
    bool hover = Clay_PointerOver(CLAY_IDI("SettingsModelRow", index));
    const char *name;
    const char *id;
    const char *provider;
    if (!m)
    {
        return;
    }
    name = m->name[0] ? m->name : (m->id[0] ? m->id : "New model");
    id = m->id[0] ? m->id : "(no id)";
    provider = m->provider[0] ? m->provider : "(no provider)";
    CLAY(CLAY_IDI("SettingsModelCard", index),
         {.layout = {.layoutDirection = CLAY_TOP_TO_BOTTOM,
                     .childGap = 8,
                     .padding = {10, 10, 8, 8},
                     .sizing = {.width = CLAY_SIZING_GROW(0)}},
          .backgroundColor = hover ? (Clay_Color){54, 54, 66, 255} : COLOR_CODE_BG,
          .cornerRadius = CLAY_CORNER_RADIUS(6)})
    {
        CLAY(CLAY_IDI("SettingsModelRow", index),
             {.layout = {.layoutDirection = CLAY_LEFT_TO_RIGHT,
                         .childGap = 8,
                         .childAlignment = {.y = CLAY_ALIGN_Y_CENTER},
                         .sizing = {.width = CLAY_SIZING_GROW(0)}}})
        {
            CLAY_AUTO_ID({.layout = {.layoutDirection = CLAY_LEFT_TO_RIGHT,
                                     .childGap = 8,
                                     .childAlignment = {.y = CLAY_ALIGN_Y_CENTER},
                                     .sizing = {.width = CLAY_SIZING_GROW(0)}}})
            {
                CLAY_TEXT(CStr(name), CLAY_TEXT_CONFIG({.fontId = FONT_REGULAR,
                                                       .fontSize = PICO_FONT_UI,
                                                       .textColor = COLOR_TEXT,
                                                       .wrapMode = CLAY_TEXT_WRAP_NONE}));
                CLAY_TEXT(CLAY_STRING("·"), CLAY_TEXT_CONFIG({.fontId = FONT_REGULAR,
                                                             .fontSize = PICO_FONT_UI,
                                                             .textColor = COLOR_MUTED,
                                                             .wrapMode = CLAY_TEXT_WRAP_NONE}));
                CLAY_TEXT(CStr(id), CLAY_TEXT_CONFIG({.fontId = FONT_REGULAR,
                                                     .fontSize = PICO_FONT_UI,
                                                     .textColor = COLOR_MUTED,
                                                     .wrapMode = CLAY_TEXT_WRAP_NONE}));
                CLAY_TEXT(CLAY_STRING("·"), CLAY_TEXT_CONFIG({.fontId = FONT_REGULAR,
                                                             .fontSize = PICO_FONT_UI,
                                                             .textColor = COLOR_MUTED,
                                                             .wrapMode = CLAY_TEXT_WRAP_NONE}));
                CLAY_TEXT(CStr(provider), CLAY_TEXT_CONFIG({.fontId = FONT_REGULAR,
                                                           .fontSize = PICO_FONT_UI,
                                                           .textColor = COLOR_MUTED,
                                                           .wrapMode = CLAY_TEXT_WRAP_NONE}));
            }
            RenderTrashButton(s, CLAY_IDI("SettingsModelRemove", index), s->draft.model_count > 1);
        }
        if (expanded)
        {
            RenderModelEditor(s, index, m);
        }
    }
}

static void SettingsRender(PicoHost *app, void *state)
{
    float sw;
    float sh;
    float card_w;
    float card_h;
    int i;
    s_active_settings_state =
        state ? (SettingsState *)state : (SettingsState *)PicoPlugins_HostState(app, "settings");
    if (!s_active_settings_state || !g_open)
    {
        return;
    }
    EnsureTrashIcon(s_active_settings_state);
    sw = (float)GetScreenWidth();
    sh = (float)GetScreenHeight();
    card_w = sw < 720.0f ? sw - 48.0f : 640.0f;
    if (card_w < 280.0f)
    {
        card_w = 280.0f;
    }
    card_h = sh * 0.82f;
    if (card_h < 280.0f)
    {
        card_h = 280.0f;
    }
    if (card_h > 760.0f)
    {
        card_h = 760.0f;
    }
    CLAY(CLAY_ID("SettingsModalDim"),
         {.floating = {.attachTo = CLAY_ATTACH_TO_ROOT,
                       .zIndex = 40,
                       .attachPoints = {.element = CLAY_ATTACH_POINT_LEFT_TOP,
                                        .parent = CLAY_ATTACH_POINT_LEFT_TOP}},
          .layout = {.layoutDirection = CLAY_TOP_TO_BOTTOM,
                     .childAlignment = {.x = CLAY_ALIGN_X_CENTER, .y = CLAY_ALIGN_Y_CENTER},
                     .sizing = {.width = CLAY_SIZING_FIXED(sw), .height = CLAY_SIZING_FIXED(sh)}},
          .backgroundColor = {0, 0, 0, 140}})
    {
        CLAY(CLAY_ID("SettingsModalCard"),
             {.layout = {.layoutDirection = CLAY_TOP_TO_BOTTOM,
                         .padding = {20, 20, 16, 16},
                         .childGap = 12,
                         .sizing = {.width = CLAY_SIZING_FIXED(card_w), .height = CLAY_SIZING_FIXED(card_h)}},
              .backgroundColor = COLOR_CONTENT_BG,
              .cornerRadius = CLAY_CORNER_RADIUS(8)})
        {
            CLAY_TEXT(CLAY_STRING("Settings"),
                      CLAY_TEXT_CONFIG({.fontId = FONT_BOLD, .fontSize = PICO_FONT_TITLE, .textColor = COLOR_TEXT}));
            CLAY(CLAY_ID("SettingsModalScrollRow"),
                 {.layout = {.layoutDirection = CLAY_LEFT_TO_RIGHT,
                             .childGap = SCROLLBAR_GAP,
                             .sizing = {.width = CLAY_SIZING_GROW(0), .height = CLAY_SIZING_GROW(0)}}})
            {
                CLAY(CLAY_ID("SettingsModalScroll"),
                     {.layout = {.layoutDirection = CLAY_TOP_TO_BOTTOM,
                                 .childGap = 10,
                                 .sizing = {.width = CLAY_SIZING_GROW(0), .height = CLAY_SIZING_GROW(0)}},
                      .clip = {.vertical = true, .horizontal = false, .childOffset = Clay_GetScrollOffset()}})
                {
                    RenderGeneral(s_active_settings_state);
                    CLAY_TEXT(CLAY_STRING("Models"), CLAY_TEXT_CONFIG({.fontId = FONT_BOLD,
                                                                      .fontSize = PICO_FONT_UI,
                                                                      .textColor = COLOR_TEXT}));
                    CLAY_AUTO_ID({.layout = {.layoutDirection = CLAY_TOP_TO_BOTTOM,
                                             .childGap = 14,
                                             .padding = {.bottom = 8},
                                             .sizing = {.width = CLAY_SIZING_GROW(0)}}})
                    {
                        for (i = 0; i < g_draft.model_count; i++)
                        {
                            RenderModelRow(s_active_settings_state, i);
                        }
                    }
                    RenderButton(CLAY_ID("SettingsAddModel"), "Add model", true, false);
                }
                if (g_overflow)
                {
                    PicoScrollbar_Render(CLAY_STRING("SettingsModalScroll"), CLAY_STRING("SettingsModalScrollTrack"),
                                         CLAY_STRING("SettingsModalScrollHandle"));
                }
            }
            if (g_error[0])
            {
                CLAY_TEXT(CStr(g_error), CLAY_TEXT_CONFIG({.fontId = FONT_REGULAR,
                                                          .fontSize = PICO_FONT_CAPTION,
                                                          .textColor = COLOR_STATUS_ERR,
                                                          .wrapMode = CLAY_TEXT_WRAP_WORDS}));
            }
            CLAY(CLAY_ID("SettingsModalButtons"),
                 {.layout = {.layoutDirection = CLAY_LEFT_TO_RIGHT,
                             .childGap = 8,
                             .childAlignment = {.y = CLAY_ALIGN_Y_CENTER},
                             .sizing = {.width = CLAY_SIZING_GROW(0)}}})
            {
                RenderButton(CLAY_ID("SettingsCancel"), "Cancel", true, false);
                CLAY_AUTO_ID({.layout = {.childAlignment = {.x = CLAY_ALIGN_X_CENTER,
                                                            .y = CLAY_ALIGN_Y_CENTER},
                                         .sizing = {.width = CLAY_SIZING_GROW(0)}}})
                {
                    CLAY_TEXT(CLAY_STRING("v" PICO_VERSION),
                              CLAY_TEXT_CONFIG({.fontId = FONT_REGULAR,
                                                .fontSize = PICO_FONT_CAPTION,
                                                .textColor = COLOR_MUTED,
                                                .wrapMode = CLAY_TEXT_WRAP_NONE}));
                }
                RenderButton(CLAY_ID("SettingsApply"), "Apply", true, true);
            }
        }
    }
}

static void KeepDefaultModelSelectionVisible(SettingsState *s)
{
    Clay_ScrollContainerData scroll;
    Clay_ElementData viewport;
    Clay_ElementData item;
    float min_y;
    float viewport_bottom;
    float item_bottom;
    if (!s || !s->model_dropdown || s->model_dropdown_selected < 0 ||
        s->model_dropdown_selected >= s->draft.model_count)
    {
        return;
    }
    scroll = Clay_GetScrollContainerData(Clay_GetElementId(CLAY_STRING("SettingsDefaultMenuScroll")));
    viewport = Clay_GetElementData(CLAY_ID("SettingsDefaultMenuScroll"));
    item = Clay_GetElementData(CLAY_IDI("SettingsDefaultItem", s->model_dropdown_selected));
    if (!scroll.found || !scroll.scrollPosition || !viewport.found || !item.found)
    {
        return;
    }
    viewport_bottom = viewport.boundingBox.y + viewport.boundingBox.height;
    item_bottom = item.boundingBox.y + item.boundingBox.height;
    if (item.boundingBox.y < viewport.boundingBox.y)
    {
        scroll.scrollPosition->y += viewport.boundingBox.y - item.boundingBox.y;
    }
    else if (item_bottom > viewport_bottom)
    {
        scroll.scrollPosition->y -= item_bottom - viewport_bottom;
    }
    min_y = viewport.boundingBox.height - scroll.contentDimensions.height;
    if (min_y > 0.0f)
    {
        min_y = 0.0f;
    }
    if (scroll.scrollPosition->y < min_y)
    {
        scroll.scrollPosition->y = min_y;
    }
    if (scroll.scrollPosition->y > 0.0f)
    {
        scroll.scrollPosition->y = 0.0f;
    }
}

static bool HoveredTextField(SettingsState *s)
{
    int i;
    if (OverId(CLAY_STRING("SettingsContextLimit")) || OverId(CLAY_STRING("SettingsCompactAt")) ||
        OverId(CLAY_STRING("SettingsFontScale")) || OverId(CLAY_STRING("SettingsChatWidth")))
    {
        return true;
    }
    for (i = 0; i < s->draft.model_count; i++)
    {
        if (!s->expanded || !s->expanded[i])
        {
            continue;
        }
        if (Clay_PointerOver(CLAY_IDI("SettingsModelId", i)) || Clay_PointerOver(CLAY_IDI("SettingsModelName", i)) ||
            Clay_PointerOver(CLAY_IDI("SettingsModelProvider", i)) || Clay_PointerOver(CLAY_IDI("SettingsModelBaseUrl", i)) ||
            Clay_PointerOver(CLAY_IDI("SettingsModelContext", i)) || Clay_PointerOver(CLAY_IDI("SettingsCustomEffort", i)))
        {
            return true;
        }
    }
    return false;
}

static bool HoveredClickable(SettingsState *s)
{
    int i;
    int p;
    const PicoWorkspace *ws;
    if (OverId(CLAY_STRING("SettingsDefaultModel")) || OverId(CLAY_STRING("SettingsContextLimit")) ||
        OverId(CLAY_STRING("SettingsCompactAt")) || OverId(CLAY_STRING("SettingsResumeLast")) ||
        OverId(CLAY_STRING("SettingsFontScale")) || OverId(CLAY_STRING("SettingsChatWidth")) ||
        OverId(CLAY_STRING("SettingsAddModel")) || OverId(CLAY_STRING("SettingsCancel")) ||
        OverId(CLAY_STRING("SettingsApply")))
    {
        return true;
    }
    if (s->model_dropdown)
    {
        if (OverId(CLAY_STRING("SettingsDefaultMenu")))
        {
            return true;
        }
        for (i = 0; i < s->draft.model_count; i++)
        {
            if (Clay_PointerOver(CLAY_IDI("SettingsDefaultItem", i)))
            {
                return true;
            }
        }
    }
    for (i = 0; i < s->draft.model_count; i++)
    {
        if (Clay_PointerOver(CLAY_IDI("SettingsModelRow", i)) || Clay_PointerOver(CLAY_IDI("SettingsModelRemove", i)))
        {
            return true;
        }
        if (!s->expanded || !s->expanded[i])
        {
            continue;
        }
        if (Clay_PointerOver(CLAY_IDI("SettingsModelId", i)) || Clay_PointerOver(CLAY_IDI("SettingsModelName", i)) ||
            Clay_PointerOver(CLAY_IDI("SettingsModelProvider", i)) || Clay_PointerOver(CLAY_IDI("SettingsModelBaseUrl", i)) ||
            Clay_PointerOver(CLAY_IDI("SettingsModelContext", i)) || Clay_PointerOver(CLAY_IDI("SettingsModelVision", i)) ||
            Clay_PointerOver(CLAY_IDI("SettingsCustomEffort", i)) || Clay_PointerOver(CLAY_IDI("SettingsAddEffort", i)))
        {
            return true;
        }
        for (p = 0; p < kEffortPresetCount; p++)
        {
            if (Clay_PointerOver(CLAY_IDI("SettingsEffortPreset", i * 16 + p)))
            {
                return true;
            }
        }
        ws = PicoHost_SelectedWorkspaceConst(s->host);
        if (ws)
        {
            for (p = 0; p < ws->provider_count; p++)
            {
                if (Clay_PointerOver(CLAY_IDI("SettingsProvChip", i * 16 + p)))
                {
                    return true;
                }
            }
        }
    }
    return false;
}

static void SettingsAfterLayout(PicoHost *app, const PicoHookEvent *event, void *state)
{
    (void)event;
    s_active_settings_state =
        state ? (SettingsState *)state : (SettingsState *)PicoPlugins_HostState(app, "settings");
    if (!s_active_settings_state || !g_open || !pico_ui_modal_is_top(app, "settings"))
    {
        return;
    }
    g_overflow = PicoScrollbar_Overflows(CLAY_STRING("SettingsModalScroll"));
    if (s_active_settings_state->model_dropdown_ensure_visible)
    {
        KeepDefaultModelSelectionVisible(s_active_settings_state);
        s_active_settings_state->model_dropdown_ensure_visible = false;
    }
    if (HoveredTextField(s_active_settings_state))
    {
        app->hovered_text = true;
    }
    if (HoveredClickable(s_active_settings_state))
    {
        app->hovered_clickable = true;
    }
    if (!IsMouseButtonPressed(MOUSE_BUTTON_LEFT))
    {
        return;
    }
    if (s_active_settings_state->model_dropdown_click_block)
    {
        return;
    }
    if (OverId(CLAY_STRING("SettingsModalCard")))
    {
        return;
    }
    if (OverId(CLAY_STRING("SettingsModalDim")))
    {
        PicoSettingsUi_Close(app);
    }
}

static void HandleKeys(SettingsState *s)
{
    char *buf;
    size_t cap = 0;
    int cp;
    if (!s || s->focus_kind == FOCUS_NONE)
    {
        return;
    }
    buf = FocusBuf(s, &cap);
    if (!buf)
    {
        return;
    }
    if (IsKeyPressed(KEY_BACKSPACE) || IsKeyPressedRepeat(KEY_BACKSPACE))
    {
        Backspace(buf);
    }
    if (s->focus_kind == FOCUS_CUSTOM_EFFORT && IsKeyPressed(KEY_ENTER))
    {
        AddCustomEffort(s, s->focus_model);
        return;
    }
    while ((cp = GetCharPressed()) != 0)
    {
        InsertAscii(buf, cap, cp);
    }
}

static bool HandleClicks(SettingsState *s)
{
    int i;
    int p;
    const PicoWorkspace *ws;
    PicoModel *m;
    if (!IsMouseButtonPressed(MOUSE_BUTTON_LEFT))
    {
        return false;
    }
    if (s->model_dropdown)
    {
        int hovered = HoveredDefaultModel(s);
        s->model_dropdown_click_block = true;
        if (hovered >= 0)
        {
            s->model_dropdown_selected = hovered;
            (void)AcceptDefaultModel(s);
            return true;
        }
        if (OverId(CLAY_STRING("SettingsDefaultMenu")))
        {
            return true;
        }
        CloseModelDropdown(s);
        return true;
    }
    if (OverId(CLAY_STRING("SettingsCancel")))
    {
        PicoSettingsUi_Close(s->host);
        return true;
    }
    if (OverId(CLAY_STRING("SettingsApply")))
    {
        if (ApplyDraft(s))
        {
            PicoSettingsUi_Close(s->host);
        }
        return true;
    }
    if (OverId(CLAY_STRING("SettingsAddModel")))
    {
        AddModel(s);
        return true;
    }
    if (OverId(CLAY_STRING("SettingsResumeLast")))
    {
        s->draft.resume_last = !s->draft.resume_last;
        return true;
    }
    if (OverId(CLAY_STRING("SettingsDefaultModel")))
    {
        OpenModelDropdown(s);
        return true;
    }
    if (OverId(CLAY_STRING("SettingsContextLimit")))
    {
        SetFocus(s, FOCUS_CONTEXT_LIMIT, -1);
        return true;
    }
    if (OverId(CLAY_STRING("SettingsCompactAt")))
    {
        SetFocus(s, FOCUS_COMPACT_AT, -1);
        return true;
    }
    if (OverId(CLAY_STRING("SettingsFontScale")))
    {
        SetFocus(s, FOCUS_FONT_SCALE, -1);
        return true;
    }
    if (OverId(CLAY_STRING("SettingsChatWidth")))
    {
        SetFocus(s, FOCUS_CHAT_WIDTH, -1);
        return true;
    }
    for (i = 0; i < s->draft.model_count; i++)
    {
        m = ModelAt(s, i);
        if (!m)
        {
            continue;
        }
        if (Clay_PointerOver(CLAY_IDI("SettingsModelRemove", i)))
        {
            RemoveModel(s, i);
            return true;
        }
        if (s->expanded && s->expanded[i])
        {
            if (Clay_PointerOver(CLAY_IDI("SettingsModelId", i)))
            {
                SetFocus(s, FOCUS_MODEL_ID, i);
                return true;
            }
            if (Clay_PointerOver(CLAY_IDI("SettingsModelName", i)))
            {
                SetFocus(s, FOCUS_MODEL_NAME, i);
                return true;
            }
            if (Clay_PointerOver(CLAY_IDI("SettingsModelProvider", i)))
            {
                SetFocus(s, FOCUS_MODEL_PROVIDER, i);
                return true;
            }
            if (Clay_PointerOver(CLAY_IDI("SettingsModelBaseUrl", i)))
            {
                SetFocus(s, FOCUS_MODEL_BASE_URL, i);
                return true;
            }
            if (Clay_PointerOver(CLAY_IDI("SettingsModelContext", i)))
            {
                SetFocus(s, FOCUS_MODEL_CONTEXT, i);
                return true;
            }
            if (Clay_PointerOver(CLAY_IDI("SettingsModelVision", i)))
            {
                m->vision = !m->vision;
                return true;
            }
            if (Clay_PointerOver(CLAY_IDI("SettingsCustomEffort", i)))
            {
                SetFocus(s, FOCUS_CUSTOM_EFFORT, i);
                return true;
            }
            if (Clay_PointerOver(CLAY_IDI("SettingsAddEffort", i)))
            {
                AddCustomEffort(s, i);
                return true;
            }
            for (p = 0; p < kEffortPresetCount; p++)
            {
                if (Clay_PointerOver(CLAY_IDI("SettingsEffortPreset", i * 16 + p)))
                {
                    ToggleEffort(m, kEffortPresets[p]);
                    return true;
                }
            }
            for (p = 0; p < m->effort_count; p++)
            {
                if (Clay_PointerOver(CLAY_IDI("SettingsEffortExtra", i * 16 + p)))
                {
                    ToggleEffort(m, m->effort[p]);
                    return true;
                }
                if (Clay_PointerOver(CLAY_IDI("SettingsSelectedEffort", i * 16 + p)))
                {
                    snprintf(m->default_effort, sizeof(m->default_effort), "%s", m->effort[p]);
                    return true;
                }
            }
            if (m->effort_count <= 0 && Clay_PointerOver(CLAY_IDI("SettingsSelectedEffort", i * 16)))
            {
                snprintf(m->default_effort, sizeof(m->default_effort), "%s", "none");
                return true;
            }
            ws = PicoHost_SelectedWorkspaceConst(s->host);
            if (ws)
            {
                for (p = 0; p < ws->provider_count; p++)
                {
                    if (Clay_PointerOver(CLAY_IDI("SettingsProvChip", i * 16 + p)) && ws->providers[p].name)
                    {
                        snprintf(m->provider, sizeof(m->provider), "%s", ws->providers[p].name);
                        return true;
                    }
                }
            }
        }
        if (Clay_PointerOver(CLAY_IDI("SettingsModelRow", i)))
        {
            if (s->expanded)
            {
                s->expanded[i] = !s->expanded[i];
            }
            SetFocus(s, FOCUS_NONE, -1);
            return true;
        }
    }
    if (OverId(CLAY_STRING("SettingsModalCard")))
    {
        SetFocus(s, FOCUS_NONE, -1);
        s->model_dropdown = false;
        return true;
    }
    return false;
}

static void SettingsOnFrame(PicoHost *app, void *state, float dt)
{
    SettingsState *s;
    (void)dt;
    s_active_settings_state =
        state ? (SettingsState *)state : (SettingsState *)PicoPlugins_HostState(app, "settings");
    if (!s_active_settings_state || !g_open || !pico_ui_modal_is_top(app, "settings"))
    {
        return;
    }
    s = s_active_settings_state;
    s->model_dropdown_click_block = false;
    PicoScrollbar_UpdateDrag(&g_scrollbar, CLAY_STRING("SettingsModalScroll"),
                             CLAY_STRING("SettingsModalScrollHandle"));
    if (s->model_dropdown)
    {
        PicoScrollbar_UpdateDrag(&s->model_dropdown_scrollbar, CLAY_STRING("SettingsDefaultMenuScroll"),
                                 CLAY_STRING("SettingsDefaultMenuScrollHandle"));
        if (IsKeyPressed(KEY_ESCAPE))
        {
            CloseModelDropdown(s);
            return;
        }
        if ((IsKeyPressed(KEY_UP) || IsKeyPressedRepeat(KEY_UP)) && s->model_dropdown_selected > 0)
        {
            s->model_dropdown_selected--;
            s->model_dropdown_ensure_visible = true;
            return;
        }
        if ((IsKeyPressed(KEY_DOWN) || IsKeyPressedRepeat(KEY_DOWN)) &&
            s->model_dropdown_selected + 1 < s->draft.model_count)
        {
            s->model_dropdown_selected++;
            s->model_dropdown_ensure_visible = true;
            return;
        }
        if (IsKeyPressed(KEY_ENTER) || IsKeyPressed(KEY_KP_ENTER) || IsKeyPressed(KEY_TAB))
        {
            (void)AcceptDefaultModel(s);
            return;
        }
    }
    if (IsKeyPressed(KEY_ESCAPE))
    {
        PicoSettingsUi_Close(app);
        return;
    }
    HandleKeys(s);
    HandleClicks(s);
}

static void CmdSettings(PicoHost *app, PicoAgentId agent_id, const char *args, void *state)
{
    (void)state;
    (void)args;
    (void)agent_id;
    PicoSettingsUi_Open(app);
    PicoComposer_SetText(app, "");
    PicoHost_RequestSubmitCancel(app);
}

static int SettingsInit(PicoHost *app, void **state_out)
{
    SettingsState *s = (SettingsState *)calloc(1, sizeof(SettingsState));
    if (!s)
    {
        return 1;
    }
    s->host = app;
    s->focus_model = -1;
    if (state_out)
    {
        *state_out = s;
    }
    s_active_settings_state = s;
    pico_host_add_command(app, "settings", "Edit user settings", CmdSettings);
    pico_host_add_view(app, PICO_SLOT_OVERLAY, 12, SettingsRender);
    pico_host_add_hook(app, PICO_HOOK_AFTER_LAYOUT, SettingsAfterLayout);
    return 0;
}

static void SettingsShutdown(PicoHost *app, void *state)
{
    SettingsState *s = (SettingsState *)state;
    (void)app;
    if (!s)
    {
        return;
    }
    s_active_settings_state = s;
    (void)Unclaim();
    PicoSettings_FreeUserDraft(&s->draft);
    free(s->expanded);
    if (s->trash_icon.id != 0)
    {
        UnloadTexture(s->trash_icon);
    }
    if (s->trash_icon_disabled.id != 0)
    {
        UnloadTexture(s->trash_icon_disabled);
    }
    free(s);
    s_active_settings_state = NULL;
}

PicoExt pico_ext_settings(void)
{
    return (PicoExt){
        .abi = PICO_EXT_ABI,
        .name = "settings",
        .description = "User settings editor",
        .host_init = SettingsInit,
        .host_shutdown = SettingsShutdown,
        .host_on_frame = SettingsOnFrame,
    };
}
