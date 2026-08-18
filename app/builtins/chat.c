#include "pico/plugin.h"
#include "pico/md_view.h"

#include "clay/clay.h"

#include <string.h>

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

void PicoChat_Render(PicoApp *app)
{
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
                            const char *line = msg->trace[t].text;
                            if (!line || !line[0])
                            {
                                continue;
                            }
                            Clay_String think = {.length = (int32_t)strlen(line), .chars = line};
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
        if (Clay_PointerOver(CLAY_IDI("Msg", i)))
        {
            app->selected_message = i;
            return;
        }
    }
}

static void ChatInit(PicoApp *app)
{
    pico_add_view(app, PICO_SLOT_MAIN, 0, PicoChat_Render);
    pico_add_hook(app, PICO_HOOK_AFTER_LAYOUT, PicoChat_HandlePointer);
}

PicoExt pico_ext_chat(void)
{
    return (PicoExt){
        .abi = PICO_EXT_ABI,
        .name = "chat",
        .init = ChatInit,
    };
}
