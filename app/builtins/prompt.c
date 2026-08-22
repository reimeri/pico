#include "pico/plugin.h"
#include "agent.h"

#include "clay/clay.h"

#include <stdlib.h>
#include <string.h>

static bool g_open;
static char *g_text;

void PicoPrompt_Close(void)
{
    g_open = false;
    free(g_text);
    g_text = NULL;
}

bool PicoPrompt_IsOpen(void)
{
    return g_open;
}

static void RenderPromptText(void)
{
    const char *text = (g_text && g_text[0]) ? g_text : "(empty)";
    Clay_Color color = (g_text && g_text[0]) ? COLOR_CODE_TEXT : COLOR_MUTED;
    const char *line = text;
    while (line)
    {
        const char *newline = strchr(line, '\n');
        int length = newline ? (int)(newline - line) : (int)strlen(line);
        if (length > 0 && line[length - 1] == '\r')
        {
            length--;
        }
        Clay_String s = {.length = length > 0 ? length : 1, .chars = length > 0 ? line : " "};
        CLAY_TEXT(s, CLAY_TEXT_CONFIG({.fontId = FONT_MONO,
                                      .fontSize = 13,
                                      .lineHeight = 18,
                                      .textColor = color,
                                      .wrapMode = CLAY_TEXT_WRAP_WORDS}));
        line = newline ? newline + 1 : NULL;
    }
}

static void PromptRender(PicoApp *app)
{
    (void)app;
    if (!g_open)
    {
        return;
    }

    float sw = (float)GetScreenWidth();
    float sh = (float)GetScreenHeight();
    float card_w = sw < 800.0f ? sw - 48.0f : 720.0f;
    if (card_w < 280.0f)
    {
        card_w = 280.0f;
    }
    float card_h = sh * 0.8f;
    if (card_h < 240.0f)
    {
        card_h = 240.0f;
    }
    if (card_h > 780.0f)
    {
        card_h = 780.0f;
    }

    CLAY(CLAY_ID("PromptModalDim"),
         {.floating = {.attachTo = CLAY_ATTACH_TO_ROOT,
                       .zIndex = 41,
                       .attachPoints = {.element = CLAY_ATTACH_POINT_LEFT_TOP,
                                        .parent = CLAY_ATTACH_POINT_LEFT_TOP}},
          .layout = {.layoutDirection = CLAY_TOP_TO_BOTTOM,
                     .childAlignment = {.x = CLAY_ALIGN_X_CENTER, .y = CLAY_ALIGN_Y_CENTER},
                     .sizing = {.width = CLAY_SIZING_FIXED(sw), .height = CLAY_SIZING_FIXED(sh)}},
          .backgroundColor = {0, 0, 0, 140}})
    {
        CLAY(CLAY_ID("PromptModalCard"),
             {.layout = {.layoutDirection = CLAY_TOP_TO_BOTTOM,
                         .padding = {20, 20, 16, 16},
                         .childGap = 12,
                         .sizing = {.width = CLAY_SIZING_FIXED(card_w), .height = CLAY_SIZING_FIXED(card_h)}},
              .backgroundColor = COLOR_CONTENT_BG,
              .cornerRadius = CLAY_CORNER_RADIUS(8)})
        {
            CLAY_TEXT(CLAY_STRING("System prompt"),
                      CLAY_TEXT_CONFIG({.fontId = FONT_BOLD, .fontSize = 18, .textColor = COLOR_TEXT}));
            CLAY_TEXT(CLAY_STRING("What the next turn sends, including extra instructions from extensions."),
                      CLAY_TEXT_CONFIG({.fontId = FONT_REGULAR,
                                        .fontSize = 12,
                                        .textColor = COLOR_MUTED,
                                        .wrapMode = CLAY_TEXT_WRAP_WORDS}));

            CLAY(CLAY_ID("PromptModalScroll"),
                 {.layout = {.layoutDirection = CLAY_TOP_TO_BOTTOM,
                             .padding = {12, 12, 10, 10},
                             .childGap = 0,
                             .sizing = {.width = CLAY_SIZING_GROW(0), .height = CLAY_SIZING_GROW(0)}},
                  .backgroundColor = COLOR_CODE_BG,
                  .cornerRadius = CLAY_CORNER_RADIUS(6),
                  .clip = {.vertical = true, .horizontal = false, .childOffset = Clay_GetScrollOffset()}})
            {
                RenderPromptText();
            }
        }
    }
}

static void PromptAfterLayout(PicoApp *app, const PicoHookEvent *event)
{
    (void)event;
    (void)app;
    if (!g_open || !IsMouseButtonPressed(MOUSE_BUTTON_LEFT))
    {
        return;
    }
    if (Clay_PointerOver(Clay_GetElementId(CLAY_STRING("PromptModalCard"))))
    {
        return;
    }
    if (Clay_PointerOver(Clay_GetElementId(CLAY_STRING("PromptModalDim"))))
    {
        PicoPrompt_Close();
    }
}

static void PromptOnFrame(PicoApp *app, float dt)
{
    (void)app;
    (void)dt;
    if (g_open && IsKeyPressed(KEY_ESCAPE))
    {
        PicoPrompt_Close();
    }
}

static void CmdShowPrompt(PicoApp *app, const char *args)
{
    (void)args;
    PicoExts_Close();
    free(g_text);
    g_text = PicoAgent_BuildInstructions(app, PicoApp_ActiveAgent(app));
    g_open = true;
    PicoComposer_SetText(app, "");
    app->submit_cancel = true;
}

static void PromptInit(PicoApp *app)
{
    pico_add_command(app, "show-prompt", "Show the system prompt sent to the agent", CmdShowPrompt);
    pico_add_view(app, PICO_SLOT_OVERLAY, 11, PromptRender);
    pico_add_hook(app, PICO_HOOK_AFTER_LAYOUT, PromptAfterLayout);
}

static void PromptShutdown(PicoApp *app)
{
    (void)app;
    PicoPrompt_Close();
}

PicoExt pico_ext_prompt(void)
{
    return (PicoExt){
        .abi = PICO_EXT_ABI,
        .name = "prompt",
        .description = "System prompt viewer",
        .init = PromptInit,
        .shutdown = PromptShutdown,
        .on_frame = PromptOnFrame,
    };
}
