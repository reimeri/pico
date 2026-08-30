#include "pico/plugin.h"
#include "scrollbar.h"
#include "host_internal.h"

#include "clay/clay.h"

#include <string.h>

typedef struct ExtensionsState {
    PicoHost *app;
    bool open;
    bool overflow;
    PicoScrollbar scrollbar;
} ExtensionsState;

static __thread ExtensionsState *s_active_exts_state = NULL;

static ExtensionsState *ActiveExtensionsState(void)
{
    return s_active_exts_state;
}

#define g_app (ActiveExtensionsState()->app)
#define g_open (ActiveExtensionsState()->open)
#define g_overflow (ActiveExtensionsState()->overflow)
#define g_scrollbar (ActiveExtensionsState()->scrollbar)

static bool Claim(void)
{
    if (g_open)
    {
        return true;
    }
    if (!g_app || !pico_ui_modal_push(g_app, "extensions"))
    {
        return false;
    }
    g_open = true;
    return true;
}

static bool Unclaim(void)
{
    if (!g_open)
    {
        return true;
    }
    if (g_app && !pico_ui_modal_pop(g_app, "extensions"))
    {
        return false;
    }
    g_open = false;
    return true;
}

void PicoExts_Close(void)
{
    if (!Unclaim())
    {
        return;
    }
    g_overflow = false;
    memset(&g_scrollbar, 0, sizeof(g_scrollbar));
}

void PicoExts_Open(void)
{
    PicoPrompt_Close();
    Claim();
}

void PicoExts_Toggle(void)
{
    if (g_open)
    {
        if (g_app && pico_ui_modal_is_top(g_app, "extensions"))
        {
            PicoExts_Close();
        }
    }
    else
    {
        PicoExts_Open();
    }
}

bool PicoExts_IsOpen(void)
{
    return g_open;
}

static bool RowToggleable(const PicoExtInfo *info)
{
    return info && info->loaded && info->name && info->name[0] && strcmp(info->name, "extensions") != 0;
}

static Clay_String CStr(const char *s)
{
    if (!s)
    {
        s = "";
    }
    return (Clay_String){.length = (int32_t)strlen(s), .chars = s};
}

static const char *BaseName(const char *path)
{
    if (!path || !path[0])
    {
        return NULL;
    }
    const char *slash = strrchr(path, '/');
    return slash ? slash + 1 : path;
}

static const char *DisplayName(const PicoExtInfo *info)
{
    if (info->name && info->name[0])
    {
        return info->name;
    }
    const char *base = BaseName(info->source);
    if (base && base[0])
    {
        return base;
    }
    return "(unnamed)";
}

static void RenderRow(int index, const PicoExtInfo *info)
{
    const char *name = DisplayName(info);
    const char *desc = NULL;
    if (!info->loaded)
    {
        desc = info->last_error ? info->last_error : "Failed to load";
    }
    else if (info->description && info->description[0])
    {
        desc = info->description;
    }

    bool toggleable = RowToggleable(info);
    bool hover = toggleable && Clay_PointerOver(CLAY_IDI("ExtModalRow", index));
    Clay_Color bg = info->loaded ? COLOR_CODE_BG : COLOR_ERROR_BG;
    if (hover)
    {
        bg = (Clay_Color){54, 54, 66, 255};
    }
    if (!info->enabled)
    {
        bg.a = 150;
    }

    CLAY(CLAY_IDI("ExtModalRow", index),
         {.layout = {.layoutDirection = CLAY_LEFT_TO_RIGHT,
                     .childGap = 12,
                     .childAlignment = {.y = CLAY_ALIGN_Y_CENTER},
                     .padding = {10, 10, 8, 8},
                     .sizing = {.width = CLAY_SIZING_GROW(0)}},
          .backgroundColor = bg,
          .cornerRadius = CLAY_CORNER_RADIUS(6)})
    {
        CLAY_AUTO_ID({.layout = {.layoutDirection = CLAY_TOP_TO_BOTTOM,
                                 .childGap = 2,
                                 .sizing = {.width = CLAY_SIZING_GROW(0)}}})
        {
            CLAY_AUTO_ID({.layout = {.layoutDirection = CLAY_LEFT_TO_RIGHT,
                                     .childGap = 8,
                                     .childAlignment = {.y = CLAY_ALIGN_Y_CENTER},
                                     .sizing = {.width = CLAY_SIZING_GROW(0)}}})
            {
                CLAY_TEXT(CStr(name), CLAY_TEXT_CONFIG({.fontId = FONT_BOLD,
                                                        .fontSize = PICO_FONT_UI,
                                                        .textColor = COLOR_TEXT,
                                                        .wrapMode = CLAY_TEXT_WRAP_WORDS}));
                if (info->builtin)
                {
                    const char *badge = (info->scope == PICO_EXTENSION_HOST) ? "built-in host" : "built-in workspace";
                    CLAY_TEXT(CStr(badge),
                              CLAY_TEXT_CONFIG({.fontId = FONT_REGULAR,
                                                .fontSize = PICO_FONT_CAPTION,
                                                .textColor = COLOR_MUTED,
                                                .wrapMode = CLAY_TEXT_WRAP_NONE}));
                }
                else
                {
                    const char *badge = (info->scope == PICO_EXTENSION_HOST) ? "host" : "workspace";
                    CLAY_TEXT(CStr(badge),
                              CLAY_TEXT_CONFIG({.fontId = FONT_REGULAR,
                                                .fontSize = PICO_FONT_CAPTION,
                                                .textColor = COLOR_MUTED,
                                                .wrapMode = CLAY_TEXT_WRAP_NONE}));
                }
                CLAY_AUTO_ID({.layout = {.sizing = {.width = CLAY_SIZING_GROW(0)}}}) {}
                CLAY(CLAY_IDI("ExtModalStatus", index),
                     {.layout = {.sizing = {.width = CLAY_SIZING_FIXED(10), .height = CLAY_SIZING_FIXED(10)}},
                      .backgroundColor = info->enabled ? COLOR_STATUS_ON : COLOR_STATUS_OFF,
                      .cornerRadius = CLAY_CORNER_RADIUS(5)})
                {
                }
            }
            if (desc)
            {
                CLAY_TEXT(CStr(desc), CLAY_TEXT_CONFIG({.fontId = FONT_REGULAR,
                                                        .fontSize = PICO_FONT_CAPTION,
                                                        .textColor = COLOR_MUTED,
                                                        .wrapMode = CLAY_TEXT_WRAP_WORDS}));
            }
        }
    }
}

static void RenderSection(PicoHost *app, bool builtin, Clay_String title)
{
    CLAY_TEXT(title, CLAY_TEXT_CONFIG({.fontId = FONT_REGULAR,
                                       .fontSize = PICO_FONT_CAPTION,
                                       .textColor = COLOR_MUTED,
                                       .wrapMode = CLAY_TEXT_WRAP_NONE}));

    int n = PicoPlugins_Count(app);
    int shown = 0;
    for (int i = 0; i < n; i++)
    {
        PicoExtInfo info;
        if (!PicoPlugins_Get(app, i, &info) || info.builtin != builtin)
        {
            continue;
        }
        RenderRow(i, &info);
        shown++;
    }
    if (shown == 0)
    {
        CLAY_TEXT(CLAY_STRING("None"), CLAY_TEXT_CONFIG({.fontId = FONT_REGULAR,
                                                         .fontSize = PICO_FONT_CAPTION,
                                                         .textColor = COLOR_MUTED,
                                                         .wrapMode = CLAY_TEXT_WRAP_NONE}));
    }
}

static void ExtsRender(PicoHost *app, void *state)
{
    s_active_exts_state = state ? (ExtensionsState *)state : (ExtensionsState *)PicoPlugins_HostState(app, "extensions");
    if (!s_active_exts_state || !g_open)
    {
        return;
    }

    float sw = (float)GetScreenWidth();
    float sh = (float)GetScreenHeight();
    float card_w = sw < 560.0f ? sw - 48.0f : 480.0f;
    if (card_w < 280.0f)
    {
        card_w = 280.0f;
    }
    float card_h = sh * 0.7f;
    if (card_h < 240.0f)
    {
        card_h = 240.0f;
    }
    if (card_h > 640.0f)
    {
        card_h = 640.0f;
    }

    CLAY(CLAY_ID("ExtModalDim"),
         {.floating = {.attachTo = CLAY_ATTACH_TO_ROOT,
                       .zIndex = 40,
                       .attachPoints = {.element = CLAY_ATTACH_POINT_LEFT_TOP,
                                        .parent = CLAY_ATTACH_POINT_LEFT_TOP}},
          .layout = {.layoutDirection = CLAY_TOP_TO_BOTTOM,
                     .childAlignment = {.x = CLAY_ALIGN_X_CENTER, .y = CLAY_ALIGN_Y_CENTER},
                     .sizing = {.width = CLAY_SIZING_FIXED(sw), .height = CLAY_SIZING_FIXED(sh)}},
          .backgroundColor = {0, 0, 0, 140}})
    {
        CLAY(CLAY_ID("ExtModalCard"),
             {.layout = {.layoutDirection = CLAY_TOP_TO_BOTTOM,
                         .padding = {20, 20, 16, 16},
                         .childGap = 12,
                         .sizing = {.width = CLAY_SIZING_FIXED(card_w), .height = CLAY_SIZING_FIXED(card_h)}},
              .backgroundColor = COLOR_CONTENT_BG,
              .cornerRadius = CLAY_CORNER_RADIUS(8)})
        {
            CLAY_TEXT(CLAY_STRING("Extensions"),
                      CLAY_TEXT_CONFIG({.fontId = FONT_BOLD, .fontSize = PICO_FONT_TITLE, .textColor = COLOR_TEXT}));

            CLAY(CLAY_ID("ExtModalScrollRow"),
                 {.layout = {.layoutDirection = CLAY_LEFT_TO_RIGHT,
                             .childGap = SCROLLBAR_GAP,
                             .sizing = {.width = CLAY_SIZING_GROW(0), .height = CLAY_SIZING_GROW(0)}}})
            {
                CLAY(CLAY_ID("ExtModalScroll"),
                     {.layout = {.layoutDirection = CLAY_TOP_TO_BOTTOM,
                                 .childGap = 10,
                                 .sizing = {.width = CLAY_SIZING_GROW(0), .height = CLAY_SIZING_GROW(0)}},
                      .clip = {.vertical = true, .horizontal = false, .childOffset = Clay_GetScrollOffset()}})
                {
                    RenderSection(app, true, CLAY_STRING("Built-in"));
                    RenderSection(app, false, CLAY_STRING("User"));
                }
                if (g_overflow)
                {
                    PicoScrollbar_Render(CLAY_STRING("ExtModalScroll"), CLAY_STRING("ExtModalScrollTrack"),
                                         CLAY_STRING("ExtModalScrollHandle"));
                }
            }
        }
    }
}

static void ExtsAfterLayout(PicoHost *app, const PicoHookEvent *event, void *state)
{
    (void)event;
    s_active_exts_state = state ? (ExtensionsState *)state : (ExtensionsState *)PicoPlugins_HostState(app, "extensions");
    if (!s_active_exts_state || !g_open || !pico_ui_modal_is_top(app, "extensions"))
    {
        return;
    }
    g_overflow = PicoScrollbar_Overflows(CLAY_STRING("ExtModalScroll"));
    int n = PicoPlugins_Count(app);
    int over_row = -1;
    for (int i = 0; i < n; i++)
    {
        PicoExtInfo info;
        if (!PicoPlugins_Get(app, i, &info) || !RowToggleable(&info))
        {
            continue;
        }
        if (Clay_PointerOver(CLAY_IDI("ExtModalRow", i)))
        {
            over_row = i;
            break;
        }
    }
    app->hovered_clickable = over_row >= 0;
    if (!IsMouseButtonPressed(MOUSE_BUTTON_LEFT))
    {
        return;
    }
    if (Clay_PointerOver(Clay_GetElementId(CLAY_STRING("ExtModalCard"))))
    {
        return;
    }
    if (Clay_PointerOver(Clay_GetElementId(CLAY_STRING("ExtModalDim"))))
    {
        PicoExts_Close();
    }
}

static void ExtsOnFrame(PicoHost *app, void *state, float dt)
{
    (void)dt;
    s_active_exts_state = state ? (ExtensionsState *)state : (ExtensionsState *)PicoPlugins_HostState(app, "extensions");
    if (!s_active_exts_state || !g_open || !pico_ui_modal_is_top(app, "extensions"))
    {
        return;
    }
    PicoScrollbar_UpdateDrag(&g_scrollbar, CLAY_STRING("ExtModalScroll"),
                             CLAY_STRING("ExtModalScrollHandle"));
    if (IsKeyPressed(KEY_ESCAPE))
    {
        PicoExts_Close();
        return;
    }
    if (!IsMouseButtonPressed(MOUSE_BUTTON_LEFT))
    {
        return;
    }
    int n = PicoPlugins_Count(app);
    for (int i = 0; i < n; i++)
    {
        PicoExtInfo info;
        if (!PicoPlugins_Get(app, i, &info) || !RowToggleable(&info))
        {
            continue;
        }
        if (Clay_PointerOver(CLAY_IDI("ExtModalRow", i)))
        {
            PicoPlugins_SetEnabled(app, i, !info.enabled);
            return;
        }
    }
}

static void CmdExtensions(PicoHost *app, PicoAgentId agent_id, const char *args, void *state)
{
    (void)state;
    (void)args;
    (void)agent_id;
    PicoExts_Open();
    PicoComposer_SetText(app, "");
    app->submit_cancel = true;
}

static int ExtsInit(PicoHost *app, void **state_out)
{
    ExtensionsState *s = (ExtensionsState *)calloc(1, sizeof(ExtensionsState));
    if (!s)
    {
        return 1;
    }
    s->app = app;
    if (state_out)
    {
        *state_out = s;
    }
    s_active_exts_state = s;
    pico_host_add_command(app, "extensions", "Manage extensions", CmdExtensions);
    pico_host_add_view(app, PICO_SLOT_OVERLAY, 10, ExtsRender);
    pico_host_add_hook(app, PICO_HOOK_AFTER_LAYOUT, ExtsAfterLayout);
    return 0;
}

static void ExtsShutdown(PicoHost *app, void *state)
{
    (void)app;
    ExtensionsState *s = (ExtensionsState *)state;
    if (!s)
    {
        return;
    }
    s_active_exts_state = s;
    (void)Unclaim();
    s->open = false;
    s->overflow = false;
    memset(&s->scrollbar, 0, sizeof(s->scrollbar));
    free(s);
    s_active_exts_state = NULL;
}

PicoExt pico_ext_extensions(void)
{
    return (PicoExt){
        .abi = PICO_EXT_ABI,
        .name = "extensions",
        .description = "Extension manager",
        .host_init = ExtsInit,
        .host_shutdown = ExtsShutdown,
        .host_on_frame = ExtsOnFrame,
    };
}
