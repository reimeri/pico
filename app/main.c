// Pico — a small C agent harness. Clay is only the layout library.

#define CLAY_IMPLEMENTATION
#include "clay/clay.h"
#include "../clay/renderers/raylib/clay_renderer_raylib.c"

#include "pico/app.h"
#include "richtext.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(int argc, char **argv)
{
    if (argc > 1)
    {
        fprintf(stderr, "usage: %s\n", argv[0]);
        return strcmp(argv[1], "--help") == 0 || strcmp(argv[1], "-h") == 0 ? 0 : 1;
    }

    uint64_t total_memory_size = Clay_MinMemorySize();
    Clay_Arena clay_memory = Clay_CreateArenaWithCapacityAndMemory(total_memory_size, malloc(total_memory_size));
    Clay_Initialize(clay_memory, (Clay_Dimensions){1100, 800}, (Clay_ErrorHandler){Pico_HandleClayErrors, 0});
    Clay_Raylib_Initialize(1100, 800, "Pico", FLAG_VSYNC_HINT | FLAG_WINDOW_RESIZABLE | FLAG_MSAA_4X_HINT);
    ChangeDirectory(GetApplicationDirectory());

    Font fonts[FONT_COUNT];
    Pico_LoadFonts(fonts);
    Clay_SetMeasureTextFunction(Pico_MeasureTextUtf8, fonts);
    RichText_SetMeasureFunction(Pico_MeasureTextUtf8, fonts);

    PicoApp app = {0};
    PicoApp_Init(&app, fonts);
    while (!WindowShouldClose())
    {
        if (Pico_NeedsClayReinit())
        {
            uint64_t size = Clay_MinMemorySize();
            Clay_Arena memory = Clay_CreateArenaWithCapacityAndMemory(size, malloc(size));
            Clay_Initialize(memory, (Clay_Dimensions){(float)GetScreenWidth(), (float)GetScreenHeight()},
                            (Clay_ErrorHandler){Pico_HandleClayErrors, 0});
            Pico_ClearClayReinit();
        }
        PicoApp_Frame(&app);
    }
    PicoApp_Free(&app);

    Pico_UnloadFonts(fonts);
    Clay_Raylib_Close();
    return 0;
}
