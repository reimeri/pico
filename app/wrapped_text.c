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

static float MeasureBytes(const char *text, int length,
                          PicoWrappedTextMeasureFn measure, void *measure_user)
{
    float width = 0.0f;
    int position = 0;
    if (!text || length <= 0 || !measure)
    {
        return 0.0f;
    }
    while (position < length)
    {
        int next = Utf8Step(text, length, position);
        width += measure(measure_user, text + position, next - position);
        position = next;
    }
    return width;
}

static bool WriteParts(char *out, int out_size, int *out_length, const char *a,
                       int alen, const char *b, int blen, const char *c, int clen,
                       const char *d, int dlen)
{
    int total = alen + blen + clen + dlen;
    if (out_length)
    {
        *out_length = 0;
    }
    if (!out || out_size <= 0)
    {
        return false;
    }
    if (alen < 0 || blen < 0 || clen < 0 || dlen < 0 || total < 0 ||
        total > out_size - 1)
    {
        out[0] = '\0';
        return false;
    }
    int n = 0;
    if (alen > 0 && a)
    {
        memcpy(out + n, a, (size_t)alen);
        n += alen;
    }
    if (blen > 0 && b)
    {
        memcpy(out + n, b, (size_t)blen);
        n += blen;
    }
    if (clen > 0 && c)
    {
        memcpy(out + n, c, (size_t)clen);
        n += clen;
    }
    if (dlen > 0 && d)
    {
        memcpy(out + n, d, (size_t)dlen);
        n += dlen;
    }
    out[n] = '\0';
    if (out_length)
    {
        *out_length = n;
    }
    return true;
}

static bool WritePrefixEllipsis(const char *text, int text_length, float width,
                                PicoWrappedTextMeasureFn measure, void *measure_user,
                                char *out, int out_size, int *out_length)
{
    static const char ellipsis[] = "\xE2\x80\xA6";
    int prefix = 0;
    PicoWrappedText_Fits(text, text_length, width, measure, measure_user, &prefix);
    if (prefix > 0)
    {
        return WriteParts(out, out_size, out_length, text, prefix, ellipsis, 3, NULL,
                          0, NULL, 0);
    }
    if (MeasureBytes(ellipsis, 3, measure, measure_user) <= width)
    {
        return WriteParts(out, out_size, out_length, ellipsis, 3, NULL, 0, NULL, 0,
                          NULL, 0);
    }
    return WriteParts(out, out_size, out_length, NULL, 0, NULL, 0, NULL, 0, NULL, 0);
}

bool PicoWrappedText_Shorten(const char *text, int text_length, float width,
                             PicoWrappedTextMeasureFn measure, void *measure_user,
                             char *out, int out_size, int *out_length)
{
    static const char ellipsis[] = "\xE2\x80\xA6";
    static const char slash[] = "/";

    if (out_length)
    {
        *out_length = 0;
    }
    if (out && out_size > 0)
    {
        out[0] = '\0';
    }
    if (!text || text_length <= 0)
    {
        return true;
    }
    if (!out || out_size <= 1)
    {
        return false;
    }
    if (!measure || !(width > 0.0f))
    {
        int n = text_length < out_size - 1 ? text_length : out_size - 1;
        WriteParts(out, out_size, out_length, text, n, NULL, 0, NULL, 0, NULL, 0);
        return false;
    }
    if (MeasureBytes(text, text_length, measure, measure_user) <= width)
    {
        WriteParts(out, out_size, out_length, text, text_length, NULL, 0, NULL, 0,
                   NULL, 0);
        return true;
    }

    int last_slash = -1;
    for (int i = 0; i < text_length; i++)
    {
        if (text[i] == '/')
        {
            last_slash = i;
        }
    }
    if (last_slash < 0 || last_slash >= text_length - 1)
    {
        WritePrefixEllipsis(text, text_length, width, measure, measure_user, out,
                            out_size, out_length);
        return false;
    }

    const char *filename = text + last_slash + 1;
    int file_len = text_length - last_slash - 1;
    float ellipsis_w = MeasureBytes(ellipsis, 3, measure, measure_user);
    float slash_w = MeasureBytes(slash, 1, measure, measure_user);
    float file_w = MeasureBytes(filename, file_len, measure, measure_user);

    for (int s = last_slash - 1; s >= 0; s--)
    {
        if (text[s] != '/')
        {
            continue;
        }
        int prefix_len = s + 1;
        float candidate_w = MeasureBytes(text, prefix_len, measure, measure_user) +
                            ellipsis_w + slash_w + file_w;
        if (candidate_w <= width &&
            WriteParts(out, out_size, out_length, text, prefix_len, ellipsis, 3, slash,
                       1, filename, file_len))
        {
            return false;
        }
    }

    if (ellipsis_w + slash_w + file_w <= width &&
        WriteParts(out, out_size, out_length, ellipsis, 3, slash, 1, filename, file_len,
                   NULL, 0))
    {
        return false;
    }

    int start = 0;
    while (start < file_len)
    {
        int remain = file_len - start;
        if (ellipsis_w + MeasureBytes(filename + start, remain, measure, measure_user) <=
                width &&
            WriteParts(out, out_size, out_length, ellipsis, 3, filename + start, remain,
                       NULL, 0, NULL, 0))
        {
            return false;
        }
        int next = Utf8Step(filename, file_len, start);
        if (next <= start)
        {
            break;
        }
        start = next;
    }

    if (ellipsis_w <= width)
    {
        WriteParts(out, out_size, out_length, ellipsis, 3, NULL, 0, NULL, 0, NULL, 0);
    }
    else
    {
        WriteParts(out, out_size, out_length, NULL, 0, NULL, 0, NULL, 0, NULL, 0);
    }
    return false;
}
