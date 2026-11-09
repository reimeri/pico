#include "font_metadata.h"

#include <stdint.h>
#include <stdlib.h>

#define FONT_METADATA_BUCKETS 256
#define FONT_ASCII_COUNT 128

typedef struct GlyphIndex {
    int codepoint, index;
} GlyphIndex;

struct PicoFontMetadata {
    struct PicoFontMetadata *next;
    const GlyphInfo *glyphs;
    const Rectangle *recs;
    int count, fallback;
    int ascii[FONT_ASCII_COUNT];
    GlyphIndex *lookup;
    Rectangle *ink;
};

/* This is an atlas-lifetime registry, not a cache of arbitrary Fonts. A by-value
 * Font copy shares its owner's entry. Removing it before glyph storage is freed
 * prevents an allocator-reused address from inheriting obsolete metadata. */
/* Unrelated Raylib CPU-image work on another thread must not race the UI
 * registry. Such fonts simply take Raylib's unregistered scan path there. */
static __thread PicoFontMetadata *s_fonts[FONT_METADATA_BUCKETS];

static unsigned FontBucket(const GlyphInfo *glyphs)
{
    uintptr_t key = (uintptr_t)glyphs;
    key ^= key >> 11;
    key ^= key >> 21;
    return (unsigned)((key >> 3) % FONT_METADATA_BUCKETS);
}

static PicoFontMetadata *FindFont(Font font)
{
    for (PicoFontMetadata *entry = s_fonts[FontBucket(font.glyphs)]; entry; entry = entry->next)
        if (entry->glyphs == font.glyphs && entry->recs == font.recs && entry->count == font.glyphCount)
            return entry;
    return NULL;
}

static int CompareGlyphs(const void *a, const void *b)
{
    const GlyphIndex *left = a, *right = b;
    if (left->codepoint != right->codepoint)
        return (left->codepoint > right->codepoint) - (left->codepoint < right->codepoint);
    /* Raylib returns the first matching glyph, even for duplicate codepoints. */
    return (left->index > right->index) - (left->index < right->index);
}

static Rectangle GlyphInkBounds(Image image, Rectangle atlas_rect)
{
    if (!image.data)
        return (Rectangle){0, 0, atlas_rect.width, atlas_rect.height};
    int left = image.width, top = image.height, right = -1, bottom = -1;
    const unsigned char *pixels = image.data;
    for (int y = 0; y < image.height; y++)
        for (int x = 0; x < image.width; x++)
        {
            size_t pixel = (size_t)y * (size_t)image.width + (size_t)x;
            /* Pico's atlas crops are GRAY_ALPHA; RGBA8 is equally direct.
             * No temporary LoadImageColors buffer (unchecked in Raylib 5.5). */
            unsigned char alpha = image.format == PIXELFORMAT_UNCOMPRESSED_GRAY_ALPHA ?
                                  pixels[pixel * 2 + 1] : pixels[pixel * 4 + 3];
            if (!alpha) continue; /* Same strict alpha > 0 threshold as before. */
            if (x < left) left = x;
            if (y < top) top = y;
            if (x > right) right = x;
            if (y > bottom) bottom = y;
        }
    if (right < left || bottom < top) return (Rectangle){0};
    return (Rectangle){(float)left, (float)top, (float)(right - left + 1), (float)(bottom - top + 1)};
}

PicoFontMetadata *PicoFontMetadata_Create(Font font)
{
    if (!font.glyphs || !font.recs || font.glyphCount <= 0)
        return NULL;
    /* Only atlas crop formats with exact, allocation-free alpha access are
     * indexed. Other formats keep the original lookup/ink paths rather than
     * depending on GetImageColor's differing float/half-float alpha semantics. */
    for (int i = 0; i < font.glyphCount; i++)
    {
        Image image = font.glyphs[i].image;
        if (image.data && image.format != PIXELFORMAT_UNCOMPRESSED_GRAY_ALPHA &&
            image.format != PIXELFORMAT_UNCOMPRESSED_R8G8B8A8)
            return NULL;
    }
    PicoFontMetadata *metadata = calloc(1, sizeof(*metadata));
    if (!metadata)
        return NULL;
    metadata->lookup = malloc((size_t)font.glyphCount * sizeof(*metadata->lookup));
    metadata->ink = malloc((size_t)font.glyphCount * sizeof(*metadata->ink));
    if (!metadata->lookup || !metadata->ink)
    {
        free(metadata->lookup);
        free(metadata->ink);
        free(metadata);
        return NULL; /* Optional acceleration: the original paths remain valid. */
    }
    metadata->glyphs = font.glyphs;
    metadata->recs = font.recs;
    metadata->count = font.glyphCount;
    for (int i = 0; i < font.glyphCount; i++)
    {
        metadata->lookup[i] = (GlyphIndex){font.glyphs[i].value, i};
        if (font.glyphs[i].value == '?') metadata->fallback = i;
        metadata->ink[i] = GlyphInkBounds(font.glyphs[i].image, font.recs[i]);
    }
    qsort(metadata->lookup, (size_t)font.glyphCount, sizeof(*metadata->lookup), CompareGlyphs);
    for (int cp = 0; cp < FONT_ASCII_COUNT; cp++) metadata->ascii[cp] = metadata->fallback;
    for (int i = font.glyphCount - 1; i >= 0; i--)
    {
        int cp = font.glyphs[i].value;
        if (cp >= 0 && cp < FONT_ASCII_COUNT) metadata->ascii[cp] = i;
    }
    unsigned bucket = FontBucket(font.glyphs);
    metadata->next = s_fonts[bucket];
    s_fonts[bucket] = metadata;
    return metadata;
}

void PicoFontMetadata_Destroy(PicoFontMetadata *metadata)
{
    if (!metadata) return;
    PicoFontMetadata **entry = &s_fonts[FontBucket(metadata->glyphs)];
    while (*entry && *entry != metadata) entry = &(*entry)->next;
    if (*entry) *entry = metadata->next;
    free(metadata->lookup);
    free(metadata->ink);
    free(metadata);
}

int PicoFontMetadata_GlyphIndex(Font font, int codepoint)
{
    PicoFontMetadata *metadata = FindFont(font);
    if (!metadata) return -1;
    if (codepoint >= 0 && codepoint < FONT_ASCII_COUNT) return metadata->ascii[codepoint];
    int low = 0, high = metadata->count;
    while (low < high)
    {
        int mid = low + (high - low) / 2;
        if (metadata->lookup[mid].codepoint < codepoint) low = mid + 1;
        else high = mid;
    }
    return low < metadata->count && metadata->lookup[low].codepoint == codepoint ?
        metadata->lookup[low].index : metadata->fallback;
}

const Rectangle *PicoFontMetadata_InkBounds(Font font)
{
    PicoFontMetadata *metadata = FindFont(font);
    return metadata ? metadata->ink : NULL;
}
