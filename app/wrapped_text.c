#include "wrapped_text.h"

#include <string.h>

static int Utf8Step(const char *text, int length, int position)
{
    if (position >= length)
    {
        return length;
    }
    unsigned char c = (unsigned char)text[position];
    int step = 1;
    if ((c & 0xE0) == 0xC0)
    {
        step = 2;
    }
    else if ((c & 0xF0) == 0xE0)
    {
        step = 3;
    }
    else if ((c & 0xF8) == 0xF0)
    {
        step = 4;
    }
    position += step;
    return position > length ? length : position;
}

void PicoWrappedText_Free(PicoWrappedText *wrapped)
{
    if (wrapped)
    {
        memset(wrapped, 0, sizeof(*wrapped));
    }
}

bool PicoWrappedText_Prepare(PicoWrappedText *wrapped, const char *text,
                             int text_length, float width, int max_lines,
                             uint64_t style_key, uint64_t scale_key,
                             PicoWrappedTextMeasureFn measure, void *measure_user)
{
    if (!wrapped || !text || text_length <= 0 || width <= 0.0f ||
        max_lines <= 0 || max_lines > PICO_WRAPPED_TEXT_LINE_CAPACITY || !measure)
    {
        return false;
    }
    if (wrapped->valid && wrapped->text == text &&
        wrapped->text_length == text_length && wrapped->width == width &&
        wrapped->style_key == style_key && wrapped->scale_key == scale_key &&
        wrapped->max_lines == max_lines)
    {
        return true;
    }
    int position = 0;
    int line_count = 0;
    while (position < text_length && line_count < max_lines)
    {
        if (text[position] == '\n')
        {
            wrapped->lines[line_count++] = (PicoWrappedTextLine){.start = position,
                                                                 .length = 0};
            position++;
            continue;
        }

        int line_start = position;
        float line_width = 0.0f;
        int break_at = -1;
        int break_resume = -1;
        bool did_wrap = false;
        while (position < text_length && text[position] != '\n')
        {
            int next = Utf8Step(text, text_length, position);
            float character_width = measure(measure_user, text + position,
                                            next - position);
            if (line_width + character_width > width && position > line_start)
            {
                int end = break_at > line_start ? break_at : position;
                wrapped->lines[line_count++] =
                    (PicoWrappedTextLine){.start = line_start,
                                          .length = end - line_start};
                position = break_at > line_start ? break_resume : position;
                did_wrap = true;
                break;
            }
            line_width += character_width;
            if (text[position] == ' ' || text[position] == '\t')
            {
                break_at = position;
                break_resume = next;
            }
            position = next;
        }
        if (!did_wrap)
        {
            wrapped->lines[line_count++] =
                (PicoWrappedTextLine){.start = line_start,
                                      .length = position - line_start};
            if (position < text_length && text[position] == '\n')
            {
                position++;
            }
        }
    }

    wrapped->text = text;
    wrapped->text_length = text_length;
    wrapped->width = width;
    wrapped->style_key = style_key;
    wrapped->scale_key = scale_key;
    wrapped->max_lines = max_lines;
    wrapped->line_count = line_count;
    wrapped->truncated = position < text_length;
    wrapped->valid = true;
    return true;
}

bool PicoWrappedText_Fits(const char *text, int text_length, float width,
                          PicoWrappedTextMeasureFn measure, void *measure_user,
                          int *prefix_length)
{
    if (prefix_length)
    {
        *prefix_length = 0;
    }
    if (!text || text_length <= 0)
    {
        return true;
    }
    if (!measure || !(width > 0.0f))
    {
        return false;
    }
    float line_width = 0.0f;
    int position = 0;
    while (position < text_length)
    {
        int next = Utf8Step(text, text_length, position);
        float character_width = measure(measure_user, text + position, next - position);
        if (line_width + character_width > width)
        {
            break;
        }
        line_width += character_width;
        position = next;
    }
    if (position >= text_length)
    {
        if (prefix_length)
        {
            *prefix_length = text_length;
        }
        return true;
    }
    static const char ellipsis[] = "\xE2\x80\xA6";
    float ellipsis_width = measure(measure_user, ellipsis, 3);
    float prefix_width = 0.0f;
    int prefix = 0;
    position = 0;
    while (position < text_length)
    {
        int next = Utf8Step(text, text_length, position);
        float character_width = measure(measure_user, text + position, next - position);
        if (prefix_width + character_width + ellipsis_width > width)
        {
            break;
        }
        prefix_width += character_width;
        position = next;
        prefix = position;
    }
    if (prefix_length)
    {
        *prefix_length = prefix;
    }
    return false;
}
