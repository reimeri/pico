#include "pico/plugin.h"
#include "pico/md_view.h"

#include "clay/clay.h"

#include <string.h>

#define TOOL_OUTPUT_MAX_LINES 100

static float ChatWidth(PicoApp *app)
{
    float width = (float)GetScreenWidth() - CONTENT_PADDING - 12;
    if (app->chat_overflow)
    {
        width -= (float)(SCROLLBAR_WIDTH + SCROLLBAR_GAP);
    }
    width -= 36; // message bubble padding
    if (width < 50)
    {
        width = 50;
    }
    return width;
}

static Clay_ElementId ToolRowId(int message_index, int trace_index)
{
    return CLAY_IDI("ToolRow", message_index * 256 + trace_index);
}

static void RenderToolOutput(const char *output)
{
    const char *text = (output && output[0]) ? output : "(empty)";
    CLAY_AUTO_ID({.layout = {.layoutDirection = CLAY_TOP_TO_BOTTOM,
                             .padding = {12, 12, 10, 10},
                             .childGap = 1,
                             .sizing = {.width = CLAY_SIZING_GROW(0)}},
                  .backgroundColor = COLOR_CODE_BG,
                  .cornerRadius = CLAY_CORNER_RADIUS(6)})
    {
        const char *line = text;
        int shown = 0;
        while (line && shown < TOOL_OUTPUT_MAX_LINES)
        {
            const char *newline = strchr(line, '\n');
            int length = newline ? (int)(newline - line) : (int)strlen(line);
            Clay_String s = {.length = length > 0 ? length : 1, .chars = length > 0 ? line : " "};
            CLAY_TEXT(s, CLAY_TEXT_CONFIG({.fontId = FONT_MONO,
                                           .fontSize = 14,
                                           .lineHeight = 18,
                                           .textColor = COLOR_CODE_TEXT,
                                           .wrapMode = CLAY_TEXT_WRAP_WORDS}));
            line = newline ? newline + 1 : NULL;
            shown++;
        }
        if (line)
        {
            CLAY_TEXT(CLAY_STRING("…"), CLAY_TEXT_CONFIG({.fontId = FONT_MONO,
                                                         .fontSize = 14,
                                                         .textColor = COLOR_MUTED,
                                                         .wrapMode = CLAY_TEXT_WRAP_NONE}));
        }
    }
}

static void RenderToolLine(PicoApp *app, PicoTraceLine *line, int message_index, int trace_index)
{
    if (!line->tool_name || !line->tool_name[0])
    {
        return;
    }
    Clay_ElementId row_id = ToolRowId(message_index, trace_index);
    bool hovered = Clay_PointerOver(row_id);
    if (hovered)
    {
        app->hovered_tool = true;
    }
    Clay_Color name_color = hovered ? COLOR_TOOL_NAME_HOVER : COLOR_TOOL_NAME;
    Clay_Color args_color = hovered ? COLOR_TOOL_ARGS_HOVER : COLOR_TOOL_ARGS;
    Clay_String name = {.length = (int32_t)strlen(line->tool_name), .chars = line->tool_name};
    Clay_String args = {.length = line->tool_args ? (int32_t)strlen(line->tool_args) : 0,
                        .chars = line->tool_args ? line->tool_args : ""};

    CLAY_AUTO_ID({.layout = {.layoutDirection = CLAY_TOP_TO_BOTTOM,
                             .childGap = 6,
                             .sizing = {.width = CLAY_SIZING_GROW(0)}}})
    {
        CLAY(row_id, {.layout = {.layoutDirection = CLAY_LEFT_TO_RIGHT,
                                 .childGap = 8,
                                 .childAlignment = {.y = CLAY_ALIGN_Y_CENTER},
                                 .sizing = {.width = CLAY_SIZING_GROW(0)}}})
        {
            CLAY_TEXT(name, CLAY_TEXT_CONFIG({.fontId = FONT_REGULAR,
                                              .fontSize = 15,
                                              .textColor = name_color,
                                              .wrapMode = CLAY_TEXT_WRAP_NONE}));
            if (args.length > 0)
            {
                CLAY_AUTO_ID({.layout = {.sizing = {.width = CLAY_SIZING_GROW(0)}}})
                {
                    CLAY_TEXT(args, CLAY_TEXT_CONFIG({.fontId = FONT_REGULAR,
                                                      .fontSize = 15,
                                                      .textColor = args_color,
                                                      .wrapMode = CLAY_TEXT_WRAP_WORDS}));
                }
            }
            else
            {
                CLAY_AUTO_ID({.layout = {.sizing = {.width = CLAY_SIZING_GROW(0)}}}) {}
            }
            CLAY(CLAY_IDI("ToolChevron", message_index * 256 + trace_index),
                 {.layout = {.sizing = {.width = CLAY_SIZING_FIXED(14), .height = CLAY_SIZING_GROW(0)}}})
            {
            }
        }
        if (line->expanded)
        {
            const char *output = line->tool_output;
            if (!output && app->agent_state == PICO_AGENT_TOOL_WAIT)
            {
                output = "Running…";
            }
            RenderToolOutput(output);
        }
    }
}

void PicoChat_Render(PicoApp *app)
{
    app->hovered_tool = false;
    CLAY(CLAY_ID("ChatRow"),
         {.layout = {.layoutDirection = CLAY_LEFT_TO_RIGHT,
                     .childGap = SCROLLBAR_GAP,
                     .sizing = {.width = CLAY_SIZING_GROW(0), .height = CLAY_SIZING_GROW(0)}}})
    {
        CLAY(CLAY_ID("ChatScroll"),
             {.layout = {.layoutDirection = CLAY_TOP_TO_BOTTOM,
                         .sizing = {.width = CLAY_SIZING_GROW(0), .height = CLAY_SIZING_GROW(0)}},
              .clip = {.vertical = true, .horizontal = false, .childOffset = Clay_GetScrollOffset()}})
        {
            CLAY(CLAY_ID("ChatContent"),
                 {.layout = {.layoutDirection = CLAY_TOP_TO_BOTTOM,
                             .childGap = 16,
                             .padding = {4, 4, 8, 8},
                             .sizing = {.width = CLAY_SIZING_GROW(0)}}})
            {
                float available_width = ChatWidth(app);
                for (int i = 0; i < app->message_count; i++)
                {
                    PicoMessage *msg = &app->messages[i];
                    bool user = msg->role == PICO_ROLE_USER;
                    bool selected = app->selected_message == i;
                    Clay_Color bg = user ? COLOR_USER_BG : COLOR_ASSISTANT_BG;
                    if (selected)
                    {
                        bg = COLOR_SELECTION_MSG;
                    }
                    CLAY(CLAY_IDI("Msg", i),
                         {.layout = {.layoutDirection = CLAY_TOP_TO_BOTTOM,
                                     .padding = {16, 16, 12, 12},
                                     .childGap = 8,
                                     .sizing = {.width = CLAY_SIZING_GROW(0)}},
                          .backgroundColor = bg,
                          .cornerRadius = user || selected ? CLAY_CORNER_RADIUS(8) : CLAY_CORNER_RADIUS(0),
                          .border = selected ? ((Clay_BorderElementConfig){.color = COLOR_LINK,
                                                                           .width = {1, 1, 1, 1, 0}})
                                             : ((Clay_BorderElementConfig){0})})
                    {
                        bool has_trace = msg->trace_count > 0;
                        bool has_source = msg->source && msg->source[0];
                        bool live = !user && i == app->message_count - 1 &&
                                    (app->agent_state == PICO_AGENT_LLM_WAIT ||
                                     app->agent_state == PICO_AGENT_TOOL_WAIT);
                        for (int t = 0; t < msg->trace_count; t++)
                        {
                            PicoTraceLine *line = &msg->trace[t];
                            if (line->is_tool)
                            {
                                RenderToolLine(app, line, i, t);
                                continue;
                            }
                            const char *text = line->text;
                            if (!text || !text[0])
                            {
                                continue;
                            }
                            Clay_String think = {.length = (int32_t)strlen(text), .chars = text};
                            CLAY_TEXT(think, CLAY_TEXT_CONFIG({.fontId = FONT_ITALIC,
                                                               .fontSize = 15,
                                                               .textColor = COLOR_MUTED,
                                                               .wrapMode = CLAY_TEXT_WRAP_WORDS}));
                        }
                        if (!has_trace && live && !has_source)
                        {
                            const char *label =
                                app->agent_activity[0] ? app->agent_activity : "Thinking…";
                            Clay_String think = {.length = (int32_t)strlen(label), .chars = label};
                            CLAY_TEXT(think, CLAY_TEXT_CONFIG({.fontId = FONT_ITALIC,
                                                               .fontSize = 15,
                                                               .textColor = COLOR_MUTED,
                                                               .wrapMode = CLAY_TEXT_WRAP_WORDS}));
                        }
                        if (has_source)
                        {
                            MdView_RenderDocument(&msg->doc, (i + 1) * 4096, available_width);
                        }
                    }
                }
            }
        }

        if (app->chat_overflow)
        {
            Clay_ScrollContainerData scroll_data =
                Clay_GetScrollContainerData(Clay_GetElementId(CLAY_STRING("ChatScroll")));
            float track_h = scroll_data.found ? scroll_data.scrollContainerDimensions.height : 0;
            float content_h = scroll_data.found ? scroll_data.contentDimensions.height : 1;
            float thumb_h = content_h > 0 ? (track_h / content_h) * track_h : track_h;
            if (thumb_h < 16)
            {
                thumb_h = 16;
            }
            float thumb_y = 0;
            if (scroll_data.found && scroll_data.scrollPosition && content_h > 0)
            {
                thumb_y = -(scroll_data.scrollPosition->y / content_h) * track_h;
            }

            CLAY(CLAY_ID("ChatScrollTrack"),
                 {.layout = {.sizing = {.width = CLAY_SIZING_FIXED((float)SCROLLBAR_WIDTH),
                                        .height = CLAY_SIZING_GROW(0)}}})
            {
                CLAY(CLAY_ID("ChatScrollBarHandle"),
                     {.floating = {.attachTo = CLAY_ATTACH_TO_PARENT,
                                   .offset = {.y = thumb_y},
                                   .zIndex = 1,
                                   .attachPoints = {.element = CLAY_ATTACH_POINT_LEFT_TOP,
                                                    .parent = CLAY_ATTACH_POINT_LEFT_TOP}},
                      .layout = {.sizing = {.width = CLAY_SIZING_FIXED((float)SCROLLBAR_WIDTH),
                                            .height = CLAY_SIZING_FIXED(thumb_h)}},
                      .backgroundColor = Clay_PointerOver(Clay_GetElementId(CLAY_STRING("ChatScrollBarHandle")))
                                             ? COLOR_SCROLLBAR_HOVER
                                             : COLOR_SCROLLBAR,
                      .cornerRadius = CLAY_CORNER_RADIUS((float)SCROLLBAR_WIDTH / 2.0f)})
                {
                }
            }
        }
    }
}

void PicoChat_HandlePointer(PicoApp *app)
{
    if (app->status_warn || !IsMouseButtonPressed(MOUSE_BUTTON_LEFT) || app->hovered_link)
    {
        return;
    }
    if (Clay_PointerOver(Clay_GetElementId(CLAY_STRING("Composer"))))
    {
        app->selected_message = -1;
        return;
    }
    for (int i = 0; i < app->message_count; i++)
    {
        PicoMessage *msg = &app->messages[i];
        for (int t = 0; t < msg->trace_count; t++)
        {
            if (!msg->trace[t].is_tool)
            {
                continue;
            }
            if (Clay_PointerOver(ToolRowId(i, t)))
            {
                msg->trace[t].expanded = !msg->trace[t].expanded;
                return;
            }
        }
    }
    for (int i = 0; i < app->message_count; i++)
    {
        if (Clay_PointerOver(CLAY_IDI("Msg", i)))
        {
            app->selected_message = i;
            return;
        }
    }
}

static Color ClayToRay(Clay_Color c)
{
    return (Color){(unsigned char)c.r, (unsigned char)c.g, (unsigned char)c.b, (unsigned char)c.a};
}

static void PicoChat_DrawChevrons(PicoApp *app)
{
    Clay_ElementData scroll = Clay_GetElementData(Clay_GetElementId(CLAY_STRING("ChatScroll")));
    if (!scroll.found || !app->fonts)
    {
        return;
    }
    Clay_BoundingBox clip = scroll.boundingBox;
    BeginScissorMode((int)clip.x, (int)clip.y, (int)clip.width, (int)clip.height);

    Font font = app->fonts[FONT_REGULAR];
    const char *glyph = "\xE2\x80\xBA";
    Vector2 size = MeasureTextEx(font, glyph, 15.0f, 0.0f);

    for (int i = 0; i < app->message_count; i++)
    {
        PicoMessage *msg = &app->messages[i];
        for (int t = 0; t < msg->trace_count; t++)
        {
            PicoTraceLine *line = &msg->trace[t];
            if (!line->is_tool)
            {
                continue;
            }
            bool hovered = Clay_PointerOver(ToolRowId(i, t));
            if (!hovered && !line->expanded)
            {
                continue;
            }
            Clay_ElementData el = Clay_GetElementData(CLAY_IDI("ToolChevron", i * 256 + t));
            if (!el.found)
            {
                continue;
            }
            Clay_BoundingBox box = el.boundingBox;
            Vector2 center = {box.x + box.width * 0.5f, box.y + box.height * 0.5f};
            Color color = ClayToRay(hovered ? COLOR_TOOL_NAME_HOVER : COLOR_TOOL_CHEVRON);
            DrawTextPro(font, glyph, center, (Vector2){size.x * 0.5f, size.y * 0.5f},
                        line->expanded ? 90.0f : 0.0f, 15.0f, 0.0f, color);
        }
    }

    EndScissorMode();
}

static void ChatInit(PicoApp *app)
{
    pico_add_view(app, PICO_SLOT_MAIN, 0, PicoChat_Render);
    pico_add_hook(app, PICO_HOOK_AFTER_LAYOUT, PicoChat_HandlePointer);
    pico_add_hook(app, PICO_HOOK_AFTER_RENDER, PicoChat_DrawChevrons);
}

PicoExt pico_ext_chat(void)
{
    return (PicoExt){
        .abi = PICO_EXT_ABI,
        .name = "chat",
        .init = ChatInit,
    };
}
