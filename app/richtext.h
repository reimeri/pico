#ifndef RICHTEXT_H
#define RICHTEXT_H

#include "../clay/clay.h"
#include "markdown.h"

// ---------------------------------------------------------------------------
// Rich text rendering for paragraphs with mixed inline styles.
//
// Clay cannot wrap across sibling elements, so a paragraph is pre-wrapped
// here: chunks are split into styled words, measured with the same text
// measure function clay uses, greedily packed into lines, and emitted as one
// LEFT_TO_RIGHT row container per line, with one CLAY_TEXT per style run.
//
// Wrapping results are cached on the MdBlock (block->wrap_cache) and only
// rebuilt when the available width changes, so steady-state frames are cheap.
// ---------------------------------------------------------------------------

typedef struct RichTextStyle {
    uint16_t font_regular;
    uint16_t font_bold;
    uint16_t font_italic;
    uint16_t font_bold_italic;
    uint16_t font_mono;
    uint16_t font_size;
    uint16_t line_height; // 0 = same as font_size
    Clay_Color text_color;
    Clay_Color code_text_color;
    Clay_Color code_bg_color;
    Clay_Color link_color;
    Clay_Color link_hover_color;
    bool force_bold; // table header cells
    Clay_LayoutAlignmentX text_align;
} RichTextStyle;

// Mutable state threaded through one layout pass. hovered_link is set while
// the pointer is over a link run; reset it to NULL before layout begins.
typedef struct RichTextEmitState {
    const char *hovered_link;
} RichTextEmitState;

// Resets per-layout-pass state (link id counter). Call once per frame before
// any RichText_RenderParagraph call.
void RichText_BeginLayout(void);

// Must be wired up before any RichText_* call: same signature as the measure
// function handed to Clay_SetMeasureTextFunction (e.g. Raylib_MeasureText).
typedef Clay_Dimensions (*RichTextMeasureFunction)(Clay_StringSlice text,
                                                   Clay_TextElementConfig *config,
                                                   void *userData);

void RichText_SetMeasureFunction(RichTextMeasureFunction measure, void *userData);

// Renders a paragraph/heading/list-item/quote block (its chunk list) as
// pre-wrapped lines. available_width is the horizontal space in pixels.
// The wrap cache is allocated from `arena` (the document arena).
// Safe to call every frame; results are cached per block + width.
void RichText_RenderParagraph(MdBlock *block, MdArena *arena, float available_width,
                              const RichTextStyle *style, RichTextEmitState *emit);

// Unwrapped preferred width (longest line) and min width (longest word) for
// a chunk list. Used to allocate table column widths.
void RichText_MeasureUnwrapped(MdChunk *chunks, int chunk_count, const RichTextStyle *style,
                               float *preferred_width, float *min_width);

#endif // RICHTEXT_H
