#include "pico/theme.h"

#include <stdio.h>
#include <stdlib.h>

static int Fail(const char *message)
{
    fprintf(stderr, "clay capacity: %s\n", message);
    return 1;
}

static void DropClay(void *memory)
{
    Clay_SetCurrentContext(NULL);
    free(memory);
}

static int TestHashMapOverflowGrows(void)
{
    uint32_t size = Clay_MinMemorySize();
    void *memory = malloc(size);
    if (!memory)
    {
        return Fail("could not allocate Clay arena");
    }

    Clay_Arena arena = Clay_CreateArenaWithCapacityAndMemory(size, memory);
    if (!Clay_Initialize(arena, (Clay_Dimensions){100, 100}, (Clay_ErrorHandler){0}))
    {
        DropClay(memory);
        return Fail("could not initialize Clay");
    }

    Pico_ClearClayReinit();
    int32_t before = Clay_GetMaxElementCount();
    Pico_HandleClayErrors((Clay_ErrorData){
        .errorType = CLAY_ERROR_TYPE_HASH_MAP_CAPACITY_EXCEEDED,
        .errorText = CLAY_STRING("hashmap overflow"),
    });

    int failed = 0;
    if (!Pico_NeedsClayReinit())
    {
        failed = Fail("hashmap overflow did not request Clay reinit");
    }
    else if (Clay_GetMaxElementCount() != before * 2)
    {
        failed = Fail("hashmap overflow did not double max element count");
    }

    Pico_ClearClayReinit();
    DropClay(memory);
    return failed;
}

static void LayoutChat(float content_h)
{
    Clay_BeginLayout();
    CLAY(CLAY_ID("ChatScroll"),
         {.layout = {.sizing = {.width = CLAY_SIZING_FIXED(80), .height = CLAY_SIZING_FIXED(100)},
                     .layoutDirection = CLAY_TOP_TO_BOTTOM},
          .clip = {.vertical = true, .childOffset = Clay_GetScrollOffset()}})
    {
        CLAY_AUTO_ID({.layout = {.sizing = {.width = CLAY_SIZING_FIXED(80), .height = CLAY_SIZING_FIXED(content_h)}}})
        {
        }
    }
    Clay_EndLayout(0.0f);
}

static int TestScrollSurvivesReinit(void)
{
    uint32_t size = Clay_MinMemorySize();
    void *memory = malloc(size);
    if (!memory)
    {
        return Fail("could not allocate Clay arena");
    }

    Clay_Arena arena = Clay_CreateArenaWithCapacityAndMemory(size, memory);
    if (!Clay_Initialize(arena, (Clay_Dimensions){200, 200}, (Clay_ErrorHandler){0}))
    {
        DropClay(memory);
        return Fail("could not initialize Clay for scroll restore");
    }

    LayoutChat(400.0f);
    Clay_ScrollContainerData data = Clay_GetScrollContainerData(Clay_GetElementId(CLAY_STRING("ChatScroll")));
    if (!data.found || !data.scrollPosition)
    {
        DropClay(memory);
        return Fail("ChatScroll was not a scroll container");
    }
    data.scrollPosition->y = -150.0f;
    Pico_RememberClayScroll();
    Pico_CaptureClayScroll();

    uint32_t new_size = Clay_MinMemorySize();
    void *new_memory = malloc(new_size);
    if (!new_memory)
    {
        DropClay(memory);
        return Fail("could not allocate replacement Clay arena");
    }
    Clay_Arena new_arena = Clay_CreateArenaWithCapacityAndMemory(new_size, new_memory);
    if (!Clay_Initialize(new_arena, (Clay_Dimensions){200, 200}, (Clay_ErrorHandler){0}))
    {
        free(new_memory);
        DropClay(memory);
        return Fail("could not reinitialize Clay");
    }
    free(memory);
    memory = new_memory;

    LayoutChat(400.0f);
    data = Clay_GetScrollContainerData(Clay_GetElementId(CLAY_STRING("ChatScroll")));
    if (!data.found || !data.scrollPosition)
    {
        DropClay(memory);
        return Fail("ChatScroll missing after reinit");
    }
    if (data.scrollPosition->y != 0.0f)
    {
        DropClay(memory);
        return Fail("reinit did not reset ChatScroll");
    }
    if (!Pico_RestoreClayScroll())
    {
        DropClay(memory);
        return Fail("restore did not apply the captured ChatScroll offset");
    }
    if (data.scrollPosition->y != -150.0f)
    {
        DropClay(memory);
        return Fail("ChatScroll offset was not restored");
    }

    DropClay(memory);
    return 0;
}

int main(void)
{
    int rc = TestHashMapOverflowGrows();
    return rc != 0 ? rc : TestScrollSurvivesReinit();
}
