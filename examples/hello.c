// Example host-scoped Pico extension (sidebar view). Copy to
// ~/.config/pico/extensions/ (a subfolder is fine) then press F5.
// Workspace-local sources must not set host callbacks.
//
//   mkdir -p ~/.config/pico/extensions/hello
//   cp examples/hello.c ~/.config/pico/extensions/hello/

#include "pico/plugin.h"

#include "clay/clay.h"

static void HelloRender(PicoHost *host, void *state)
{
    (void)state;
    (void)host;
    CLAY(CLAY_ID("HelloExt"),
         {.layout = {.layoutDirection = CLAY_TOP_TO_BOTTOM, .childGap = 6}})
    {
        CLAY_TEXT(CLAY_STRING("hello"),
                  CLAY_TEXT_CONFIG({.fontId = FONT_BOLD, .fontSize = 16, .textColor = COLOR_TEXT}));
        CLAY_TEXT(CLAY_STRING("from a .c extension"),
                  CLAY_TEXT_CONFIG({.fontId = FONT_REGULAR, .fontSize = 13, .textColor = COLOR_MUTED}));
    }
}

static int HelloInit(PicoHost *host, void **state_out)
{
    (void)state_out;
    pico_host_add_view(host, PICO_SLOT_SIDEBAR, 0, HelloRender);
    return 0;
}

PicoExt pico_ext(void)
{
    return (PicoExt){
        .abi = PICO_EXT_ABI,
        .name = "hello",
        .description = "Sidebar hello view",
        .host_init = HelloInit,
    };
}
