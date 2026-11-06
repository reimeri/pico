#ifndef PICO_TEXT_INK_H
#define PICO_TEXT_INK_H

#include "pico/theme.h"

/* Internal opt-in for standalone labels, not normal baseline-aligned text.
 * Use this process-lifetime marker as Clay text userData; never dereference
 * arbitrary extension userData in the renderer. */
extern const char Pico_TextInkCentered;

/* Visible glyph bounds relative to DrawTextEx's origin, in logical pixels.
 * Uses the same atlas, fallback glyphs, advances and spacing as the renderer. */
Rectangle Pico_TextInkBounds(Font font, Clay_StringSlice text, float size, float spacing);
Vector2 Pico_TextDrawOrigin(const Clay_RenderCommand *command, Font font);

#endif
