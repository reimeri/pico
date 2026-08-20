#include "pico/plugin.h"
#include "overlay.h"
#include "json.h"

#include "clay/clay.h"

#include <stdlib.h>
#include <string.h>

#define NOTIFY_TTL 5.0f

static char *g_notify;
static float g_notify_ttl;

void PicoOverlay_Notify(PicoApp *app, const char *text)
{
    (void)app;
    free(g_notify);
    g_notify = NULL;
    g_notify_ttl = 0.0f;
    if (!text || !text[0])
    {
        return;
    }
    g_notify = JsonDup(text);
    if (g_notify)
    {
        g_notify_ttl = NOTIFY_TTL;
    }
}

static bool HasError(const PicoApp *app)
{
    return (app->status_warn && app->status_warn[0]) || (app->agent_error && app->agent_error[0]);
}

static void RenderError(PicoApp *app)
{
    const char *warn = app->status_warn;
    const char *agent = (!warn || !warn[0]) ? app->agent_error : NULL;
    if ((!warn || !warn[0]) && (!agent || !agent[0]))
    {
        return;
    }

    const char *title = warn && warn[0] ? "Extension error  (Esc to dismiss, F5 to reload)"
                                        : "Agent error  (Esc to dismiss)";
    const char *body = warn && warn[0] ? warn : agent;
    Clay_String title_s = {.length = (int32_t)strlen(title), .chars = title};
    Clay_String text = {.length = (int32_t)strlen(body), .chars = body};
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
        CLAY_TEXT(title_s, CLAY_TEXT_CONFIG({.fontId = FONT_BOLD, .fontSize = 14, .textColor = COLOR_TEXT}));
        CLAY_TEXT(text, CLAY_TEXT_CONFIG({.fontId = FONT_MONO,
                                          .fontSize = 13,
                                          .textColor = COLOR_MUTED,
                                          .wrapMode = CLAY_TEXT_WRAP_WORDS}));
    }
}

static void RenderToast(PicoApp *app)
{
    if (!g_notify || !g_notify[0] || g_notify_ttl <= 0.0f)
    {
        return;
    }
    float y = HasError(app) ? 110.0f : 12.0f;
    Clay_String text = {.length = (int32_t)strlen(g_notify), .chars = g_notify};
    CLAY(CLAY_ID("NotifyToast"),
         {.floating = {.attachTo = CLAY_ATTACH_TO_ROOT,
                       .zIndex = 21,
                       .attachPoints = {.element = CLAY_ATTACH_POINT_CENTER_TOP,
                                        .parent = CLAY_ATTACH_POINT_CENTER_TOP},
                       .offset = {.y = y}},
          .layout = {.layoutDirection = CLAY_TOP_TO_BOTTOM,
                     .padding = {16, 16, 12, 12},
                     .sizing = {.width = CLAY_SIZING_FIT(200, 720)}},
          .backgroundColor = COLOR_CONTENT_BG,
          .cornerRadius = CLAY_CORNER_RADIUS(8)})
    {
        CLAY_TEXT(text, CLAY_TEXT_CONFIG({.fontId = FONT_REGULAR,
                                          .fontSize = 14,
                                          .textColor = COLOR_TEXT,
                                          .wrapMode = CLAY_TEXT_WRAP_WORDS}));
    }
}

void PicoOverlay_Render(PicoApp *app)
{
    RenderError(app);
    RenderToast(app);
}

void PicoOverlay_OnFrame(PicoApp *app, float dt)
{
    if (g_notify)
    {
        g_notify_ttl -= dt;
        if (g_notify_ttl <= 0.0f)
        {
            free(g_notify);
            g_notify = NULL;
            g_notify_ttl = 0.0f;
        }
    }
    if (PicoExts_IsOpen() || !IsKeyPressed(KEY_ESCAPE))
    {
        return;
    }
    if (app->status_warn)
    {
        free(app->status_warn);
        app->status_warn = NULL;
        return;
    }
}

static void OverlayAfterLayout(PicoApp *app)
{
    if (PicoExts_IsOpen() || (!app->status_warn && !app->agent_error))
    {
        return;
    }
    if (IsMouseButtonPressed(MOUSE_BUTTON_LEFT) &&
        Clay_PointerOver(Clay_GetElementId(CLAY_STRING("ExtWarn"))))
    {
        if (app->status_warn)
        {
            free(app->status_warn);
            app->status_warn = NULL;
        }
        else if (app->agent_error && app->agent_state == PICO_AGENT_ERROR)
        {
            free(app->agent_error);
            app->agent_error = NULL;
            app->agent_state = PICO_AGENT_IDLE;
        }
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
        .description = "Errors and notifications",
        .init = OverlayInit,
        .on_frame = PicoOverlay_OnFrame,
    };
}
