// richtext.c - pre-wraps styled chunks into lines and emits them as clay
// elements. See richtext.h for the approach.

#include "richtext.h"
#include "chat_sel.h"
#include "pico/theme.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// ---------------------------------------------------------------------------
// Measure function wiring

static RichTextMeasureFunction s_measure;
static void *s_measure_user_data;
static int s_link_serial; // reset each layout pass by RichText_BeginLayout

void RichText_SetMeasureFunction(RichTextMeasureFunction measure, void *userData)
{
    s_measure = measure;
    s_measure_user_data = userData;
}

void RichText_BeginLayout(void)
{
    s_link_serial = 0;
}

static Clay_Dimensions Measure(const char *text, int length, Clay_TextElementConfig *config)
{
    Clay_StringSlice slice = {.length = length, .chars = text, .baseChars = text};
    return s_measure(slice, config, s_measure_user_data);
}

// ---------------------------------------------------------------------------
// Wrap cache structures (deep copied into the document arena)

typedef struct RtWord {
    const char *text; // points into the owning MdChunk text
    int length;
    bool bold;
    bool italic;
    bool code;
    bool strike;
    char *link_url; // arena owned, stable for the document lifetime
    bool hard_break; // length is 0; forces a line break
    bool space_before; // the source had whitespace before this word
    float width;
} RtWord;

typedef struct RtRun {
    char *text; // arena owned
    bool bold;
    bool italic;
    bool code;
    bool strike;
    char *link_url;
    bool space_before; // emit a space before this run (source had whitespace)
} RtRun;

typedef struct RtLine {
    RtRun *runs; // arena owned
    int run_count;
} RtLine;

typedef struct RtCache {
    float width;
    uint16_t font_size;
    float font_scale;
    RtLine *lines; // arena owned
    int line_count;
} RtCache;

// ---------------------------------------------------------------------------
// Config helpers

static uint16_t FontFor(const RichTextStyle *style, bool bold, bool italic, bool code)
{
    if (code)
    {
        return style->font_mono;
    }
    if (bold && italic)
    {
        return style->font_bold_italic;
    }
    if (bold)
    {
        return style->font_bold;
    }
    if (italic)
    {
        return style->font_italic;
    }
    return style->font_regular;
}

static Clay_TextElementConfig TextConfigFor(const RichTextStyle *style, bool bold, bool italic,
                                            bool code, bool is_link)
{
    Clay_TextElementConfig config = {0};
    config.fontId = FontFor(style, bold, italic, code);
    config.fontSize = style->font_size;
    config.lineHeight = Pico_FontPxU16(style->line_height > 0 ? style->line_height : style->font_size);
    config.wrapMode = CLAY_TEXT_WRAP_NONE;
    config.textColor = code ? style->code_text_color : style->text_color;
    if (is_link)
    {
        config.textColor = style->link_color;
    }
    return config;
}

// ---------------------------------------------------------------------------
// Word splitting + line wrapping

typedef struct WordArray {
    RtWord *items;
    int count;
    int capacity;
} WordArray;

static void PushWord(WordArray *words, RtWord word)
{
    if (words->count >= words->capacity)
    {
        int new_capacity = words->capacity == 0 ? 64 : words->capacity * 2;
        words->items = (RtWord *)realloc(words->items, (size_t)new_capacity * sizeof(RtWord));
        words->capacity = new_capacity;
    }
    words->items[words->count++] = word;
}

static void SplitChunksIntoWords(MdChunk *chunks, int chunk_count, bool force_bold,
                                 WordArray *words)
{
    // Tracks whether whitespace preceded the next word; carries across chunk
    // boundaries so a space at the end of one chunk separates the first word
    // of the next.
    bool pending_space = false;
    for (int c = 0; c < chunk_count; c++)
    {
        MdChunk *chunk = &chunks[c];
        const char *start = NULL;
        for (int i = 0; i <= chunk->length; i++)
        {
            char ch = i < chunk->length ? chunk->text[i] : ' ';
            if (ch == ' ' || ch == '\t' || ch == '\n')
            {
                if (start)
                {
                    RtWord word = {0};
                    word.text = start;
                    word.length = (int)(chunk->text + i - start);
                    word.bold = chunk->bold || force_bold;
                    word.italic = chunk->italic;
                    word.code = chunk->code;
                    word.strike = chunk->strike;
                    word.link_url = chunk->link_url;
                    word.space_before = pending_space;
                    PushWord(words, word);
                    start = NULL;
                }
                pending_space = true;
                if (ch == '\n')
                {
                    RtWord br = {0};
                    br.hard_break = true;
                    PushWord(words, br);
                }
            }
            else if (!start)
            {
                start = &chunk->text[i];
            }
        }
    }
}

static bool SameStyle(RtWord a, RtWord b)
{
    return a.bold == b.bold && a.italic == b.italic && a.code == b.code &&
           a.strike == b.strike && a.link_url == b.link_url;
}

static void MeasureWords(WordArray *words, const RichTextStyle *style)
{
    for (int i = 0; i < words->count; i++)
    {
        RtWord *word = &words->items[i];
        if (word->hard_break)
        {
            continue;
        }
        Clay_TextElementConfig config =
            TextConfigFor(style, word->bold, word->italic, word->code, word->link_url != NULL);
        word->width = Measure(word->text, word->length, &config).width;
    }
}

// A scratch line being assembled during wrapping; runs are joined strings
// (malloc'd), deep copied into the arena afterwards.
typedef struct ScratchRun {
    char *text;
    int length;
    int capacity;
    bool bold;
    bool italic;
    bool code;
    bool strike;
    char *link_url;
    bool space_before;
} ScratchRun;

typedef struct ScratchLine {
    ScratchRun *runs;
    int run_count;
    int run_capacity;
} ScratchLine;

static void ScratchRunAppendWord(ScratchRun *run, RtWord *word, bool add_space)
{
    int needed = run->length + word->length + (add_space ? 1 : 0) + 1;
    if (needed > run->capacity)
    {
        int new_capacity = run->capacity == 0 ? 64 : run->capacity * 2;
        while (new_capacity < needed)
        {
            new_capacity *= 2;
        }
        run->text = (char *)realloc(run->text, (size_t)new_capacity);
        run->capacity = new_capacity;
    }
    if (add_space)
    {
        run->text[run->length++] = ' ';
    }
    memcpy(run->text + run->length, word->text, (size_t)word->length);
    run->length += word->length;
    run->text[run->length] = '\0';
}

static void PushLine(ScratchLine **lines, int *line_count, int *line_capacity, ScratchLine line)
{
    if (*line_count >= *line_capacity)
    {
        int new_capacity = *line_capacity == 0 ? 32 : *line_capacity * 2;
        *lines = (ScratchLine *)realloc(*lines, (size_t)new_capacity * sizeof(ScratchLine));
        *line_capacity = new_capacity;
    }
    (*lines)[(*line_count)++] = line;
}

static void FreeScratch(ScratchLine *lines, int line_count)
{
    for (int i = 0; i < line_count; i++)
    {
        for (int r = 0; r < lines[i].run_count; r++)
        {
            free(lines[i].runs[r].text);
        }
        free(lines[i].runs);
    }
    free(lines);
}

static RtCache *BuildWrapCache(MdBlock *block, MdArena *arena, float available_width,
                               const RichTextStyle *style)
{
    if (available_width < 10.0f)
    {
        available_width = 10.0f;
    }

    WordArray words = {0};
    bool force_bold = style->force_bold || block->type == MDB_HEADING;
    SplitChunksIntoWords(block->chunks, block->chunk_count, force_bold, &words);
    MeasureWords(&words, style);

    Clay_TextElementConfig space_config = TextConfigFor(style, false, false, false, false);
    float space_width = Measure(" ", 1, &space_config).width;

    ScratchLine *scratch_lines = NULL;
    int scratch_line_count = 0;
    int scratch_line_capacity = 0;

    ScratchLine current_line = {0};
    float line_x = 0;

    for (int i = 0; i < words.count; i++)
    {
        RtWord *word = &words.items[i];

        if (word->hard_break)
        {
            PushLine(&scratch_lines, &scratch_line_count, &scratch_line_capacity, current_line);
            ScratchLine empty = {0};
            current_line = empty;
            line_x = 0;
            continue;
        }

        ScratchRun *last_run = current_line.run_count > 0 ? &current_line.runs[current_line.run_count - 1] : NULL;
        bool gap = line_x > 0 && word->space_before;
        bool fits = line_x + word->width + (gap ? space_width : 0) <= available_width;

        if (line_x == 0)
        {
            // Start a new line.
            if (current_line.run_count >= current_line.run_capacity)
            {
                int new_capacity = current_line.run_capacity == 0 ? 4 : current_line.run_capacity * 2;
                current_line.runs = (ScratchRun *)realloc(current_line.runs, (size_t)new_capacity * sizeof(ScratchRun));
                current_line.run_capacity = new_capacity;
            }
            ScratchRun new_run = {0};
            new_run.bold = word->bold;
            new_run.italic = word->italic;
            new_run.code = word->code;
            new_run.strike = word->strike;
            new_run.link_url = word->link_url;
            current_line.runs[current_line.run_count++] = new_run;
            ScratchRunAppendWord(&current_line.runs[current_line.run_count - 1], word, false);
            line_x = word->width;
        }
        else if (fits && last_run && last_run->bold == word->bold && last_run->italic == word->italic &&
                 last_run->code == word->code && last_run->strike == word->strike &&
                 last_run->link_url == word->link_url)
        {
            // Extend the current run.
            ScratchRunAppendWord(last_run, word, word->space_before);
            line_x += (word->space_before ? space_width : 0) + word->width;
        }
        else if (fits)
        {
            // Same line, new run.
            if (current_line.run_count >= current_line.run_capacity)
            {
                int new_capacity = current_line.run_capacity == 0 ? 4 : current_line.run_capacity * 2;
                current_line.runs = (ScratchRun *)realloc(current_line.runs, (size_t)new_capacity * sizeof(ScratchRun));
                current_line.run_capacity = new_capacity;
            }
            ScratchRun new_run = {0};
            new_run.bold = word->bold;
            new_run.italic = word->italic;
            new_run.code = word->code;
            new_run.strike = word->strike;
            new_run.link_url = word->link_url;
            new_run.space_before = word->space_before;
            current_line.runs[current_line.run_count++] = new_run;
            ScratchRunAppendWord(&current_line.runs[current_line.run_count - 1], word, false);
            line_x += (word->space_before ? space_width : 0) + word->width;
        }
        else
        {
            // Wrap to a new line.
            PushLine(&scratch_lines, &scratch_line_count, &scratch_line_capacity, current_line);
            ScratchLine empty = {0};
            current_line = empty;
            ScratchRun new_run = {0};
            new_run.bold = word->bold;
            new_run.italic = word->italic;
            new_run.code = word->code;
            new_run.strike = word->strike;
            new_run.link_url = word->link_url;
            current_line.runs = (ScratchRun *)malloc(4 * sizeof(ScratchRun));
            current_line.run_capacity = 4;
            current_line.runs[current_line.run_count++] = new_run;
            ScratchRunAppendWord(&current_line.runs[current_line.run_count - 1], word, false);
            line_x = word->width;
        }
    }
    // Push the final line (possibly empty -> blank paragraph still takes
    // space; blank lines from hard breaks were pushed already).
    PushLine(&scratch_lines, &scratch_line_count, &scratch_line_capacity, current_line);

    // Deep copy scratch lines into the arena.
    RtCache *cache = (RtCache *)MdArena_Alloc(arena, sizeof(RtCache), 8);
    cache->width = available_width;
    cache->font_size = style->font_size;
    cache->font_scale = Pico_FontScale();
    cache->line_count = scratch_line_count;
    cache->lines = (RtLine *)MdArena_Alloc(arena, (size_t)scratch_line_count * sizeof(RtLine), 8);
    for (int l = 0; l < scratch_line_count; l++)
    {
        RtLine *line = &cache->lines[l];
        line->run_count = scratch_lines[l].run_count;
        line->runs = (RtRun *)MdArena_Alloc(arena, (size_t)line->run_count * sizeof(RtRun), 8);
        for (int r = 0; r < line->run_count; r++)
        {
            ScratchRun *src = &scratch_lines[l].runs[r];
            RtRun *dst = &line->runs[r];
            dst->text = MdArena_Dup(arena, src->text, (size_t)src->length);
            dst->bold = src->bold;
            dst->italic = src->italic;
            dst->code = src->code;
            dst->strike = src->strike;
            dst->link_url = src->link_url;
            dst->space_before = src->space_before;
        }
    }

    FreeScratch(scratch_lines, scratch_line_count);
    free(words.items);
    return cache;
}

// ---------------------------------------------------------------------------
// Emission

static void EmitStrikeBar(Clay_Color color)
{
    CLAY_AUTO_ID({.floating = {.attachTo = CLAY_ATTACH_TO_PARENT,
                               .attachPoints = {.element = CLAY_ATTACH_POINT_LEFT_CENTER,
                                                .parent = CLAY_ATTACH_POINT_LEFT_CENTER},
                               .pointerCaptureMode = CLAY_POINTER_CAPTURE_MODE_PASSTHROUGH,
                               .clipTo = CLAY_CLIP_TO_ATTACHED_PARENT},
                  .layout = {.sizing = {.width = CLAY_SIZING_GROW(0), .height = CLAY_SIZING_FIXED(1)}},
                  .backgroundColor = color})
    {
    }
}

static void EmitRun(RtRun *run, const RichTextStyle *style, RichTextEmitState *emit)
{
    Clay_TextElementConfig config =
        TextConfigFor(style, run->bold, run->italic, run->code, run->link_url != NULL);
    Clay_String text = {.length = (int32_t)strlen(run->text), .chars = run->text};

    if (run->code)
    {
        CLAY_AUTO_ID({.layout = {.padding = {4, 4, 0, 0}},
                      .backgroundColor = style->code_bg_color,
                      .cornerRadius = CLAY_CORNER_RADIUS(4)})
        {
            if (run->strike)
            {
                CLAY_AUTO_ID({.layout = {.sizing = {.width = CLAY_SIZING_FIT(0)}}})
                {
                    PicoChatSel_Text(text, config);
                    EmitStrikeBar(config.textColor);
                }
            }
            else
            {
                PicoChatSel_Text(text, config);
            }
        }
    }
    else if (run->link_url)
    {
        Clay_ElementId id = CLAY_IDI("MdLink", s_link_serial++);
        bool hovered = Clay_PointerOver(id);
        if (hovered)
        {
            emit->hovered_link = run->link_url;
        }
        config.textColor = hovered ? style->link_hover_color : style->link_color;
        CLAY(id, {})
        {
            PicoChatSel_Text(text, config);
            if (run->strike)
            {
                EmitStrikeBar(config.textColor);
            }
        }
    }
    else if (run->strike)
    {
        CLAY_AUTO_ID({.layout = {.sizing = {.width = CLAY_SIZING_FIT(0)}}})
        {
            PicoChatSel_Text(text, config);
            EmitStrikeBar(config.textColor);
        }
    }
    else
    {
        PicoChatSel_Text(text, config);
    }
}

static void EmitLines(RtCache *cache, const RichTextStyle *style, RichTextEmitState *emit)
{
    Clay_TextElementConfig space_config = TextConfigFor(style, false, false, false, false);
    uint16_t row_h = Pico_FontPxU16(style->line_height > 0 ? style->line_height : style->font_size);
    CLAY_AUTO_ID({.layout = {.layoutDirection = CLAY_TOP_TO_BOTTOM,
                             .sizing = {.width = CLAY_SIZING_GROW(0)}}})
    {
        for (int l = 0; l < cache->line_count; l++)
        {
            RtLine *line = &cache->lines[l];
            CLAY_AUTO_ID({.layout = {.layoutDirection = CLAY_LEFT_TO_RIGHT,
                                     .sizing = {.width = CLAY_SIZING_GROW(0),
                                                .height = CLAY_SIZING_FIXED((float)row_h)},
                                     .childAlignment = {.x = style->text_align,
                                                        .y = CLAY_ALIGN_Y_CENTER}}})
            {
                if (line->run_count == 0)
                {
                    PicoChatSel_Text(CLAY_STRING(" "), space_config);
                }
                else
                {
                    for (int r = 0; r < line->run_count; r++)
                    {
                        if (r > 0 && line->runs[r].space_before)
                        {
                            PicoChatSel_Text(CLAY_STRING(" "), space_config);
                        }
                        EmitRun(&line->runs[r], style, emit);
                    }
                }
            }
        }
    }
}

// ---------------------------------------------------------------------------
// Public API

void RichText_RenderParagraph(MdBlock *block, MdArena *arena, float available_width,
                              const RichTextStyle *style, RichTextEmitState *emit)
{
    RtCache *cache = (RtCache *)block->wrap_cache;
    if (!cache || cache->width != available_width || cache->font_size != style->font_size ||
        cache->font_scale != Pico_FontScale())
    {
        cache = BuildWrapCache(block, arena, available_width, style);
        block->wrap_cache = cache;
    }
    EmitLines(cache, style, emit);
}

void RichText_MeasureUnwrapped(MdChunk *chunks, int chunk_count, const RichTextStyle *style,
                               float *preferred_width, float *min_width)
{
    float preferred = 0;
    float min = 0;
    WordArray words = {0};
    SplitChunksIntoWords(chunks, chunk_count, style->force_bold, &words);
    MeasureWords(&words, style);

    Clay_TextElementConfig space_config = TextConfigFor(style, false, false, false, false);
    float space_width = Measure(" ", 1, &space_config).width;

    float line_x = 0;
    for (int i = 0; i < words.count; i++)
    {
        RtWord *word = &words.items[i];
        if (word->hard_break)
        {
            if (line_x > preferred)
            {
                preferred = line_x;
            }
            line_x = 0;
            continue;
        }
        if (word->width > min)
        {
            min = word->width;
        }
        float gap = (line_x > 0 && word->space_before) ? space_width : 0;
        line_x += gap + word->width;
    }
    if (line_x > preferred)
    {
        preferred = line_x;
    }

    free(words.items);
    if (preferred_width)
    {
        *preferred_width = preferred;
    }
    if (min_width)
    {
        *min_width = min;
    }
}
