#include "scrollbar.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

static Clay_Dimensions MeasureText(Clay_StringSlice text, Clay_TextElementConfig *config, void *user_data)
{
    (void)user_data;
    return (Clay_Dimensions){.width = (float)text.length * (float)config->fontSize * 0.5f,
                             .height = (float)config->fontSize};
}

static int Fail(const char *test, const char *message)
{
    fprintf(stderr, "%s: %s\n", test, message);
    return 1;
}

static int ExpectFloat(const char *test, const char *what, float got, float want)
{
    float d = got - want;
    if (d < 0.0f)
    {
        d = -d;
    }
    if (d > 0.01f)
    {
        fprintf(stderr, "%s: %s got %f want %f\n", test, what, (double)got, (double)want);
        return 1;
    }
    return 0;
}

static int TestPinToBottom(void)
{
    const char *test = "pin_to_bottom";
    float scroll_y = -100.0f;
    if (!PicoScrollbar_PinToBottom(100.0f, 260.0f, &scroll_y) ||
        ExpectFloat(test, "grown content", scroll_y, -160.0f))
    {
        return Fail(test, "content growth did not request a corrected bottom layout");
    }
    if (PicoScrollbar_PinToBottom(100.0f, 260.0f, &scroll_y))
    {
        return Fail(test, "an already pinned viewport requested another layout");
    }
    scroll_y = -20.0f;
    if (!PicoScrollbar_PinToBottom(100.0f, 80.0f, &scroll_y) ||
        ExpectFloat(test, "fitting content", scroll_y, 0.0f))
    {
        return Fail(test, "fitting content was not reset to the top");
    }
    return 0;
}

static int TestExactFit(void)
{
    const char *test = "exact_fit";
    PicoScrollbarThumb thumb = PicoScrollbar_Metrics(100.0f, 100.0f, 0.0f);
    if (ExpectFloat(test, "height", thumb.height, 100.0f))
    {
        return 1;
    }
    return ExpectFloat(test, "y", thumb.y, 0.0f);
}

static int TestOverflowEnds(void)
{
    const char *test = "overflow_ends";
    PicoScrollbarThumb top = PicoScrollbar_Metrics(100.0f, 200.0f, 0.0f);
    if (ExpectFloat(test, "top.height", top.height, 50.0f) || ExpectFloat(test, "top.y", top.y, 0.0f))
    {
        return 1;
    }
    PicoScrollbarThumb bottom = PicoScrollbar_Metrics(100.0f, 200.0f, -100.0f);
    if (ExpectFloat(test, "bottom.height", bottom.height, 50.0f))
    {
        return 1;
    }
    if (ExpectFloat(test, "bottom.y", bottom.y, 50.0f))
    {
        return 1;
    }
    if (bottom.y + bottom.height < 99.99f || bottom.y + bottom.height > 100.01f)
    {
        return Fail(test, "thumb is not flush with the track bottom");
    }
    return 0;
}

static int TestMinThumb(void)
{
    const char *test = "min_thumb";
    PicoScrollbarThumb top = PicoScrollbar_Metrics(100.0f, 10000.0f, 0.0f);
    if (ExpectFloat(test, "top.y", top.y, 0.0f))
    {
        return 1;
    }
    PicoScrollbarThumb bottom = PicoScrollbar_Metrics(100.0f, 10000.0f, -9900.0f);
    PicoScrollbarThumb larger = PicoScrollbar_Metrics(100.0f, 20000.0f, 0.0f);
    if (top.height <= 1.0f || top.height >= 100.0f ||
        ExpectFloat(test, "minimum stays usable as content grows", larger.height, top.height) ||
        ExpectFloat(test, "drag preserves thumb size", bottom.height, top.height))
    {
        return 1;
    }
    if (bottom.y + bottom.height < 99.99f || bottom.y + bottom.height > 100.01f)
    {
        return Fail(test, "min thumb is not flush with the track bottom");
    }
    return 0;
}

/* A modal-style card: fixed height, a GROW scroll pane with a scrollbar, an optional
 * status line, then a footer. The footer must stay inside the card when the status line
 * appears no matter how far the pane is scrolled. */
static Clay_RenderCommandArray LayoutCardWithScrollbar(bool show_status)
{
    Clay_SetLayoutDimensions((Clay_Dimensions){640.0f, 800.0f});
    Clay_BeginLayout();
    CLAY(CLAY_ID("ScrollbarTestCard"),
         {.layout = {.layoutDirection = CLAY_TOP_TO_BOTTOM,
                     .padding = {16, 16, 16, 16},
                     .childGap = 12,
                     .sizing = {.width = CLAY_SIZING_FIXED(400.0f), .height = CLAY_SIZING_FIXED(400.0f)}}})
    {
        CLAY_TEXT(CLAY_STRING("Title"), CLAY_TEXT_CONFIG({.fontId = 0, .fontSize = 20, .textColor = {0}}));
        CLAY(CLAY_ID("ScrollbarTestRow"),
             {.layout = {.layoutDirection = CLAY_LEFT_TO_RIGHT,
                         .childGap = 8,
                         .sizing = {.width = CLAY_SIZING_GROW(0), .height = CLAY_SIZING_GROW(0)}}})
        {
            CLAY(CLAY_ID("ScrollbarTestScroll"),
                 {.layout = {.layoutDirection = CLAY_TOP_TO_BOTTOM,
                             .sizing = {.width = CLAY_SIZING_GROW(0), .height = CLAY_SIZING_GROW(0)}},
                  .clip = {.vertical = true, .horizontal = false, .childOffset = Clay_GetScrollOffset()}})
            {
                CLAY_AUTO_ID({.layout = {.sizing = {.width = CLAY_SIZING_GROW(0),
                                                    .height = CLAY_SIZING_FIXED(1200.0f)}}})
                {
                }
            }
            PicoScrollbar_Render(CLAY_STRING("ScrollbarTestScroll"), CLAY_STRING("ScrollbarTestTrack"),
                                 CLAY_STRING("ScrollbarTestThumb"));
        }
        if (show_status)
        {
            CLAY_TEXT(CLAY_STRING("Something is invalid."),
                      CLAY_TEXT_CONFIG(
                          {.fontId = 0, .fontSize = 14, .textColor = {0}, .wrapMode = CLAY_TEXT_WRAP_WORDS}));
        }
        CLAY(CLAY_ID("ScrollbarTestFooter"),
             {.layout = {.layoutDirection = CLAY_LEFT_TO_RIGHT, .sizing = {.width = CLAY_SIZING_GROW(0)}}})
        {
            CLAY_AUTO_ID({.layout = {.sizing = {.width = CLAY_SIZING_FIXED(60.0f),
                                                .height = CLAY_SIZING_FIXED(30.0f)}}})
            {
            }
        }
    }
    return Clay_EndLayout(0.0f);
}

static int FooterInsideCard(const char *test)
{
    Clay_ElementData card = Clay_GetElementData(CLAY_ID("ScrollbarTestCard"));
    Clay_ElementData footer = Clay_GetElementData(CLAY_ID("ScrollbarTestFooter"));
    if (!card.found || !footer.found)
    {
        return Fail(test, "card or footer missing from layout");
    }
    float card_bottom = card.boundingBox.y + card.boundingBox.height;
    float footer_bottom = footer.boundingBox.y + footer.boundingBox.height;
    if (footer_bottom > card_bottom + 0.5f)
    {
        return Fail(test, "status line pushed the footer out of the card");
    }
    return 0;
}

static int TestThumbOffsetDoesNotResistCompression(void)
{
    const char *test = "thumb_offset_compression";
    Clay_ScrollContainerData scroll;

    LayoutCardWithScrollbar(false);
    scroll = Clay_GetScrollContainerData(CLAY_ID("ScrollbarTestScroll"));
    if (!scroll.found || !scroll.scrollPosition)
    {
        return Fail(test, "scroll container missing");
    }
    /* Scrolled to the very bottom: the thumb spacer is tallest here. */
    scroll.scrollPosition->y = scroll.scrollContainerDimensions.height - scroll.contentDimensions.height;
    LayoutCardWithScrollbar(false);
    LayoutCardWithScrollbar(true);
    if (FooterInsideCard(test))
    {
        return 1;
    }

    scroll = Clay_GetScrollContainerData(CLAY_ID("ScrollbarTestScroll"));
    scroll.scrollPosition->y = 0.0f;
    LayoutCardWithScrollbar(false);
    LayoutCardWithScrollbar(true);
    return FooterInsideCard(test);
}

int main(void)
{
    int failed = 0;
    uint32_t arena_size = Clay_MinMemorySize();
    void *memory = malloc(arena_size);
    if (!memory)
    {
        return Fail("setup", "could not allocate Clay arena");
    }
    Clay_Arena arena = Clay_CreateArenaWithCapacityAndMemory(arena_size, memory);
    if (!Clay_Initialize(arena, (Clay_Dimensions){640.0f, 800.0f}, (Clay_ErrorHandler){0}))
    {
        free(memory);
        return Fail("setup", "could not initialize Clay");
    }
    Clay_SetMeasureTextFunction(MeasureText, NULL);
    failed |= TestPinToBottom();
    failed |= TestExactFit();
    failed |= TestOverflowEnds();
    failed |= TestMinThumb();
    failed |= TestThumbOffsetDoesNotResistCompression();
    free(memory);
    return failed;
}
