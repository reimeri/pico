#ifndef PICO_HIGHLIGHT_H
#define PICO_HIGHLIGHT_H

#include <stdbool.h>

/* Table-driven syntax highlighting for chat-rendered code: markdown code
 * blocks and the diff viewer. Produces flat, sorted, non-overlapping spans
 * of style classes over a whole text; callers intersect spans with lines at
 * render time. Highlighting is intentionally approximate (raw strings,
 * template-literal interpolation, heredocs, and similar corners may
 * mis-color) in exchange for a small dependency-free engine. */

typedef enum PicoHlClass {
    PICO_HL_NORMAL = 0, /* never emitted; spans exist only for styled text */
    PICO_HL_KEYWORD,
    PICO_HL_TYPE,    /* builtin types and constants: int, true, None, nil */
    PICO_HL_STRING,  /* string and char literals */
    PICO_HL_NUMBER,
    PICO_HL_COMMENT,
    PICO_HL_PREPROC, /* C/# directives, markdown list/fence markers */
    PICO_HL_FUNC,    /* identifier immediately followed by '(' */
    PICO_HL_FIELD,   /* JSON object keys */
    PICO_HL_VAR,     /* $shell variables */
    PICO_HL_ADD,     /* diff language: + lines */
    PICO_HL_DEL,     /* diff language: - lines */
    PICO_HL_HUNK,    /* diff language: @@ hunks and +++/--- headers */
} PicoHlClass;

typedef struct PicoHlSpan {
    int start; /* byte offset into the highlighted text */
    int end;
    PicoHlClass class;
} PicoHlSpan;

typedef struct PicoHlLang PicoHlLang;

/* Resolves a fence tag ("c", "py", "rust", "diff", ...) case-insensitively.
 * NULL for unknown tags: callers render plain text in that case. */
const PicoHlLang *PicoHl_Lookup(const char *tag);

/* Resolves a language from a file path's extension (".c" -> c). NULL when
 * the extension is unknown or the path has none. */
const PicoHlLang *PicoHl_LangForPath(const char *path);

/* Two-pass highlighting over text (NUL-terminated; may be empty).
 * Count returns the number of spans Fill would produce. Fill writes up to
 * cap spans and returns the number written; pass a buffer of Count() spans.
 * Spans cover only styled text; gaps between spans render as default.
 * A NULL language yields zero spans. */
int PicoHl_Count(const PicoHlLang *lang, const char *text);
int PicoHl_Fill(const PicoHlLang *lang, const char *text, PicoHlSpan *out, int cap);

#endif
