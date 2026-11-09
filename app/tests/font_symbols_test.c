#define _POSIX_C_SOURCE 200809L

#include "pico/theme.h"
#include "text_ink.h"
#include "docs_path.h"
#include "font_metadata.h"
#include <math.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef PICO_FONT_METADATA_TEST_WRAPS
static int g_alpha_scans;
Rectangle __real_GetImageAlphaBorder(Image image, float threshold);
Rectangle __wrap_GetImageAlphaBorder(Image image, float threshold)
{
    g_alpha_scans++;
    return __real_GetImageAlphaBorder(image, threshold);
}
#endif

static int Fail(const char *message)
{
    fprintf(stderr, "font symbols test: %s\n", message);
    return 1;
}

static Clay_Dimensions MeasureSlice(const char *str, uint16_t fontId, uint16_t fontSize)
{
    Clay_StringSlice slice = {
        .length = (int32_t)strlen(str),
        .chars = str,
        .baseChars = str,
    };
    Clay_TextElementConfig config = {
        .fontId = fontId,
        .fontSize = fontSize,
        .letterSpacing = 0,
        .wrapMode = CLAY_TEXT_WRAP_NONE,
    };
    return Pico_MeasureTextUtf8(slice, &config, NULL);
}

static int TestSymbolMeasurements(void)
{
    // Test arrow symbols (U+2190 .. U+2194, U+21D2)
    const char *arrows[] = {
        "\xE2\x86\x92", // → (right arrow)
        "\xE2\x86\x90", // ← (left arrow)
        "\xE2\x86\x91", // ↑ (up arrow)
        "\xE2\x86\x93", // ↓ (down arrow)
        "\xE2\x86\x94", // ↔ (left-right arrow)
        "\xE2\x87\x92", // ⇒ (right double arrow)
    };
    for (size_t i = 0; i < sizeof(arrows) / sizeof(arrows[0]); i++)
    {
        Clay_Dimensions dim = MeasureSlice(arrows[i], FONT_REGULAR, PICO_FONT_BODY);
        if (dim.width <= 0.0f || dim.height <= 0.0f)
        {
            return Fail("arrow symbol has zero dimension");
        }
    }

    // Verify arrow width contributes proportionally to combined string
    Clay_Dimensions dim_ab = MeasureSlice("A B", FONT_REGULAR, PICO_FONT_BODY);
    Clay_Dimensions dim_arrow = MeasureSlice("A \xE2\x86\x92 B", FONT_REGULAR, PICO_FONT_BODY);
    if (dim_arrow.width <= dim_ab.width)
    {
        return Fail("string with arrow is not wider than string with space");
    }

    // Test checkmarks, dingbats, and misc symbols
    const char *symbols[] = {
        "\xE2\x9C\x93", // ✓ (checkmark)
        "\xE2\x9C\x94", // ✔ (heavy checkmark)
        "\xE2\x9C\x97", // ✗ (cross)
        "\xE2\x98\x85", // ★ (black star)
        "\xE2\x9A\xA0", // ⚠ (warning)
        "\xE2\x9A\x99", // ⚙ (gear)
    };
    for (size_t i = 0; i < sizeof(symbols) / sizeof(symbols[0]); i++)
    {
        Clay_Dimensions dim = MeasureSlice(symbols[i], FONT_REGULAR, PICO_FONT_BODY);
        if (dim.width <= 0.0f || dim.height <= 0.0f)
        {
            return Fail("symbol character has zero dimension");
        }
    }

    // Test math operators
    const char *math_symbols[] = {
        "\xE2\x89\xA0", // ≠ (not equal)
        "\xE2\x89\xA4", // ≤ (less-than or equal)
        "\xE2\x89\xA5", // ≥ (greater-than or equal)
        "\xE2\x88\x9E", // ∞ (infinity)
        "\xE2\x88\x9A", // √ (square root)
        "\xE2\x88\x91", // ∑ (n-ary summation)
        "\xE2\x89\x88", // ≈ (almost equal)
        "\xC2\xB1",     // ± (plus-minus)
        "\xC3\x97",     // × (multiplication)
        "\xC3\xB7",     // ÷ (division)
    };
    for (size_t i = 0; i < sizeof(math_symbols) / sizeof(math_symbols[0]); i++)
    {
        Clay_Dimensions dim = MeasureSlice(math_symbols[i], FONT_REGULAR, PICO_FONT_BODY);
        if (dim.width <= 0.0f || dim.height <= 0.0f)
        {
            return Fail("math operator has zero dimension");
        }
    }

    // Test Greek and Cyrillic
    Clay_Dimensions dim_greek = MeasureSlice("\xCE\xB1 \xCE\xB2 \xCE\xBB \xCF\x80 \xCE\xA9", FONT_REGULAR, PICO_FONT_BODY);
    if (dim_greek.width <= 0.0f || dim_greek.height <= 0.0f)
    {
        return Fail("greek characters have zero dimension");
    }

    Clay_Dimensions dim_cyrillic = MeasureSlice("\xD0\x9F\xD1\x80\xD0\xB8\xD0\xB2\xD0\xB5\xD1\x82", FONT_REGULAR, PICO_FONT_BODY);
    if (dim_cyrillic.width <= 0.0f || dim_cyrillic.height <= 0.0f)
    {
        return Fail("cyrillic characters have zero dimension");
    }

    // Test Box Drawing
    Clay_Dimensions dim_box = MeasureSlice("\xE2\x94\x8C\xE2\x94\x80\xE2\x94\xAC\xE2\x94\x80\xE2\x94\x90", FONT_MONO, PICO_FONT_UI);
    if (dim_box.width <= 0.0f || dim_box.height <= 0.0f)
    {
        return Fail("box drawing characters have zero dimension");
    }

    // Blank-line preservation uses U+00A0 as an invisible marker. It must remain
    // addressable instead of falling back to a visible question mark.
    Font regular = Pico_FontAt(FONT_REGULAR, PICO_FONT_BODY);
    GlyphInfo nbsp = GetGlyphInfo(regular, 0x00A0);
    if (nbsp.value != 0x00A0)
    {
        return Fail("non-breaking space fell back to another glyph");
    }

    // Test missing codepoints in loaded range (U+2AE0, U+2072) and out-of-range (U+4F60)
    // They must fall back to '?' and have positive width instead of measuring 0.0 or disappearing
    Clay_Dimensions dim_question = MeasureSlice("?", FONT_REGULAR, PICO_FONT_BODY);
    if (dim_question.width <= 0.0f)
    {
        return Fail("fallback question mark has zero width");
    }

    Clay_Dimensions dim_missing_in_range1 = MeasureSlice("\xE2\xAB\xA0", FONT_REGULAR, PICO_FONT_BODY); // U+2AE0
    if (dim_missing_in_range1.width != dim_question.width || dim_missing_in_range1.height <= 0.0f)
    {
        return Fail("missing assigned codepoint in range did not fall back to question mark");
    }

    Clay_Dimensions dim_missing_in_range2 = MeasureSlice("\xE2\x81\xB2", FONT_REGULAR, PICO_FONT_BODY); // U+2072
    if (dim_missing_in_range2.width != dim_question.width || dim_missing_in_range2.height <= 0.0f)
    {
        return Fail("unassigned codepoint in range did not fall back to question mark");
    }

    Clay_Dimensions dim_cjk = MeasureSlice("\xE4\xBD\xA0", FONT_REGULAR, PICO_FONT_BODY); // U+4F60
    if (dim_cjk.width != dim_question.width || dim_cjk.height <= 0.0f)
    {
        return Fail("out-of-range codepoint did not fall back to question mark");
    }

    // Test font styles (bold, italic, mono)
    for (uint16_t face = 0; face < FONT_COUNT; face++)
    {
        Clay_Dimensions dim = MeasureSlice("test \xE2\x86\x92 \xE2\x9C\x93", face, PICO_FONT_BODY);
        if (dim.width <= 0.0f || dim.height <= 0.0f)
        {
            return Fail("symbol measurement failed for font face");
        }
    }

    return 0;
}

static int TestBadgeFontAndOrdinaryText(void)
{
    /* Narrow and wide letters must occupy the same monospace advance. */
    Clay_Dimensions narrow = MeasureSlice("UI", FONT_MONO_BOLD, PICO_FONT_UI);
    Clay_Dimensions wide = MeasureSlice("MW", FONT_MONO_BOLD, PICO_FONT_UI);
    if (narrow.width <= 0 || narrow.width != wide.width)
        return Fail("bold monospace face must give narrow and wide initials equal advances");

    /* Non-badge labels may carry extension-owned userData; none of it opts
     * into ink centering or changes their established baseline origin. */
    int extension_data = 42;
    Clay_RenderCommand command = {
        .boundingBox = {12.25f, 23.5f, 40, 18}, .userData = &extension_data,
        .renderData.text = {.stringContents = {.chars = "UI", .length = 2},
                            .fontId = FONT_BOLD, .fontSize = PICO_FONT_UI},
    };
    Vector2 origin = Pico_TextDrawOrigin(&command, Pico_FontAt(FONT_BOLD, PICO_FONT_UI));
    if (origin.x != command.boundingBox.x || origin.y != command.boundingBox.y)
        return Fail("ordinary text must retain its unadjusted draw origin");
    return 0;
}

static int TestIndexedLookupAndStorageReuse(void)
{
    /* External fonts remain unordered and mutable under Raylib's original
     * ownership rules. First match wins; a missing character uses the final
     * '?' (or glyph zero when no '?' exists). */
    GlyphInfo glyphs[] = {{.value = 0x03a9}, {.value = '?'}, {.value = 'A'}, {.value = '?'}};
    Rectangle recs[4] = {{0}};
    Font font = {.baseSize = 16, .glyphCount = 4, .glyphs = glyphs, .recs = recs};
    const struct { int cp, expected; } cases[] = {{'A', 2}, {0x03a9, 0}, {'?', 1}, {'Z', 3}, {0x10ffff, 3}};
    int rc = 0;
    for (int indexed = 0; indexed < 2; indexed++)
    {
        PicoFontMetadata *metadata = indexed ? PicoFontMetadata_Create(font) : NULL;
        if (indexed && !metadata) return Fail("font metadata creation failed");
        for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++)
            if (GetGlyphIndex(font, cases[i].cp) != cases[i].expected)
                rc = Fail("indexed and ordinary Raylib fonts must retain lookup/fallback semantics");
        PicoFontMetadata_Destroy(metadata);
        if (rc) return rc;
    }
    /* Reuse the exact storage, not just a likely allocator-reused address. */
    glyphs[0].value = 'Z';
    glyphs[1].value = 'B';
    glyphs[3].value = 'C';
    if (GetGlyphIndex(font, 'Z') != 0 || GetGlyphIndex(font, 'B') != 1 ||
        GetGlyphIndex(font, '?') != 0)
        return Fail("unregistered/reused glyph storage must not inherit a former font's index");
    PicoFontMetadata *metadata = PicoFontMetadata_Create(font);
    if (!metadata) return Fail("reused font metadata creation failed");
    int missing = GetGlyphIndex(font, '?');
    PicoFontMetadata_Destroy(metadata);
    if (missing != 0) return Fail("indexed fonts without '?' must fall back to glyph zero");
    return 0;
}

static int TestExternalFontInk(void)
{
    unsigned char pixels[16] = {0};
    pixels[7] = 1; /* Even minimum nonzero alpha contributes visible ink. */
    GlyphInfo glyph = {.value = 'A', .advanceX = 2,
        .image = {.data = pixels, .width = 2, .height = 2, .mipmaps = 1,
                  .format = PIXELFORMAT_UNCOMPRESSED_R8G8B8A8}};
    Rectangle rec = {0, 0, 2, 2};
    Font font = {.baseSize = 16, .glyphCount = 1, .glyphs = &glyph, .recs = &rec};
    Clay_StringSlice text = {.chars = "A", .length = 1};
    PicoFontMetadata *metadata = PicoFontMetadata_Create(font);
    if (!metadata) return Fail("bitmap metadata creation failed");
    Rectangle indexed = Pico_TextInkBounds(font, text, 16, 0);
    PicoFontMetadata_Destroy(metadata);
    if (indexed.x != 1 || indexed.y != 0 || indexed.width != 1 || indexed.height != 1)
        return Fail("minimum nonzero alpha must be included in precomputed ink");
    /* External fonts are not implicitly cached: changing their bitmap after
     * unregistering must be reflected by the ordinary ink-query path. */
    pixels[7] = 0; pixels[11] = 255;
    Rectangle changed = Pico_TextInkBounds(font, text, 16, 0);
    if (changed.x != 0 || changed.y != 1 || changed.width != 1 || changed.height != 1)
        return Fail("external-font ink queries must reflect current bitmap contents");
    /* Unhandled atlas formats must retain correct original alpha semantics,
     * not inherit Raylib GetImageColor's float-RGBA red-as-alpha bug. */
    float float_pixel[] = {0, 0, 0, 1};
    glyph.image = (Image){.data = float_pixel, .width = 1, .height = 1, .mipmaps = 1,
                          .format = PIXELFORMAT_UNCOMPRESSED_R32G32B32A32};
    metadata = PicoFontMetadata_Create(font);
    Rectangle float_ink = Pico_TextInkBounds(font, text, 16, 0);
    PicoFontMetadata_Destroy(metadata);
    if (float_ink.x != 0 || float_ink.y != 0 || float_ink.width != 1 || float_ink.height != 1)
        return Fail("other image formats must preserve original alpha semantics");
    return 0;
}

static int TestOwnedGlyphsAndInk(void)
{
    for (uint16_t face = 0; face < FONT_COUNT; face++)
    {
        Font font = Pico_FontAt(face, PICO_FONT_UI);
        for (int i = 0; i < font.glyphCount; i++)
            if (font.glyphs[GetGlyphIndex(font, font.glyphs[i].value)].value != font.glyphs[i].value)
                return Fail("indexed owned fonts must resolve every loaded Unicode glyph");
    }
    Font font = Pico_FontAt(FONT_MONO_BOLD, PICO_FONT_UI);
    Rectangle blank = Pico_TextInkBounds(font, (Clay_StringSlice){.chars = " \t", .length = 2},
                                          Pico_FontPx(PICO_FONT_UI), 0);
    if (blank.x != 0 || blank.y != 0 || blank.width != 0 || blank.height != 0)
        return Fail("spaces and tabs must not acquire visible ink from fallback glyphs");
    const char *labels[] = {"UI", "MW", "ΩЖ", "ÉÉ", "A ?", "東"};
    for (size_t i = 0; i < sizeof(labels) / sizeof(labels[0]); i++)
    {
        Clay_StringSlice text = {.chars = labels[i], .length = (int32_t)strlen(labels[i])};
        /* Independently union painted pixels rather than using the production
         * alpha-border helper to calculate the expected placement. */
        float pen = 0, left = INFINITY, top = INFINITY, right = -INFINITY, bottom = -INFINITY;
        float size = Pico_FontPx(PICO_FONT_UI), scale = size / font.baseSize;
        for (int pos = 0; pos < text.length;)
        {
            int bytes = 0, cp = GetCodepointNext(text.chars + pos, &bytes);
            pos += bytes;
            int index = GetGlyphIndex(font, cp);
            GlyphInfo glyph = font.glyphs[index];
            Color *pixels = LoadImageColors(glyph.image);
            if (!pixels) return Fail("glyph pixel data unavailable");
            if (cp != ' ' && cp != '\t')
                for (int y = 0; y < glyph.image.height; y++)
                    for (int x = 0; x < glyph.image.width; x++)
                        if (pixels[y * glyph.image.width + x].a)
                        {
                            left = fminf(left, pen + (glyph.offsetX + x) * scale);
                            top = fminf(top, (glyph.offsetY + y) * scale);
                            right = fmaxf(right, pen + (glyph.offsetX + x + 1) * scale);
                            bottom = fmaxf(bottom, (glyph.offsetY + y + 1) * scale);
                        }
            UnloadImageColors(pixels);
            pen += (glyph.advanceX ? glyph.advanceX : font.recs[index].width) * scale + 1.25f;
        }
#ifdef PICO_FONT_METADATA_TEST_WRAPS
        g_alpha_scans = 0;
#endif
        Rectangle ink = Pico_TextInkBounds(font, text, size, 1.25f);
        if (fabsf(ink.x - left) > 0.001f || fabsf(ink.y - top) > 0.001f ||
            fabsf(ink.width - (right - left)) > 0.001f || fabsf(ink.height - (bottom - top)) > 0.001f)
            return Fail("precomputed ink bounds must match painted glyph pixels and spacing");
#ifdef PICO_FONT_METADATA_TEST_WRAPS
        if (g_alpha_scans) return Fail("owned-font ink queries must not rescan glyph bitmaps");
#endif
    }
    return 0;
}

static int TestFontAtlasReplacement(void)
{
    float original = Pico_DeviceFontScale();
    const float devices[] = {1.0f, 1.25f, 2.0f, 1.0f};
    int rc = 0;
    for (size_t i = 0; i < sizeof(devices) / sizeof(devices[0]); i++)
    {
        Pico_SetDeviceFontScale(devices[i]);
        if (TestOwnedGlyphsAndInk()) { rc = 1; break; }
    }
    Pico_SetDeviceFontScale(original);
    return rc;
}

static int TestFontLifecycle(void)
{
    Font fonts[FONT_COUNT];
    memset(fonts, 0, sizeof(fonts));
    Pico_LoadFonts(fonts);
    for (int i = 0; i < FONT_COUNT; i++)
    {
        if (fonts[i].glyphCount <= 0 || !fonts[i].glyphs)
        {
            return Fail("loaded font has no glyphs");
        }
    }
    Pico_UnloadFonts(fonts);
    for (int i = 0; i < FONT_COUNT; i++)
    {
        if (fonts[i].glyphs != NULL)
        {
            return Fail("unloaded font retained glyph pointer in caller array");
        }
    }

    // Test that fonts can be cleanly reloaded after unload without double free or crash
    Font reloaded = Pico_FontAt(FONT_REGULAR, PICO_FONT_UI);
    if (reloaded.glyphCount <= 0 || !reloaded.glyphs)
    {
        return Fail("reloading font after unload failed");
    }
    Clay_Dimensions dim = MeasureSlice("\xE2\x86\x92", FONT_REGULAR, PICO_FONT_UI);
    if (dim.width <= 0.0f || dim.height <= 0.0f)
    {
        return Fail("measurement after font reload failed");
    }
    Pico_UnloadFonts(NULL);
    return 0;
}

int main(void)
{
#ifdef PICO_SOURCE_ROOT
    Pico_PathsInit(PICO_SOURCE_ROOT "/app");
#else
    Pico_PathsInit("app");
#endif
    int rc = TestSymbolMeasurements();
    if (rc != 0)
    {
        return rc;
    }
    if (TestBadgeFontAndOrdinaryText() || TestIndexedLookupAndStorageReuse() ||
        TestExternalFontInk() || TestFontAtlasReplacement()) return 1;
    return TestFontLifecycle();
}
