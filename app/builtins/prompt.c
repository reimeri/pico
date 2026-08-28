#include "pico/plugin.h"
#include "agent.h"
#include "scrollbar.h"
#include "settings.h"
#include "host_internal.h"

#include "clay/clay.h"

#include <stdlib.h>
#include <string.h>

static PicoHost *g_app;
static bool g_open;
static char *g_text;
static PicoPromptSpan g_spans[PICO_PROMPT_SPAN_MAX];
static int g_span_count;
static bool g_overflow;
static PicoScrollbar g_scrollbar;

#define COLOR_PROMPT_BASE (Clay_Color){186, 164, 122, 255}
#define COLOR_PROMPT_WORKSPACE (Clay_Color){122, 156, 148, 255}
#define COLOR_PROMPT_AGENTS (Clay_Color){132, 154, 186, 255}
#define COLOR_PROMPT_HOOK (Clay_Color){138, 160, 128, 255}

static bool Unclaim(void)
{
    if (!g_open)
    {
        return true;
    }
    if (g_app && !pico_ui_modal_pop(g_app, "prompt"))
    {
        return false;
    }
    g_open = false;
    return true;
}

static void ClearPrompt(void)
{
    free(g_text);
    g_text = NULL;
    g_span_count = 0;
    memset(g_spans, 0, sizeof(g_spans));
    g_overflow = false;
    memset(&g_scrollbar, 0, sizeof(g_scrollbar));
}

void PicoPrompt_Close(void)
{
    if (!Unclaim())
    {
        return;
    }
    ClearPrompt();
}

bool PicoPrompt_IsOpen(void)
{
    return g_open;
}

static Clay_Color PromptSourceColor(PicoPromptSource source)
{
    switch (source)
    {
    case PICO_PROMPT_SOURCE_BASE:
        return COLOR_PROMPT_BASE;
    case PICO_PROMPT_SOURCE_WORKSPACE_SYSTEM:
        return COLOR_PROMPT_WORKSPACE;
    case PICO_PROMPT_SOURCE_AGENTS:
        return COLOR_PROMPT_AGENTS;
    case PICO_PROMPT_SOURCE_LLM_HOOK:
        return COLOR_PROMPT_HOOK;
    }
    return COLOR_CODE_TEXT;
}

static const char *PromptSourceLabel(PicoPromptSource source)
{
    switch (source)
    {
    case PICO_PROMPT_SOURCE_BASE:
        return "Base system prompt";
    case PICO_PROMPT_SOURCE_WORKSPACE_SYSTEM:
        return ".pico/SYSTEM.md";
    case PICO_PROMPT_SOURCE_AGENTS:
        return "AGENTS.md";
    case PICO_PROMPT_SOURCE_LLM_HOOK:
        return "Extra instructions";
    }
    return "";
}

static bool PromptHasSource(PicoPromptSource source)
{
    for (int i = 0; i < g_span_count; i++)
    {
        if (g_spans[i].source == source && g_spans[i].length > 0)
        {
            return true;
        }
    }
    return false;
}

static Clay_Color ColorAt(size_t offset)
{
    for (int i = 0; i < g_span_count; i++)
    {
        size_t start = g_spans[i].start;
        size_t end = start + g_spans[i].length;
        if (offset >= start && offset < end)
        {
            return PromptSourceColor(g_spans[i].source);
        }
    }
    return COLOR_CODE_TEXT;
}

static Clay_String CStr(const char *s)
{
    if (!s)
    {
        s = "";
    }
    return (Clay_String){.length = (int32_t)strlen(s), .chars = s};
}

static void RenderPromptText(void)
{
    const char *text = (g_text && g_text[0]) ? g_text : "(empty)";
    bool empty = !g_text || !g_text[0];
    const char *line = text;
    size_t offset = 0;
    while (line)
    {
        const char *newline = strchr(line, '\n');
        int length = newline ? (int)(newline - line) : (int)strlen(line);
        if (length > 0 && line[length - 1] == '\r')
        {
            length--;
        }
        Clay_String s = {.length = length > 0 ? length : 1, .chars = length > 0 ? line : " "};
        Clay_Color color = empty ? COLOR_MUTED : ColorAt(offset);
        CLAY_TEXT(s, CLAY_TEXT_CONFIG({.fontId = FONT_MONO,
                                      .fontSize = 13,
                                      .lineHeight = Pico_FontPxU16(18),
                                      .textColor = color,
                                      .wrapMode = CLAY_TEXT_WRAP_WORDS}));
        if (newline)
        {
            offset += (size_t)(newline - line) + 1;
            line = newline + 1;
        }
        else
        {
            line = NULL;
        }
    }
}

static void RenderLegendItem(PicoPromptSource source)
{
    if (!PromptHasSource(source))
    {
        return;
    }
    CLAY_AUTO_ID({.layout = {.layoutDirection = CLAY_LEFT_TO_RIGHT,
                             .childGap = 6,
                             .childAlignment = {.y = CLAY_ALIGN_Y_CENTER}}})
    {
        CLAY_AUTO_ID({.layout = {.sizing = {.width = CLAY_SIZING_FIXED(8), .height = CLAY_SIZING_FIXED(8)}},
                      .backgroundColor = PromptSourceColor(source),
                      .cornerRadius = CLAY_CORNER_RADIUS(4)})
        {
        }
        CLAY_TEXT(CStr(PromptSourceLabel(source)),
                  CLAY_TEXT_CONFIG({.fontId = FONT_REGULAR,
                                    .fontSize = 11,
                                    .textColor = COLOR_MUTED,
                                    .wrapMode = CLAY_TEXT_WRAP_NONE}));
    }
}

static void RenderLegend(void)
{
    if (g_span_count <= 0)
    {
        return;
    }
    CLAY_AUTO_ID({.layout = {.layoutDirection = CLAY_LEFT_TO_RIGHT,
                             .childGap = 14,
                             .childAlignment = {.y = CLAY_ALIGN_Y_CENTER},
                             .sizing = {.width = CLAY_SIZING_GROW(0)}}})
    {
        RenderLegendItem(PICO_PROMPT_SOURCE_BASE);
        RenderLegendItem(PICO_PROMPT_SOURCE_WORKSPACE_SYSTEM);
        RenderLegendItem(PICO_PROMPT_SOURCE_AGENTS);
        RenderLegendItem(PICO_PROMPT_SOURCE_LLM_HOOK);
    }
}

static void PromptRender(PicoHost *app, void *state)
{
    (void)state;
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
            if (g_overflow)
            {
                PicoScrollbar_RenderOverlay(CLAY_STRING("PromptModalScroll"), CLAY_STRING("PromptModalScrollTrack"),
                                            CLAY_STRING("PromptModalScrollHandle"));
            }
            RenderLegend();
        }
    }
}

static void PromptAfterLayout(PicoHost *app, const PicoHookEvent *event, void *state)
{
    (void)state;
    (void)event;
    (void)app;
    if (!g_open || !pico_ui_modal_is_top(app, "prompt"))
    {
        return;
    }
    g_overflow = PicoScrollbar_Overflows(CLAY_STRING("PromptModalScroll"));
    if (!IsMouseButtonPressed(MOUSE_BUTTON_LEFT))
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

static void PromptOnFrame(PicoHost *app, void *state, float dt)
{
    (void)dt;
    if (!g_open || !pico_ui_modal_is_top(app, "prompt"))
    {
        return;
    }
    PicoScrollbar_UpdateDragOverlay(&g_scrollbar, CLAY_STRING("PromptModalScroll"),
                                    CLAY_STRING("PromptModalScrollHandle"));
    if (IsKeyPressed(KEY_ESCAPE))
    {
        PicoPrompt_Close();
    }
}

static void CmdShowPrompt(PicoHost *app, PicoAgentId agent_id, const char *args, void *state)
{
    (void)state;
    (void)args;
    PicoExts_Close();
    free(g_text);
    g_span_count = 0;
    memset(g_spans, 0, sizeof(g_spans));
    g_text = PicoAgent_BuildInstructionsSpans(app, PicoHost_FindAgent(app, agent_id), g_spans, &g_span_count);
    if (!g_open && pico_ui_modal_push(app, "prompt"))
    {
        g_open = true;
    }
    PicoComposer_SetText(app, "");
    app->submit_cancel = true;
}

static int PromptInit(PicoHost *app, void **state_out)
{
    (void)state_out;
    g_app = app;
    if (g_open && !pico_ui_modal_has(app, "prompt"))
    {
        g_open = false;
        ClearPrompt();
    }
    pico_host_add_command(app, "show-prompt", "Show the system prompt sent to the agent", CmdShowPrompt);
    pico_host_add_view(app, PICO_SLOT_OVERLAY, 11, PromptRender);
    pico_host_add_hook(app, PICO_HOOK_AFTER_LAYOUT, PromptAfterLayout);
    return 0;
}

static void PromptShutdown(PicoHost *app, void *state)
{
    (void)state;
    (void)Unclaim();
    g_open = false;
    ClearPrompt();
    g_app = NULL;
    (void)app;
}

PicoExt pico_ext_prompt(void)
{
    return (PicoExt){
        .abi = PICO_EXT_ABI,
        .name = "prompt",
        .description = "System prompt viewer",
        .host_init = PromptInit,
        .host_shutdown = PromptShutdown,
        .host_on_frame = PromptOnFrame,
    };
}
