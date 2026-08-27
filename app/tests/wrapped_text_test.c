#include "wrapped_text.h"

#include <stdio.h>
#include <string.h>

typedef struct MeasureState {
    int calls;
} MeasureState;

static int Fail(const char *message)
{
    fprintf(stderr, "wrapped text: %s\n", message);
    return 1;
}

static float UnitMeasure(void *user, const char *text, int length)
{
    MeasureState *state = (MeasureState *)user;
    (void)text;
    (void)length;
    state->calls++;
    return 1.0f;
}

static int LineEquals(const PicoWrappedText *wrapped, int index,
                      const char *source, const char *expected)
{
    if (!wrapped || index < 0 || index >= wrapped->line_count)
    {
        return 0;
    }
    PicoWrappedTextLine line = wrapped->lines[index];
    size_t expected_length = strlen(expected);
    return line.length == (int)expected_length &&
           memcmp(source + line.start, expected, expected_length) == 0;
}

int main(void)
{
    PicoWrappedText wrapped = {0};
    MeasureState measure = {0};
    const char *words = "ab cd";

    if (!PicoWrappedText_Prepare(&wrapped, words, (int)strlen(words), 3.0f,
                                 48, 10, 20, UnitMeasure, &measure) ||
        wrapped.line_count != 2 || !LineEquals(&wrapped, 0, words, "ab") ||
        !LineEquals(&wrapped, 1, words, "cd") || wrapped.truncated)
    {
        PicoWrappedText_Free(&wrapped);
        return Fail("word wrapping did not preserve the expected line boundaries");
    }
    int measured = measure.calls;
    if (!PicoWrappedText_Prepare(&wrapped, words, (int)strlen(words), 3.0f,
                                 48, 10, 20, UnitMeasure, &measure) ||
        measure.calls != measured)
    {
        PicoWrappedText_Free(&wrapped);
        return Fail("cache hit repeated character measurement");
    }
    if (!PicoWrappedText_Prepare(&wrapped, words, (int)strlen(words), 2.0f,
                                 48, 10, 20, UnitMeasure, &measure) ||
        measure.calls == measured || wrapped.line_count != 3)
    {
        PicoWrappedText_Free(&wrapped);
        return Fail("width change did not invalidate cached wrapping");
    }
    measured = measure.calls;
    if (!PicoWrappedText_Prepare(&wrapped, words, (int)strlen(words), 2.0f,
                                 48, 11, 20, UnitMeasure, &measure) ||
        measure.calls == measured)
    {
        PicoWrappedText_Free(&wrapped);
        return Fail("font/style change did not invalidate cached wrapping");
    }
    measured = measure.calls;
    if (!PicoWrappedText_Prepare(&wrapped, words, (int)strlen(words), 2.0f,
                                 48, 11, 21, UnitMeasure, &measure) ||
        measure.calls == measured)
    {
        PicoWrappedText_Free(&wrapped);
        return Fail("font scale change did not invalidate cached wrapping");
    }

    const char *newlines = "a\n\nb";
    if (!PicoWrappedText_Prepare(&wrapped, newlines, (int)strlen(newlines),
                                 20.0f, 48, 11, 21, UnitMeasure, &measure) ||
        wrapped.line_count != 3 || !LineEquals(&wrapped, 0, newlines, "a") ||
        wrapped.lines[1].length != 0 || !LineEquals(&wrapped, 2, newlines, "b"))
    {
        PicoWrappedText_Free(&wrapped);
        return Fail("explicit blank lines were not preserved");
    }

    const char *token = "abcdef";
    if (!PicoWrappedText_Prepare(&wrapped, token, (int)strlen(token), 2.0f,
                                 2, 11, 21, UnitMeasure, &measure) ||
        wrapped.line_count != 2 || !wrapped.truncated ||
        !LineEquals(&wrapped, 0, token, "ab") ||
        !LineEquals(&wrapped, 1, token, "cd"))
    {
        PicoWrappedText_Free(&wrapped);
        return Fail("line limit did not preserve truncation behavior");
    }

    const char *utf8 = "åäö";
    if (!PicoWrappedText_Prepare(&wrapped, utf8, (int)strlen(utf8), 2.0f,
                                 48, 11, 21, UnitMeasure, &measure) ||
        wrapped.line_count != 2 || !LineEquals(&wrapped, 0, utf8, "åä") ||
        !LineEquals(&wrapped, 1, utf8, "ö"))
    {
        PicoWrappedText_Free(&wrapped);
        return Fail("UTF-8 codepoint boundaries were not preserved");
    }

    PicoWrappedText_Free(&wrapped);
    return 0;
}
