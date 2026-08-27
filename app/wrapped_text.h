#ifndef PICO_WRAPPED_TEXT_H
#define PICO_WRAPPED_TEXT_H

#include <stdbool.h>
#include <stdint.h>

#define PICO_WRAPPED_TEXT_LINE_CAPACITY 48

typedef float (*PicoWrappedTextMeasureFn)(void *user, const char *text, int length);

typedef struct PicoWrappedTextLine {
    int start;
    int length;
} PicoWrappedTextLine;

typedef struct PicoWrappedText {
    const char *text;
    int text_length;
    float width;
    uint64_t style_key;
    uint64_t scale_key;
    int max_lines;
    PicoWrappedTextLine lines[PICO_WRAPPED_TEXT_LINE_CAPACITY];
    int line_count;
    bool truncated;
    bool valid;
} PicoWrappedText;

void PicoWrappedText_Free(PicoWrappedText *wrapped);
bool PicoWrappedText_Prepare(PicoWrappedText *wrapped, const char *text,
                             int text_length, float width, int max_lines,
                             uint64_t style_key, uint64_t scale_key,
                             PicoWrappedTextMeasureFn measure, void *measure_user);

#endif
