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

/* Single-line measurement. Returns true when the whole text fits inside
 * width. When it does not, returns false and writes the byte length of the
 * longest UTF-8 prefix that fits once an ellipsis is appended to
 * *prefix_length (0 when not even one codepoint fits next to the ellipsis). */
bool PicoWrappedText_Fits(const char *text, int text_length, float width,
                          PicoWrappedTextMeasureFn measure, void *measure_user,
                          int *prefix_length);

#endif
