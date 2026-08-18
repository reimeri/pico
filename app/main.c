// Pico — a small C agent harness. Clay is only the layout library.

#define CLAY_IMPLEMENTATION
#include "clay/clay.h"
#include "../clay/renderers/raylib/clay_renderer_raylib.c"

#include "pico/app.h"
#include "richtext.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static void PrintUsage(const char *argv0)
{
    fprintf(stderr,
            "usage: %s [--safe]\n"
            "  --safe   load builtin UI only (skip ~/.config/pico/extensions and .pico/extensions)\n"
            "  -h       this help\n",
            argv0);
}

int main(int argc, char **argv)
{
    bool safe_mode = false;
    for (int i = 1; i < argc; i++)
    {
        if (strcmp(argv[i], "--safe") == 0)
        {
            safe_mode = true;
        }
        else if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0)
        {
            PrintUsage(argv[0]);
            return 0;
        }
        else
        {
            PrintUsage(argv[0]);
            return 1;
        }
    }

    uint64_t total_memory_size = Clay_MinMemorySize();
    Clay_Arena clay_memory = Clay_CreateArenaWithCapacityAndMemory(total_memory_size, malloc(total_memory_size));
    Clay_Initialize(clay_memory, (Clay_Dimensions){1100, 800}, (Clay_ErrorHandler){Pico_HandleClayErrors, 0});
    Clay_Raylib_Initialize(1100, 800, "Pico", FLAG_VSYNC_HINT | FLAG_WINDOW_RESIZABLE | FLAG_MSAA_4X_HINT);

    char workspace[4096];
    if (!getcwd(workspace, sizeof(workspace)))
    {
        snprintf(workspace, sizeof(workspace), ".");
    }
    ChangeDirectory(GetApplicationDirectory());

    Font fonts[FONT_COUNT];
    Pico_LoadFonts(fonts);
    Clay_SetMeasureTextFunction(Pico_MeasureTextUtf8, fonts);
    RichText_SetMeasureFunction(Pico_MeasureTextUtf8, fonts);

    PicoApp app = {0};
    PicoApp_Init(&app, fonts, workspace, safe_mode);
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
