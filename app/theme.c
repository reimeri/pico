#include "theme_internal.h"

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(__linux__)
#include <execinfo.h>
#include <unistd.h>
#endif

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

static int RoundedFontPx(uint16_t design)
{
    float px = (float)design * g_font_scale;
    int n = px <= 0.0f ? 0 : (int)(px + 0.5f);
    if (n > UINT16_MAX)
    {
        n = UINT16_MAX;
    }
    return n;
}

float Pico_FontPx(uint16_t design)
{
    return (float)RoundedFontPx(design);
}

uint16_t Pico_FontPxU16(uint16_t design)
{
    return (uint16_t)RoundedFontPx(design);
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

#define PICO_CLAY_SCROLL_MAX 16

typedef struct PicoClayScrollSnap {
    uint32_t id;
    Clay_Vector2 pos;
    const char *name;
} PicoClayScrollSnap;

static PicoClayScrollSnap clay_scroll_snaps[PICO_CLAY_SCROLL_MAX];
static int clay_scroll_snap_count;
static bool clay_scroll_restore_pending;

static void ReportClayInternalError(Clay_String error_text)
{
    if (error_text.chars)
    {
        fprintf(stderr, "%.*s\n", error_text.length, error_text.chars);
    }
    else
    {
        fputs("Clay encountered an unspecified internal error.\n", stderr);
    }
#if defined(__linux__)
    void *frames[32];
    int frame_count = backtrace(frames, (int)(sizeof(frames) / sizeof(frames[0])));
    if (frame_count > 0)
    {
        fputs("Clay failure backtrace:\n", stderr);
        backtrace_symbols_fd(frames, frame_count, STDERR_FILENO);
    }
#endif
}

void Pico_HandleClayErrors(Clay_ErrorData error_data)
{
    if (error_data.errorType == CLAY_ERROR_TYPE_INTERNAL_ERROR)
    {
        ReportClayInternalError(error_data.errorText);
    }
    else
    {
        printf("%.*s\n", error_data.errorText.length, error_data.errorText.chars);
    }
    if (error_data.errorType == CLAY_ERROR_TYPE_ELEMENTS_CAPACITY_EXCEEDED ||
        error_data.errorType == CLAY_ERROR_TYPE_HASH_MAP_CAPACITY_EXCEEDED)
    {
        int32_t before = Clay_GetMaxElementCount();
        float chat_y = 0.0f;
        int has_chat = 0;
        for (int i = 0; i < clay_scroll_snap_count; i++)
        {
            if (clay_scroll_snaps[i].name && strcmp(clay_scroll_snaps[i].name, "ChatScroll") == 0)
            {
                chat_y = clay_scroll_snaps[i].pos.y;
                has_chat = 1;
                break;
            }
        }
        fprintf(stderr, "clay-scroll: error %s max=%d snaps=%d remembered_chat=%d y=%.1f\n",
                error_data.errorType == CLAY_ERROR_TYPE_HASH_MAP_CAPACITY_EXCEEDED ? "hashmap" : "elements",
                (int)before, clay_scroll_snap_count, has_chat, (double)chat_y);
        needs_clay_reinit = true;
        Clay_SetMaxElementCount(before * 2);
        fprintf(stderr, "clay-scroll: doubled max %d -> %d snaps=%d\n", (int)before,
                (int)Clay_GetMaxElementCount(), clay_scroll_snap_count);
    }
    else if (error_data.errorType == CLAY_ERROR_TYPE_TEXT_MEASUREMENT_CAPACITY_EXCEEDED)
    {
        fprintf(stderr, "clay-scroll: error text-cache max=%d snaps=%d\n", (int)Clay_GetMaxElementCount(),
                clay_scroll_snap_count);
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

static float ClampAxis(float value, float viewport, float content)
{
    float overflow = content - viewport;
    float min_v = overflow > 0.0f ? -overflow : 0.0f;
    if (value > 0.0f)
    {
        return 0.0f;
    }
    if (value < min_v)
    {
        return min_v;
    }
    return value;
}

static bool NearZero(float value)
{
    return value > -0.01f && value < 0.01f;
}

static void CaptureScrollId(const char *name, bool verbose)
{
    Clay_String label = {.length = (int32_t)strlen(name), .chars = name};
    Clay_ElementId id = Clay_GetElementId(label);
    Clay_ScrollContainerData data = Clay_GetScrollContainerData(id);
    if (!data.found || !data.scrollPosition)
    {
        if (verbose)
        {
            fprintf(stderr, "clay-scroll: snapshot miss %s found=%d pos=%p\n", name, data.found ? 1 : 0,
                    (void *)data.scrollPosition);
        }
        return;
    }
    if (data.scrollContainerDimensions.width <= 0.0f && data.scrollContainerDimensions.height <= 0.0f &&
        data.contentDimensions.width <= 0.0f && data.contentDimensions.height <= 0.0f)
    {
        if (verbose)
        {
            fprintf(stderr, "clay-scroll: snapshot skip unmeasured %s\n", name);
        }
        return;
    }
    if (clay_scroll_snap_count >= PICO_CLAY_SCROLL_MAX)
    {
        if (verbose)
        {
            fprintf(stderr, "clay-scroll: snapshot drop %s (full)\n", name);
        }
        return;
    }
    clay_scroll_snaps[clay_scroll_snap_count].id = id.id;
    clay_scroll_snaps[clay_scroll_snap_count].pos = *data.scrollPosition;
    clay_scroll_snaps[clay_scroll_snap_count].name = name;
    clay_scroll_snap_count++;
    if (verbose)
    {
        fprintf(stderr,
                "clay-scroll: snapshot %s id=%u y=%.1f x=%.1f view=%.1fx%.1f content=%.1fx%.1f\n", name,
                (unsigned)id.id, (double)data.scrollPosition->y, (double)data.scrollPosition->x,
                (double)data.scrollContainerDimensions.width, (double)data.scrollContainerDimensions.height,
                (double)data.contentDimensions.width, (double)data.contentDimensions.height);
    }
}

static void SnapshotScrollers(bool verbose, const char *reason)
{
    if (!Clay_GetCurrentContext())
    {
        if (verbose)
        {
            fprintf(stderr, "clay-scroll: %s skip (no clay context)\n", reason);
        }
        return;
    }
    clay_scroll_snap_count = 0;
    if (verbose)
    {
        fprintf(stderr, "clay-scroll: %s begin\n", reason);
    }
    CaptureScrollId("ChatScroll", verbose);
    CaptureScrollId("SubagentChatScroll", verbose);
    CaptureScrollId("ComposerScroll", verbose);
    CaptureScrollId("AskUserTextScroll", verbose);
    CaptureScrollId("PromptModalScroll", verbose);
    CaptureScrollId("TodoListScroll", verbose);
    CaptureScrollId("ExtModalScroll", verbose);
    CaptureScrollId("DiffScroll", verbose);
    CaptureScrollId("FooterMenuScroll", verbose);
    CaptureScrollId("AskModalScroll", verbose);
    if (verbose)
    {
        fprintf(stderr, "clay-scroll: %s done count=%d\n", reason, clay_scroll_snap_count);
    }
}

void Pico_RememberClayScroll(void)
{
    SnapshotScrollers(false, "remember");
}

void Pico_CaptureClayScroll(void)
{
    clay_scroll_restore_pending = true;
    if (clay_scroll_snap_count > 0)
    {
        fprintf(stderr, "clay-scroll: capture skip (already %d snaps) restore_pending=1\n",
                clay_scroll_snap_count);
        return;
    }
    SnapshotScrollers(true, "capture");
}

bool Pico_RestoreClayScroll(void)
{
    if (!clay_scroll_restore_pending)
    {
        return false;
    }
    if (clay_scroll_snap_count <= 0)
    {
        clay_scroll_restore_pending = false;
        return false;
    }
    if (needs_clay_reinit)
    {
        fprintf(stderr, "clay-scroll: restore defer (reinit pending) count=%d\n", clay_scroll_snap_count);
        return false;
    }
    fprintf(stderr, "clay-scroll: restore begin count=%d\n", clay_scroll_snap_count);
    int applied = 0;
    int skipped = 0;
    bool changed = false;
    for (int i = 0; i < clay_scroll_snap_count; i++)
    {
        const char *name = clay_scroll_snaps[i].name ? clay_scroll_snaps[i].name : "?";
        Clay_ScrollContainerData data =
            Clay_GetScrollContainerData((Clay_ElementId){.id = clay_scroll_snaps[i].id});
        if (!data.found || !data.scrollPosition)
        {
            fprintf(stderr, "clay-scroll: restore miss %s found=%d pos=%p saved_y=%.1f\n", name,
                    data.found ? 1 : 0, (void *)data.scrollPosition, (double)clay_scroll_snaps[i].pos.y);
            skipped++;
            continue;
        }
        if (data.contentDimensions.width <= 0.0f && data.contentDimensions.height <= 0.0f)
        {
            fprintf(stderr, "clay-scroll: restore unmeasured %s y=%.1f saved_y=%.1f\n", name,
                    (double)data.scrollPosition->y, (double)clay_scroll_snaps[i].pos.y);
            skipped++;
            continue;
        }
        Clay_Vector2 want = clay_scroll_snaps[i].pos;
        want.x = ClampAxis(want.x, data.scrollContainerDimensions.width, data.contentDimensions.width);
        want.y = ClampAxis(want.y, data.scrollContainerDimensions.height, data.contentDimensions.height);
        if (NearZero(want.y) && NearZero(want.x) &&
            (!NearZero(data.scrollPosition->x) || !NearZero(data.scrollPosition->y)))
        {
            fprintf(stderr, "clay-scroll: restore keep %s current_y=%.1f (saved was 0)\n", name,
                    (double)data.scrollPosition->y);
            skipped++;
            continue;
        }
        applied++;
        float before_y = data.scrollPosition->y;
        float dx = want.x - data.scrollPosition->x;
        float dy = want.y - data.scrollPosition->y;
        if (dx < 0.0f)
        {
            dx = -dx;
        }
        if (dy < 0.0f)
        {
            dy = -dy;
        }
        bool wrote = false;
        if (dx > 0.01f || dy > 0.01f)
        {
            *data.scrollPosition = want;
            changed = true;
            wrote = true;
        }
        fprintf(stderr,
                "clay-scroll: restore %s y %.1f -> %.1f (saved=%.1f clamped=%.1f) view_h=%.1f "
                "content_h=%.1f wrote=%d\n",
                name, (double)before_y, (double)data.scrollPosition->y, (double)clay_scroll_snaps[i].pos.y,
                (double)want.y, (double)data.scrollContainerDimensions.height,
                (double)data.contentDimensions.height, wrote ? 1 : 0);
    }
    if (applied > 0)
    {
        fprintf(stderr, "clay-scroll: restore consumed snaps applied=%d skipped=%d changed=%d\n", applied,
                skipped, changed ? 1 : 0);
        clay_scroll_snap_count = 0;
        clay_scroll_restore_pending = false;
    }
    else
    {
        fprintf(stderr, "clay-scroll: restore keep snaps applied=0 skipped=%d\n", skipped);
    }
    return changed;
}
