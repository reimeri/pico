// Example Pico extension. Copy to ~/.config/pico/extensions/ or
// <workspace>/.pico/extensions/ (a subfolder is fine) then press F5.
//
//   mkdir -p ~/.config/pico/extensions/hello
//   cp examples/hello.c ~/.config/pico/extensions/hello/

#include "pico/plugin.h"

#include "clay/clay.h"

static void HelloRender(PicoApp *app)
{
    (void)app;
    CLAY(CLAY_ID("HelloExt"),
         {.layout = {.layoutDirection = CLAY_TOP_TO_BOTTOM, .childGap = 6}})
    {
        CLAY_TEXT(CLAY_STRING("hello"),
                  CLAY_TEXT_CONFIG({.fontId = FONT_BOLD, .fontSize = 16, .textColor = COLOR_TEXT}));
        CLAY_TEXT(CLAY_STRING("from a .c extension"),
                  CLAY_TEXT_CONFIG({.fontId = FONT_REGULAR, .fontSize = 13, .textColor = COLOR_MUTED}));
    }
}

static void HelloInit(PicoApp *app)
{
    pico_add_view(app, PICO_SLOT_SIDEBAR, 0, HelloRender);
}

PicoExt pico_ext(void)
{
    return (PicoExt){
        .abi = PICO_EXT_ABI,
        .name = "hello",
        .init = HelloInit,
    };
}
