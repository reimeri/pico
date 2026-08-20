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
#define COLOR_USER_BG (Clay_Color){30, 30, 36, 255}
#define COLOR_ASSISTANT_BG (Clay_Color){0, 0, 0, 0}
#define COLOR_COMPOSER_BG (Clay_Color){34, 34, 42, 255}
#define COLOR_FOOTER_BG (Clay_Color){22, 22, 26, 255}
#define COLOR_TEXT (Clay_Color){222, 222, 228, 255}
#define COLOR_MUTED (Clay_Color){140, 140, 150, 255}
#define COLOR_TOOL_NAME (Clay_Color){188, 190, 202, 255}
#define COLOR_TOOL_ARGS (Clay_Color){118, 118, 128, 255}
#define COLOR_TOOL_NAME_HOVER (Clay_Color){232, 234, 242, 255}
#define COLOR_TOOL_ARGS_HOVER (Clay_Color){198, 200, 210, 255}
#define COLOR_TOOL_CHEVRON (Clay_Color){176, 178, 190, 255}
#define COLOR_LINK (Clay_Color){120, 160, 255, 255}
#define COLOR_LINK_HOVER (Clay_Color){170, 200, 255, 255}
#define COLOR_CODE_BG (Clay_Color){42, 42, 52, 255}
#define COLOR_CODE_TEXT (Clay_Color){230, 200, 140, 255}
#define COLOR_QUOTE_BG (Clay_Color){36, 38, 48, 255}
#define COLOR_QUOTE_BORDER (Clay_Color){120, 160, 255, 255}
#define COLOR_HR (Clay_Color){64, 64, 74, 255}
#define COLOR_TABLE_BORDER (Clay_Color){64, 64, 74, 255}
#define COLOR_TABLE_HEADER (Clay_Color){38, 38, 46, 255}
#define COLOR_SCROLLBAR (Clay_Color){120, 120, 160, 150}
#define COLOR_SCROLLBAR_HOVER (Clay_Color){100, 100, 140, 150}
#define COLOR_ERROR_BG (Clay_Color){70, 40, 40, 255}
#define COLOR_CURSOR (Clay_Color){200, 210, 255, 255}
#define COLOR_SELECTION (Clay_Color){90, 130, 210, 110}

#define CONTENT_PADDING 20
#define SCROLLBAR_WIDTH 6
#define SCROLLBAR_GAP 8
#define BLOCK_SPACING 14

Clay_Dimensions Pico_MeasureTextUtf8(Clay_StringSlice text, Clay_TextElementConfig *config, void *userData);
void Pico_LoadFonts(Font *fonts);
void Pico_UnloadFonts(Font *fonts);
void Pico_HandleClayErrors(Clay_ErrorData error_data);
bool Pico_NeedsClayReinit(void);
void Pico_ClearClayReinit(void);

#endif
