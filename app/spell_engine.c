#include "spell_engine.h"

#include <utf8proc.h>

#include <stdlib.h>
#include <string.h>

/* Tokenizer contract (the "obvious non-prose" skip rules):
 * - A token is a run of Unicode letters/digits. An apostrophe (' or U+2019)
 *   between two letters stays inside the token ("don't"). A '.' between a
 *   token and a following letter joins the token and marks it dot-joined
 *   ("example.com", "file.txt", "e.g").
 * - Runs of backticks toggle a code span; tokens inside are skipped. An
 *   unclosed backtick mutes the rest of the text, which matches how the
 *   composer is actually typed.
 * - A token is not checked when it: sits inside a code span, contains a
 *   digit, is dot-joined, is a single letter, is an all-caps acronym, or
 *   touches characters that mark handles/paths/URLs (before: @ # / : .,
 *   after: @ / :).
 * - '_' and '-' simply break tokens, so identifiers composed of real words
 *   pass while genuinely misspelled parts are still flagged. */

typedef struct Token {
    int start; /* byte offset, -1 when no token is open */
    int letters;
    bool has_digit;
    bool has_dot;
    bool has_lower;
    bool has_upper;
} Token;

static bool IsLetter(utf8proc_int32_t cp)
{
    utf8proc_category_t cat = utf8proc_category(cp);
    return cat == UTF8PROC_CATEGORY_LU || cat == UTF8PROC_CATEGORY_LL || cat == UTF8PROC_CATEGORY_LT ||
           cat == UTF8PROC_CATEGORY_LM || cat == UTF8PROC_CATEGORY_LO;
}

static bool IsDigit(utf8proc_int32_t cp)
{
    return utf8proc_category(cp) == UTF8PROC_CATEGORY_ND;
}

static bool IsApostrophe(utf8proc_int32_t cp)
{
    return cp == '\'' || cp == 0x2019;
}

/* Decodes one codepoint. Invalid bytes decode to -1 with an advance of 1 so
 * they act as token separators and cannot wedge the loop. */
static utf8proc_int32_t Decode(const char *text, int length, int pos, int *advance)
{
    utf8proc_int32_t cp = -1;
    utf8proc_ssize_t n = utf8proc_iterate((const utf8proc_uint8_t *)text + pos, length - pos, &cp);
    if (n <= 0)
    {
        *advance = 1;
        return -1;
    }
    *advance = (int)n;
    return cp;
}

static bool SkippedNeighborBefore(unsigned char c)
{
    return c == '@' || c == '#' || c == '/' || c == ':' || c == '.';
}

static bool SkippedNeighborAfter(unsigned char c)
{
    return c == '@' || c == '/' || c == ':';
}

static void PushRange(PicoSpellRanges *out, int start, int end)
{
    if (out->count == out->capacity)
    {
        int capacity = out->capacity > 0 ? out->capacity * 2 : 16;
        PicoSpellRange *items = (PicoSpellRange *)realloc(out->items, (size_t)capacity * sizeof(*items));
        if (!items)
        {
            return; /* best effort: keep the ranges collected so far */
        }
        out->items = items;
        out->capacity = capacity;
    }
    out->items[out->count++] = (PicoSpellRange){start, end};
}

static void EmitToken(const PicoSpellBackend *backend, const char *text, int length, bool in_code, Token tok,
                      int end, PicoSpellRanges *out)
{
    if (in_code || tok.letters < 2 || tok.has_digit || tok.has_dot)
    {
        return;
    }
    if (tok.has_upper && !tok.has_lower)
    {
        return; /* acronym */
    }
    if (tok.start > 0 && SkippedNeighborBefore((unsigned char)text[tok.start - 1]))
    {
        return;
    }
    if (end < length && SkippedNeighborAfter((unsigned char)text[end]))
    {
        return;
    }
    if (!backend->check(backend->ctx, text + tok.start, end - tok.start))
    {
        PushRange(out, tok.start, end);
    }
}

void pico_spell_check_text(const PicoSpellBackend *backend, const char *text, int length,
                           PicoSpellRanges *out)
{
    if (!out)
    {
        return;
    }
    out->count = 0;
    if (!backend || !backend->check || !text || length <= 0)
    {
        return;
    }

    bool in_code = false;
    Token tok = {.start = -1};
    int pos = 0;
    while (pos < length)
    {
        int adv = 1;
        utf8proc_int32_t cp = Decode(text, length, pos, &adv);

        if (cp == '`')
        {
            EmitToken(backend, text, length, in_code, tok, pos, out);
            tok = (Token){.start = -1};
            while (pos < length && text[pos] == '`')
            {
                pos++;
            }
            in_code = !in_code;
            continue;
        }

        bool letter = cp >= 0 && IsLetter(cp);
        bool digit = cp >= 0 && IsDigit(cp);
        if (letter || digit)
        {
            if (tok.start < 0)
            {
                tok.start = pos;
            }
            if (letter)
            {
                utf8proc_category_t cat = utf8proc_category(cp);
                tok.letters++;
                tok.has_lower = tok.has_lower || cat == UTF8PROC_CATEGORY_LL;
                tok.has_upper = tok.has_upper || cat == UTF8PROC_CATEGORY_LU || cat == UTF8PROC_CATEGORY_LT;
            }
            else
            {
                tok.has_digit = true;
            }
            pos += adv;
            continue;
        }

        if (tok.start >= 0 && pos + adv < length && (IsApostrophe(cp) || cp == '.'))
        {
            int next_adv = 1;
            utf8proc_int32_t next = Decode(text, length, pos + adv, &next_adv);
            if (next >= 0 && IsLetter(next))
            {
                tok.has_dot = tok.has_dot || cp == '.';
                pos += adv;
                continue;
            }
        }

        EmitToken(backend, text, length, in_code, tok, pos, out);
        tok = (Token){.start = -1};
        pos += adv;
    }
    EmitToken(backend, text, length, in_code, tok, pos, out);
}

int pico_spell_dict_tags(const char *locale, char full[32], char lang[32])
{
    if (!locale || !full || !lang)
    {
        return 0;
    }
    size_t n = 0;
    for (const char *p = locale; *p && *p != '.' && *p != '@' && n < 31; p++)
    {
        full[n++] = *p;
    }
    full[n] = '\0';
    if (n == 0 || strcmp(full, "C") == 0 || strcmp(full, "POSIX") == 0)
    {
        return 0;
    }
    char *underscore = strchr(full, '_');
    if (!underscore || underscore == full)
    {
        return 1;
    }
    size_t lang_len = (size_t)(underscore - full);
    memcpy(lang, full, lang_len);
    lang[lang_len] = '\0';
    return 2;
}

void pico_spell_ranges_free(PicoSpellRanges *ranges)
{
    if (!ranges)
    {
        return;
    }
    free(ranges->items);
    ranges->items = NULL;
    ranges->count = 0;
    ranges->capacity = 0;
}
