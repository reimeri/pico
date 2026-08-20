#include "pico/plugin.h"

#include "clay/clay.h"

#include <string.h>

static bool g_open;

void PicoExts_Close(void)
{
    g_open = false;
}

bool PicoExts_IsOpen(void)
{
    return g_open;
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
        desc = "Failed to load";
    }
    else if (info->description && info->description[0])
    {
        desc = info->description;
    }

    CLAY(CLAY_IDI("ExtModalRow", index),
         {.layout = {.layoutDirection = CLAY_LEFT_TO_RIGHT,
                     .childGap = 12,
                     .childAlignment = {.y = CLAY_ALIGN_Y_CENTER},
                     .padding = {10, 10, 8, 8},
                     .sizing = {.width = CLAY_SIZING_GROW(0)}},
          .backgroundColor = info->loaded ? COLOR_CODE_BG : COLOR_ERROR_BG,
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
                                                        .fontSize = 15,
                                                        .textColor = COLOR_TEXT,
                                                        .wrapMode = CLAY_TEXT_WRAP_WORDS}));
                if (info->builtin)
                {
                    CLAY_TEXT(CLAY_STRING("built-in"),
                              CLAY_TEXT_CONFIG({.fontId = FONT_REGULAR,
                                                .fontSize = 12,
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
                                                        .fontSize = 13,
                                                        .textColor = COLOR_MUTED,
                                                        .wrapMode = CLAY_TEXT_WRAP_WORDS}));
            }
        }
    }
}

static void RenderSection(bool builtin, Clay_String title)
{
    CLAY_TEXT(title, CLAY_TEXT_CONFIG({.fontId = FONT_REGULAR,
                                       .fontSize = 12,
                                       .textColor = COLOR_MUTED,
                                       .wrapMode = CLAY_TEXT_WRAP_NONE}));

    int n = PicoPlugins_Count();
    int shown = 0;
    for (int i = 0; i < n; i++)
    {
        PicoExtInfo info;
        if (!PicoPlugins_Get(i, &info) || info.builtin != builtin)
        {
            continue;
        }
        RenderRow(i, &info);
        shown++;
    }
    if (shown == 0)
    {
        CLAY_TEXT(CLAY_STRING("None"), CLAY_TEXT_CONFIG({.fontId = FONT_REGULAR,
                                                         .fontSize = 13,
                                                         .textColor = COLOR_MUTED,
                                                         .wrapMode = CLAY_TEXT_WRAP_NONE}));
    }
}

static void ExtsRender(PicoApp *app)
{
    (void)app;
    if (!g_open)
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
                      CLAY_TEXT_CONFIG({.fontId = FONT_BOLD, .fontSize = 18, .textColor = COLOR_TEXT}));

            CLAY(CLAY_ID("ExtModalScroll"),
                 {.layout = {.layoutDirection = CLAY_TOP_TO_BOTTOM,
                             .childGap = 10,
                             .sizing = {.width = CLAY_SIZING_GROW(0), .height = CLAY_SIZING_GROW(0)}},
                  .clip = {.vertical = true, .horizontal = false, .childOffset = Clay_GetScrollOffset()}})
            {
                RenderSection(true, CLAY_STRING("Built-in"));
                RenderSection(false, CLAY_STRING("User"));
            }
        }
    }
}

static void ExtsAfterLayout(PicoApp *app)
{
    (void)app;
    if (!g_open || !IsMouseButtonPressed(MOUSE_BUTTON_LEFT))
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

static void ExtsOnFrame(PicoApp *app, float dt)
{
    (void)app;
    (void)dt;
    if (g_open && IsKeyPressed(KEY_ESCAPE))
    {
        PicoExts_Close();
    }
}

static void CmdExtensions(PicoApp *app, const char *args)
{
    (void)args;
    g_open = true;
    PicoComposer_SetText(app, "");
    app->submit_cancel = true;
}

static void ExtsInit(PicoApp *app)
{
    pico_add_command(app, "extensions", "Show installed extensions", CmdExtensions);
    pico_add_view(app, PICO_SLOT_OVERLAY, 10, ExtsRender);
    pico_add_hook(app, PICO_HOOK_AFTER_LAYOUT, ExtsAfterLayout);
}

PicoExt pico_ext_extensions(void)
{
    return (PicoExt){
        .abi = PICO_EXT_ABI,
        .name = "extensions",
        .description = "Extension manager",
        .init = ExtsInit,
        .on_frame = ExtsOnFrame,
    };
}
