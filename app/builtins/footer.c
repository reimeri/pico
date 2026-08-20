#define _DEFAULT_SOURCE
#define _POSIX_C_SOURCE 200809L

#include "pico/plugin.h"
#include "settings.h"

#include "clay/clay.h"

#include <stdio.h>
#include <stdlib.h>
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

static void FormatCwd(const char *workspace, char *out, size_t cap)
{
    char real[4096];
    const char *src = workspace && workspace[0] ? workspace : ".";
    if (!realpath(src, real))
    {
        snprintf(real, sizeof(real), "%s", src);
    }

    const char *home = getenv("HOME");
    if (home && home[0])
    {
        size_t n = strlen(home);
        while (n > 1 && home[n - 1] == '/')
        {
            n--;
        }
        if (strncmp(real, home, n) == 0 && (real[n] == '\0' || real[n] == '/'))
        {
            snprintf(out, cap, "~%s", real + n);
            return;
        }
    }
    snprintf(out, cap, "%s", real);
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

    char cwd[256];
    FormatCwd(app->workspace, cwd, sizeof(cwd));
    if (app->tokens_used > 0)
    {
        int pct = (int)((long)app->tokens_cached * 100 / app->tokens_used);
        if (pct > 100)
        {
            pct = 100;
        }
        snprintf(app->footer_text, sizeof(app->footer_text), "%s  ·  %s  ·  %d / %d tokens  ·  %d%% cache%s", cwd,
                 AgentStateName(app->agent_state), app->tokens_used, app->tokens_limit, pct, extra);
    }
    else
    {
        snprintf(app->footer_text, sizeof(app->footer_text), "%s  ·  %s  ·  %d / %d tokens%s", cwd,
                 AgentStateName(app->agent_state), app->tokens_used, app->tokens_limit, extra);
    }

    const char *effort = PicoSettings_ActiveEffort(app);
    bool show_effort = effort && effort[0] && strcmp(effort, "none") != 0 && strcmp(effort, "off") != 0;
    const char *model = app->model_name ? app->model_name : "?";
    if (show_effort)
    {
        snprintf(app->footer_right, sizeof(app->footer_right), "%s  ·  %s", model, effort);
    }
    else
    {
        snprintf(app->footer_right, sizeof(app->footer_right), "%s", model);
    }

    Clay_String left = {.length = (int32_t)strlen(app->footer_text), .chars = app->footer_text};
    Clay_String right = {.length = (int32_t)strlen(app->footer_right), .chars = app->footer_right};

    CLAY(CLAY_ID("Footer"),
         {.layout = {.layoutDirection = CLAY_LEFT_TO_RIGHT,
                     .padding = {14, 14, 8, 8},
                     .childAlignment = {.y = CLAY_ALIGN_Y_CENTER},
                     .sizing = {.width = CLAY_SIZING_GROW(0), .height = CLAY_SIZING_FIT(0)}},
          .backgroundColor = COLOR_FOOTER_BG})
    {
        CLAY_TEXT(left, CLAY_TEXT_CONFIG({.fontId = FONT_REGULAR,
                                          .fontSize = 13,
                                          .textColor = COLOR_MUTED,
                                          .wrapMode = CLAY_TEXT_WRAP_NONE}));
        CLAY_AUTO_ID({.layout = {.sizing = {.width = CLAY_SIZING_GROW(0)}}}) {}
        CLAY_TEXT(right, CLAY_TEXT_CONFIG({.fontId = FONT_REGULAR,
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
