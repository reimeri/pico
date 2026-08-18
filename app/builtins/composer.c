#include "pico/plugin.h"

#include "clay/clay.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define PASTE_TEMP_THRESHOLD 4096
#define COMPOSER_PAD_X 14
#define COMPOSER_PAD_Y 10
#define COMPOSER_FONT_SIZE 16
#define COMPOSER_MAX_LINES 64

typedef struct CompLine {
    int start;
    int length;
} CompLine;

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
    pos += step;
    return pos > length ? length : pos;
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

static float MeasureSlice(Font font, const char *s, int start, int length, float font_size)
{
    if (length <= 0)
    {
        return 0;
    }
    char saved = ((char *)s)[start + length];
    ((char *)s)[start + length] = '\0';
    Vector2 size = MeasureTextEx(font, s + start, font_size, 0);
    ((char *)s)[start + length] = saved;
    return size.x;
}

static int WrapComposer(const PicoComposer *c, Font font, float max_width, CompLine *lines, int max_lines,
                        float *line_height)
{
    Vector2 sample = MeasureTextEx(font, "Hg", COMPOSER_FONT_SIZE, 0);
    *line_height = sample.y > 1 ? sample.y : (float)COMPOSER_FONT_SIZE;
    if (!c->text || c->length == 0)
    {
        lines[0].start = 0;
        lines[0].length = 0;
        return 1;
    }

    int line_count = 0;
    int i = 0;
    while (i < c->length && line_count < max_lines)
    {
        int line_start = i;
        if (c->text[i] == '\n')
        {
            lines[line_count].start = line_start;
            lines[line_count].length = 0;
            line_count++;
            i++;
            continue;
        }

        float width = 0;
        int break_at = -1;
        int break_resume = -1;
        int wrapped = 0;
        while (i < c->length && c->text[i] != '\n')
        {
            int next = Utf8Next(c->text, c->length, i);
            float ch_w = MeasureSlice(font, c->text, i, next - i, COMPOSER_FONT_SIZE);
            if (width + ch_w > max_width && i > line_start)
            {
                if (break_at > line_start)
                {
                    lines[line_count].start = line_start;
                    lines[line_count].length = break_at - line_start;
                    line_count++;
                    i = break_resume;
                }
                else
                {
                    lines[line_count].start = line_start;
                    lines[line_count].length = i - line_start;
                    line_count++;
                }
                wrapped = 1;
                break;
            }
            width += ch_w;
            if (c->text[i] == ' ' || c->text[i] == '\t')
            {
                break_at = i;
                break_resume = next;
            }
            i = next;
        }
        if (!wrapped)
        {
            lines[line_count].start = line_start;
            lines[line_count].length = i - line_start;
            line_count++;
            if (i < c->length && c->text[i] == '\n')
            {
                i++;
            }
        }
    }
    if (line_count == 0)
    {
        lines[0].start = 0;
        lines[0].length = c->length;
        return 1;
    }
    return line_count;
}

static int OffsetAtPoint(PicoApp *app, float x, float y)
{
    PicoComposer *c = &app->composer;
    Clay_ElementData box = Clay_GetElementData(Clay_GetElementId(CLAY_STRING("Composer")));
    if (!box.found)
    {
        return c->cursor;
    }
    float local_x = x - box.boundingBox.x - COMPOSER_PAD_X;
    float local_y = y - box.boundingBox.y - COMPOSER_PAD_Y;
    float max_width = box.boundingBox.width - COMPOSER_PAD_X * 2;
    if (max_width < 10)
    {
        max_width = 10;
    }

    CompLine lines[COMPOSER_MAX_LINES];
    float line_height = COMPOSER_FONT_SIZE;
    int line_count = WrapComposer(c, app->fonts[FONT_REGULAR], max_width, lines, COMPOSER_MAX_LINES, &line_height);
    if (local_y < 0)
    {
        return 0;
    }
    int line_i = (int)(local_y / line_height);
    if (line_i >= line_count)
    {
        return c->length;
    }
    if (line_i < 0)
    {
        line_i = 0;
    }
    CompLine line = lines[line_i];
    if (local_x <= 0)
    {
        return line.start;
    }
    float width = 0;
    int pos = line.start;
    int end = line.start + line.length;
    while (pos < end)
    {
        int next = Utf8Next(c->text, c->length, pos);
        float ch_w = MeasureSlice(app->fonts[FONT_REGULAR], c->text, pos, next - pos, COMPOSER_FONT_SIZE);
        if (width + ch_w * 0.5f >= local_x)
        {
            return pos;
        }
        width += ch_w;
        pos = next;
    }
    if (line_i < line_count - 1)
    {
        return end;
    }
    return end;
}

static void CaretPos(PicoApp *app, float *out_x, float *out_y, float *out_h)
{
    PicoComposer *c = &app->composer;
    Clay_ElementData box = Clay_GetElementData(Clay_GetElementId(CLAY_STRING("Composer")));
    *out_x = box.boundingBox.x + COMPOSER_PAD_X;
    *out_y = box.boundingBox.y + COMPOSER_PAD_Y;
    *out_h = (float)COMPOSER_FONT_SIZE;
    if (!box.found)
    {
        return;
    }
    float max_width = box.boundingBox.width - COMPOSER_PAD_X * 2;
    CompLine lines[COMPOSER_MAX_LINES];
    float line_height = COMPOSER_FONT_SIZE;
    int line_count = WrapComposer(c, app->fonts[FONT_REGULAR], max_width, lines, COMPOSER_MAX_LINES, &line_height);
    *out_h = line_height;
    int cursor = c->cursor;
    int line_i = 0;
    for (int i = 0; i < line_count; i++)
    {
        if (cursor >= lines[i].start)
        {
            line_i = i;
        }
    }
    int start = lines[line_i].start;
    int take = cursor - start;
    if (take > lines[line_i].length)
    {
        take = lines[line_i].length;
    }
    if (take < 0)
    {
        take = 0;
    }
    *out_y = box.boundingBox.y + COMPOSER_PAD_Y + (float)line_i * line_height;
    *out_x = box.boundingBox.x + COMPOSER_PAD_X +
             MeasureSlice(app->fonts[FONT_REGULAR], c->text ? c->text : "", start, take, COMPOSER_FONT_SIZE);
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
        c->sel_anchor = 0;
    }
}

static int SelFrom(const PicoComposer *c)
{
    return c->sel_anchor < c->cursor ? c->sel_anchor : c->cursor;
}

static int SelTo(const PicoComposer *c)
{
    return c->sel_anchor > c->cursor ? c->sel_anchor : c->cursor;
}

bool PicoComposer_HasSelection(const PicoApp *app)
{
    return app->composer.sel_anchor != app->composer.cursor;
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
    c->sel_anchor = from;
    c->text[c->length] = '\0';
}

static void DeleteSelection(PicoComposer *c)
{
    if (c->sel_anchor != c->cursor)
    {
        ComposerDeleteRange(c, SelFrom(c), SelTo(c));
    }
}

static void ComposerInsert(PicoComposer *c, const char *bytes, int nbytes)
{
    if (nbytes <= 0)
    {
        return;
    }
    DeleteSelection(c);
    ComposerReserve(c, nbytes);
    if (!c->text)
    {
        return;
    }
    memmove(c->text + c->cursor + nbytes, c->text + c->cursor, (size_t)(c->length - c->cursor));
    memcpy(c->text + c->cursor, bytes, (size_t)nbytes);
    c->length += nbytes;
    c->cursor += nbytes;
    c->sel_anchor = c->cursor;
    c->text[c->length] = '\0';
}

static void MoveCursor(PicoComposer *c, int pos, bool extend)
{
    if (pos < 0)
    {
        pos = 0;
    }
    if (pos > c->length)
    {
        pos = c->length;
    }
    c->cursor = pos;
    if (!extend)
    {
        c->sel_anchor = pos;
    }
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

void PicoComposer_Copy(PicoApp *app)
{
    PicoComposer *c = &app->composer;
    if (c->sel_anchor == c->cursor || !c->text)
    {
        return;
    }
    int from = SelFrom(c);
    int to = SelTo(c);
    int n = to - from;
    char *copy = (char *)malloc((size_t)n + 1);
    if (!copy)
    {
        return;
    }
    memcpy(copy, c->text + from, (size_t)n);
    copy[n] = '\0';
    SetClipboardText(copy);
    free(copy);
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
    bool shift = IsShiftDown();
    const char *text = c->text ? c->text : "";
    bool repeat_left = IsKeyPressed(KEY_LEFT) || IsKeyPressedRepeat(KEY_LEFT);
    bool repeat_right = IsKeyPressed(KEY_RIGHT) || IsKeyPressedRepeat(KEY_RIGHT);
    bool repeat_back = IsKeyPressed(KEY_BACKSPACE) || IsKeyPressedRepeat(KEY_BACKSPACE);
    bool repeat_del = IsKeyPressed(KEY_DELETE) || IsKeyPressedRepeat(KEY_DELETE);

    if (ctrl && IsKeyPressed(KEY_C))
    {
        if (PicoComposer_HasSelection(app))
        {
            PicoComposer_Copy(app);
        }
        else if (app->selected_message >= 0 && app->selected_message < app->message_count)
        {
            const char *src = app->messages[app->selected_message].source;
            if (src)
            {
                SetClipboardText(src);
            }
        }
        return;
    }
    if (ctrl && IsKeyPressed(KEY_X))
    {
        PicoComposer_Copy(app);
        DeleteSelection(c);
        return;
    }

    if (ctrl && (IsKeyPressed(KEY_A) || IsKeyPressed(KEY_HOME)))
    {
        MoveCursor(c, LineStart(text, c->cursor), shift);
    }
    else if (IsKeyPressed(KEY_HOME))
    {
        MoveCursor(c, LineStart(text, c->cursor), shift);
    }

    if (ctrl && (IsKeyPressed(KEY_E) || IsKeyPressed(KEY_END)))
    {
        MoveCursor(c, LineEnd(text, c->length, c->cursor), shift);
    }
    else if (IsKeyPressed(KEY_END))
    {
        MoveCursor(c, LineEnd(text, c->length, c->cursor), shift);
    }

    if (repeat_left)
    {
        int pos = ctrl ? PrevWord(text, c->cursor) : Utf8Prev(text, c->cursor);
        MoveCursor(c, pos, shift);
    }
    if (repeat_right)
    {
        int pos = ctrl ? NextWord(text, c->length, c->cursor) : Utf8Next(text, c->length, c->cursor);
        MoveCursor(c, pos, shift);
    }

    if (ctrl && (IsKeyPressed(KEY_W) || IsKeyPressedRepeat(KEY_W)))
    {
        if (PicoComposer_HasSelection(app))
        {
            DeleteSelection(c);
        }
        else
        {
            ComposerDeleteRange(c, PrevWord(text, c->cursor), c->cursor);
        }
    }
    else if (repeat_back)
    {
        if (PicoComposer_HasSelection(app))
        {
            DeleteSelection(c);
        }
        else
        {
            ComposerDeleteRange(c, Utf8Prev(text, c->cursor), c->cursor);
        }
    }

    if (ctrl && IsKeyPressed(KEY_K))
    {
        int to = LineEnd(text, c->length, c->cursor);
        if (to == c->cursor && to < c->length && text[to] == '\n')
        {
            to++;
        }
        ComposerDeleteRange(c, c->cursor, to);
    }
    else if (repeat_del)
    {
        if (PicoComposer_HasSelection(app))
        {
            DeleteSelection(c);
        }
        else
        {
            ComposerDeleteRange(c, c->cursor, Utf8Next(text, c->length, c->cursor));
        }
    }

    if (ctrl && IsKeyPressed(KEY_V))
    {
        PasteClipboard(c);
    }

    if ((IsKeyPressed(KEY_ENTER) || IsKeyPressed(KEY_KP_ENTER)) && !shift)
    {
        PicoApp_Submit(app);
        return;
    }

    if ((IsKeyPressed(KEY_ENTER) || IsKeyPressed(KEY_KP_ENTER)) && shift)
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

void PicoComposer_HandlePointer(PicoApp *app)
{
    PicoComposer *c = &app->composer;
    Vector2 mouse = GetMousePosition();
    bool over = Clay_PointerOver(Clay_GetElementId(CLAY_STRING("Composer")));

    if (IsMouseButtonPressed(MOUSE_BUTTON_LEFT) && over)
    {
        int pos = OffsetAtPoint(app, mouse.x, mouse.y);
        MoveCursor(c, pos, IsShiftDown());
        c->mouse_selecting = true;
        app->selected_message = -1;
    }
    if (!IsMouseButtonDown(MOUSE_BUTTON_LEFT))
    {
        c->mouse_selecting = false;
    }
    else if (c->mouse_selecting)
    {
        MoveCursor(c, OffsetAtPoint(app, mouse.x, mouse.y), true);
    }
}

void PicoComposer_Render(PicoApp *app)
{
    PicoComposer *c = &app->composer;
    const char *placeholder = "Message Pico…  (Enter to send, Shift+Enter for newline)";
    bool empty = c->length == 0;
    const char *shown = empty ? placeholder : (c->text ? c->text : "");
    int shown_len = empty ? (int)strlen(placeholder) : c->length;
    Clay_Color text_color = empty ? COLOR_MUTED : COLOR_TEXT;
    Clay_String text = {.length = shown_len, .chars = shown};

    CLAY(CLAY_ID("Composer"),
         {.layout = {.layoutDirection = CLAY_TOP_TO_BOTTOM,
                     .padding = {COMPOSER_PAD_X, COMPOSER_PAD_X, COMPOSER_PAD_Y, COMPOSER_PAD_Y},
                     .sizing = {.width = CLAY_SIZING_GROW(0), .height = CLAY_SIZING_FIT(56, 160)}},
          .backgroundColor = COLOR_COMPOSER_BG,
          .cornerRadius = CLAY_CORNER_RADIUS(8)})
    {
        CLAY_TEXT(text, CLAY_TEXT_CONFIG({.fontId = FONT_REGULAR,
                                          .fontSize = COMPOSER_FONT_SIZE,
                                          .textColor = text_color,
                                          .wrapMode = CLAY_TEXT_WRAP_WORDS}));
    }
}

void PicoComposer_DrawOverlay(PicoApp *app)
{
    PicoComposer *c = &app->composer;
    Clay_ElementData box = Clay_GetElementData(Clay_GetElementId(CLAY_STRING("Composer")));
    if (!box.found)
    {
        return;
    }

    if (PicoComposer_HasSelection(app) && c->text)
    {
        float max_width = box.boundingBox.width - COMPOSER_PAD_X * 2;
        CompLine lines[COMPOSER_MAX_LINES];
        float line_height = COMPOSER_FONT_SIZE;
        int line_count = WrapComposer(c, app->fonts[FONT_REGULAR], max_width, lines, COMPOSER_MAX_LINES, &line_height);
        int sel_from = SelFrom(c);
        int sel_to = SelTo(c);
        Color fill = {(unsigned char)COLOR_SELECTION.r, (unsigned char)COLOR_SELECTION.g, (unsigned char)COLOR_SELECTION.b,
                      (unsigned char)COLOR_SELECTION.a};
        for (int i = 0; i < line_count; i++)
        {
            int start = lines[i].start;
            int end = start + lines[i].length;
            int a = sel_from > start ? sel_from : start;
            int b = sel_to < end ? sel_to : end;
            if (a >= b)
            {
                continue;
            }
            float x0 = MeasureSlice(app->fonts[FONT_REGULAR], c->text, start, a - start, COMPOSER_FONT_SIZE);
            float x1 = MeasureSlice(app->fonts[FONT_REGULAR], c->text, start, b - start, COMPOSER_FONT_SIZE);
            DrawRectangle((int)(box.boundingBox.x + COMPOSER_PAD_X + x0),
                          (int)(box.boundingBox.y + COMPOSER_PAD_Y + (float)i * line_height),
                          (int)(x1 - x0 < 2 ? 2 : x1 - x0), (int)line_height, fill);
        }
    }

    if (((int)(GetTime() * 2.0) & 1) == 0)
    {
        float x, y, h;
        CaretPos(app, &x, &y, &h);
        Color caret = {(unsigned char)COLOR_CURSOR.r, (unsigned char)COLOR_CURSOR.g, (unsigned char)COLOR_CURSOR.b, 255};
        DrawRectangle((int)x, (int)y, 2, (int)h, caret);
    }
}

static void ComposerFrame(PicoApp *app, float dt)
{
    (void)dt;
    PicoComposer_HandleInput(app);
}

static void ComposerInit(PicoApp *app)
{
    pico_add_view(app, PICO_SLOT_COMPOSER, 0, PicoComposer_Render);
    pico_add_hook(app, PICO_HOOK_AFTER_LAYOUT, PicoComposer_HandlePointer);
    pico_add_hook(app, PICO_HOOK_AFTER_RENDER, PicoComposer_DrawOverlay);
}

PicoExt pico_ext_composer(void)
{
    return (PicoExt){
        .abi = PICO_EXT_ABI,
        .name = "composer",
        .init = ComposerInit,
        .on_frame = ComposerFrame,
    };
}
