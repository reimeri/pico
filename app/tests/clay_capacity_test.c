#include "theme_internal.h"

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

#ifdef PICO_CLAY_FAULT_TESTS
static bool fail_allocation;
static bool fail_initialization;

void *__real_malloc(size_t size);
void *__wrap_malloc(size_t size)
{
    if (fail_allocation)
    {
        fail_allocation = false;
        return NULL;
    }
    return __real_malloc(size);
}

Clay_Context *__real_Clay_Initialize(Clay_Arena arena, Clay_Dimensions dimensions,
                                    Clay_ErrorHandler handler);
Clay_Context *__wrap_Clay_Initialize(Clay_Arena arena, Clay_Dimensions dimensions,
                                    Clay_ErrorHandler handler)
{
    Clay_Context *context = __real_Clay_Initialize(arena, dimensions, handler);
    if (fail_initialization)
    {
        fail_initialization = false;
        /* Exercise rollback even if initialization already changed current context. */
        return NULL;
    }
    return context;
}
#endif

static int TestOverflowGrows(Clay_ErrorType error_type)
{
    if (!Pico_InitClay((Clay_Dimensions){100, 100}))
    {
        return Fail("could not initialize Clay");
    }

    Pico_ClearClayReinit();
    int32_t before = Clay_GetMaxElementCount();
    Pico_HandleClayErrors((Clay_ErrorData){
        .errorType = error_type,
        .errorText = CLAY_STRING("capacity overflow"),
    });

    int failed = 0;
    if (!Pico_NeedsClayReinit())
    {
        failed = Fail("capacity overflow did not request Clay reinit");
    }
    else if (Clay_GetMaxElementCount() <= before)
    {
        failed = Fail("capacity overflow did not increase max element count");
    }

    Pico_ClearClayReinit();
    Pico_FreeClay();
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

static Clay_Dimensions MeasureCapacityText(Clay_StringSlice text, Clay_TextElementConfig *config,
                                           void *user_data)
{
    (void)user_data;
    return (Clay_Dimensions){text.length * config->fontSize * 0.5f, config->fontSize};
}

static int TestLayoutRecoversAfterOverflow(void)
{
    if (!Pico_InitClay((Clay_Dimensions){200, 200}))
    {
        return Fail("could not initialize small arena for overflow recovery");
    }
    Clay_SetMaxElementCount(128);
    if (!Pico_ReinitClay(NULL, false))
    {
        Pico_FreeClay();
        return Fail("could not prepare small arena for overflow recovery");
    }
    int recoveries = 0;
    for (int attempt = 0; attempt < 4; attempt++)
    {
        Clay_SetMeasureTextFunction(MeasureCapacityText, NULL);
        LayoutClippedElements(CLAY_STRING("OverflowItem"), 200);
        if (!Pico_NeedsClayReinit()) break;
        if (!Pico_ReinitClay(NULL, false)) break;
        recoveries++;
    }
    bool recovered = recoveries > 0 && !Pico_NeedsClayReinit() &&
        Clay_GetScrollContainerData(Clay_GetElementIdWithIndex(CLAY_STRING("OverflowItem"), 199)).found;
    Pico_FreeClay();
    return recovered ? 0 : Fail("capacity recovery did not rebuild a complete usable layout");
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
    if (!Pico_InitClay((Clay_Dimensions){200, 200}))
    {
        return Fail("could not initialize Clay for scroll restore");
    }
    /* Repeated production replacements must release every superseded arena.
     * LeakSanitizer checks this once final teardown removes the current root. */
    const Clay_ErrorType overflows[] = {
        CLAY_ERROR_TYPE_ELEMENTS_CAPACITY_EXCEEDED,
        CLAY_ERROR_TYPE_TEXT_MEASUREMENT_CAPACITY_EXCEEDED,
        CLAY_ERROR_TYPE_HASH_MAP_CAPACITY_EXCEEDED,
    };
    int failed = 0;
    for (size_t i = 0; i < sizeof(overflows) / sizeof(overflows[0]); i++)
    {
        LayoutChat(400.0f);
        Clay_ScrollContainerData data = Clay_GetScrollContainerData(CLAY_ID("ChatScroll"));
        if (!data.found || !data.scrollPosition)
        {
            failed = Fail("ChatScroll was not a scroll container");
            break;
        }
        data.scrollPosition->y = -150.0f;
        Pico_RememberClayScroll();
        Pico_HandleClayErrors((Clay_ErrorData){.errorType = overflows[i],
                                              .errorText = CLAY_STRING("capacity overflow")});
        if (!Pico_ReinitClay(NULL, false))
        {
            failed = Fail("could not replace Clay arena");
            break;
        }
        LayoutChat(400.0f);
        Pico_RestoreClayScroll();
        /* Never reuse the previous arena's scrollPosition pointer. */
        data = Clay_GetScrollContainerData(CLAY_ID("ChatScroll"));
        if (!data.found || !data.scrollPosition || data.scrollPosition->y != -150.0f)
        {
            failed = Fail("replacement did not preserve the chat scroll position");
            break;
        }
    }
    Pico_FreeClay();
    if (Clay_GetCurrentContext())
    {
        return Fail("final teardown left a dangling Clay context");
    }
    return failed;
}

#ifdef PICO_CLAY_FAULT_TESTS
static int TestArenaFailure(bool allocation)
{
    if (allocation) fail_allocation = true;
    else fail_initialization = true;
    if (Pico_InitClay((Clay_Dimensions){200, 200}) || Clay_GetCurrentContext())
    {
        Pico_FreeClay();
        return Fail("failed initial arena must leave no current context");
    }
    if (!Pico_InitClay((Clay_Dimensions){200, 200}))
    {
        return Fail("could not initialize Clay after a failed attempt");
    }
    LayoutChat(400.0f);
    Clay_ScrollContainerData data = Clay_GetScrollContainerData(CLAY_ID("ChatScroll"));
    data.scrollPosition->y = -150.0f;
    Pico_RememberClayScroll();
    Pico_HandleClayErrors((Clay_ErrorData){.errorType = CLAY_ERROR_TYPE_ELEMENTS_CAPACITY_EXCEEDED,
                                          .errorText = CLAY_STRING("capacity overflow")});
    if (allocation) fail_allocation = true;
    else fail_initialization = true;
    if (Pico_ReinitClay(NULL, false) || !Pico_NeedsClayReinit())
    {
        Pico_FreeClay();
        return Fail("failed replacement must leave recovery pending");
    }
    /* The previous layout and its borrowed pointers remain valid on failure. */
    Clay_ScrollContainerData retained = Clay_GetScrollContainerData(CLAY_ID("ChatScroll"));
    if (!retained.found || !retained.scrollPosition ||
        retained.scrollPosition->y != -150.0f || data.scrollPosition->y != -150.0f)
    {
        Pico_FreeClay();
        return Fail("failed replacement lost the previous layout");
    }
    if (!Pico_ReinitClay(NULL, false))
    {
        Pico_FreeClay();
        return Fail("replacement did not recover after the allocation/initialization failure");
    }
    LayoutChat(400.0f);
    Pico_RestoreClayScroll();
    data = Clay_GetScrollContainerData(CLAY_ID("ChatScroll"));
    int failed = (!data.found || !data.scrollPosition || data.scrollPosition->y != -150.0f)
                     ? Fail("retry lost the captured scroll position") : 0;
    Pico_FreeClay();
    return failed;
}
#endif

int main(void)
{
    int rc = TestOverflowGrows(CLAY_ERROR_TYPE_HASH_MAP_CAPACITY_EXCEEDED);
    if (rc != 0) return rc;
    rc = TestOverflowGrows(CLAY_ERROR_TYPE_UNBALANCED_OPEN_CLOSE);
    if (rc != 0) return rc;
    rc = TestLayoutRecoversAfterOverflow();
    if (rc != 0) return rc;
    rc = TestManyClippedElements();
    if (rc != 0) return rc;
    rc = TestScrollSurvivesReinit();
    if (rc != 0) return rc;
#ifdef PICO_CLAY_FAULT_TESTS
    rc = TestArenaFailure(true);
    if (rc != 0) return rc;
    rc = TestArenaFailure(false);
#endif
    return rc;
}
