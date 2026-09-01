#include "theme_internal.h"
#include "docs_path.h"

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

static int *g_codepoints = NULL;
static int g_codepoint_count = 0;
static int g_codepoint_capacity = 0;
static Font g_fonts[FONT_COUNT][PICO_FONT_SIZE_SLOTS];
static bool g_font_ready[FONT_COUNT][PICO_FONT_SIZE_SLOTS];
static bool g_font_owned[FONT_COUNT][PICO_FONT_SIZE_SLOTS];
static float g_font_scale = PICO_FONT_SCALE_DEFAULT;

static bool AddCodepointRange(int start, int end)
{
    for (int c = start; c <= end; c++)
    {
        if (g_codepoint_count >= g_codepoint_capacity)
        {
            int new_cap = g_codepoint_capacity == 0 ? 1024 : g_codepoint_capacity * 2;
            int *new_pts = (int *)realloc(g_codepoints, (size_t)new_cap * sizeof(int));
            if (!new_pts)
            {
                free(g_codepoints);
                g_codepoints = NULL;
                g_codepoint_count = 0;
                g_codepoint_capacity = 0;
                return false;
            }
            g_codepoints = new_pts;
            g_codepoint_capacity = new_cap;
        }
        g_codepoints[g_codepoint_count++] = c;
    }
    return true;
}

static bool AddCodepoint(int c)
{
    return AddCodepointRange(c, c);
}

static void EnsureCodepoints(void)
{
    if (g_codepoint_count > 0)
    {
        return;
    }
    // Basic Latin & Latin-1 Supplement
    if (!AddCodepointRange(0x0020, 0x007E) ||
        !AddCodepointRange(0x00A0, 0x00FF) ||
        !AddCodepointRange(0x0100, 0x024F) ||
        !AddCodepointRange(0x0250, 0x02AF) ||
        !AddCodepointRange(0x02B0, 0x02FF) ||
        !AddCodepointRange(0x0300, 0x036F) ||
        !AddCodepointRange(0x0370, 0x03FF) ||
        !AddCodepointRange(0x0400, 0x052F) ||
        !AddCodepointRange(0x1E00, 0x1EFF) ||
        !AddCodepointRange(0x2000, 0x206F) ||
        !AddCodepointRange(0x2070, 0x209F) ||
        !AddCodepointRange(0x20A0, 0x20CF) ||
        !AddCodepointRange(0x2100, 0x214F) ||
        !AddCodepointRange(0x2150, 0x218F) ||
        !AddCodepointRange(0x2190, 0x21FF) ||
        !AddCodepointRange(0x2200, 0x22FF) ||
        !AddCodepointRange(0x2300, 0x23FF) ||
        !AddCodepointRange(0x2460, 0x24FF) ||
        !AddCodepointRange(0x2500, 0x257F) ||
        !AddCodepointRange(0x2580, 0x259F) ||
        !AddCodepointRange(0x25A0, 0x25FF) ||
        !AddCodepointRange(0x2600, 0x26FF) ||
        !AddCodepointRange(0x2700, 0x27BF) ||
        !AddCodepointRange(0x27C0, 0x27EF) ||
        !AddCodepointRange(0x27F0, 0x27FF) ||
        !AddCodepointRange(0x2900, 0x297F) ||
        !AddCodepointRange(0x2A00, 0x2AFF) ||
        !AddCodepointRange(0x2B00, 0x2BFF) ||
        !AddCodepointRange(0xFB00, 0xFB06) ||
        !AddCodepoint(0xFFFD))
    {
        return;
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

static void FontPath(uint16_t fontId, char *out, size_t cap)
{
    const char *relative = kFontPaths[fontId];
    if (!Pico_DataPath(relative, out, cap))
    {
        snprintf(out, cap, "%s", relative);
    }
}

static void FallbackFontPath(char *out, size_t cap)
{
    const char *relative = "resources/DejaVuSans.ttf";
    if (!Pico_DataPath(relative, out, cap))
    {
        snprintf(out, cap, "%s", relative);
    }
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

static Font LoadFontWithFallback(const char *primaryPath, const char *fallbackPath, int pixelSize, int *codepoints,
                                 int codepointCount)
{
    int primaryDataSize = 0;
    unsigned char *primaryData = LoadFileData(primaryPath, &primaryDataSize);
    int fallbackDataSize = 0;
    unsigned char *fallbackData =
        (fallbackPath && fallbackPath[0]) ? LoadFileData(fallbackPath, &fallbackDataSize) : NULL;

    if (!primaryData && !fallbackData)
    {
        return GetFontDefault();
    }

    SetTraceLogLevel(LOG_ERROR);
    GlyphInfo *p_glyphs = primaryData ? LoadFontData(primaryData, primaryDataSize, pixelSize, codepoints,
                                                     codepointCount, FONT_DEFAULT)
                                      : NULL;
    GlyphInfo *f_glyphs = fallbackData ? LoadFontData(fallbackData, fallbackDataSize, pixelSize, codepoints,
                                                     codepointCount, FONT_DEFAULT)
                                      : NULL;
    SetTraceLogLevel(LOG_INFO);

    if (primaryData)
    {
        UnloadFileData(primaryData);
    }
    if (fallbackData)
    {
        UnloadFileData(fallbackData);
    }

    if (!p_glyphs && !f_glyphs)
    {
        return GetFontDefault();
    }

    int valid_count = 0;
    for (int i = 0; i < codepointCount; i++)
    {
        bool has_p = (p_glyphs && p_glyphs[i].image.data != NULL);
        bool has_f = (f_glyphs && f_glyphs[i].image.data != NULL);
        bool keep_blank = codepoints[i] == 0x00A0 && (p_glyphs || f_glyphs);
        if (has_p || has_f || keep_blank)
        {
            valid_count++;
        }
    }

    if (valid_count == 0)
    {
        if (p_glyphs)
        {
            UnloadFontData(p_glyphs, codepointCount);
        }
        if (f_glyphs)
        {
            UnloadFontData(f_glyphs, codepointCount);
        }
        return GetFontDefault();
    }

    GlyphInfo *glyphs = (GlyphInfo *)RL_CALLOC((size_t)valid_count, sizeof(GlyphInfo));
    int dst = 0;
    for (int i = 0; i < codepointCount; i++)
    {
        bool has_p = (p_glyphs && p_glyphs[i].image.data != NULL);
        bool has_f = (f_glyphs && f_glyphs[i].image.data != NULL);
        if (has_p)
        {
            glyphs[dst++] = p_glyphs[i];
            p_glyphs[i].image.data = NULL;
        }
        else if (has_f)
        {
            glyphs[dst++] = f_glyphs[i];
            f_glyphs[i].image.data = NULL;
        }
        else if (codepoints[i] == 0x00A0 && p_glyphs)
        {
            glyphs[dst++] = p_glyphs[i];
        }
        else if (codepoints[i] == 0x00A0 && f_glyphs)
        {
            glyphs[dst++] = f_glyphs[i];
        }
    }

    if (p_glyphs)
    {
        for (int i = 0; i < codepointCount; i++)
        {
            if (p_glyphs[i].image.data != NULL)
            {
                UnloadImage(p_glyphs[i].image);
            }
        }
        RL_FREE(p_glyphs);
    }
    if (f_glyphs)
    {
        for (int i = 0; i < codepointCount; i++)
        {
            if (f_glyphs[i].image.data != NULL)
            {
                UnloadImage(f_glyphs[i].image);
            }
        }
        RL_FREE(f_glyphs);
    }

    int padding = 4;
    Rectangle *recs = NULL;
    Image atlas = GenImageFontAtlas(glyphs, &recs, valid_count, pixelSize, padding, 0);
    Texture2D texture = (Texture2D){0};
    if (IsWindowReady())
    {
        texture = LoadTextureFromImage(atlas);
    }

    for (int i = 0; i < valid_count; i++)
    {
        if (glyphs[i].image.data != NULL)
        {
            UnloadImage(glyphs[i].image);
            glyphs[i].image.data = NULL;
        }
        if (recs && atlas.data != NULL && recs[i].width > 0 && recs[i].height > 0)
        {
            glyphs[i].image = ImageFromImage(atlas, recs[i]);
        }
    }
    UnloadImage(atlas);

    Font font = {
        .baseSize = pixelSize,
        .glyphCount = valid_count,
        .glyphPadding = padding,
        .texture = texture,
        .recs = recs,
        .glyphs = glyphs,
    };
    return font;
}

static void Pico_UnloadFont(Font font)
{
    Font fallback = GetFontDefault();
    if (font.glyphs != NULL && font.glyphs != fallback.glyphs)
    {
        UnloadFontData(font.glyphs, font.glyphCount);
        if (font.recs != NULL)
        {
            RL_FREE(font.recs);
        }
        if (font.texture.id != 0 && font.texture.id != fallback.texture.id && IsWindowReady())
        {
            UnloadTexture(font.texture);
        }
    }
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
    char path[4096];
    char fallback_path[4096];
    FontPath(fontId, path, sizeof(path));
    FallbackFontPath(fallback_path, sizeof(fallback_path));
    Font font = LoadFontWithFallback(path, fallback_path, pixel_size, g_codepoints, g_codepoint_count);
    Font fallback = GetFontDefault();
    bool owned = (font.glyphs != NULL && font.glyphs != fallback.glyphs);
    if (!owned)
    {
        font = fallback;
    }
    else if (font.texture.id != 0 && font.texture.id != fallback.texture.id)
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
        fonts[i] = Pico_FontAt((uint16_t)i, PICO_FONT_UI);
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
                Pico_UnloadFont(g_fonts[face][i]);
            }
            g_fonts[face][i] = (Font){0};
            g_font_ready[face][i] = false;
            g_font_owned[face][i] = false;
        }
    }
    free(g_codepoints);
    g_codepoints = NULL;
    g_codepoint_count = 0;
    g_codepoint_capacity = 0;
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

static bool clay_capacity_grown;

static void RequestCapacityReinit(const char *reason)
{
    needs_clay_reinit = true;
    if (clay_capacity_grown)
    {
        fprintf(stderr, "clay-scroll: %s reinit (cap already doubled this overflow)\n", reason);
        return;
    }
    int32_t before = Clay_GetMaxElementCount();
    Clay_SetMaxElementCount(before * 2);
    clay_capacity_grown = true;
    fprintf(stderr, "clay-scroll: doubled max %d -> %d (%s) snaps=%d\n", (int)before,
            (int)Clay_GetMaxElementCount(), reason, clay_scroll_snap_count);
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
        error_data.errorType == CLAY_ERROR_TYPE_HASH_MAP_CAPACITY_EXCEEDED ||
        error_data.errorType == CLAY_ERROR_TYPE_UNBALANCED_OPEN_CLOSE)
    {
        const char *reason = "elements";
        if (error_data.errorType == CLAY_ERROR_TYPE_HASH_MAP_CAPACITY_EXCEEDED)
        {
            reason = "hashmap";
        }
        else if (error_data.errorType == CLAY_ERROR_TYPE_UNBALANCED_OPEN_CLOSE)
        {
            reason = "unbalanced";
        }
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
        fprintf(stderr, "clay-scroll: error %s max=%d snaps=%d remembered_chat=%d y=%.1f\n", reason,
                (int)Clay_GetMaxElementCount(), clay_scroll_snap_count, has_chat, (double)chat_y);
        RequestCapacityReinit(reason);
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
    clay_capacity_grown = false;
}

void Pico_ReinitClay(Font *fonts, bool debug_enabled)
{
    fprintf(stderr, "clay-scroll: reinit begin max=%d mem=%llu\n", (int)Clay_GetMaxElementCount(),
            (unsigned long long)Clay_MinMemorySize());
    Pico_CaptureClayScroll();
    uint64_t size = Clay_MinMemorySize();
    void *block = malloc(size);
    if (!block)
    {
        fprintf(stderr, "clay-scroll: reinit malloc failed size=%llu\n", (unsigned long long)size);
        return;
    }
    Clay_Arena memory = Clay_CreateArenaWithCapacityAndMemory(size, block);
    if (!Clay_Initialize(memory, (Clay_Dimensions){(float)GetScreenWidth(), (float)GetScreenHeight()},
                         (Clay_ErrorHandler){Pico_HandleClayErrors, 0}))
    {
        fprintf(stderr, "clay-scroll: reinit initialize failed\n");
        free(block);
        return;
    }
    Clay_SetMeasureTextFunction(Pico_MeasureTextUtf8, fonts);
#ifdef PICO_CLAY_DEBUG
    Clay_SetDebugModeEnabled(debug_enabled);
#else
    (void)debug_enabled;
#endif
    Pico_ClearClayReinit();
    fprintf(stderr, "clay-scroll: reinit done max=%d size=%llu\n", (int)Clay_GetMaxElementCount(),
            (unsigned long long)size);
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
