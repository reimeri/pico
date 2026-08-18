#include "pico/app.h"
#include "pico/md_view.h"

#include "clay/clay.h"

#include <string.h>

static float ChatWidth(void)
{
    float width = (float)GetScreenWidth() - CONTENT_PADDING * 2 - SCROLLBAR_WIDTH - 28;
    if (width < 50)
    {
        width = 50;
    }
    return width;
}

void PicoChat_Render(PicoApp *app)
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
            float available_width = ChatWidth();
            for (int i = 0; i < app->message_count; i++)
            {
                PicoMessage *msg = &app->messages[i];
                bool user = msg->role == PICO_ROLE_USER;
                CLAY(CLAY_IDI("Msg", i),
                     {.layout = {.layoutDirection = CLAY_TOP_TO_BOTTOM,
                                 .padding = {16, 16, 12, 12},
                                 .childGap = 8,
                                 .sizing = {.width = CLAY_SIZING_GROW(0)}},
                      .backgroundColor = user ? COLOR_USER_BG : COLOR_ASSISTANT_BG,
                      .cornerRadius = CLAY_CORNER_RADIUS(8)})
                {
                    CLAY_TEXT(user ? CLAY_STRING("You") : CLAY_STRING("Pico"),
                              CLAY_TEXT_CONFIG({.fontId = FONT_BOLD,
                                                .fontSize = 13,
                                                .textColor = COLOR_MUTED,
                                                .wrapMode = CLAY_TEXT_WRAP_NONE}));
                    MdView_RenderDocument(&msg->doc, (i + 1) * 4096, available_width);
                }
            }
        }
    }

    Clay_ScrollContainerData scroll_data = Clay_GetScrollContainerData(Clay_GetElementId(CLAY_STRING("ChatScroll")));
    if (scroll_data.found && scroll_data.contentDimensions.height > 0)
    {
        float thumb_height = (scroll_data.scrollContainerDimensions.height / scroll_data.contentDimensions.height) *
                             scroll_data.scrollContainerDimensions.height;
        if (thumb_height < 24)
        {
            thumb_height = 24;
        }
        CLAY(CLAY_ID("ChatScrollBar"),
             {.floating = {.attachTo = CLAY_ATTACH_TO_ELEMENT_WITH_ID,
                           .offset = {.y = scroll_data.contentDimensions.height > 0
                                               ? -(scroll_data.scrollPosition->y / scroll_data.contentDimensions.height) *
                                                     scroll_data.scrollContainerDimensions.height
                                               : 0},
                           .zIndex = 1,
                           .parentId = Clay_GetElementId(CLAY_STRING("ChatScroll")).id,
                           .attachPoints = {.element = CLAY_ATTACH_POINT_RIGHT_TOP,
                                            .parent = CLAY_ATTACH_POINT_RIGHT_TOP}}})
        {
            CLAY(CLAY_ID("ChatScrollBarHandle"),
                 {.layout = {.sizing = {.width = CLAY_SIZING_FIXED(SCROLLBAR_WIDTH),
                                        .height = CLAY_SIZING_FIXED(thumb_height)}},
                  .backgroundColor = Clay_PointerOver(Clay_GetElementId(CLAY_STRING("ChatScrollBar")))
                                         ? COLOR_SCROLLBAR_HOVER
                                         : COLOR_SCROLLBAR,
                  .cornerRadius = CLAY_CORNER_RADIUS(SCROLLBAR_WIDTH / 2)})
            {
            }
        }
    }
}
