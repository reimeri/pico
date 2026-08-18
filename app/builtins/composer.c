#include "pico/app.h"

#include "clay/clay.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define PASTE_TEMP_THRESHOLD 4096

static bool IsCtrlDown(void)
{
    return IsKeyDown(KEY_LEFT_CONTROL) || IsKeyDown(KEY_RIGHT_CONTROL);
}

static bool IsShiftDown(void)
{
    return IsKeyDown(KEY_LEFT_SHIFT) || IsKeyDown(KEY_RIGHT_SHIFT);
}

static int Utf8Next(const char *s, int length, int pos)
{
    if (pos >= length)
    {
        return length;
    }
    unsigned char c = (unsigned char)s[pos];
    int step = 1;
    if ((c & 0x80) == 0)
    {
        step = 1;
    }
    else if ((c & 0xE0) == 0xC0)
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
    pos += step;
    if (pos > length)
    {
        pos = length;
    }
    return pos;
}

static int Utf8Prev(const char *s, int pos)
{
    if (pos <= 0)
    {
        return 0;
    }
    pos--;
    while (pos > 0 && ((unsigned char)s[pos] & 0xC0) == 0x80)
    {
        pos--;
    }
    return pos;
}

static bool IsWordByte(unsigned char c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c >= 0x80;
}

static int PrevWord(const char *s, int pos)
{
    while (pos > 0 && isspace((unsigned char)s[pos - 1]))
    {
        pos--;
    }
    while (pos > 0 && IsWordByte((unsigned char)s[pos - 1]))
    {
        pos--;
    }
    if (pos > 0 && !IsWordByte((unsigned char)s[pos - 1]) && !isspace((unsigned char)s[pos - 1]))
    {
        pos--;
        while (pos > 0 && !IsWordByte((unsigned char)s[pos - 1]) && !isspace((unsigned char)s[pos - 1]))
        {
            pos--;
        }
    }
    return pos;
}

static int NextWord(const char *s, int length, int pos)
{
    while (pos < length && isspace((unsigned char)s[pos]))
    {
        pos++;
    }
    while (pos < length && IsWordByte((unsigned char)s[pos]))
    {
        pos++;
    }
    if (pos < length && !IsWordByte((unsigned char)s[pos]) && !isspace((unsigned char)s[pos]))
    {
        while (pos < length && !IsWordByte((unsigned char)s[pos]) && !isspace((unsigned char)s[pos]))
        {
            pos++;
        }
    }
    return pos;
}

static int LineStart(const char *s, int pos)
{
    while (pos > 0 && s[pos - 1] != '\n')
    {
        pos--;
    }
    return pos;
}

static int LineEnd(const char *s, int length, int pos)
{
    while (pos < length && s[pos] != '\n')
    {
        pos++;
    }
    return pos;
}

static void ComposerReserve(PicoComposer *c, int extra)
{
    int needed = c->length + extra + 1;
    if (needed <= c->capacity)
    {
        return;
    }
    int capacity = c->capacity == 0 ? 256 : c->capacity;
    while (capacity < needed)
    {
        capacity *= 2;
    }
    c->text = (char *)realloc(c->text, (size_t)capacity);
    c->capacity = capacity;
    if (!c->text)
    {
        c->capacity = 0;
        c->length = 0;
        c->cursor = 0;
    }
}

static void ComposerInsert(PicoComposer *c, const char *bytes, int nbytes)
{
    if (nbytes <= 0)
    {
        return;
    }
    ComposerReserve(c, nbytes);
    if (!c->text)
    {
        return;
    }
    memmove(c->text + c->cursor + nbytes, c->text + c->cursor, (size_t)(c->length - c->cursor));
    memcpy(c->text + c->cursor, bytes, (size_t)nbytes);
    c->length += nbytes;
    c->cursor += nbytes;
    c->text[c->length] = '\0';
}

static void ComposerDeleteRange(PicoComposer *c, int from, int to)
{
    if (from < 0)
    {
        from = 0;
    }
    if (to > c->length)
    {
        to = c->length;
    }
    if (to <= from)
    {
        return;
    }
    memmove(c->text + from, c->text + to, (size_t)(c->length - to));
    c->length -= (to - from);
    c->cursor = from;
    c->text[c->length] = '\0';
}

static int Utf8Encode(int cp, char out[4])
{
    if (cp < 0x80)
    {
        out[0] = (char)cp;
        return 1;
    }
    if (cp < 0x800)
    {
        out[0] = (char)(0xC0 | (cp >> 6));
        out[1] = (char)(0x80 | (cp & 0x3F));
        return 2;
    }
    if (cp < 0x10000)
    {
        out[0] = (char)(0xE0 | (cp >> 12));
        out[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
        out[2] = (char)(0x80 | (cp & 0x3F));
        return 3;
    }
    out[0] = (char)(0xF0 | (cp >> 18));
    out[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
    out[2] = (char)(0x80 | ((cp >> 6) & 0x3F));
    out[3] = (char)(0x80 | (cp & 0x3F));
    return 4;
}

static void PasteClipboard(PicoComposer *c)
{
    const char *clip = GetClipboardText();
    if (!clip || clip[0] == '\0')
    {
        return;
    }
    int len = (int)strlen(clip);
    if (len >= PASTE_TEMP_THRESHOLD)
    {
        char tmpl[] = "/tmp/pico-paste-XXXXXX";
        int fd = mkstemp(tmpl);
        if (fd >= 0)
        {
            ssize_t written = write(fd, clip, (size_t)len);
            close(fd);
            if (written == (ssize_t)len)
            {
                ComposerInsert(c, tmpl, (int)strlen(tmpl));
                return;
            }
        }
    }
    ComposerInsert(c, clip, len);
}

void PicoComposer_HandleInput(PicoApp *app)
{
    PicoComposer *c = &app->composer;
    bool ctrl = IsCtrlDown();
    bool repeat_left = IsKeyPressed(KEY_LEFT) || IsKeyPressedRepeat(KEY_LEFT);
    bool repeat_right = IsKeyPressed(KEY_RIGHT) || IsKeyPressedRepeat(KEY_RIGHT);
    bool repeat_back = IsKeyPressed(KEY_BACKSPACE) || IsKeyPressedRepeat(KEY_BACKSPACE);
    bool repeat_del = IsKeyPressed(KEY_DELETE) || IsKeyPressedRepeat(KEY_DELETE);

    if (ctrl && (IsKeyPressed(KEY_A) || IsKeyPressed(KEY_HOME)))
    {
        c->cursor = LineStart(c->text ? c->text : "", c->cursor);
    }
    else if (IsKeyPressed(KEY_HOME))
    {
        c->cursor = LineStart(c->text ? c->text : "", c->cursor);
    }

    if (ctrl && (IsKeyPressed(KEY_E) || IsKeyPressed(KEY_END)))
    {
        c->cursor = LineEnd(c->text ? c->text : "", c->length, c->cursor);
    }
    else if (IsKeyPressed(KEY_END))
    {
        c->cursor = LineEnd(c->text ? c->text : "", c->length, c->cursor);
    }

    if (repeat_left)
    {
        if (ctrl)
        {
            c->cursor = PrevWord(c->text ? c->text : "", c->cursor);
        }
        else
        {
            c->cursor = Utf8Prev(c->text ? c->text : "", c->cursor);
        }
    }
    if (repeat_right)
    {
        if (ctrl)
        {
            c->cursor = NextWord(c->text ? c->text : "", c->length, c->cursor);
        }
        else
        {
            c->cursor = Utf8Next(c->text ? c->text : "", c->length, c->cursor);
        }
    }

    if (ctrl && (IsKeyPressed(KEY_W) || IsKeyPressedRepeat(KEY_W)))
    {
        int from = PrevWord(c->text ? c->text : "", c->cursor);
        ComposerDeleteRange(c, from, c->cursor);
    }
    else if (repeat_back)
    {
        int from = Utf8Prev(c->text ? c->text : "", c->cursor);
        ComposerDeleteRange(c, from, c->cursor);
    }

    if (ctrl && IsKeyPressed(KEY_K))
    {
        int to = LineEnd(c->text ? c->text : "", c->length, c->cursor);
        if (to == c->cursor && to < c->length && (c->text ? c->text[to] : 0) == '\n')
        {
            to++;
        }
        ComposerDeleteRange(c, c->cursor, to);
    }
    else if (repeat_del)
    {
        int to = Utf8Next(c->text ? c->text : "", c->length, c->cursor);
        ComposerDeleteRange(c, c->cursor, to);
    }

    if (ctrl && IsKeyPressed(KEY_V))
    {
        PasteClipboard(app->composer.text ? &app->composer : c);
    }

    if ((IsKeyPressed(KEY_ENTER) || IsKeyPressed(KEY_KP_ENTER)) && !IsShiftDown())
    {
        PicoApp_Submit(app);
        return;
    }

    if ((IsKeyPressed(KEY_ENTER) || IsKeyPressed(KEY_KP_ENTER)) && IsShiftDown())
    {
        ComposerInsert(c, "\n", 1);
    }

    if (IsKeyPressed(KEY_TAB))
    {
        ComposerInsert(c, "  ", 2);
    }

    if (!ctrl)
    {
        int cp;
        while ((cp = GetCharPressed()) != 0)
        {
            if (cp < 32)
            {
                continue;
            }
            char bytes[4];
            int n = Utf8Encode(cp, bytes);
            ComposerInsert(c, bytes, n);
        }
    }
}

void PicoComposer_Render(PicoApp *app)
{
    PicoComposer *c = &app->composer;
    static char display[1 << 16];
    const char *shown;
    int shown_len;

    bool blink = ((int)(GetTime() * 2.0) & 1) == 0;
    if (c->length == 0)
    {
        if (blink)
        {
            snprintf(display, sizeof(display), "|");
            shown = display;
            shown_len = 1;
        }
        else
        {
            shown = "Message Pico…  (Enter to send, Shift+Enter for newline)";
            shown_len = (int)strlen(shown);
        }
    }
    else
    {
        int cursor = c->cursor;
        if (cursor < 0)
        {
            cursor = 0;
        }
        if (cursor > c->length)
        {
            cursor = c->length;
        }
        if (c->length + 2 < (int)sizeof(display))
        {
            memcpy(display, c->text, (size_t)cursor);
            int n = 0;
            if (blink)
            {
                display[cursor] = '|';
                n = 1;
            }
            memcpy(display + cursor + n, c->text + cursor, (size_t)(c->length - cursor));
            shown_len = c->length + n;
            display[shown_len] = '\0';
            shown = display;
        }
        else
        {
            shown = c->text;
            shown_len = c->length;
        }
    }

    Clay_Color text_color = (c->length == 0 && !blink) ? COLOR_MUTED : COLOR_TEXT;

    CLAY(CLAY_ID("Composer"),
         {.layout = {.layoutDirection = CLAY_TOP_TO_BOTTOM,
                     .padding = {14, 14, 10, 10},
                     .sizing = {.width = CLAY_SIZING_GROW(0), .height = CLAY_SIZING_FIT(56, 160)}},
          .backgroundColor = COLOR_COMPOSER_BG,
          .cornerRadius = CLAY_CORNER_RADIUS(8)})
    {
        Clay_String text = {.length = shown_len, .chars = shown};
        CLAY_TEXT(text, CLAY_TEXT_CONFIG({.fontId = FONT_REGULAR,
                                          .fontSize = 16,
                                          .textColor = text_color,
                                          .wrapMode = CLAY_TEXT_WRAP_WORDS}));
    }
}
