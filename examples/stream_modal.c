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

static void CloseModal(PicoHost *app)
{
    if (!g_open)
    {
        return;
    }
    (void)pico_ui_modal_pop(app, kName);
    g_open = false;
}

static void OpenModal(PicoHost *app)
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

static void StreamRender(PicoHost *app, void *state)
{
    (void)state;
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

static void StreamAfterLayout(PicoHost *app, const PicoHookEvent *event, void *state)
{
    (void)state;
    (void)event;
    if (!g_open)
    {
        return;
    }
    if (Clay_PointerOver(Clay_GetElementId(CLAY_STRING("StreamModalCard"))))
    {
        pico_host_set_hovered_clickable(app);
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

static void StreamOnFrame(PicoHost *app, void *state, float dt)
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

static void CmdStream(PicoHost *app, PicoAgentId agent_id, const char *args, void *state)
{
    (void)state;
    (void)args;
    (void)agent_id;
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
    PicoHost_RequestSubmitCancel(app);
}

static void StreamDemoRun(PicoAgentContext *ctx, const char *args_json, PicoToolResult *out, void *state)
{
    (void)state;
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

static void StreamToolRow(PicoWorkspace *workspace, PicoToolRowEvent *event, void *state)
{
    (void)state;
    if (!event || !event->name || strcmp(event->name, "stream_demo") != 0)
    {
        return;
    }
    OpenModal(pico_workspace_host(workspace));
    event->handled = true;
}

static int StreamHostInit(PicoHost *app, void **state_out)
{
    (void)state_out;
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
    pico_host_add_view(app, PICO_SLOT_OVERLAY, 50, StreamRender);
    pico_host_add_hook(app, PICO_HOOK_AFTER_LAYOUT, StreamAfterLayout);
    pico_host_add_command(app, "stream", "Toggle the streaming overlay modal", CmdStream);
    return 0;
}

static int StreamWorkspaceInit(PicoWorkspace *workspace, void **state_out)
{
    (void)state_out;
    pico_add_tool(workspace, "stream_demo", "Post fake search progress into the overlay mailbox", kParams,
                  StreamDemoRun, NULL);
    pico_add_tool_row_hook(workspace, StreamToolRow);
    return 0;
}

static void StreamShutdown(PicoHost *app, void *state)
{
    (void)state;
    CloseModal(app);
}

PicoExt pico_ext(void)
{
    return (PicoExt){
        .abi = PICO_EXT_ABI,
        .name = "stream-modal",
        .description = "Named mailbox streaming into an overlay modal",
        .host_init = StreamHostInit,
        .workspace_init = StreamWorkspaceInit,
        .host_shutdown = StreamShutdown,
        .host_on_frame = StreamOnFrame,
    };
}
