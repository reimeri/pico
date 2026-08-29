// Stream worker text into a named overlay mailbox keyed by
// (agent_id, runtime_generation, name). Copy then F5:
//
//   mkdir -p ~/.config/pico/extensions/stream_modal
//   cp examples/stream_modal.c ~/.config/pico/extensions/stream_modal/

#include "pico/plugin.h"
#include "json.h"

#include "clay/clay.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *kName = "stream-modal";

typedef struct StreamState {
    PicoAgentId owner_id;
    char status[PICO_UI_POST_STATUS_MAX];
    char text[PICO_UI_POST_TEXT_MAX + 1];
} StreamState;

static const char *kParams = "{\"type\":\"object\",\"properties\":{}}";

static void CloseModal(PicoHost *host)
{
    (void)pico_ui_modal_pop(host, kName);
}

static void OpenModal(PicoHost *host)
{
    (void)pico_ui_modal_push(host, kName);
}

static void ResetDisplay(StreamState *s)
{
    if (!s)
    {
        return;
    }
    s->owner_id = 0;
    s->status[0] = '\0';
    s->text[0] = '\0';
}

static void CloseOwned(PicoHost *host, StreamState *s, bool clear_mailbox)
{
    CloseModal(host);
    if (clear_mailbox && s && s->owner_id)
    {
        pico_agent_ui_clear(host, s->owner_id, kName);
    }
    ResetDisplay(s);
}

static Clay_String CStr(const char *s)
{
    if (!s)
    {
        s = "";
    }
    return (Clay_String){.length = (int32_t)strlen(s), .chars = s};
}

static void StreamRender(PicoHost *host, void *state)
{
    StreamState *s = (StreamState *)state;
    if (!s || !pico_ui_modal_has(host, kName))
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
            (void)host;
        }
    }
}

static void StreamAfterLayout(PicoHost *host, const PicoHookEvent *event, void *state)
{
    StreamState *s = (StreamState *)state;
    (void)event;
    if (!s || !pico_ui_modal_is_top(host, kName))
    {
        return;
    }
    if (Clay_PointerOver(Clay_GetElementId(CLAY_STRING("StreamModalCard"))))
    {
        pico_host_set_hovered_clickable(host);
        return;
    }
    if (IsMouseButtonPressed(MOUSE_BUTTON_LEFT) &&
        Clay_PointerOver(Clay_GetElementId(CLAY_STRING("StreamModalDim"))))
    {
        CloseOwned(host, s, true);
    }
}

static void StreamOnFrame(PicoHost *host, void *state, float dt)
{
    StreamState *s = (StreamState *)state;
    PicoUiPost post;
    PicoAgentId selected = pico_agent_active(host);
    (void)dt;
    if (!s)
    {
        return;
    }
    if (pico_ui_modal_has(host, kName) && s->owner_id && selected != s->owner_id)
    {
        CloseOwned(host, s, false);
    }
    if (selected && pico_agent_ui_latest(host, selected, kName, &post))
    {
        s->owner_id = selected;
        snprintf(s->status, sizeof(s->status), "%s", post.status ? post.status : "");
        snprintf(s->text, sizeof(s->text), "%s", post.text ? post.text : "");
        OpenModal(host);
    }
    if (!pico_ui_modal_is_top(host, kName))
    {
        return;
    }
    if (IsKeyPressed(KEY_ESCAPE))
    {
        CloseOwned(host, s, true);
    }
}

static void CmdStream(PicoHost *host, PicoAgentId agent_id, const char *args, void *state)
{
    StreamState *s = (StreamState *)state;
    (void)args;
    if (!s)
    {
        return;
    }
    if (pico_ui_modal_has(host, kName))
    {
        CloseModal(host);
        pico_agent_ui_clear(host, agent_id, kName);
        ResetDisplay(s);
    }
    else
    {
        s->owner_id = agent_id;
        OpenModal(host);
    }
    PicoComposer_SetText(host, "");
    PicoHost_RequestSubmitCancel(host);
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

static int StreamHostInit(PicoHost *host, void **state_out)
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
    PicoAgentId id = pico_agent_active(host);
    if (id && pico_agent_ui_latest(host, id, kName, &post) && !pico_ui_modal_has(host, kName))
    {
        s->owner_id = id;
        snprintf(s->status, sizeof(s->status), "%s", post.status ? post.status : "");
        snprintf(s->text, sizeof(s->text), "%s", post.text ? post.text : "");
        (void)pico_ui_modal_push(host, kName);
    }
    pico_host_add_view(host, PICO_SLOT_OVERLAY, 50, StreamRender);
    pico_host_add_hook(host, PICO_HOOK_AFTER_LAYOUT, StreamAfterLayout);
    pico_host_add_command(host, "stream", "Toggle the streaming overlay modal", CmdStream);
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

static void StreamShutdown(PicoHost *host, void *state)
{
    StreamState *s = (StreamState *)state;
    CloseOwned(host, s, false);
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
