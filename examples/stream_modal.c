// Stream worker text into a named overlay. Copy then F5:
//
//   mkdir -p ~/.config/pico/extensions/stream_modal
//   cp examples/stream_modal.c ~/.config/pico/extensions/stream_modal/

#include "pico/plugin.h"
#include "json.h"

#include "clay/clay.h"

#include <stdio.h>
#include <string.h>

static const char *kName = "stream-modal";
static bool g_open;
static char g_status[PICO_UI_POST_STATUS_MAX];
static char g_text[PICO_UI_POST_TEXT_MAX + 1];

static const char *kParams = "{\"type\":\"object\",\"properties\":{}}";

static void CloseModal(PicoApp *app)
{
    if (!g_open)
    {
        return;
    }
    (void)pico_ui_modal_pop(app, kName);
    g_open = false;
}

static void OpenModal(PicoApp *app)
{
    if (g_open)
    {
        return;
    }
    if (!pico_ui_modal_push(app, kName))
    {
        return;
    }
    g_open = true;
}

static Clay_String CStr(const char *s)
{
    if (!s)
    {
        s = "";
    }
    return (Clay_String){.length = (int32_t)strlen(s), .chars = s};
}

static void StreamRender(PicoApp *app)
{
    float sw;
    float sh;
    if (!g_open)
    {
        return;
    }
    sw = (float)GetScreenWidth();
    sh = (float)GetScreenHeight();
    CLAY(CLAY_ID("StreamModalDim"),
         {.floating = {.attachTo = CLAY_ATTACH_TO_ROOT,
                       .zIndex = 50,
                       .attachPoints = {.element = CLAY_ATTACH_POINT_LEFT_TOP,
                                        .parent = CLAY_ATTACH_POINT_LEFT_TOP}},
          .layout = {.layoutDirection = CLAY_TOP_TO_BOTTOM,
                     .childAlignment = {.x = CLAY_ALIGN_X_CENTER, .y = CLAY_ALIGN_Y_CENTER},
                     .sizing = {.width = CLAY_SIZING_FIXED(sw), .height = CLAY_SIZING_FIXED(sh)}},
          .backgroundColor = {0, 0, 0, 140}})
    {
        CLAY(CLAY_ID("StreamModalCard"),
             {.layout = {.layoutDirection = CLAY_TOP_TO_BOTTOM,
                         .padding = {20, 20, 16, 16},
                         .childGap = 8,
                         .sizing = {.width = CLAY_SIZING_FIXED(460)}},
              .backgroundColor = COLOR_CONTENT_BG,
              .cornerRadius = CLAY_CORNER_RADIUS(8)})
        {
            CLAY_TEXT(CLAY_STRING("Stream mailbox"),
                      CLAY_TEXT_CONFIG({.fontId = FONT_BOLD, .fontSize = 16, .textColor = COLOR_TEXT}));
            if (g_status[0])
            {
                CLAY_TEXT(CStr(g_status),
                          CLAY_TEXT_CONFIG({.fontId = FONT_BOLD,
                                            .fontSize = 14,
                                            .textColor = COLOR_MUTED,
                                            .wrapMode = CLAY_TEXT_WRAP_WORDS}));
            }
            CLAY_TEXT(g_text[0] ? CStr(g_text) : CLAY_STRING("Waiting for worker posts…"),
                      CLAY_TEXT_CONFIG({.fontId = FONT_REGULAR,
                                        .fontSize = 14,
                                        .textColor = COLOR_TEXT,
                                        .wrapMode = CLAY_TEXT_WRAP_WORDS}));
            (void)app;
        }
    }
}

static void StreamAfterLayout(PicoApp *app, const PicoHookEvent *event)
{
    (void)event;
    if (!g_open)
    {
        return;
    }
    if (Clay_PointerOver(Clay_GetElementId(CLAY_STRING("StreamModalCard"))))
    {
        app->hovered_clickable = true;
        return;
    }
    if (IsMouseButtonPressed(MOUSE_BUTTON_LEFT) &&
        Clay_PointerOver(Clay_GetElementId(CLAY_STRING("StreamModalDim"))))
    {
        CloseModal(app);
        pico_ui_clear(app, kName);
        g_status[0] = '\0';
        g_text[0] = '\0';
    }
}

static void StreamOnFrame(PicoApp *app, float dt)
{
    PicoUiPost post;
    (void)dt;
    if (pico_ui_latest(app, kName, &post))
    {
        snprintf(g_status, sizeof(g_status), "%s", post.status ? post.status : "");
        snprintf(g_text, sizeof(g_text), "%s", post.text ? post.text : "");
        OpenModal(app);
    }
    if (!g_open)
    {
        return;
    }
    if (IsKeyPressed(KEY_ESCAPE))
    {
        CloseModal(app);
        pico_ui_clear(app, kName);
        g_status[0] = '\0';
        g_text[0] = '\0';
    }
}

static void CmdStream(PicoApp *app, const char *args)
{
    (void)args;
    if (g_open)
    {
        CloseModal(app);
        pico_ui_clear(app, kName);
        g_status[0] = '\0';
        g_text[0] = '\0';
    }
    else
    {
        OpenModal(app);
    }
    PicoComposer_SetText(app, "");
    app->submit_cancel = true;
}

static void StreamDemoRun(PicoAgentContext *ctx, const char *args_json, PicoToolResult *out)
{
    (void)args_json;
    pico_ui_post(ctx, kName, PICO_UI_POST_STATUS, "searching", 9);
    pico_ui_post(ctx, kName, PICO_UI_POST_TEXT, "Query: example\n", 15);
    pico_ui_post(ctx, kName, PICO_UI_POST_TEXT, "1. First hit\n", 13);
    pico_ui_post(ctx, kName, PICO_UI_POST_TEXT, "2. Second hit\n", 14);
    pico_ui_post(ctx, kName, PICO_UI_POST_STATUS, "done", 4);
    if (out)
    {
        memset(out, 0, sizeof(*out));
        out->output = JsonDup("stream_demo: posted search progress into the overlay");
    }
}

static void StreamToolRow(PicoApp *app, PicoToolRowEvent *ev)
{
    if (!ev || !ev->name || strcmp(ev->name, "stream_demo") != 0)
    {
        return;
    }
    OpenModal(app);
    ev->handled = true;
}

static void StreamInit(PicoApp *app)
{
    PicoUiPost post;
    if (pico_ui_latest(app, kName, &post) && !pico_ui_modal_has(app, kName))
    {
        snprintf(g_status, sizeof(g_status), "%s", post.status ? post.status : "");
        snprintf(g_text, sizeof(g_text), "%s", post.text ? post.text : "");
        (void)pico_ui_modal_push(app, kName);
        g_open = true;
    }
    else if (g_open && !pico_ui_modal_has(app, kName))
    {
        (void)pico_ui_modal_push(app, kName);
    }
    pico_add_view(app, PICO_SLOT_OVERLAY, 50, StreamRender);
    pico_add_hook(app, PICO_HOOK_AFTER_LAYOUT, StreamAfterLayout);
    pico_add_command(app, "stream", "Toggle the streaming overlay modal", CmdStream);
    pico_add_tool(app, "stream_demo", "Post fake search progress into the overlay mailbox", kParams,
                  StreamDemoRun, NULL);
    pico_add_tool_row_hook(app, StreamToolRow);
}

static void StreamShutdown(PicoApp *app)
{
    CloseModal(app);
}

PicoExt pico_ext(void)
{
    return (PicoExt){
        .abi = PICO_EXT_ABI,
        .name = "stream-modal",
        .description = "Named mailbox streaming into an overlay modal",
        .init = StreamInit,
        .shutdown = StreamShutdown,
        .on_frame = StreamOnFrame,
    };
}
