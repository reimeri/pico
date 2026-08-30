// Example workspace-scoped empty-state view. Copy to ~/.config/pico/extensions/ or
// <workspace>/.pico/extensions/ (a subfolder is fine) then press F5.
//
//   mkdir -p ~/.config/pico/extensions/empty_banner
//   cp examples/empty_banner.c ~/.config/pico/extensions/empty_banner/
//
// Adds a line above the builtin Tools / Context / Skills cards. To replace
// those cards entirely, register PICO_EMPTY_REPLACE instead (see /docs views).

#include "pico/plugin.h"

#include "clay/clay.h"

static void BannerRender(PicoWorkspace *workspace, PicoAgentId selected_agent_id, void *state)
{
    (void)state;
    (void)selected_agent_id;
    (void)workspace;
    CLAY(CLAY_ID("EmptyBanner"),
         {.layout = {.layoutDirection = CLAY_TOP_TO_BOTTOM, .childGap = 4}})
    {
        CLAY_TEXT(CLAY_STRING("Start a conversation"),
                  CLAY_TEXT_CONFIG({.fontId = FONT_BOLD, .fontSize = PICO_FONT_UI, .textColor = COLOR_TEXT}));
        CLAY_TEXT(CLAY_STRING("from a .c extension"),
                  CLAY_TEXT_CONFIG({.fontId = FONT_REGULAR, .fontSize = PICO_FONT_CAPTION, .textColor = COLOR_MUTED}));
    }
}

static int BannerInit(PicoWorkspace *workspace, void **state_out)
{
    (void)state_out;
    pico_workspace_add_empty_view(workspace, PICO_EMPTY_ABOVE, 0, BannerRender);
    return 0;
}

PicoExt pico_ext(void)
{
    return (PicoExt){
        .abi = PICO_EXT_ABI,
        .name = "empty_banner",
        .description = "Banner above the empty-state cards",
        .workspace_init = BannerInit,
    };
}
