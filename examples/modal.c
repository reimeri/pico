// Named overlay modal + tool-row click. Copy then F5:
//
//   mkdir -p ~/.config/pico/extensions/modal
//   cp examples/modal.c ~/.config/pico/extensions/modal/

#include "pico/plugin.h"
#include "json.h"

#include "clay/clay.h"

#include <string.h>

static const char *kName = "example-modal";
static bool g_open;

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

static void ModalRender(PicoApp *app)
{
    float sw;
    float sh;
    if (!g_open)
    {
        return;
    }
    sw = (float)GetScreenWidth();
    sh = (float)GetScreenHeight();
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

static void ModalAfterLayout(PicoApp *app, const PicoHookEvent *event)
{
    (void)event;
    if (!g_open)
    {
        return;
    }
    if (Clay_PointerOver(Clay_GetElementId(CLAY_STRING("ExampleModalCard"))))
    {
        app->hovered_clickable = true;
        return;
    }
    if (IsMouseButtonPressed(MOUSE_BUTTON_LEFT) &&
        Clay_PointerOver(Clay_GetElementId(CLAY_STRING("ExampleModalDim"))))
    {
        CloseModal(app);
    }
}

static void ModalOnFrame(PicoApp *app, float dt)
{
    (void)dt;
    if (!g_open)
    {
        return;
    }
    if (IsKeyPressed(KEY_ESCAPE))
    {
        CloseModal(app);
    }
}

static void CmdModal(PicoApp *app, const char *args)
{
    (void)args;
    if (g_open)
    {
        CloseModal(app);
    }
    else
    {
        OpenModal(app);
    }
    PicoComposer_SetText(app, "");
    app->submit_cancel = true;
}

static void ModalDemoRun(PicoAgentContext *ctx, const char *args_json, PicoToolResult *out)
{
    (void)ctx;
    (void)args_json;
    if (out)
    {
        memset(out, 0, sizeof(*out));
        out->output = JsonDup("modal_demo: click the tool row to reopen the overlay");
    }
}

static void ModalToolRow(PicoApp *app, PicoToolRowEvent *ev)
{
    if (!ev || !ev->name || strcmp(ev->name, "modal_demo") != 0)
    {
        return;
    }
    OpenModal(app);
    ev->handled = true;
}

static void ModalInit(PicoApp *app)
{
    if (g_open && !pico_ui_modal_has(app, kName))
    {
        (void)pico_ui_modal_push(app, kName);
    }
    pico_add_view(app, PICO_SLOT_OVERLAY, 50, ModalRender);
    pico_add_hook(app, PICO_HOOK_AFTER_LAYOUT, ModalAfterLayout);
    pico_add_command(app, "modal", "Toggle the example overlay modal", CmdModal);
    pico_add_tool(app, "modal_demo", "Open the example overlay from its tool row", kParams, ModalDemoRun,
                  NULL);
    pico_add_tool_row_hook(app, ModalToolRow);
}

static void ModalShutdown(PicoApp *app)
{
    CloseModal(app);
}

PicoExt pico_ext(void)
{
    return (PicoExt){
        .abi = PICO_EXT_ABI,
        .name = "modal",
        .description = "Named overlay modal and tool-row click example",
        .init = ModalInit,
        .shutdown = ModalShutdown,
        .on_frame = ModalOnFrame,
    };
}
