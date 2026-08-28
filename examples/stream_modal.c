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

typedef struct StreamState {
    char status[PICO_UI_POST_STATUS_MAX];
    char text[PICO_UI_POST_TEXT_MAX + 1];
} StreamState;

static const char *kParams = "{\"type\":\"object\",\"properties\":{}}";

static void CloseModal(PicoHost *app)
{
    (void)pico_ui_modal_pop(app, kName);
}

static void OpenModal(PicoHost *app)
{
    (void)pico_ui_modal_push(app, kName);
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
    StreamState *s = (StreamState *)state;
    if (!s || !pico_ui_modal_has(app, kName))
    {
        return;
    }
    float sw = (float)GetScreenWidth();
    float sh = (float)GetScreenHeight();
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
            if (s->status[0])
            {
                CLAY_TEXT(CStr(s->status),
                          CLAY_TEXT_CONFIG({.fontId = FONT_BOLD,
                                            .fontSize = 14,
                                            .textColor = COLOR_MUTED,
                                            .wrapMode = CLAY_TEXT_WRAP_WORDS}));
            }
            CLAY_TEXT(s->text[0] ? CStr(s->text) : CLAY_STRING("Waiting for worker posts…"),
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
    StreamState *s = (StreamState *)state;
    (void)event;
    if (!s || !pico_ui_modal_is_top(app, kName))
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
        s->status[0] = '\0';
        s->text[0] = '\0';
    }
}

static void StreamOnFrame(PicoHost *app, void *state, float dt)
{
    StreamState *s = (StreamState *)state;
    PicoUiPost post;
    (void)dt;
    if (!s)
    {
        return;
    }
    if (pico_ui_latest(app, kName, &post))
    {
        snprintf(s->status, sizeof(s->status), "%s", post.status ? post.status : "");
        snprintf(s->text, sizeof(s->text), "%s", post.text ? post.text : "");
        OpenModal(app);
    }
    if (!pico_ui_modal_is_top(app, kName))
    {
        return;
    }
    if (IsKeyPressed(KEY_ESCAPE))
    {
        CloseModal(app);
        pico_ui_clear(app, kName);
        s->status[0] = '\0';
        s->text[0] = '\0';
    }
}

static void CmdStream(PicoHost *app, PicoAgentId agent_id, const char *args, void *state)
{
    StreamState *s = (StreamState *)state;
    (void)args;
    (void)agent_id;
    if (!s)
    {
        return;
    }
    if (pico_ui_modal_has(app, kName))
    {
        CloseModal(app);
        pico_ui_clear(app, kName);
        s->status[0] = '\0';
        s->text[0] = '\0';
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
    StreamState *s = (StreamState *)calloc(1, sizeof(StreamState));
    if (!s)
    {
        return 1;
    }
    if (state_out)
    {
        *state_out = s;
    }
    PicoUiPost post;
    if (pico_ui_latest(app, kName, &post) && !pico_ui_modal_has(app, kName))
    {
        snprintf(s->status, sizeof(s->status), "%s", post.status ? post.status : "");
        snprintf(s->text, sizeof(s->text), "%s", post.text ? post.text : "");
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
    StreamState *s = (StreamState *)state;
    CloseModal(app);
    free(s);
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
