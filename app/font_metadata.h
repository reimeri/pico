#ifndef PICO_FONT_METADATA_H
#define PICO_FONT_METADATA_H

#include "raylib.h"

/* Private font-atlas metadata. Create/destroy on the UI thread alongside the
 * immutable glyph storage; unregister before releasing that storage. Pico font
 * access (including measurement) is UI-thread-only. Unregistered Raylib fonts
 * keep Raylib's ordinary lookup and bitmap behavior. */
typedef struct PicoFontMetadata PicoFontMetadata;
/* Optional: NULL on allocation failure or unsupported atlas image formats. */
PicoFontMetadata *PicoFontMetadata_Create(Font font);
void PicoFontMetadata_Destroy(PicoFontMetadata *metadata);
/* Raylib GetGlyphIndex hook: -1 means this is not an indexed Pico font. */
int PicoFontMetadata_GlyphIndex(Font font, int codepoint);
/* Borrowed per-glyph atlas-pixel bounds, or NULL for an unregistered font. */
const Rectangle *PicoFontMetadata_InkBounds(Font font);

#endif
