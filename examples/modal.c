// Named overlay modal + tool-row click. Copy then F5:
//
//   mkdir -p ~/.config/pico/extensions/modal
//   cp examples/modal.c ~/.config/pico/extensions/modal/

#include "pico/plugin.h"
#include "json.h"

#include "clay/clay.h"

#include <string.h>

static const char *kName = "example-modal";
static const char *kParams = "{\"type\":\"object\",\"properties\":{}}";

static void CloseModal(PicoHost *app)
{
    (void)pico_ui_modal_pop(app, kName);
}

static void OpenModal(PicoHost *app)
{
    (void)pico_ui_modal_push(app, kName);
}

static void ModalRender(PicoHost *app, void *state)
{
    (void)state;
    if (!pico_ui_modal_has(app, kName))
    {
        return;
    }
    float sw = (float)GetScreenWidth();
    float sh = (float)GetScreenHeight();
    CLAY(CLAY_ID("ExampleModalDim"),
         {.floating = {.attachTo = CLAY_ATTACH_TO_ROOT,
                       .zIndex = 50,
                       .attachPoints = {.element = CLAY_ATTACH_POINT_LEFT_TOP,
                                        .parent = CLAY_ATTACH_POINT_LEFT_TOP}},
          .layout = {.layoutDirection = CLAY_TOP_TO_BOTTOM,
                     .childAlignment = {.x = CLAY_ALIGN_X_CENTER, .y = CLAY_ALIGN_Y_CENTER},
                     .sizing = {.width = CLAY_SIZING_FIXED(sw), .height = CLAY_SIZING_FIXED(sh)}},
          .backgroundColor = {0, 0, 0, 140}})
    {
        CLAY(CLAY_ID("ExampleModalCard"),
             {.layout = {.layoutDirection = CLAY_TOP_TO_BOTTOM,
                         .padding = {20, 20, 16, 16},
                         .childGap = 8,
                         .sizing = {.width = CLAY_SIZING_FIXED(420)}},
              .backgroundColor = COLOR_CONTENT_BG,
              .cornerRadius = CLAY_CORNER_RADIUS(8)})
        {
            CLAY_TEXT(CLAY_STRING("Example modal"),
                      CLAY_TEXT_CONFIG({.fontId = FONT_BOLD, .fontSize = 16, .textColor = COLOR_TEXT}));
            CLAY_TEXT(CLAY_STRING("Esc or click outside closes. Composer input is skipped while open."),
                      CLAY_TEXT_CONFIG({.fontId = FONT_REGULAR,
                                        .fontSize = 14,
                                        .textColor = COLOR_MUTED,
                                        .wrapMode = CLAY_TEXT_WRAP_WORDS}));
            (void)app;
        }
    }
}

static void ModalAfterLayout(PicoHost *app, const PicoHookEvent *event, void *state)
{
    (void)state;
    (void)event;
    if (!pico_ui_modal_is_top(app, kName))
    {
        return;
    }
    if (Clay_PointerOver(Clay_GetElementId(CLAY_STRING("ExampleModalCard"))))
    {
        pico_host_set_hovered_clickable(app);
        return;
    }
    if (IsMouseButtonPressed(MOUSE_BUTTON_LEFT) &&
        Clay_PointerOver(Clay_GetElementId(CLAY_STRING("ExampleModalDim"))))
    {
        CloseModal(app);
    }
}

static void ModalOnFrame(PicoHost *app, void *state, float dt)
{
    (void)dt;
    (void)state;
    if (!pico_ui_modal_is_top(app, kName))
    {
        return;
    }
    if (IsKeyPressed(KEY_ESCAPE))
    {
        CloseModal(app);
    }
}

static void CmdModal(PicoHost *app, PicoAgentId agent_id, const char *args, void *state)
{
    (void)state;
    (void)args;
    (void)agent_id;
    if (pico_ui_modal_has(app, kName))
    {
        CloseModal(app);
    }
    else
    {
        OpenModal(app);
    }
    PicoComposer_SetText(app, "");
    PicoHost_RequestSubmitCancel(app);
}

static void ModalDemoRun(PicoAgentContext *ctx, const char *args_json, PicoToolResult *out, void *state)
{
    (void)state;
    (void)ctx;
    (void)args_json;
    if (out)
    {
        memset(out, 0, sizeof(*out));
        out->output = JsonDup("modal_demo: click the tool row to reopen the overlay");
    }
}

static void ModalToolRow(PicoWorkspace *workspace, PicoToolRowEvent *event, void *state)
{
    (void)state;
    if (!event || !event->name || strcmp(event->name, "modal_demo") != 0)
    {
        return;
    }
    OpenModal(pico_workspace_host(workspace));
    event->handled = true;
}

static int ModalHostInit(PicoHost *app, void **state_out)
{
    (void)state_out;
    pico_host_add_view(app, PICO_SLOT_OVERLAY, 50, ModalRender);
    pico_host_add_hook(app, PICO_HOOK_AFTER_LAYOUT, ModalAfterLayout);
    pico_host_add_command(app, "modal", "Toggle the example overlay modal", CmdModal);
    return 0;
}

static int ModalWorkspaceInit(PicoWorkspace *workspace, void **state_out)
{
    (void)state_out;
    pico_add_tool(workspace, "modal_demo", "Open the example overlay from its tool row", kParams, ModalDemoRun,
                  NULL);
    pico_add_tool_row_hook(workspace, ModalToolRow);
    return 0;
}

static void ModalShutdown(PicoHost *app, void *state)
{
    (void)state;
    CloseModal(app);
}

PicoExt pico_ext(void)
{
    return (PicoExt){
        .abi = PICO_EXT_ABI,
        .name = "modal",
        .description = "Named overlay modal and tool-row click example",
        .host_init = ModalHostInit,
        .workspace_init = ModalWorkspaceInit,
        .host_shutdown = ModalShutdown,
        .host_on_frame = ModalOnFrame,
    };
}
