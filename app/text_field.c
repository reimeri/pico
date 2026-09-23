#include "text_field.h"

#include <limits.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

static void NoteEdit(PicoTextField *f)
{
    f->revision++;
}

static bool Reserve(PicoTextField *f, int length)
{
    int64_t needed;
    int64_t capacity;
    char *next;
    if (!f->bound || length < 0)
    {
        return false;
    }
    needed = (int64_t)length + 1;
    if (needed <= f->capacity)
    {
        return true;
    }
    if (!f->owns_text)
    {
        return false;
    }
    capacity = f->capacity > 0 ? f->capacity : 64;
    while (capacity < needed)
    {
        capacity *= 2;
    }
    if (capacity > INT_MAX)
    {
        return false;
    }
    next = (char *)realloc(f->text, (size_t)capacity);
    if (!next)
    {
        return false;
    }
    f->text = next;
    f->capacity = (int)capacity;
    return true;
}

static void ResetEditState(PicoTextField *f)
{
    f->length = f->text ? (int)strlen(f->text) : 0;
    f->cursor = f->anchor = f->length;
    f->scroll_x = 0.0f;
    f->granularity = 1;
    f->unit_from = f->unit_to = f->length;
    f->dragging = false;
    PicoClickSeq_Reset(&f->click_seq);
}

void PicoTextField_Bind(PicoTextField *f, char *buf, int capacity)
{
    if (!f)
    {
        return;
    }
    char *owned = f->owns_text ? f->text : NULL;
    *f = (PicoTextField){0};
    free(owned);
    f->text = buf;
    f->capacity = capacity;
    f->owns_text = false;
    f->bound = buf && capacity > 0;
    if (f->bound)
    {
        buf[capacity - 1] = '\0';
    }
    ResetEditState(f);
}

void PicoTextField_BindGrowable(PicoTextField *f)
{
    if (!f)
    {
        return;
    }
    char *owned = f->owns_text ? f->text : NULL;
    *f = (PicoTextField){0};
    free(owned);
    f->owns_text = true;
    f->bound = true;
    ResetEditState(f);
}

void PicoTextField_Unbind(PicoTextField *f)
{
    if (!f)
    {
        return;
    }
    char *owned = f->owns_text ? f->text : NULL;
    *f = (PicoTextField){0};
    free(owned);
}

void PicoTextField_Free(PicoTextField *f)
{
    PicoTextField_Unbind(f);
}

void PicoTextField_SetText(PicoTextField *f, const char *s)
{
    if (!f || !f->bound)
    {
        return;
    }
    if (!s)
    {
        s = "";
    }
    int n = (int)strlen(s);
    if (n + 1 > f->capacity && !Reserve(f, n))
    {
        n = f->capacity - 1;
        while (n > 0 && ((unsigned char)s[n] & 0xC0) == 0x80)
        {
            n--;
        }
    }
    for (int i = 0; i < n; i++)
    {
        f->text[i] = (unsigned char)s[i] < 32 ? ' ' : s[i];
    }
    f->text[n] = '\0';
    ResetEditState(f);
    NoteEdit(f);
}

void PicoTextField_Refresh(PicoTextField *f)
{
    if (!f || !f->bound || !f->text || f->capacity <= 0)
    {
        return;
    }
    f->text[f->capacity - 1] = '\0';
    f->length = (int)strlen(f->text);
    if (f->cursor > f->length)
    {
        f->cursor = f->length;
    }
    if (f->anchor > f->length)
    {
        f->anchor = f->length;
    }
}

bool PicoTextField_HasSelection(const PicoTextField *f)
{
    return f && f->anchor != f->cursor;
}

int PicoTextField_SelFrom(const PicoTextField *f)
{
    return f->anchor < f->cursor ? f->anchor : f->cursor;
}

int PicoTextField_SelTo(const PicoTextField *f)
{
    return f->anchor > f->cursor ? f->anchor : f->cursor;
}

void PicoTextField_Move(PicoTextField *f, int pos, bool extend)
{
    if (!f || !f->bound)
    {
        return;
    }
    if (pos < 0)
    {
        pos = 0;
    }
    if (pos > f->length)
    {
        pos = f->length;
    }
    f->cursor = pos;
    if (!extend)
    {
        f->anchor = pos;
    }
}

void PicoTextField_SelectAll(PicoTextField *f)
{
    if (!f || !f->bound)
    {
        return;
    }
    f->anchor = 0;
    f->cursor = f->length;
}

void PicoTextField_DeleteRange(PicoTextField *f, int from, int to)
{
    if (!f || !f->bound || !f->text)
    {
        return;
    }
    if (from < 0)
    {
        from = 0;
    }
    if (to > f->length)
    {
        to = f->length;
    }
    if (to <= from)
    {
        return;
    }
    memmove(f->text + from, f->text + to, (size_t)(f->length - to));
    f->length -= to - from;
    f->text[f->length] = '\0';
    f->cursor = f->anchor = from;
    NoteEdit(f);
}

void PicoTextField_DeleteSelection(PicoTextField *f)
{
    if (PicoTextField_HasSelection(f))
    {
        PicoTextField_DeleteRange(f, PicoTextField_SelFrom(f), PicoTextField_SelTo(f));
    }
}

bool PicoTextField_Insert(PicoTextField *f, const char *bytes, int n)
{
    if (!f || !f->bound || !bytes || n <= 0)
    {
        return false;
    }
    PicoTextField_DeleteSelection(f);
    int room = f->capacity - 1 - f->length;
    if (n > room && (int64_t)f->length + n + 1 > INT_MAX)
    {
        return false;
    }
    if (n > room && !Reserve(f, f->length + n))
    {
        n = room;
        while (n > 0 && ((unsigned char)bytes[n] & 0xC0) == 0x80)
        {
            n--;
        }
    }
    if (n <= 0)
    {
        return false;
    }
    memmove(f->text + f->cursor + n, f->text + f->cursor, (size_t)(f->length - f->cursor));
    memcpy(f->text + f->cursor, bytes, (size_t)n);
    f->length += n;
    f->cursor += n;
    f->anchor = f->cursor;
    f->text[f->length] = '\0';
    NoteEdit(f);
    return true;
}

bool PicoTextField_DeleteBackward(PicoTextField *f, bool word)
{
    if (!f || !f->bound || !f->text)
    {
        return false;
    }
    if (PicoTextField_HasSelection(f))
    {
        PicoTextField_DeleteSelection(f);
        return true;
    }
    int from = word ? PicoText_PrevWord(f->text, f->cursor) : PicoText_Utf8Prev(f->text, f->cursor);
    if (from >= f->cursor)
    {
        return false;
    }
    PicoTextField_DeleteRange(f, from, f->cursor);
    return true;
}

bool PicoTextField_DeleteForward(PicoTextField *f, bool word)
{
    if (!f || !f->bound || !f->text)
    {
        return false;
    }
    if (PicoTextField_HasSelection(f))
    {
        PicoTextField_DeleteSelection(f);
        return true;
    }
    int to = word ? PicoText_NextWord(f->text, f->length, f->cursor)
                  : PicoText_Utf8Next(f->text, f->length, f->cursor);
    if (to <= f->cursor)
    {
        return false;
    }
    PicoTextField_DeleteRange(f, f->cursor, to);
    return true;
}

bool PicoTextField_PasteText(PicoTextField *f, const char *text)
{
    if (!f || !f->bound || !text || !text[0])
    {
        return false;
    }
    int n = (int)strlen(text);
    char *copy = (char *)malloc((size_t)n + 1);
    if (!copy)
    {
        return false;
    }
    for (int i = 0; i < n; i++)
    {
        copy[i] = (unsigned char)text[i] < 32 ? ' ' : text[i];
    }
    copy[n] = '\0';
    bool ok = PicoTextField_Insert(f, copy, n);
    free(copy);
    return ok;
}

int PicoTextField_CopyText(const PicoTextField *f, char *out, int cap)
{
    if (!out || cap <= 0)
    {
        return 0;
    }
    out[0] = '\0';
    if (!PicoTextField_HasSelection(f) || !f->text)
    {
        return 0;
    }
    int from = PicoTextField_SelFrom(f);
    int n = PicoTextField_SelTo(f) - from;
    if (n > cap - 1)
    {
        n = cap - 1;
        while (n > 0 && ((unsigned char)f->text[from + n] & 0xC0) == 0x80)
        {
            n--;
        }
    }
    memcpy(out, f->text + from, (size_t)n);
    out[n] = '\0';
    return n;
}

static void UnitRange(const PicoTextField *f, int pos, int granularity, int *from, int *to)
{
    const char *text = f->text ? f->text : "";
    if (granularity >= 3)
    {
        PicoText_ParaRange(text, f->length, pos, from, to);
    }
    else
    {
        PicoText_WordRange(text, f->length, pos, from, to);
    }
}

void PicoTextField_SelectUnit(PicoTextField *f, int pos, int granularity, bool extend)
{
    if (!f || !f->bound)
    {
        return;
    }
    f->granularity = granularity;
    if (granularity <= 1)
    {
        f->unit_from = pos;
        f->unit_to = pos;
        PicoTextField_Move(f, pos, extend);
        return;
    }
    int from = pos;
    int to = pos;
    UnitRange(f, pos, granularity, &from, &to);
    f->unit_from = from;
    f->unit_to = to;
    f->anchor = from;
    f->cursor = to;
}

void PicoTextField_ExtendUnit(PicoTextField *f, int pos)
{
    if (!f || !f->bound)
    {
        return;
    }
    if (f->granularity <= 1)
    {
        PicoTextField_Move(f, pos, true);
        return;
    }
    int from = pos;
    int to = pos;
    UnitRange(f, pos, f->granularity, &from, &to);
    int span_from = 0;
    int span_to = 0;
    PicoText_UnionRange(f->unit_from, f->unit_to, from, to, &span_from, &span_to);
    if (pos >= f->unit_from)
    {
        f->anchor = f->unit_from;
        f->cursor = span_to;
    }
    else
    {
        f->anchor = f->unit_to;
        f->cursor = span_from;
    }
}
