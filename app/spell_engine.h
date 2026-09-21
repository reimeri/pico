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

/* Derives dictionary tags to try from a locale string: "fi_FI.UTF-8" yields
 * full="fi_FI" and lang="fi". Returns the number of valid out params
 * (0 = nothing usable, 1 = full only, 2 = full and lang). Used by backend
 * probes; kept here because it is pure text handling. */
int pico_spell_dict_tags(const char *locale, char full[32], char lang[32]);

void pico_spell_ranges_free(PicoSpellRanges *ranges);

#endif
