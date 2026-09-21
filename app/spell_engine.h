#ifndef PICO_SPELL_ENGINE_H
#define PICO_SPELL_ENGINE_H

#include <stdbool.h>

/* Pure spell-check engine: tokenizes UTF-8 text into words and asks a
 * backend whether each word is correctly spelled. No I/O, no raylib, no
 * Clay, no global state. The backend is a single callback so tests can
 * stub it and the real enchant probe stays thin. */

typedef bool (*PicoSpellCheckFn)(void *ctx, const char *word, int length);

typedef struct PicoSpellBackend {
    void *ctx;
    PicoSpellCheckFn check; /* true when the word is correctly spelled */
} PicoSpellBackend;

typedef struct PicoSpellRange {
    int start; /* byte offset, inclusive */
    int end;   /* byte offset, exclusive */
} PicoSpellRange;

typedef struct PicoSpellRanges {
    PicoSpellRange *items;
    int count;
    int capacity;
} PicoSpellRanges;

/* Replaces out's contents with the byte ranges of misspelled words, in
 * document order (capacity is reused across calls). A NULL backend or
 * check fn produces zero ranges; that is the silent-disabled path. */
void pico_spell_check_text(const PicoSpellBackend *backend, const char *text, int length,
                           PicoSpellRanges *out);

/* Word token containing a caret position, using the same spans
 * pico_spell_check_text checks. The caret belongs to a token when
 * start <= pos <= end, so either edge still counts. A caret sitting just
 * after an apostrophe that has not yet joined a following letter still
 * belongs to that token ("isn'" is "isn" until the next letter). Returns
 * false on a separator or when pos is outside the text. */
bool pico_spell_token_at(const char *text, int length, int pos, int *start, int *end);

/* Word currently being typed. A text edit arms the token under the caret;
 * the caret leaving that token disarms it. Moving back without an edit does
 * not arm it again. Callers keep the misspelling in the cached ranges and
 * only skip painting it while armed, so a caret move can reveal it without
 * rechecking. */
typedef struct PicoSpellPending {
    bool active;
    int start; /* token byte span, [start, end) */
    int end;
} PicoSpellPending;

void pico_spell_pending_update(PicoSpellPending *pending, const char *text, int length, int cursor,
                               bool text_changed);

/* True when [start, end) is the armed in-progress word and should not be drawn. */
bool pico_spell_pending_hides(const PicoSpellPending *pending, int start, int end);

/* Derives dictionary tags to try from a locale string: "fi_FI.UTF-8" yields
 * full="fi_FI" and lang="fi". Returns the number of valid out params
 * (0 = nothing usable, 1 = full only, 2 = full and lang). Used by backend
 * probes; kept here because it is pure text handling. */
int pico_spell_dict_tags(const char *locale, char full[32], char lang[32]);

void pico_spell_ranges_free(PicoSpellRanges *ranges);

#endif
