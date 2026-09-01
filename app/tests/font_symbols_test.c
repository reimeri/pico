#define _POSIX_C_SOURCE 200809L

#include "pico/theme.h"
#include "docs_path.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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
    return TestFontLifecycle();
}
