#include "pico/md_view.h"
#include "md_view_internal.h"
#include "richtext.h"
#include "clay/clay.h"

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static Clay_Dimensions MeasureText(Clay_StringSlice text, Clay_TextElementConfig *config,
                                   void *user_data)
{
    (void)user_data;
    return (Clay_Dimensions){.width = (float)text.length * (float)config->fontSize * 0.6f,
                             .height = (float)config->fontSize};
}

float Pico_FontScale(void)
{
    return 1.0f;
}

float Pico_FontPx(uint16_t design)
{
    return (float)design;
}

uint16_t Pico_FontPxU16(uint16_t design)
{
    return design;
}

static int Fail(const char *message)
{
    fprintf(stderr, "FAIL: %s\n", message);
    return 1;
}

static void RenderDocument(MdDocument *doc, int id_base, float width)
{
    Clay_SetLayoutDimensions((Clay_Dimensions){width, 600.0f});
    MdView_BeginFrame();
    Clay_BeginLayout();
    CLAY(CLAY_ID("MdScrollTestRoot"),
         {.layout = {.layoutDirection = CLAY_TOP_TO_BOTTOM,
                     .sizing = {.width = CLAY_SIZING_FIXED(width),
                                .height = CLAY_SIZING_FIXED(200.0f)}}})
    {
        CLAY(CLAY_ID("MdScrollTestChat"),
             {.layout = {.layoutDirection = CLAY_TOP_TO_BOTTOM,
                         .sizing = {.width = CLAY_SIZING_FIXED(width),
                                    .height = CLAY_SIZING_FIXED(200.0f)}},
              .clip = {.vertical = true, .childOffset = Clay_GetScrollOffset()}})
        {
            CLAY_AUTO_ID({.layout = {.layoutDirection = CLAY_TOP_TO_BOTTOM,
                                     .sizing = {.width = CLAY_SIZING_FIXED(width)}}})
            {
                MdView_RenderDocument(doc, id_base, width);
                CLAY_AUTO_ID({.layout = {.sizing = {
                                  .width = CLAY_SIZING_FIXED(width),
                                  .height = CLAY_SIZING_FIXED(300.0f)}}})
                {
                }
            }
        }
    }
    (void)Clay_EndLayout(0.0f);
}

static int AssertHorizontalOverflow(MdDocument *doc, int id_base, const char *kind)
{
    const float width = 180.0f;
    Clay_ScrollContainerData previous_chat =
        Clay_GetScrollContainerData(CLAY_ID("MdScrollTestChat"));
    if (previous_chat.found && previous_chat.scrollPosition)
    {
        previous_chat.scrollPosition->y = 0.0f;
    }
    RenderDocument(doc, id_base, width);

    Clay_ElementId id = Clay_GetElementIdWithIndex(CLAY_STRING("MdHorizontalScroll"),
                                                    (uint32_t)id_base);
    Clay_ScrollContainerData scroll = Clay_GetScrollContainerData(id);
    if (!scroll.found || !scroll.scrollPosition || !scroll.config.horizontal)
    {
        return Fail(kind);
    }
    if (fabsf(scroll.scrollContainerDimensions.width - width) > 0.01f)
    {
        return Fail("markdown horizontal scroller did not stay within the chat width");
    }
    if (scroll.contentDimensions.width <= scroll.scrollContainerDimensions.width + 0.5f)
    {
        return Fail("wide markdown content did not create horizontal overflow");
    }

    Clay_ScrollContainerData chat = Clay_GetScrollContainerData(CLAY_ID("MdScrollTestChat"));
    if (!chat.found || !chat.scrollPosition)
    {
        return Fail("nested chat scroll container was not laid out");
    }
    Clay_BoundingBox box = Clay_GetElementData(id).boundingBox;
    Clay_SetPointerState((Clay_Vector2){box.x + box.width / 2.0f,
                                        box.y + box.height / 2.0f},
                         false);
    chat.scrollPosition->y = -40.0f;
    if (!MdView_ScrollHoveredHorizontal(-1.0f))
    {
        return Fail("hovered markdown overflow did not handle horizontal input");
    }
    if (scroll.scrollPosition->x >= -0.01f)
    {
        return Fail("horizontal input did not reveal clipped markdown content");
    }
    if (fabsf(chat.scrollPosition->y + 40.0f) > 0.01f)
    {
        return Fail("horizontal markdown input changed the chat's vertical position");
    }

    int unrelated_id_base = id_base + 7919;
    RenderDocument(doc, unrelated_id_base, width);
    Clay_ElementId unrelated_id = Clay_GetElementIdWithIndex(
        CLAY_STRING("MdHorizontalScroll"), (uint32_t)unrelated_id_base);
    Clay_ScrollContainerData unrelated = Clay_GetScrollContainerData(unrelated_id);
    if (!unrelated.found || !unrelated.scrollPosition || fabsf(unrelated.scrollPosition->x) > 0.01f)
    {
        return Fail("unrelated markdown content inherited a horizontal scroll offset");
    }
    return 0;
}

static int TestWideCodeBlock(void)
{
    const char *source =
        "```text\n"
        "this_is_a_single_code_line_that_is_far_wider_than_the_chat_viewport_and_must_scroll\n"
        "```";
    MdDocument doc = MdDocument_Parse(source, strlen(source));
    int result = doc.block_count == 1 && doc.blocks[0].type == MDB_CODE
                     ? AssertHorizontalOverflow(&doc, 1000, "code block was not a horizontal scroller")
                     : Fail("wide code markdown did not parse as one code block");
    MdDocument_Free(&doc);
    return result;
}

static int TestWideTable(void)
{
    const char *source =
        "| first_column_with_a_long_value | second_column_with_a_long_value | third_column_with_a_long_value |\n"
        "| --- | --- | --- |\n"
        "| alpha_value_that_cannot_wrap | beta_value_that_cannot_wrap | gamma_value_that_cannot_wrap |";
    MdDocument doc = MdDocument_Parse(source, strlen(source));
    int result = doc.block_count == 1 && doc.blocks[0].type == MDB_TABLE
                     ? AssertHorizontalOverflow(&doc, 2000, "table was not a horizontal scroller")
                     : Fail("wide table markdown did not parse as one table");
    MdDocument_Free(&doc);
    return result;
}

int main(void)
{
    uint32_t arena_size = Clay_MinMemorySize();
    void *memory = malloc(arena_size);
    if (!memory)
    {
        return Fail("could not allocate Clay arena");
    }
    Clay_Arena arena = Clay_CreateArenaWithCapacityAndMemory(arena_size, memory);
    if (!Clay_Initialize(arena, (Clay_Dimensions){180.0f, 600.0f},
                         (Clay_ErrorHandler){0}))
    {
        free(memory);
        return Fail("could not initialize Clay");
    }
    Clay_SetMeasureTextFunction(MeasureText, NULL);
    RichText_SetMeasureFunction(MeasureText, NULL);

    int result = TestWideCodeBlock();
    if (result == 0)
    {
        result = TestWideTable();
    }
    free(memory);
    return result;
}
