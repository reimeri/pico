#ifndef PICO_THEME_H
#define PICO_THEME_H

#include "clay/clay.h"
#include "raylib.h"

enum {
    FONT_REGULAR = 0,
    FONT_BOLD,
    FONT_ITALIC,
    FONT_BOLD_ITALIC,
    FONT_MONO,
    FONT_COUNT,
};

#define COLOR_BG (Clay_Color){24, 24, 28, 255}
#define COLOR_CONTENT_BG (Clay_Color){30, 30, 36, 255}
#define COLOR_USER_BG (Clay_Color){38, 42, 58, 255}
#define COLOR_ASSISTANT_BG (Clay_Color){30, 30, 36, 255}
#define COLOR_COMPOSER_BG (Clay_Color){34, 34, 42, 255}
#define COLOR_FOOTER_BG (Clay_Color){22, 22, 26, 255}
#define COLOR_TEXT (Clay_Color){222, 222, 228, 255}
#define COLOR_MUTED (Clay_Color){140, 140, 150, 255}
#define COLOR_LINK (Clay_Color){120, 160, 255, 255}
#define COLOR_LINK_HOVER (Clay_Color){170, 200, 255, 255}
#define COLOR_CODE_BG (Clay_Color){42, 42, 52, 255}
#define COLOR_CODE_TEXT (Clay_Color){230, 200, 140, 255}
#define COLOR_QUOTE_BG (Clay_Color){36, 38, 48, 255}
#define COLOR_QUOTE_BORDER (Clay_Color){120, 160, 255, 255}
#define COLOR_HR (Clay_Color){64, 64, 74, 255}
#define COLOR_SCROLLBAR (Clay_Color){120, 120, 160, 150}
#define COLOR_SCROLLBAR_HOVER (Clay_Color){100, 100, 140, 150}
#define COLOR_ERROR_BG (Clay_Color){70, 40, 40, 255}
#define COLOR_CURSOR (Clay_Color){200, 210, 255, 255}

#define CONTENT_PADDING 24
#define SCROLLBAR_WIDTH 14
#define BLOCK_SPACING 14

Clay_Dimensions Pico_MeasureTextUtf8(Clay_StringSlice text, Clay_TextElementConfig *config, void *userData);
void Pico_LoadFonts(Font *fonts);
void Pico_UnloadFonts(Font *fonts);
void Pico_HandleClayErrors(Clay_ErrorData error_data);
bool Pico_NeedsClayReinit(void);
void Pico_ClearClayReinit(void);

#endif
