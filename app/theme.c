#include "pico/theme.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

Clay_Dimensions Pico_MeasureTextUtf8(Clay_StringSlice text, Clay_TextElementConfig *config, void *userData)
{
    static char *buffer = NULL;
    static size_t buffer_capacity = 0;

    Font *fonts = (Font *)userData;
    Font font = fonts[config->fontId];
    if (!font.glyphs)
    {
        font = GetFontDefault();
    }

    if ((size_t)text.length + 1 > buffer_capacity)
    {
        size_t new_capacity = buffer_capacity == 0 ? 256 : buffer_capacity;
        while (new_capacity < (size_t)text.length + 1)
        {
            new_capacity *= 2;
        }
        free(buffer);
        buffer = (char *)malloc(new_capacity);
        buffer_capacity = new_capacity;
    }
    memcpy(buffer, text.chars, (size_t)text.length);
    buffer[text.length] = '\0';

    Vector2 size = MeasureTextEx(font, buffer, (float)config->fontSize, (float)config->letterSpacing);
    return (Clay_Dimensions){.width = size.x, .height = size.y};
}

static void LoadFontWithGlyphs(const char *path, Font *out)
{
    static int codepoints[256];
    static int count = 0;
    if (count == 0)
    {
        for (int c = 32; c < 127; c++)
        {
            codepoints[count++] = c;
        }
        for (int c = 160; c < 256; c++)
        {
            codepoints[count++] = c;
        }
        int extra[] = {0x2013, 0x2014, 0x2018, 0x2019, 0x201C, 0x201D, 0x2022, 0x2026, 0x203A, 0x25BE, 0x2603,
                       0x2610, 0x2611};
        for (size_t i = 0; i < sizeof(extra) / sizeof(extra[0]); i++)
        {
            codepoints[count++] = extra[i];
        }
    }
    *out = LoadFontEx(path, 48, codepoints, count);
    SetTextureFilter(out->texture, TEXTURE_FILTER_BILINEAR);
}

void Pico_LoadFonts(Font *fonts)
{
    LoadFontWithGlyphs("resources/Roboto-Regular.ttf", &fonts[FONT_REGULAR]);
    LoadFontWithGlyphs("resources/Roboto-Bold.ttf", &fonts[FONT_BOLD]);
    LoadFontWithGlyphs("resources/Roboto-Italic.ttf", &fonts[FONT_ITALIC]);
    LoadFontWithGlyphs("resources/Roboto-BoldItalic.ttf", &fonts[FONT_BOLD_ITALIC]);
    LoadFontWithGlyphs("resources/RobotoMono-Medium.ttf", &fonts[FONT_MONO]);
}

void Pico_UnloadFonts(Font *fonts)
{
    for (int i = 0; i < FONT_COUNT; i++)
    {
        UnloadFont(fonts[i]);
    }
}

static bool needs_clay_reinit = false;

void Pico_HandleClayErrors(Clay_ErrorData error_data)
{
    printf("%s\n", error_data.errorText.chars);
    if (error_data.errorType == CLAY_ERROR_TYPE_ELEMENTS_CAPACITY_EXCEEDED)
    {
        needs_clay_reinit = true;
        Clay_SetMaxElementCount(Clay_GetMaxElementCount() * 2);
    }
    else if (error_data.errorType == CLAY_ERROR_TYPE_TEXT_MEASUREMENT_CAPACITY_EXCEEDED)
    {
        needs_clay_reinit = true;
        Clay_SetMaxMeasureTextCacheWordCount(Clay_GetMaxMeasureTextCacheWordCount() * 2);
    }
}

bool Pico_NeedsClayReinit(void)
{
    return needs_clay_reinit;
}

void Pico_ClearClayReinit(void)
{
    needs_clay_reinit = false;
}
