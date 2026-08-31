#include "pico/md_view.h"
#include "pico/theme.h"
#include "chat_sel.h"
#include "md_view_internal.h"
#include "richtext.h"
#include "clay/clay.h"

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static Clay_Dimensions MeasureRichText(Clay_StringSlice text, Clay_TextElementConfig *config,
                                       void *user_data)
{
    (void)user_data;
    int codepoints = 0;
    for (int i = 0; i < text.length; i++)
    {
        if (((unsigned char)text.chars[i] & 0xC0) != 0x80)
        {
            codepoints++;
        }
    }
    return (Clay_Dimensions){.width = (float)codepoints * (float)config->fontSize * 0.6f,
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

static Clay_RenderCommandArray RenderDocument(MdDocument *doc, int id_base, float width,
                                               bool selectable)
{
    Clay_ScrollContainerData previous_chat =
        Clay_GetScrollContainerData(CLAY_ID("MdScrollTestChat"));
    if (previous_chat.found && previous_chat.scrollPosition)
    {
        previous_chat.scrollPosition->y = 0.0f;
    }
    Clay_SetLayoutDimensions((Clay_Dimensions){width, 600.0f});
    MdView_BeginFrame();
    PicoChatSel_BeginFrame(selectable ? 1 : 0);
    PicoChatSel_SetMessage(selectable ? 0 : -1);
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
    PicoChatSel_SetMessage(-1);
    return Clay_EndLayout(0.0f);
}

static bool TextCommandEquals(const Clay_RenderCommand *command, const char *text,
                              uint16_t font_id)
{
    if (!command || command->commandType != CLAY_RENDER_COMMAND_TYPE_TEXT ||
        command->renderData.text.fontId != font_id)
    {
        return false;
    }
    Clay_StringSlice contents = command->renderData.text.stringContents;
    size_t length = strlen(text);
    return contents.length == (int32_t)length && memcmp(contents.chars, text, length) == 0;
}

static Clay_RenderCommand *FindTextCommand(Clay_RenderCommandArray *commands, const char *text,
                                           uint16_t font_id)
{
    for (int i = 0; i < commands->length; i++)
    {
        Clay_RenderCommand *command = Clay_RenderCommandArray_Get(commands, i);
        if (TextCommandEquals(command, text, font_id))
        {
            return command;
        }
    }
    return NULL;
}

static int AssertWrappedAfter(Clay_RenderCommandArray *commands, const char *leading,
                              uint16_t leading_font, const char *continuation,
                              uint16_t continuation_font)
{
    Clay_RenderCommand *first = FindTextCommand(commands, leading, leading_font);
    Clay_RenderCommand *next = FindTextCommand(commands, continuation, continuation_font);
    if (!first || !next)
    {
        return Fail("markdown width test did not render its expected styled runs");
    }
    if (next->boundingBox.y <= first->boundingBox.y + 0.01f)
    {
        return Fail("decorated markdown content wrapped to the outer width");
    }
    if (fabsf(next->boundingBox.x - first->boundingBox.x) > 0.01f)
    {
        return Fail("wrapped markdown continuation did not align with its content column");
    }
    return 0;
}

static int AssertNoTextOverlap(Clay_RenderCommandArray commands)
{
    int text_count = 0;
    for (int i = 0; i < commands.length; i++)
    {
        Clay_RenderCommand *a = Clay_RenderCommandArray_Get(&commands, i);
        if (!a || a->commandType != CLAY_RENDER_COMMAND_TYPE_TEXT)
        {
            continue;
        }
        text_count++;
        for (int j = i + 1; j < commands.length; j++)
        {
            Clay_RenderCommand *b = Clay_RenderCommandArray_Get(&commands, j);
            if (!b || b->commandType != CLAY_RENDER_COMMAND_TYPE_TEXT)
            {
                continue;
            }
            float overlap_x = fminf(a->boundingBox.x + a->boundingBox.width,
                                    b->boundingBox.x + b->boundingBox.width) -
                              fmaxf(a->boundingBox.x, b->boundingBox.x);
            bool same_line = fabsf(a->boundingBox.y - b->boundingBox.y) <= 0.01f;
            if (overlap_x > 0.01f && same_line)
            {
                return Fail("adjacent rendered markdown text overlapped");
            }
        }
    }
    return text_count > 0 ? 0 : Fail("markdown width test rendered no text");
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
    (void)RenderDocument(doc, id_base, width, false);

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
    (void)RenderDocument(doc, unrelated_id_base, width, false);
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

static int TestListItemsUseInnerWidth(void)
{
    const char *source =
        "- prefix `code` after *italics* x\n\n"
        "10. alpha words before **bolded!**\n\n"
        "- parent\n"
        "  - [ ] task words before *italic*";
    MdDocument doc = MdDocument_Parse(source, strlen(source));
    Clay_RenderCommandArray commands = RenderDocument(&doc, 3000, 300.0f, true);
    int result = AssertNoTextOverlap(commands);
    if (result == 0)
    {
        result = AssertWrappedAfter(&commands, "alpha words before", FONT_REGULAR,
                                    "bolded!", FONT_BOLD);
    }
    if (result == 0)
    {
        result = AssertWrappedAfter(&commands, "task words before", FONT_REGULAR,
                                    "italic", FONT_ITALIC);
    }
    if (result == 0 && !FindTextCommand(&commands, "code", FONT_MONO))
    {
        result = Fail("list width test did not render inline code");
    }
    MdDocument_Free(&doc);
    return result;
}

static int TestQuoteUsesInnerWidth(void)
{
    const char *source = "> quote words before *italic*";
    MdDocument doc = MdDocument_Parse(source, strlen(source));
    Clay_RenderCommandArray commands = RenderDocument(&doc, 4000, 300.0f, true);
    int result = AssertNoTextOverlap(commands);
    if (result == 0)
    {
        result = AssertWrappedAfter(&commands, "quote words before", FONT_REGULAR,
                                    "italic", FONT_ITALIC);
    }
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
    Clay_SetMeasureTextFunction(MeasureRichText, NULL);
    RichText_SetMeasureFunction(MeasureRichText, NULL);

    int result = TestWideCodeBlock();
    if (result == 0)
    {
        result = TestWideTable();
    }
    if (result == 0)
    {
        result = TestListItemsUseInnerWidth();
    }
    if (result == 0)
    {
        result = TestQuoteUsesInnerWidth();
    }
    free(memory);
    return result;
}
