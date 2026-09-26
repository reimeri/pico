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

/* Short-string measurement cache.
 *
 * Rebuilding a text layout re-measures every word of a paragraph: streaming
 * into an expanded thinking block re-wraps the whole body every frame, and
 * raylib's MeasureTextEx scans the entire glyph table per codepoint. Words
 * repeat across frames and within a frame, so identical (text, font, size,
 * spacing) lookups are served from this bounded exact-match table. Longer
 * strings bypass the cache; entries are validated by full byte comparison so
 * a hash collision can only cost a probe, never a wrong width. */
#define PICO_MEASURE_CACHE_SLOTS 4096
#define PICO_MEASURE_CACHE_MAX_TEXT 48
#define PICO_MEASURE_CACHE_PROBES 4

typedef struct PicoMeasureCacheEntry {
    uint64_t hash; /* 0 = empty slot */
    int32_t length;
    uint16_t font_id;
    uint16_t font_size;
    uint16_t letter_spacing;
    float font_scale;
    float width;
    float height;
    char bytes[PICO_MEASURE_CACHE_MAX_TEXT];
} PicoMeasureCacheEntry;

static PicoMeasureCacheEntry s_measure_cache[PICO_MEASURE_CACHE_SLOTS];
static uint64_t s_font_generation = 1;

uint64_t Pico_FontGeneration(void)
{
    return s_font_generation;
}
static char *s_measure_scratch;
static size_t s_measure_scratch_cap;

static uint64_t MeasureCacheHash(const char *text, int32_t length,
                                 const Clay_TextElementConfig *config)
{
    uint64_t hash = UINT64_C(0xcbf29ce484222325);
    for (int32_t i = 0; i < length; i++)
    {
        hash ^= (unsigned char)text[i];
        hash *= UINT64_C(0x100000001b3);
    }
    hash ^= (uint64_t)config->fontId;
    hash *= UINT64_C(0x100000001b3);
    hash ^= (uint64_t)config->fontSize;
    hash *= UINT64_C(0x100000001b3);
    hash ^= (uint64_t)config->letterSpacing;
    hash *= UINT64_C(0x100000001b3);
    {
        float scale = Pico_FontScale();
        uint32_t bits;
        memcpy(&bits, &scale, sizeof(bits));
        hash ^= bits;
        hash *= UINT64_C(0x100000001b3);
    }
    return hash ? hash : 1;
}

void Pico_MeasureCacheReset(void)
{
    memset(s_measure_cache, 0, sizeof(s_measure_cache));
}

static Clay_Dimensions MeasureWithFont(const char *text, int32_t length,
                                       const Clay_TextElementConfig *config, Font font)
{
    if ((size_t)length + 1 > s_measure_scratch_cap)
    {
        size_t new_capacity = s_measure_scratch_cap == 0 ? 256 : s_measure_scratch_cap;
        while (new_capacity < (size_t)length + 1)
        {
            new_capacity *= 2;
        }
        free(s_measure_scratch);
        s_measure_scratch = (char *)malloc(new_capacity);
        s_measure_scratch_cap = s_measure_scratch ? new_capacity : 0;
        if (!s_measure_scratch)
        {
            return (Clay_Dimensions){0};
        }
    }
    memcpy(s_measure_scratch, text, (size_t)length);
    s_measure_scratch[length] = '\0';
    Vector2 size = MeasureTextEx(font, s_measure_scratch, Pico_FontPx(config->fontSize),
                                 Pico_FontPx(config->letterSpacing));
    return (Clay_Dimensions){.width = size.x, .height = size.y};
}

Clay_Dimensions Pico_MeasureTextUtf8(Clay_StringSlice text, Clay_TextElementConfig *config, void *userData)
{
    (void)userData;

    Font font = Pico_FontAt(config->fontId, config->fontSize);
    if (!font.glyphs)
    {
        font = GetFontDefault();
    }

    if (text.length > 0 && text.length <= PICO_MEASURE_CACHE_MAX_TEXT)
    {
        uint64_t hash = MeasureCacheHash(text.chars, text.length, config);
        uint32_t mask = PICO_MEASURE_CACHE_SLOTS - 1;
        uint32_t index = (uint32_t)hash & mask;
        uint32_t insert = index;
        for (int probe = 0; probe < PICO_MEASURE_CACHE_PROBES; probe++, index = (index + 1) & mask)
        {
            PicoMeasureCacheEntry *entry = &s_measure_cache[index];
            if (entry->hash == 0)
            {
                insert = index;
                break;
            }
            if (entry->hash == hash && entry->length == text.length &&
                entry->font_id == config->fontId && entry->font_size == config->fontSize &&
                entry->letter_spacing == config->letterSpacing &&
                entry->font_scale == Pico_FontScale() &&
                memcmp(entry->bytes, text.chars, (size_t)text.length) == 0)
            {
                return (Clay_Dimensions){.width = entry->width, .height = entry->height};
            }
        }
        Clay_Dimensions measured = MeasureWithFont(text.chars, text.length, config, font);
        PicoMeasureCacheEntry *entry = &s_measure_cache[insert];
        entry->hash = hash;
        entry->length = text.length;
        entry->font_id = config->fontId;
        entry->font_size = config->fontSize;
        entry->letter_spacing = config->letterSpacing;
        entry->font_scale = Pico_FontScale();
        entry->width = measured.width;
        entry->height = measured.height;
        memcpy(entry->bytes, text.chars, (size_t)text.length);
        return measured;
    }

    return MeasureWithFont(text.chars, text.length, config, font);
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
    Pico_MeasureCacheReset();
    s_font_generation++;
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

static struct {
    void *memory; /* Keep the malloc address: Clay aligns its arena internally. */
    Clay_Context *context;
} clay_arena;

static bool needs_clay_reinit = false;
static bool clay_internal_error_reported = false;
static int32_t pending_measure_word_count = 0;

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

static void RequestMeasureCacheReinit(void)
{
    int32_t current = Clay_GetMaxMeasureTextCacheWordCount();
    needs_clay_reinit = true;
    if (pending_measure_word_count < current)
    {
        pending_measure_word_count = current;
    }
    if (pending_measure_word_count < 1)
    {
        pending_measure_word_count = 1;
    }
    pending_measure_word_count *= 2;
    fprintf(stderr, "clay-scroll: text-cache reinit pending words=%d (live=%d) snaps=%d\n",
            (int)pending_measure_word_count, (int)current, clay_scroll_snap_count);
}

void Pico_HandleClayErrors(Clay_ErrorData error_data)
{
    if (error_data.errorType == CLAY_ERROR_TYPE_INTERNAL_ERROR)
    {
        /* One backtrace per overflow: Clay can range-check on every subsequent
         * cache lookup in the same layout, and printing each one freezes the UI. */
        if (!clay_internal_error_reported)
        {
            ReportClayInternalError(error_data.errorText);
            clay_internal_error_reported = true;
        }
        needs_clay_reinit = true;
        return;
    }
    printf("%.*s\n", error_data.errorText.length, error_data.errorText.chars);
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
        /* Grow the word cache on the next reinit. Changing maxMeasureTextCacheWordCount
         * on the live context resizes Clay's hash-bucket modulus without reallocating
         * the map, which then range-checks as an internal error for the rest of the layout. */
        RequestMeasureCacheReinit();
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
    clay_internal_error_reported = false;
}

/* Clay reads capacities from the previous current context during initialization.
 * Keep it alive until the replacement is initialized and accepted. */
static bool ReplaceClayArena(Clay_Dimensions dimensions)
{
    Clay_Context *previous = Clay_GetCurrentContext();
    uint32_t size = Clay_MinMemorySize();
    void *memory = malloc(size);
    if (!memory)
    {
        fprintf(stderr, "clay: arena allocation failed (%u bytes)\n", size);
        return false;
    }
    Clay_Context *context = Clay_Initialize(Clay_CreateArenaWithCapacityAndMemory(size, memory),
                                            dimensions, (Clay_ErrorHandler){Pico_HandleClayErrors, 0});
    if (!context)
    {
        /* Initialization may have changed the global context before failing. */
        Clay_SetCurrentContext(previous);
        free(memory);
        fprintf(stderr, "clay: arena initialization failed\n");
        return false;
    }
    void *old_memory = clay_arena.memory;
    clay_arena.memory = memory;
    clay_arena.context = context;
    free(old_memory);
    return true;
}

bool Pico_InitClay(Clay_Dimensions dimensions)
{
    if (clay_arena.memory || Clay_GetCurrentContext())
    {
        return false;
    }
    if (!ReplaceClayArena(dimensions))
    {
        return false;
    }
    Pico_ClearClayReinit();
    return true;
}

void Pico_FreeClay(void)
{
    if (Clay_GetCurrentContext() == clay_arena.context)
    {
        Clay_SetCurrentContext(NULL);
    }
    free(clay_arena.memory);
    memset(&clay_arena, 0, sizeof(clay_arena));
    memset(clay_scroll_snaps, 0, sizeof(clay_scroll_snaps));
    clay_scroll_snap_count = 0;
    clay_scroll_restore_pending = false;
    pending_measure_word_count = 0;
    Pico_ClearClayReinit();
}

bool Pico_ReinitClay(Font *fonts, bool debug_enabled)
{
    needs_clay_reinit = true;
    if (!clay_arena.context || Clay_GetCurrentContext() != clay_arena.context)
    {
        return false;
    }
    if (pending_measure_word_count > Clay_GetMaxMeasureTextCacheWordCount())
    {
        Clay_SetMaxMeasureTextCacheWordCount(pending_measure_word_count);
    }
    Pico_CaptureClayScroll();
    if (!ReplaceClayArena(Clay_GetLayoutDimensions()))
    {
        return false;
    }
    pending_measure_word_count = 0;
    Clay_SetMeasureTextFunction(Pico_MeasureTextUtf8, fonts);
#ifdef PICO_CLAY_DEBUG
    Clay_SetDebugModeEnabled(debug_enabled);
#else
    (void)debug_enabled;
#endif
    Pico_ClearClayReinit();
    return true;
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
    CaptureScrollId("AskUserBody", verbose);
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
