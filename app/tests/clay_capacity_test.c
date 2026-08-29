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

static int TestUnbalancedOverflowGrows(void)
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
        .errorType = CLAY_ERROR_TYPE_UNBALANCED_OPEN_CLOSE,
        .errorText = CLAY_STRING("unbalanced"),
    });

    int failed = 0;
    if (!Pico_NeedsClayReinit())
    {
        failed = Fail("unbalanced open/close did not request Clay reinit");
    }
    else if (Clay_GetMaxElementCount() != before * 2)
    {
        failed = Fail("unbalanced open/close did not double max element count");
    }

    Pico_ClearClayReinit();
    DropClay(memory);
    return failed;
}

static int clay_internal_error_count;

static void CountClayInternalErrors(Clay_ErrorData error)
{
    if (error.errorType == CLAY_ERROR_TYPE_INTERNAL_ERROR)
    {
        clay_internal_error_count++;
    }
}

static void LayoutClippedElements(Clay_String id_prefix, int32_t count)
{
    Clay_BeginLayout();
    CLAY(CLAY_ID("ClipRoot"),
         {.layout = {.sizing = {.width = CLAY_SIZING_FIXED(100), .height = CLAY_SIZING_FIXED(100)}}})
    {
        for (int32_t i = 0; i < count; i++)
        {
            CLAY(Clay_GetElementIdWithIndex(id_prefix, (uint32_t)i),
                 {.layout = {.sizing = {.width = CLAY_SIZING_FIXED(1), .height = CLAY_SIZING_FIXED(1)}},
                  .clip = {.horizontal = true}})
            {
            }
        }
    }
    Clay_EndLayout(0.0f);
}

static int TestManyClippedElements(void)
{
    uint32_t size = Clay_MinMemorySize();
    void *memory = malloc(size);
    if (!memory)
    {
        return Fail("could not allocate Clay arena for clipped elements");
    }

    Clay_Arena arena = Clay_CreateArenaWithCapacityAndMemory(size, memory);
    if (!Clay_Initialize(arena, (Clay_Dimensions){200, 200},
                         (Clay_ErrorHandler){CountClayInternalErrors, 0}))
    {
        DropClay(memory);
        return Fail("could not initialize Clay for clipped elements");
    }

    Clay_SetMaxElementCount(300);
    uint32_t resized_size = Clay_MinMemorySize();
    void *resized_memory = malloc(resized_size);
    if (!resized_memory)
    {
        DropClay(memory);
        return Fail("could not resize Clay arena for clipped elements");
    }
    Clay_Arena resized_arena = Clay_CreateArenaWithCapacityAndMemory(resized_size, resized_memory);
    if (!Clay_Initialize(resized_arena, (Clay_Dimensions){200, 200},
                         (Clay_ErrorHandler){CountClayInternalErrors, 0}))
    {
        free(resized_memory);
        DropClay(memory);
        return Fail("could not reinitialize Clay for clipped elements");
    }
    free(memory);
    memory = resized_memory;

    clay_internal_error_count = 0;
    LayoutClippedElements(CLAY_STRING("ClipA"), 200);
    Clay_UpdateScrollContainers(false, (Clay_Vector2){0}, 0.0f);
    LayoutClippedElements(CLAY_STRING("ClipB"), 200);

    int failed = 0;
    if (clay_internal_error_count != 0)
    {
        failed = Fail("clipped elements exhausted Clay's internal scroll container array");
    }
    else if (!Clay_GetScrollContainerData(Clay_GetElementIdWithIndex(CLAY_STRING("ClipB"), 199)).found)
    {
        failed = Fail("last clipped element was not registered as a scroll container");
    }

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
    if (rc != 0)
    {
        return rc;
    }
    rc = TestUnbalancedOverflowGrows();
    if (rc != 0)
    {
        return rc;
    }
    rc = TestManyClippedElements();
    return rc != 0 ? rc : TestScrollSurvivesReinit();
}
