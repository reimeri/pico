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
    int end;   /* byte offset, exclusive; valid when start >= 0 */
    int letters;
    bool has_digit;
    bool has_dot;
    bool has_lower;
    bool has_upper;
    bool in_code;
} Token;

typedef void (*TokenFn)(void *ctx, const char *text, int length, Token tok);

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

static void CloseToken(Token *tok, int end, bool in_code, TokenFn fn, void *ctx, const char *text, int length)
{
    if (tok->start < 0)
    {
        return;
    }
    tok->end = end;
    tok->in_code = in_code;
    fn(ctx, text, length, *tok);
    *tok = (Token){.start = -1};
}

/* Walks the same token spans the checker uses. fn is not called for empty
 * gaps; skipped tokens (code, digits, acronyms) are still reported so a
 * caret query can see them. */
static void WalkTokens(const char *text, int length, TokenFn fn, void *ctx)
{
    bool in_code = false;
    Token tok = {.start = -1};
    int pos = 0;
    while (pos < length)
    {
        int adv = 1;
        utf8proc_int32_t cp = Decode(text, length, pos, &adv);

        if (cp == '`')
        {
            CloseToken(&tok, pos, in_code, fn, ctx, text, length);
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

        CloseToken(&tok, pos, in_code, fn, ctx, text, length);
        pos += adv;
    }
    CloseToken(&tok, pos, in_code, fn, ctx, text, length);
}

typedef struct CheckCtx {
    const PicoSpellBackend *backend;
    PicoSpellRanges *out;
} CheckCtx;

static void EmitToken(void *ctx, const char *text, int length, Token tok)
{
    CheckCtx *c = (CheckCtx *)ctx;
    if (tok.in_code || tok.letters < 2 || tok.has_digit || tok.has_dot)
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
    if (tok.end < length && SkippedNeighborAfter((unsigned char)text[tok.end]))
    {
        return;
    }
    if (!c->backend->check(c->backend->ctx, text + tok.start, tok.end - tok.start))
    {
        PushRange(c->out, tok.start, tok.end);
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
    CheckCtx ctx = {backend, out};
    WalkTokens(text, length, EmitToken, &ctx);
}

typedef struct AtCtx {
    int pos;
    int start;
    int end;
    bool found;
} AtCtx;

static void FindToken(void *ctx, const char *text, int length, Token tok)
{
    (void)text;
    (void)length;
    AtCtx *a = (AtCtx *)ctx;
    if (a->found)
    {
        return;
    }
    if (tok.start <= a->pos && a->pos <= tok.end)
    {
        a->found = true;
        a->start = tok.start;
        a->end = tok.end;
    }
}

/* Byte length of an apostrophe ending at pos, or 0. U+2019 is the same
 * apostrophe the tokenizer keeps inside a word once a letter follows. */
static int ApostropheLenBefore(const char *text, int pos)
{
    if (pos >= 1 && text[pos - 1] == '\'')
    {
        return 1;
    }
    if (pos >= 3 && (unsigned char)text[pos - 3] == 0xE2 && (unsigned char)text[pos - 2] == 0x80 &&
        (unsigned char)text[pos - 1] == 0x99)
    {
        return 3;
    }
    return 0;
}

bool pico_spell_token_at(const char *text, int length, int pos, int *start, int *end)
{
    if (!text || length <= 0 || pos < 0 || pos > length || !start || !end)
    {
        return false;
    }
    AtCtx at = {.pos = pos};
    WalkTokens(text, length, FindToken, &at);
    if (!at.found)
    {
        int apo = ApostropheLenBefore(text, pos);
        if (apo > 0)
        {
            at.pos = pos - apo;
            WalkTokens(text, length, FindToken, &at);
            if (at.found && at.end != pos - apo)
            {
                at.found = false;
            }
        }
    }
    if (!at.found)
    {
        return false;
    }
    *start = at.start;
    *end = at.end;
    return true;
}

void pico_spell_pending_update(PicoSpellPending *pending, const char *text, int length, int cursor,
                               bool text_changed)
{
    if (!pending)
    {
        return;
    }
    int start = 0;
    int end = 0;
    bool in_token = cursor >= 0 && text && length > 0 &&
                    pico_spell_token_at(text, length, cursor, &start, &end);
    if (text_changed)
    {
        pending->active = in_token;
        if (in_token)
        {
            pending->start = start;
            pending->end = end;
        }
        return;
    }
    if (!pending->active)
    {
        return;
    }
    if (!in_token || start != pending->start || end != pending->end)
    {
        pending->active = false;
    }
}

bool pico_spell_pending_hides(const PicoSpellPending *pending, int start, int end)
{
    return pending && pending->active && pending->start == start && pending->end == end;
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
