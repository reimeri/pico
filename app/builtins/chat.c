#include "pico/plugin.h"
#include "pico/md_view.h"
#include "chat_sel.h"
#include "settings.h"

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
            PicoChatSel_Text(s, (Clay_TextElementConfig){.fontId = FONT_MONO,
                                                         .fontSize = 14,
                                                         .lineHeight = 18,
                                                         .textColor = COLOR_CODE_TEXT,
                                                         .wrapMode = CLAY_TEXT_WRAP_WORDS});
            PicoChatSel_Break();
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
            PicoChatSel_Text(name, (Clay_TextElementConfig){.fontId = FONT_REGULAR,
                                                            .fontSize = 15,
                                                            .textColor = name_color,
                                                            .wrapMode = CLAY_TEXT_WRAP_NONE});
            if (args.length > 0)
            {
                PicoChatSel_Glue(" ");
                CLAY_AUTO_ID({.layout = {.sizing = {.width = CLAY_SIZING_GROW(0)}}})
                {
                    PicoChatSel_Text(args, (Clay_TextElementConfig){.fontId = FONT_REGULAR,
                                                                   .fontSize = 15,
                                                                   .textColor = args_color,
                                                                   .wrapMode = CLAY_TEXT_WRAP_WORDS});
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
    PicoChatSel_Break();
}

static Clay_String EmptyCStr(const char *s)
{
    if (!s)
    {
        s = "";
    }
    return (Clay_String){.length = (int32_t)strlen(s), .chars = s};
}

static void RenderEmptyCard(int id, Clay_String title, const char **items, int n)
{
    CLAY(CLAY_IDI("EmptyCard", id),
         {.layout = {.layoutDirection = CLAY_TOP_TO_BOTTOM,
                     .padding = {16, 16, 16, 16},
                     .childGap = 8,
                     .sizing = {.width = CLAY_SIZING_GROW(0)}},
          .backgroundColor = COLOR_CONTENT_BG,
          .cornerRadius = CLAY_CORNER_RADIUS(8)})
    {
        CLAY_TEXT(title, CLAY_TEXT_CONFIG({.fontId = FONT_BOLD, .fontSize = 15, .textColor = COLOR_TEXT}));
        if (n <= 0)
        {
            CLAY_TEXT(CLAY_STRING("None"), CLAY_TEXT_CONFIG({.fontId = FONT_REGULAR,
                                                             .fontSize = 13,
                                                             .textColor = COLOR_MUTED,
                                                             .wrapMode = CLAY_TEXT_WRAP_NONE}));
        }
        else
        {
            for (int i = 0; i < n; i++)
            {
                CLAY_TEXT(EmptyCStr(items[i]), CLAY_TEXT_CONFIG({.fontId = FONT_REGULAR,
                                                                 .fontSize = 13,
                                                                 .textColor = COLOR_TEXT,
                                                                 .wrapMode = CLAY_TEXT_WRAP_WORDS}));
            }
        }
    }
}

static void RenderEmptyCards(PicoApp *app)
{
    const char *tools[PICO_MAX_TOOLS];
    int tool_n = 0;
    for (int i = 0; i < app->tool_count && tool_n < PICO_MAX_TOOLS; i++)
    {
        if (app->tools[i].name && app->tools[i].name[0])
        {
            tools[tool_n++] = app->tools[i].name;
        }
    }
    const char *ctx[8];
    int ctx_n = PicoSettings_LoadedContext(app, ctx, 8);
    bool narrow = GetScreenWidth() < 720;
    CLAY(CLAY_ID("EmptyCards"),
         {.layout = {.layoutDirection = narrow ? CLAY_TOP_TO_BOTTOM : CLAY_LEFT_TO_RIGHT,
                     .childGap = 12,
                     .sizing = {.width = CLAY_SIZING_GROW(0, 900)}}})
    {
        RenderEmptyCard(0, CLAY_STRING("Tools"), tools, tool_n);
        RenderEmptyCard(1, CLAY_STRING("Context"), ctx, ctx_n);
        RenderEmptyCard(2, CLAY_STRING("Skills"), NULL, 0);
    }
}

void PicoChat_Render(PicoApp *app)
{
    app->hovered_tool = false;
    PicoChatSel_BeginFrame(app->message_count);
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
            bool empty = app->message_count == 0;
            Clay_ChildAlignment align = empty ? (Clay_ChildAlignment){.x = CLAY_ALIGN_X_CENTER, .y = CLAY_ALIGN_Y_CENTER}
                                              : (Clay_ChildAlignment){0};
            Clay_Sizing content_size = {.width = CLAY_SIZING_GROW(0)};
            if (empty)
            {
                content_size.height = CLAY_SIZING_GROW(0);
            }
            CLAY(CLAY_ID("ChatContent"),
                 {.layout = {.layoutDirection = CLAY_TOP_TO_BOTTOM,
                             .childGap = 16,
                             .padding = {4, 4, 8, 8},
                             .childAlignment = align,
                             .sizing = content_size}})
            {
                if (empty)
                {
                    RenderEmptyCards(app);
                }
                float available_width = ChatWidth(app);
                for (int i = 0; i < app->message_count; i++)
                {
                    PicoMessage *msg = &app->messages[i];
                    bool user = msg->role == PICO_ROLE_USER;
                    Clay_Color bg = user ? COLOR_USER_BG : COLOR_ASSISTANT_BG;
                    CLAY(CLAY_IDI("Msg", i),
                         {.layout = {.layoutDirection = CLAY_TOP_TO_BOTTOM,
                                     .padding = {16, 16, 12, 12},
                                     .childGap = 8,
                                     .sizing = {.width = CLAY_SIZING_GROW(0)}},
                          .backgroundColor = bg,
                          .cornerRadius = user ? CLAY_CORNER_RADIUS(8) : CLAY_CORNER_RADIUS(0)})
                    {
                        PicoChatSel_SetMessage(i);
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
                            PicoChatSel_Text(think, (Clay_TextElementConfig){.fontId = FONT_ITALIC,
                                                                             .fontSize = 15,
                                                                             .textColor = COLOR_MUTED,
                                                                             .wrapMode = CLAY_TEXT_WRAP_WORDS});
                            PicoChatSel_Break();
                        }
                        if (!has_trace && live && !has_source)
                        {
                            const char *label =
                                app->agent_activity[0] ? app->agent_activity : "Thinking…";
                            Clay_String think = {.length = (int32_t)strlen(label), .chars = label};
                            PicoChatSel_Text(think, (Clay_TextElementConfig){.fontId = FONT_ITALIC,
                                                                             .fontSize = 15,
                                                                             .textColor = COLOR_MUTED,
                                                                             .wrapMode = CLAY_TEXT_WRAP_WORDS});
                            PicoChatSel_Break();
                        }
                        if (has_source)
                        {
                            MdView_RenderDocument(&msg->doc, (i + 1) * 4096, available_width);
                        }
                        PicoChatSel_SetMessage(-1);
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
    PicoChatSel_Clamp(app);
    if (app->status_warn || PicoExts_IsOpen())
    {
        return;
    }

    Vector2 mouse = GetMousePosition();
    bool over_bar = Clay_PointerOver(Clay_GetElementId(CLAY_STRING("ChatScrollBarHandle"))) ||
                    Clay_PointerOver(Clay_GetElementId(CLAY_STRING("ChatScrollTrack")));
    bool over_composer = Clay_PointerOver(Clay_GetElementId(CLAY_STRING("Composer")));
    bool over_chat = Clay_PointerOver(Clay_GetElementId(CLAY_STRING("ChatScroll")));

    if (over_bar || over_composer)
    {
        if (IsMouseButtonPressed(MOUSE_BUTTON_LEFT))
        {
            if (over_composer)
            {
                PicoChatSel_Clear(app);
            }
            app->chat_sel.mouse_selecting = false;
            app->chat_sel.dragging = false;
            app->chat_sel.pressed_tool = false;
        }
        return;
    }

    if (IsMouseButtonPressed(MOUSE_BUTTON_LEFT) && over_chat)
    {
        int tool_msg = -1;
        int tool_idx = -1;
        for (int i = 0; i < app->message_count; i++)
        {
            PicoMessage *msg = &app->messages[i];
            for (int t = 0; t < msg->trace_count; t++)
            {
                if (msg->trace[t].is_tool && Clay_PointerOver(ToolRowId(i, t)))
                {
                    tool_msg = i;
                    tool_idx = t;
                    break;
                }
            }
            if (tool_idx >= 0)
            {
                break;
            }
        }

        int msg = -1;
        int pos = PicoChatSel_OffsetAtPoint(app, mouse.x, mouse.y, -1, &msg);
        app->chat_sel.mouse_selecting = true;
        app->chat_sel.dragging = false;
        app->chat_sel.press_x = mouse.x;
        app->chat_sel.press_y = mouse.y;
        app->chat_sel.pressed_tool = tool_idx >= 0;
        app->chat_sel.tool_msg = tool_msg;
        app->chat_sel.tool_idx = tool_idx;
        app->chat_sel.msg = msg;
        app->chat_sel.anchor = pos;
        app->chat_sel.cursor = pos;
        app->composer.sel_anchor = app->composer.cursor;
    }

    if (!IsMouseButtonDown(MOUSE_BUTTON_LEFT))
    {
        if (app->chat_sel.mouse_selecting && !app->chat_sel.dragging && app->chat_sel.pressed_tool &&
            app->chat_sel.tool_msg >= 0 && app->chat_sel.tool_msg < app->message_count)
        {
            PicoMessage *msg = &app->messages[app->chat_sel.tool_msg];
            int t = app->chat_sel.tool_idx;
            if (t >= 0 && t < msg->trace_count && msg->trace[t].is_tool &&
                Clay_PointerOver(ToolRowId(app->chat_sel.tool_msg, t)))
            {
                msg->trace[t].expanded = !msg->trace[t].expanded;
            }
            app->chat_sel.anchor = app->chat_sel.cursor;
        }
        app->chat_sel.mouse_selecting = false;
        app->chat_sel.pressed_tool = false;
        if (!app->chat_sel.dragging && app->chat_sel.anchor == app->chat_sel.cursor)
        {
            app->chat_sel.dragging = false;
        }
        return;
    }

    if (app->chat_sel.mouse_selecting)
    {
        float dx = mouse.x - app->chat_sel.press_x;
        float dy = mouse.y - app->chat_sel.press_y;
        if (dx * dx + dy * dy > 16.0f)
        {
            app->chat_sel.dragging = true;
        }
        if (app->chat_sel.dragging)
        {
            int msg = app->chat_sel.msg;
            int pos = PicoChatSel_OffsetAtPoint(app, mouse.x, mouse.y, msg, &msg);
            if (app->chat_sel.msg < 0)
            {
                app->chat_sel.msg = msg;
                app->chat_sel.anchor = pos;
            }
            app->chat_sel.cursor = pos;
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

void PicoChat_DrawOverlay(PicoApp *app)
{
    PicoChatSel_DrawOverlay(app);
    PicoChat_DrawChevrons(app);
}

static void ChatInit(PicoApp *app)
{
    pico_add_view(app, PICO_SLOT_MAIN, 0, PicoChat_Render);
    pico_add_hook(app, PICO_HOOK_AFTER_LAYOUT, PicoChat_HandlePointer);
    pico_add_hook(app, PICO_HOOK_AFTER_RENDER, PicoChat_DrawOverlay);
}

PicoExt pico_ext_chat(void)
{
    return (PicoExt){
        .abi = PICO_EXT_ABI,
        .name = "chat",
        .description = "Chat transcript",
        .init = ChatInit,
    };
}
