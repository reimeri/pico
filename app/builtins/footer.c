#include "pico/plugin.h"

#include "clay/clay.h"

#include <stdio.h>
#include <string.h>

static const char *AgentStateName(PicoAgentState state)
{
    switch (state)
    {
        case PICO_AGENT_IDLE:
            return "idle";
        case PICO_AGENT_LLM_WAIT:
            return "waiting on model";
        case PICO_AGENT_TOOL_WAIT:
            return "running tool";
        case PICO_AGENT_COMPACT_WAIT:
            return "compacting";
        case PICO_AGENT_ERROR:
            return "error";
        default:
            return "unknown";
    }
}

void PicoFooter_Render(PicoApp *app)
{
    const char *extra = "";
    if (app->agent_state == PICO_AGENT_LLM_WAIT || app->agent_state == PICO_AGENT_TOOL_WAIT)
    {
        extra = "  ·  Esc to cancel";
    }
    else if (app->agent_state == PICO_AGENT_ERROR)
    {
        extra = "  ·  Esc to dismiss";
    }
    snprintf(app->footer_text, sizeof(app->footer_text), "%s  ·  %s  ·  %d / %d tokens%s",
             AgentStateName(app->agent_state), app->model_name ? app->model_name : "?", app->tokens_used,
             app->tokens_limit, extra);
    Clay_String text = {.length = (int32_t)strlen(app->footer_text), .chars = app->footer_text};

    CLAY(CLAY_ID("Footer"),
         {.layout = {.layoutDirection = CLAY_LEFT_TO_RIGHT,
                     .padding = {14, 14, 8, 8},
                     .sizing = {.width = CLAY_SIZING_GROW(0), .height = CLAY_SIZING_FIT(0)}},
          .backgroundColor = COLOR_FOOTER_BG})
    {
        CLAY_TEXT(text, CLAY_TEXT_CONFIG({.fontId = FONT_REGULAR,
                                          .fontSize = 13,
                                          .textColor = COLOR_MUTED,
                                          .wrapMode = CLAY_TEXT_WRAP_NONE}));
    }
}

static void FooterInit(PicoApp *app)
{
    pico_add_view(app, PICO_SLOT_FOOTER, 0, PicoFooter_Render);
}

PicoExt pico_ext_footer(void)
{
    return (PicoExt){
        .abi = PICO_EXT_ABI,
        .name = "footer",
        .init = FooterInit,
    };
}
