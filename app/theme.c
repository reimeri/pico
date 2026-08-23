#include "theme_internal.h"

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define PICO_FONT_SIZE_MIN 8
#define PICO_FONT_SIZE_MAX 128
#define PICO_FONT_SIZE_SLOTS (PICO_FONT_SIZE_MAX - PICO_FONT_SIZE_MIN + 1)
#define PICO_FONT_SCALE_MIN 0.5f
#define PICO_FONT_SCALE_MAX 3.0f
#define PICO_FONT_SCALE_DEFAULT 1.0f

static const char *kFontPaths[FONT_COUNT] = {
    "resources/Roboto-Regular.ttf",
    "resources/Roboto-Bold.ttf",
    "resources/Roboto-Italic.ttf",
    "resources/Roboto-BoldItalic.ttf",
    "resources/RobotoMono-Medium.ttf",
};

static int g_codepoints[256];
static int g_codepoint_count;
static Font g_fonts[FONT_COUNT][PICO_FONT_SIZE_SLOTS];
static bool g_font_ready[FONT_COUNT][PICO_FONT_SIZE_SLOTS];
static bool g_font_owned[FONT_COUNT][PICO_FONT_SIZE_SLOTS];
static float g_font_scale = PICO_FONT_SCALE_DEFAULT;

static void EnsureCodepoints(void)
{
    if (g_codepoint_count > 0)
    {
        return;
    }
    for (int c = 32; c < 127; c++)
    {
        g_codepoints[g_codepoint_count++] = c;
    }
    for (int c = 160; c < 256; c++)
    {
        g_codepoints[g_codepoint_count++] = c;
    }
    int extra[] = {0x2013, 0x2014, 0x2018, 0x2019, 0x201C, 0x201D, 0x2022, 0x2026, 0x203A, 0x25BE, 0x2603, 0x2610,
                   0x2611};
    for (size_t i = 0; i < sizeof(extra) / sizeof(extra[0]); i++)
    {
        g_codepoints[g_codepoint_count++] = extra[i];
    }
}

static int SizeIndex(uint16_t fontSize)
{
    int size = (int)fontSize;
    if (size < PICO_FONT_SIZE_MIN)
    {
        size = PICO_FONT_SIZE_MIN;
    }
    if (size > PICO_FONT_SIZE_MAX)
    {
        size = PICO_FONT_SIZE_MAX;
    }
    return size - PICO_FONT_SIZE_MIN;
}

void Pico_SetFontScale(float scale)
{
    if (!(scale >= PICO_FONT_SCALE_MIN && scale <= PICO_FONT_SCALE_MAX))
    {
        return;
    }
    g_font_scale = scale;
}

float Pico_FontScale(void)
{
    return g_font_scale;
}

float Pico_FontPx(uint16_t design)
{
    return (float)design * g_font_scale;
}

uint16_t Pico_FontPxU16(uint16_t design)
{
    float px = Pico_FontPx(design);
    int n = px <= 0.0f ? 0 : (int)(px + 0.5f);
    if (n > UINT16_MAX)
    {
        n = UINT16_MAX;
    }
    return (uint16_t)n;
}

Font Pico_FontAt(uint16_t fontId, uint16_t fontSize)
{
    if (fontId >= FONT_COUNT)
    {
        fontId = FONT_REGULAR;
    }
    int idx = SizeIndex(Pico_FontPxU16(fontSize));
    if (g_font_ready[fontId][idx])
    {
        return g_fonts[fontId][idx];
    }

    EnsureCodepoints();
    int pixel_size = idx + PICO_FONT_SIZE_MIN;
    Font font = LoadFontEx(kFontPaths[fontId], pixel_size, g_codepoints, g_codepoint_count);
    Font fallback = GetFontDefault();
    bool owned = font.texture.id != 0 && font.texture.id != fallback.texture.id;
    if (!owned)
    {
        font = fallback;
    }
    else
    {
        SetTextureFilter(font.texture, TEXTURE_FILTER_POINT);
    }
    g_fonts[fontId][idx] = font;
    g_font_ready[fontId][idx] = true;
    g_font_owned[fontId][idx] = owned;
    return font;
}

Clay_Dimensions Pico_MeasureTextUtf8(Clay_StringSlice text, Clay_TextElementConfig *config, void *userData)
{
    static char *buffer = NULL;
    static size_t buffer_capacity = 0;
    (void)userData;

    Font font = Pico_FontAt(config->fontId, config->fontSize);
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

    Vector2 size = MeasureTextEx(font, buffer, Pico_FontPx(config->fontSize), Pico_FontPx(config->letterSpacing));
    return (Clay_Dimensions){.width = size.x, .height = size.y};
}

void Pico_LoadFonts(Font *fonts)
{
    EnsureCodepoints();
    if (!fonts)
    {
        return;
    }
    for (int i = 0; i < FONT_COUNT; i++)
    {
        fonts[i] = Pico_FontAt((uint16_t)i, 16);
    }
}

void Pico_UnloadFonts(Font *fonts)
{
    for (int face = 0; face < FONT_COUNT; face++)
    {
        for (int i = 0; i < PICO_FONT_SIZE_SLOTS; i++)
        {
            if (g_font_owned[face][i])
            {
                UnloadFont(g_fonts[face][i]);
            }
            g_fonts[face][i] = (Font){0};
            g_font_ready[face][i] = false;
            g_font_owned[face][i] = false;
        }
    }
    if (fonts)
    {
        memset(fonts, 0, sizeof(Font) * FONT_COUNT);
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
