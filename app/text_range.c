#include "text_range.h"

#include <ctype.h>

#define PICO_CLICK_SEC 0.5
#define PICO_CLICK_SLOP2 16.0f

bool PicoText_IsWordByte(unsigned char c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c >= 0x80;
}

static void ClampPos(int len, int *pos)
{
    if (*pos < 0)
    {
        *pos = 0;
    }
    if (len <= 0)
    {
        *pos = 0;
        return;
    }
    if (*pos >= len)
    {
        *pos = len - 1;
    }
}

void PicoText_WordRange(const char *s, int len, int pos, int *from, int *to)
{
    if (!from || !to)
    {
        return;
    }
    if (!s || len <= 0)
    {
        *from = 0;
        *to = 0;
        return;
    }
    ClampPos(len, &pos);
    unsigned char c = (unsigned char)s[pos];
    int start = pos;
    int end = pos + 1;
    if (isspace(c))
    {
        while (start > 0 && isspace((unsigned char)s[start - 1]))
        {
            start--;
        }
        while (end < len && isspace((unsigned char)s[end]))
        {
            end++;
        }
    }
    else if (PicoText_IsWordByte(c))
    {
        while (start > 0 && PicoText_IsWordByte((unsigned char)s[start - 1]))
        {
            start--;
        }
        while (end < len && PicoText_IsWordByte((unsigned char)s[end]))
        {
            end++;
        }
    }
    else
    {
        while (start > 0 && !PicoText_IsWordByte((unsigned char)s[start - 1]) &&
               !isspace((unsigned char)s[start - 1]))
        {
            start--;
        }
        while (end < len && !PicoText_IsWordByte((unsigned char)s[end]) && !isspace((unsigned char)s[end]))
        {
            end++;
        }
    }
    *from = start;
    *to = end;
}

void PicoText_ParaRange(const char *s, int len, int pos, int *from, int *to)
{
    if (!from || !to)
    {
        return;
    }
    if (!s || len <= 0)
    {
        *from = 0;
        *to = 0;
        return;
    }
    if (pos < 0)
    {
        pos = 0;
    }
    if (pos > len)
    {
        pos = len;
    }
    int start = pos;
    while (start > 0 && s[start - 1] != '\n')
    {
        start--;
    }
    int end = pos;
    while (end < len && s[end] != '\n')
    {
        end++;
    }
    *from = start;
    *to = end;
}

void PicoText_UnionRange(int a0, int a1, int b0, int b1, int *from, int *to)
{
    if (!from || !to)
    {
        return;
    }
    *from = a0 < b0 ? a0 : b0;
    *to = a1 > b1 ? a1 : b1;
}

int PicoClickSeq_Press(PicoClickSeq *seq, double now, float x, float y)
{
    if (!seq)
    {
        return 1;
    }
    float dx = x - seq->last_x;
    float dy = y - seq->last_y;
    bool clustered = seq->count > 0 && (now - seq->last_time) <= PICO_CLICK_SEC && (dx * dx + dy * dy) <= PICO_CLICK_SLOP2;
    int count = clustered ? seq->count + 1 : 1;
    if (count > 3)
    {
        count = 1;
    }
    seq->count = count;
    seq->last_time = now;
    seq->last_x = x;
    seq->last_y = y;
    return count;
}

void PicoClickSeq_Reset(PicoClickSeq *seq)
{
    if (!seq)
    {
        return;
    }
    seq->count = 0;
    seq->last_time = 0;
    seq->last_x = 0;
    seq->last_y = 0;
}
