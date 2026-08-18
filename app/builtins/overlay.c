#include "pico/plugin.h"

#include "clay/clay.h"

#include <stdlib.h>
#include <string.h>

void PicoOverlay_Render(PicoApp *app)
{
    if (!app->status_warn || !app->status_warn[0])
    {
        return;
    }

    Clay_String text = {.length = (int32_t)strlen(app->status_warn), .chars = app->status_warn};
    CLAY(CLAY_ID("ExtWarn"),
         {.floating = {.attachTo = CLAY_ATTACH_TO_ROOT,
                       .zIndex = 20,
                       .attachPoints = {.element = CLAY_ATTACH_POINT_CENTER_TOP,
                                        .parent = CLAY_ATTACH_POINT_CENTER_TOP},
                       .offset = {.y = 12}},
          .layout = {.layoutDirection = CLAY_TOP_TO_BOTTOM,
                     .padding = {16, 16, 12, 12},
                     .childGap = 8,
                     .sizing = {.width = CLAY_SIZING_FIT(200, 720)}},
          .backgroundColor = COLOR_ERROR_BG,
          .cornerRadius = CLAY_CORNER_RADIUS(8)})
    {
        CLAY_TEXT(CLAY_STRING("Extension error  (Esc to dismiss, F5 to reload)"),
                  CLAY_TEXT_CONFIG({.fontId = FONT_BOLD, .fontSize = 14, .textColor = COLOR_TEXT}));
        CLAY_TEXT(text, CLAY_TEXT_CONFIG({.fontId = FONT_MONO, .fontSize = 13, .textColor = COLOR_MUTED}));
    }
}

void PicoOverlay_OnFrame(PicoApp *app, float dt)
{
    (void)dt;
    if (app->status_warn && IsKeyPressed(KEY_ESCAPE))
    {
        free(app->status_warn);
        app->status_warn = NULL;
    }
}

static void OverlayAfterLayout(PicoApp *app)
{
    if (!app->status_warn)
    {
        return;
    }
    if (IsMouseButtonPressed(MOUSE_BUTTON_LEFT) &&
        Clay_PointerOver(Clay_GetElementId(CLAY_STRING("ExtWarn"))))
    {
        free(app->status_warn);
        app->status_warn = NULL;
    }
}

static void OverlayInit(PicoApp *app)
{
    pico_add_view(app, PICO_SLOT_OVERLAY, 0, PicoOverlay_Render);
    pico_add_hook(app, PICO_HOOK_AFTER_LAYOUT, OverlayAfterLayout);
}

PicoExt pico_ext_overlay(void)
{
    return (PicoExt){
        .abi = PICO_EXT_ABI,
        .name = "overlay",
        .init = OverlayInit,
        .on_frame = PicoOverlay_OnFrame,
    };
}
