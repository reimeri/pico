#ifndef PICO_SPELL_INTERNAL_H
#define PICO_SPELL_INTERNAL_H

#include "pico/plugin.h"

#include "clay/clay.h"

/* App-internal seam between text fields and the spell builtin. A field
 * (composer today, ask_user text inputs later) describes its already-wrapped
 * geometry once per frame; the spell builtin re-checks the text when it
 * changed and draws squiggle underlines for misspelled ranges. The word
 * containing the caret is not underlined until the caret leaves it. Call
 * this inside an active scissor matching clip, including when the field is
 * empty, so clearing the text commits the word that was being typed. */

typedef struct PicoSpellLine {
    int start;  /* byte offset of the wrapped line */
    int length; /* byte length of the wrapped line */
} PicoSpellLine;

typedef struct PicoSpellView {
    const char *text; /* writable in practice: measurement briefly NUL-terminates slices */
    int length;
    const PicoSpellLine *lines;
    int line_count;
    float origin_x;
    float origin_y;
    float line_height;
    float scroll_y;
    Clay_BoundingBox clip; /* used to cull off-screen lines */
    Font font;
    float font_px;
    int cursor; /* byte offset; -1 if the field has no caret */
} PicoSpellView;

/* Stable, unique address identifying the composer text field. */
#define PICO_SPELL_FIELD_COMPOSER ((const void *)"pico.composer")

/* No-op unless the spell backend is ready and enabled. */
void PicoSpell_DrawSquiggles(PicoHost *host, const void *field_key, const PicoSpellView *view);

#endif
