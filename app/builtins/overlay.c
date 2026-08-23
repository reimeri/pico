#include "pico/plugin.h"

#include "../agent_internal.h"
#include "overlay.h"
#include "json.h"

#include "clay/clay.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define NOTIFY_TTL 5.0f
#define INVALID_ASK_ANSWER "{\"error\":\"invalid ask payload; fix it and try again\"}"

static char *g_notify;
static float g_notify_ttl;

static uint64_t g_ask_id;
static bool g_ask_show;
static char *g_ask_msg;

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
    const PicoAgent *agent = PicoApp_ActiveAgentConst(app);
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
}

static void RejectInvalidAsk(PicoApp *app, uint64_t id)
{
    ClearAskUi();
    pico_tool_answer(app, id, INVALID_ASK_ANSWER);
}

static void PrepareAsk(PicoApp *app)
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

static void RenderAsk(PicoApp *app)
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
            CLAY(CLAY_ID("AskModalScroll"),
                 {.layout = {.layoutDirection = CLAY_TOP_TO_BOTTOM,
                             .padding = {0, 8, 0, 0},
                             .sizing = {.width = CLAY_SIZING_GROW(0),
                                        .height = CLAY_SIZING_FIT(0, body_h)}},
                  .clip = {.vertical = true, .horizontal = false, .childOffset = Clay_GetScrollOffset()}})
            {
                CLAY_TEXT(CStr(g_ask_msg), CLAY_TEXT_CONFIG({.fontId = FONT_REGULAR,
                                                             .fontSize = 14,
                                                             .textColor = COLOR_TEXT,
                                                             .wrapMode = CLAY_TEXT_WRAP_WORDS}));
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

static void RenderError(PicoApp *app)
{
    const char *warn = app->status_warn;
    const char *agent = (!warn || !warn[0]) ? PicoApp_ActiveAgent(app)->error : NULL;
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
    RenderAsk(app);
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
    if (PicoExts_IsOpen() || PicoPrompt_IsOpen() || PicoFooter_MenuOpen() ||
        PicoChat_InspectIsOpen() || !IsKeyPressed(KEY_ESCAPE))
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

static void OverlayAfterLayout(PicoApp *app, const PicoHookEvent *event)
{
    (void)event;
    if (g_ask_show)
    {
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

    if (PicoExts_IsOpen() || PicoPrompt_IsOpen() || (!app->status_warn && !PicoApp_ActiveAgent(app)->error))
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
        else if (PicoApp_ActiveAgent(app)->error && PicoApp_ActiveAgent(app)->state == PICO_AGENT_ERROR)
        {
            free(PicoApp_ActiveAgent(app)->error);
            PicoApp_ActiveAgent(app)->error = NULL;
            PicoApp_ActiveAgent(app)->state = PICO_AGENT_IDLE;
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
