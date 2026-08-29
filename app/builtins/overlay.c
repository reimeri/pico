#include "pico/plugin.h"
#include "host_internal.h"

#include "../agent_internal.h"
#include "../composer_internal.h"
#include "overlay.h"
#include "json.h"
#include "scrollbar.h"

#include "clay/clay.h"

#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define NOTIFY_TTL 5.0f
#define NOTIFY_TOAST_RADIUS 8.0f
#define NOTIFY_TIMER_H 3.0f
#define INVALID_ASK_ANSWER "{\"error\":\"invalid ask payload; fix it and try again\"}"

typedef struct OverlayState {
    char *notify;
    float notify_ttl;
    uint64_t ask_id;
    bool ask_show;
    char *ask_msg;
    bool ask_overflow;
    PicoScrollbar ask_bar;
} OverlayState;

static __thread OverlayState *s_active_overlay_state = NULL;

static OverlayState *ActiveOverlayState(void)
{
    return s_active_overlay_state;
}

#define g_notify (ActiveOverlayState()->notify)
#define g_notify_ttl (ActiveOverlayState()->notify_ttl)
#define g_ask_id (ActiveOverlayState()->ask_id)
#define g_ask_show (ActiveOverlayState()->ask_show)
#define g_ask_msg (ActiveOverlayState()->ask_msg)
#define g_ask_overflow (ActiveOverlayState()->ask_overflow)
#define g_ask_bar (ActiveOverlayState()->ask_bar)

void PicoOverlay_Notify(PicoHost *app, const char *text)
{
    s_active_overlay_state = (OverlayState *)PicoPlugins_HostState(app, "overlay");
    if (!s_active_overlay_state)
    {
        return;
    }
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

static bool HasError(const PicoHost *app)
{
    const PicoAgent *agent = PicoHost_SelectedAgentConst(app);
    return (app->status_warn && app->status_warn[0]) || (agent->error && agent->error[0]);
}

static Clay_String CStr(const char *s)
{
    if (!s)
    {
        s = "";
    }
    return (Clay_String){.length = (int32_t)strlen(s), .chars = s};
}

static void ClearAskUi(void)
{
    free(g_ask_msg);
    g_ask_msg = NULL;
    g_ask_id = 0;
    g_ask_show = false;
    g_ask_overflow = false;
    memset(&g_ask_bar, 0, sizeof(g_ask_bar));
}

static void RejectInvalidAsk(PicoHost *app, uint64_t id)
{
    ClearAskUi();
    pico_tool_answer(app, id, INVALID_ASK_ANSWER);
}

static void PrepareAsk(PicoHost *app)
{
    PicoToolAsk ask;
    if (!pico_tool_pending_ask(app, &ask) || !ask.request_json)
    {
        ClearAskUi();
        return;
    }
    if (g_ask_show && g_ask_id == ask.id && g_ask_msg)
    {
        return;
    }

    ClearAskUi();
    JsonDoc doc;
    if (JsonParse(&doc, ask.request_json, strlen(ask.request_json)) != 0)
    {
        RejectInvalidAsk(app, ask.id);
        return;
    }
    if (!JsonEq(&doc, JsonObjGet(&doc, 0, "type"), "confirm") ||
        JsonEq(&doc, JsonObjGet(&doc, 0, "ui"), "custom"))
    {
        JsonFree(&doc);
        return;
    }
    char *msg = JsonObjStr(&doc, 0, "message");
    JsonFree(&doc);
    if (!msg || !msg[0])
    {
        free(msg);
        RejectInvalidAsk(app, ask.id);
        return;
    }
    g_ask_id = ask.id;
    g_ask_show = true;
    g_ask_msg = msg;
}

static void AskButton(Clay_String id, const char *label)
{
    Clay_ElementId eid = CLAY_SID(id);
    bool hover = Clay_PointerOver(eid);
    CLAY(eid, {.layout = {.padding = {14, 14, 8, 8}},
               .backgroundColor = hover ? COLOR_CODE_BG : COLOR_FOOTER_BG,
               .cornerRadius = CLAY_CORNER_RADIUS(6)})
    {
        CLAY_TEXT(CStr(label), CLAY_TEXT_CONFIG({.fontId = FONT_BOLD, .fontSize = 14, .textColor = COLOR_TEXT}));
    }
}

static void RenderAsk(PicoHost *app)
{
    PrepareAsk(app);
    if (!g_ask_show)
    {
        return;
    }
    float sw = (float)GetScreenWidth();
    float sh = (float)GetScreenHeight();
    float card_w = sw < 680.0f ? sw - 48.0f : 580.0f;
    if (card_w < 260.0f)
    {
        card_w = 260.0f;
    }
    float body_h = sh * 0.55f;
    if (body_h < 120.0f)
    {
        body_h = 120.0f;
    }
    if (body_h > 480.0f)
    {
        body_h = 480.0f;
    }

    CLAY(CLAY_ID("AskModalDim"),
         {.floating = {.attachTo = CLAY_ATTACH_TO_ROOT,
                       .zIndex = 42,
                       .attachPoints = {.element = CLAY_ATTACH_POINT_LEFT_TOP,
                                        .parent = CLAY_ATTACH_POINT_LEFT_TOP}},
          .layout = {.layoutDirection = CLAY_TOP_TO_BOTTOM,
                     .childAlignment = {.x = CLAY_ALIGN_X_CENTER, .y = CLAY_ALIGN_Y_CENTER},
                     .sizing = {.width = CLAY_SIZING_FIXED(sw), .height = CLAY_SIZING_FIXED(sh)}},
          .backgroundColor = {0, 0, 0, 140}})
    {
        CLAY(CLAY_ID("AskModalCard"),
             {.layout = {.layoutDirection = CLAY_TOP_TO_BOTTOM,
                         .padding = {20, 20, 16, 16},
                         .childGap = 14,
                         .sizing = {.width = CLAY_SIZING_FIXED(card_w)}},
              .backgroundColor = COLOR_CONTENT_BG,
              .cornerRadius = CLAY_CORNER_RADIUS(8)})
        {
            CLAY_TEXT(CLAY_STRING("Confirm"),
                      CLAY_TEXT_CONFIG({.fontId = FONT_BOLD, .fontSize = 18, .textColor = COLOR_TEXT}));
            CLAY(CLAY_ID("AskModalScrollRow"),
                 {.layout = {.layoutDirection = CLAY_LEFT_TO_RIGHT,
                             .childGap = SCROLLBAR_GAP,
                             .sizing = {.width = CLAY_SIZING_GROW(0),
                                        .height = CLAY_SIZING_FIT(0, body_h)}}})
            {
                CLAY(CLAY_ID("AskModalScroll"),
                     {.layout = {.layoutDirection = CLAY_TOP_TO_BOTTOM,
                                 .sizing = {.width = CLAY_SIZING_GROW(0),
                                            .height = CLAY_SIZING_GROW(0)}},
                      .clip = {.vertical = true, .horizontal = false, .childOffset = Clay_GetScrollOffset()}})
                {
                    CLAY_TEXT(CStr(g_ask_msg), CLAY_TEXT_CONFIG({.fontId = FONT_REGULAR,
                                                                 .fontSize = 14,
                                                                 .textColor = COLOR_TEXT,
                                                                 .wrapMode = CLAY_TEXT_WRAP_WORDS}));
                }
                if (g_ask_overflow)
                {
                    PicoScrollbar_Render(CLAY_STRING("AskModalScroll"), CLAY_STRING("AskModalScrollTrack"),
                                         CLAY_STRING("AskModalScrollHandle"));
                }
            }
            CLAY(CLAY_ID("AskModalButtons"),
                 {.layout = {.layoutDirection = CLAY_LEFT_TO_RIGHT,
                             .childGap = 8,
                             .childAlignment = {.x = CLAY_ALIGN_X_RIGHT},
                             .sizing = {.width = CLAY_SIZING_GROW(0)}}})
            {
                AskButton(CLAY_STRING("AskDeny"), "Deny");
                AskButton(CLAY_STRING("AskApprove"), "Approve");
            }
        }
    }
}

static void RenderError(PicoHost *app)
{
    const char *warn = app->status_warn;
    const char *agent = (!warn || !warn[0]) ? PicoHost_SelectedAgent(app)->error : NULL;
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

static Color ClayToRay(Clay_Color c)
{
    return (Color){(unsigned char)c.r, (unsigned char)c.g, (unsigned char)c.b, (unsigned char)c.a};
}

static float NotifyRemaining(void)
{
    float remaining = g_notify_ttl / NOTIFY_TTL;
    if (remaining < 0.0f)
    {
        return 0.0f;
    }
    if (remaining > 1.0f)
    {
        return 1.0f;
    }
    return remaining;
}

static void RenderToast(PicoHost *app)
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
                     .sizing = {.width = CLAY_SIZING_FIT(200, 720)}},
          .backgroundColor = COLOR_CONTENT_BG,
          .cornerRadius = CLAY_CORNER_RADIUS(NOTIFY_TOAST_RADIUS)})
    {
        CLAY(CLAY_ID("NotifyToastBody"),
             {.layout = {.padding = {16, 16, 12, 12},
                         .sizing = {.width = CLAY_SIZING_GROW(0)}}})
        {
            CLAY_TEXT(text, CLAY_TEXT_CONFIG({.fontId = FONT_REGULAR,
                                              .fontSize = 14,
                                              .textColor = COLOR_TEXT,
                                              .wrapMode = CLAY_TEXT_WRAP_WORDS}));
        }
        CLAY(CLAY_ID("NotifyToastTimer"),
             {.layout = {.sizing = {.width = CLAY_SIZING_GROW(0),
                                    .height = CLAY_SIZING_FIXED(NOTIFY_TIMER_H)}}})
        {
        }
    }
}

void PicoOverlay_Render(PicoHost *app, void *state)
{
    s_active_overlay_state = state ? (OverlayState *)state : (OverlayState *)PicoPlugins_HostState(app, "overlay");
    if (!s_active_overlay_state)
    {
        return;
    }
    RenderError(app);
    RenderToast(app);
    RenderAsk(app);
}

void PicoOverlay_OnFrame(PicoHost *app, void *state, float dt)
{
    s_active_overlay_state = state ? (OverlayState *)state : (OverlayState *)PicoPlugins_HostState(app, "overlay");
    if (!s_active_overlay_state)
    {
        return;
    }
    if (g_ask_show)
    {
        PicoScrollbar_UpdateDrag(&g_ask_bar, CLAY_STRING("AskModalScroll"),
                                 CLAY_STRING("AskModalScrollHandle"));
    }
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
    if (pico_ui_modal_claimed(app) || PicoFooter_MenuOpen() || !IsKeyPressed(KEY_ESCAPE))
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

static bool OverAsk(Clay_String id)
{
    return Clay_PointerOver(CLAY_SID(id));
}

static void OverlayAfterLayout(PicoHost *app, const PicoHookEvent *event, void *state)
{
    (void)event;
    s_active_overlay_state = state ? (OverlayState *)state : (OverlayState *)PicoPlugins_HostState(app, "overlay");
    if (!s_active_overlay_state)
    {
        return;
    }
    if (g_ask_show)
    {
        g_ask_overflow = PicoScrollbar_Overflows(CLAY_STRING("AskModalScroll"));
        if (OverAsk(CLAY_STRING("AskApprove")) || OverAsk(CLAY_STRING("AskDeny")))
        {
            app->hovered_clickable = true;
        }
        if (IsMouseButtonPressed(MOUSE_BUTTON_LEFT))
        {
            if (OverAsk(CLAY_STRING("AskApprove")))
            {
                pico_tool_answer(app, g_ask_id, "{\"ok\":true}");
                return;
            }
            if (OverAsk(CLAY_STRING("AskDeny")))
            {
                pico_tool_answer(app, g_ask_id, "{\"ok\":false}");
                return;
            }
        }
    }

    PicoAgent *agent = PicoHost_SelectedAgent(app);
    if (PicoExts_IsOpen() || PicoPrompt_IsOpen() || (!app->status_warn && !(agent && agent->error)))
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
        else if (agent && agent->error && agent->state == PICO_AGENT_ERROR)
        {
            free(agent->error);
            agent->error = NULL;
            agent->state = PICO_AGENT_IDLE;
        }
    }
}

static void OverlayAfterRender(PicoHost *app, const PicoHookEvent *event, void *state)
{
    (void)event;
    s_active_overlay_state = state ? (OverlayState *)state : (OverlayState *)PicoPlugins_HostState(app, "overlay");
    if (!s_active_overlay_state || !g_notify || !g_notify[0] || g_notify_ttl <= 0.0f)
    {
        return;
    }
    Clay_ElementData toast = Clay_GetElementData(Clay_GetElementId(CLAY_STRING("NotifyToast")));
    if (!toast.found || toast.boundingBox.width <= 0.0f || toast.boundingBox.height <= 0.0f)
    {
        return;
    }
    Clay_BoundingBox b = toast.boundingBox;
    float remaining = NotifyRemaining();
    float min_dim = b.width < b.height ? b.width : b.height;
    float roundness = min_dim > 0.0f ? (NOTIFY_TOAST_RADIUS * 2.0f) / min_dim : 0.0f;
    Rectangle rec = {b.x, b.y, b.width, b.height};
    int x0 = (int)roundf(b.x);
    int y1 = (int)roundf(b.y + b.height);
    int x1 = (int)roundf(b.x + b.width);
    int bar = (int)roundf(NOTIFY_TIMER_H);
    if (bar < 1)
    {
        bar = 1;
    }
    int width = x1 - x0;
    if (width <= 0)
    {
        return;
    }
    BeginScissorMode(x0, y1 - bar, width, bar);
    DrawRectangleRounded(rec, roundness, 8, ClayToRay(COLOR_HR));
    EndScissorMode();
    int fill = (int)roundf((float)width * remaining);
    if (fill > 0)
    {
        BeginScissorMode(x0, y1 - bar, fill, bar);
        DrawRectangleRounded(rec, roundness, 8, ClayToRay(COLOR_LINK));
        EndScissorMode();
    }
}

static int OverlayInit(PicoHost *app, void **state_out)
{
    OverlayState *s = (OverlayState *)calloc(1, sizeof(OverlayState));
    if (!s)
    {
        return 1;
    }
    if (state_out)
    {
        *state_out = s;
    }
    pico_host_add_view(app, PICO_SLOT_OVERLAY, 0, PicoOverlay_Render);
    pico_host_add_hook(app, PICO_HOOK_AFTER_LAYOUT, OverlayAfterLayout);
    pico_host_add_hook(app, PICO_HOOK_AFTER_RENDER, OverlayAfterRender);
    return 0;
}

static void OverlayShutdown(PicoHost *app, void *state)
{
    (void)app;
    OverlayState *s = (OverlayState *)state;
    if (!s)
    {
        return;
    }
    free(s->notify);
    free(s->ask_msg);
    free(s);
}

PicoExt pico_ext_overlay(void)
{
    return (PicoExt){
        .abi = PICO_EXT_ABI,
        .name = "overlay",
        .description = "Errors and notifications",
        .host_init = OverlayInit,
        .host_shutdown = OverlayShutdown,
        .host_on_frame = PicoOverlay_OnFrame,
    };
}
