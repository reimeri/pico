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
#define COMPOSER_MAX_LINES 256
#define COMPOSER_MIN_HEIGHT 56
#define COMPOSER_MAX_GROW_LINES 10

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
    if (c->length > 0 && c->text[c->length - 1] == '\n' && line_count < max_lines)
    {
        lines[line_count].start = c->length;
        lines[line_count].length = 0;
        line_count++;
    }
    if (line_count == 0)
    {
        lines[0].start = 0;
        lines[0].length = c->length;
        return 1;
    }
    return line_count;
}

typedef struct ComposerView {
    CompLine lines[COMPOSER_MAX_LINES];
    int line_count;
    float line_height;
    float wrap_width;
    float origin_x;
    float origin_y;
    float scroll_y;
    Clay_BoundingBox clip;
    bool found;
} ComposerView;

static float s_wrap_width = 0;
static int s_seen_cursor = -1;
static int s_seen_length = -1;
static float s_goal_x = -1;

static void MoveCursor(PicoComposer *c, int pos, bool extend);

static ComposerView GetComposerView(PicoApp *app)
{
    ComposerView v = {0};
    PicoComposer *c = &app->composer;
    Clay_ElementData scroll_box = Clay_GetElementData(Clay_GetElementId(CLAY_STRING("ComposerScroll")));
    Clay_ElementData composer_box = Clay_GetElementData(Clay_GetElementId(CLAY_STRING("Composer")));
    v.found = scroll_box.found || composer_box.found;
    v.clip = scroll_box.found ? scroll_box.boundingBox : composer_box.boundingBox;
    if (scroll_box.found)
    {
        v.origin_x = scroll_box.boundingBox.x;
        v.origin_y = scroll_box.boundingBox.y;
        v.wrap_width = scroll_box.boundingBox.width;
    }
    else if (composer_box.found)
    {
        v.origin_x = composer_box.boundingBox.x + COMPOSER_PAD_X;
        v.origin_y = composer_box.boundingBox.y + COMPOSER_PAD_Y;
        v.wrap_width = composer_box.boundingBox.width - COMPOSER_PAD_X * 2;
    }
    else
    {
        v.wrap_width = s_wrap_width;
    }
    if (v.wrap_width < 10)
    {
        v.wrap_width = s_wrap_width > 10 ? s_wrap_width : (float)GetScreenWidth() - 80;
    }
    Clay_ScrollContainerData scroll = Clay_GetScrollContainerData(Clay_GetElementId(CLAY_STRING("ComposerScroll")));
    if (scroll.found && scroll.scrollPosition)
    {
        v.scroll_y = scroll.scrollPosition->y;
    }
    v.line_count = WrapComposer(c, app->fonts[FONT_REGULAR], v.wrap_width, v.lines, COMPOSER_MAX_LINES, &v.line_height);
    return v;
}

static int CaretLineIndex(const ComposerView *v, int cursor)
{
    int line_i = 0;
    for (int i = 0; i < v->line_count; i++)
    {
        if (cursor >= v->lines[i].start)
        {
            line_i = i;
        }
    }
    return line_i;
}

static int OffsetAtXOnLine(Font font, const PicoComposer *c, CompLine line, float target_x)
{
    if (target_x <= 0 || line.length <= 0 || !c->text)
    {
        return line.start;
    }
    float width = 0;
    int pos = line.start;
    int end = line.start + line.length;
    while (pos < end)
    {
        int next = Utf8Next(c->text, c->length, pos);
        float ch_w = MeasureSlice(font, c->text, pos, next - pos, COMPOSER_FONT_SIZE);
        if (width + ch_w * 0.5f >= target_x)
        {
            return pos;
        }
        width += ch_w;
        pos = next;
    }
    return end;
}

static void MoveVertical(PicoApp *app, int dir, bool extend)
{
    PicoComposer *c = &app->composer;
    CompLine lines[COMPOSER_MAX_LINES];
    float line_height = COMPOSER_FONT_SIZE;
    float wrap = s_wrap_width > 10 ? s_wrap_width : (float)GetScreenWidth() - 80;
    int line_count = WrapComposer(c, app->fonts[FONT_REGULAR], wrap, lines, COMPOSER_MAX_LINES, &line_height);
    int line_i = 0;
    for (int i = 0; i < line_count; i++)
    {
        if (c->cursor >= lines[i].start)
        {
            line_i = i;
        }
    }
    int start = lines[line_i].start;
    int take = c->cursor - start;
    if (take > lines[line_i].length)
    {
        take = lines[line_i].length;
    }
    if (take < 0)
    {
        take = 0;
    }
    float x = MeasureSlice(app->fonts[FONT_REGULAR], c->text ? c->text : "", start, take, COMPOSER_FONT_SIZE);
    float goal = s_goal_x >= 0 ? s_goal_x : x;
    int next = line_i + dir;
    int pos;
    if (next < 0)
    {
        pos = 0;
    }
    else if (next >= line_count)
    {
        pos = c->length;
    }
    else
    {
        pos = OffsetAtXOnLine(app->fonts[FONT_REGULAR], c, lines[next], goal);
    }
    MoveCursor(c, pos, extend);
    s_goal_x = goal;
}

static int OffsetAtPoint(PicoApp *app, float x, float y)
{
    PicoComposer *c = &app->composer;
    ComposerView v = GetComposerView(app);
    if (!v.found)
    {
        return c->cursor;
    }
    float local_x = x - v.origin_x;
    float local_y = y - v.origin_y - v.scroll_y;
    if (local_y < 0)
    {
        return 0;
    }
    int line_i = (int)(local_y / v.line_height);
    int line_count = v.line_count;
    if (line_i >= line_count)
    {
        return c->length;
    }
    if (line_i < 0)
    {
        line_i = 0;
    }
    CompLine line = v.lines[line_i];
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
    ComposerView v = GetComposerView(app);
    *out_x = v.origin_x;
    *out_y = v.origin_y;
    *out_h = v.line_height > 1 ? v.line_height : (float)COMPOSER_FONT_SIZE;
    if (!v.found)
    {
        return;
    }
    int line_i = CaretLineIndex(&v, c->cursor);
    int start = v.lines[line_i].start;
    int take = c->cursor - start;
    if (take > v.lines[line_i].length)
    {
        take = v.lines[line_i].length;
    }
    if (take < 0)
    {
        take = 0;
    }
    *out_y = v.origin_y + (float)line_i * v.line_height + v.scroll_y;
    *out_x = v.origin_x +
             MeasureSlice(app->fonts[FONT_REGULAR], c->text ? c->text : "", start, take, COMPOSER_FONT_SIZE);
}

static void EnsureCaretVisible(PicoApp *app)
{
    PicoComposer *c = &app->composer;
    if (c->cursor == s_seen_cursor && c->length == s_seen_length)
    {
        return;
    }
    s_seen_cursor = c->cursor;
    s_seen_length = c->length;

    ComposerView v = GetComposerView(app);
    Clay_ScrollContainerData scroll = Clay_GetScrollContainerData(Clay_GetElementId(CLAY_STRING("ComposerScroll")));
    if (!scroll.found || !scroll.scrollPosition || v.line_height < 1)
    {
        return;
    }
    int line_i = CaretLineIndex(&v, c->cursor);
    float caret_top = (float)line_i * v.line_height;
    float caret_bot = caret_top + v.line_height;
    float view_h = scroll.scrollContainerDimensions.height;
    float vis_top = -scroll.scrollPosition->y;
    float vis_bot = vis_top + view_h;
    if (caret_top < vis_top)
    {
        scroll.scrollPosition->y = -caret_top;
    }
    else if (caret_bot > vis_bot)
    {
        scroll.scrollPosition->y = -(caret_bot - view_h);
    }
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

static void ComposerDeleteRange(PicoComposer *c, int from, int to);
static void ComposerInsert(PicoComposer *c, const char *bytes, int nbytes);

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
    s_goal_x = -1;
}

void PicoComposer_ReplaceRange(PicoApp *app, int from, int to, const char *text)
{
    PicoComposer *c = &app->composer;
    c->sel_anchor = c->cursor;
    if (from > to)
    {
        int tmp = from;
        from = to;
        to = tmp;
    }
    ComposerDeleteRange(c, from, to);
    if (text && text[0])
    {
        ComposerInsert(c, text, (int)strlen(text));
    }
}

void PicoComposer_SetText(PicoApp *app, const char *text)
{
    PicoComposer *c = &app->composer;
    c->sel_anchor = 0;
    c->cursor = 0;
    ComposerDeleteRange(c, 0, c->length);
    if (text && text[0])
    {
        ComposerInsert(c, text, (int)strlen(text));
    }
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
    s_goal_x = -1;
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
    s_goal_x = -1;
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
    if (PicoUi_ModalOpen(app))
    {
        return;
    }
    PicoComposer *c = &app->composer;
    bool ctrl = IsCtrlDown();
    bool shift = IsShiftDown();
    const char *text = c->text ? c->text : "";
    bool repeat_left = IsKeyPressed(KEY_LEFT) || IsKeyPressedRepeat(KEY_LEFT);
    bool repeat_right = IsKeyPressed(KEY_RIGHT) || IsKeyPressedRepeat(KEY_RIGHT);
    bool repeat_back = IsKeyPressed(KEY_BACKSPACE) || IsKeyPressedRepeat(KEY_BACKSPACE);
    bool repeat_del = IsKeyPressed(KEY_DELETE) || IsKeyPressedRepeat(KEY_DELETE);

    if (PicoComplete_HandleKeys(app))
    {
        return;
    }

    if (ctrl && Pico_ShortcutPressed('c'))
    {
        if (PicoChatSel_HasSelection(app))
        {
            PicoChatSel_Copy(app);
        }
        else if (PicoComposer_HasSelection(app))
        {
            PicoComposer_Copy(app);
        }
        return;
    }
    if (ctrl && Pico_ShortcutPressed('x'))
    {
        PicoComposer_Copy(app);
        DeleteSelection(c);
        return;
    }

    if (ctrl && (Pico_ShortcutPressed('a') || IsKeyPressed(KEY_HOME)))
    {
        MoveCursor(c, LineStart(text, c->cursor), shift);
    }
    else if (IsKeyPressed(KEY_HOME))
    {
        MoveCursor(c, LineStart(text, c->cursor), shift);
    }

    if (ctrl && (Pico_ShortcutPressed('e') || IsKeyPressed(KEY_END)))
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

    bool repeat_up = IsKeyPressed(KEY_UP) || IsKeyPressedRepeat(KEY_UP);
    bool repeat_down = IsKeyPressed(KEY_DOWN) || IsKeyPressedRepeat(KEY_DOWN);
    if (repeat_up)
    {
        MoveVertical(app, -1, shift);
    }
    if (repeat_down)
    {
        MoveVertical(app, 1, shift);
    }

    if (ctrl && Pico_ShortcutRepeat('w'))
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

    if (ctrl && Pico_ShortcutPressed('k'))
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

    if (ctrl && Pico_ShortcutPressed('v'))
    {
        PasteClipboard(c);
    }

    if ((IsKeyPressed(KEY_ENTER) || IsKeyPressed(KEY_KP_ENTER)) && !shift)
    {
        PicoApp_Submit(app);
        return;
    }

    if ((IsKeyPressed(KEY_ENTER) || IsKeyPressedRepeat(KEY_ENTER) || IsKeyPressed(KEY_KP_ENTER) ||
         IsKeyPressedRepeat(KEY_KP_ENTER)) &&
        shift)
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
    PicoComplete_Refresh(app);
}

void PicoComposer_HandlePointer(PicoApp *app)
{
    PicoComposer *c = &app->composer;
    Vector2 mouse = GetMousePosition();
    bool over_bar = Clay_PointerOver(Clay_GetElementId(CLAY_STRING("CompScrollBarHandle"))) ||
                    Clay_PointerOver(Clay_GetElementId(CLAY_STRING("CompScrollTrack")));
    bool over = Clay_PointerOver(Clay_GetElementId(CLAY_STRING("ComposerScroll")));

    if (over_bar)
    {
        if (IsMouseButtonPressed(MOUSE_BUTTON_LEFT))
        {
            c->mouse_selecting = false;
        }
        return;
    }

    if (IsMouseButtonPressed(MOUSE_BUTTON_LEFT) && over)
    {
        int pos = OffsetAtPoint(app, mouse.x, mouse.y);
        MoveCursor(c, pos, IsShiftDown());
        c->mouse_selecting = true;
        PicoChatSel_Clear(app);
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
    float wrap_width = s_wrap_width > 10 ? s_wrap_width : (float)GetScreenWidth() - 80;
    CompLine lines[COMPOSER_MAX_LINES];
    float line_height = COMPOSER_FONT_SIZE;
    int line_count = empty ? 1 : WrapComposer(c, app->fonts[FONT_REGULAR], wrap_width, lines, COMPOSER_MAX_LINES, &line_height);
    if (line_height < 1)
    {
        line_height = (float)COMPOSER_FONT_SIZE;
    }

    float content_h = (float)line_count * line_height;
    float box_h = content_h + (float)COMPOSER_PAD_Y * 2;
    if (box_h < (float)COMPOSER_MIN_HEIGHT)
    {
        box_h = (float)COMPOSER_MIN_HEIGHT;
    }
    float max_h = (float)COMPOSER_MAX_GROW_LINES * line_height + (float)COMPOSER_PAD_Y * 2;
    if (box_h > max_h)
    {
        box_h = max_h;
    }

    CLAY(CLAY_ID("Composer"),
         {.layout = {.layoutDirection = CLAY_TOP_TO_BOTTOM,
                     .padding = {COMPOSER_PAD_X, COMPOSER_PAD_X, COMPOSER_PAD_Y, COMPOSER_PAD_Y},
                     .sizing = {.width = CLAY_SIZING_GROW(0), .height = CLAY_SIZING_FIXED(box_h)}},
          .backgroundColor = COLOR_COMPOSER_BG,
          .cornerRadius = CLAY_CORNER_RADIUS(8)})
    {
        CLAY(CLAY_ID("ComposerRow"),
             {.layout = {.layoutDirection = CLAY_LEFT_TO_RIGHT,
                         .childGap = SCROLLBAR_GAP,
                         .sizing = {.width = CLAY_SIZING_GROW(0), .height = CLAY_SIZING_GROW(0)}}})
        {
            CLAY(CLAY_ID("ComposerScroll"),
                 {.layout = {.layoutDirection = CLAY_TOP_TO_BOTTOM,
                             .sizing = {.width = CLAY_SIZING_GROW(0), .height = CLAY_SIZING_GROW(0)}},
                  .clip = {.vertical = true, .horizontal = false, .childOffset = Clay_GetScrollOffset()}})
            {
                CLAY(CLAY_ID("ComposerContent"),
                     {.layout = {.layoutDirection = CLAY_TOP_TO_BOTTOM,
                                 .sizing = {.width = CLAY_SIZING_GROW(0), .height = CLAY_SIZING_FIT(0)}}})
                {
                    if (empty)
                    {
                        Clay_String text = {.length = (int32_t)strlen(placeholder), .chars = placeholder};
                        CLAY_TEXT(text, CLAY_TEXT_CONFIG({.fontId = FONT_REGULAR,
                                                          .fontSize = COMPOSER_FONT_SIZE,
                                                          .textColor = COLOR_MUTED,
                                                          .wrapMode = CLAY_TEXT_WRAP_WORDS}));
                    }
                    else
                    {
                        for (int i = 0; i < line_count; i++)
                        {
                            CLAY(CLAY_IDI("CompLine", i),
                                 {.layout = {.sizing = {.width = CLAY_SIZING_GROW(0),
                                                        .height = CLAY_SIZING_FIXED(line_height)}}})
                            {
                                if (lines[i].length > 0)
                                {
                                    Clay_String text = {.length = (int32_t)lines[i].length,
                                                        .chars = c->text + lines[i].start};
                                    CLAY_TEXT(text, CLAY_TEXT_CONFIG({.fontId = FONT_REGULAR,
                                                                      .fontSize = COMPOSER_FONT_SIZE,
                                                                      .textColor = COLOR_TEXT,
                                                                      .wrapMode = CLAY_TEXT_WRAP_NONE}));
                                }
                            }
                        }
                    }
                }
            }
            if (app->composer_overflow)
            {
                Clay_ScrollContainerData scroll_data =
                    Clay_GetScrollContainerData(Clay_GetElementId(CLAY_STRING("ComposerScroll")));
                float track_h = scroll_data.found ? scroll_data.scrollContainerDimensions.height : 0;
                float content_h_scroll = scroll_data.found ? scroll_data.contentDimensions.height : 1;
                float thumb_h = content_h_scroll > 0 ? (track_h / content_h_scroll) * track_h : track_h;
                if (thumb_h < 16)
                {
                    thumb_h = 16;
                }
                float thumb_y = 0;
                if (scroll_data.found && scroll_data.scrollPosition && content_h_scroll > 0)
                {
                    thumb_y = -(scroll_data.scrollPosition->y / content_h_scroll) * track_h;
                }
                CLAY(CLAY_ID("CompScrollTrack"),
                     {.layout = {.sizing = {.width = CLAY_SIZING_FIXED((float)SCROLLBAR_WIDTH),
                                            .height = CLAY_SIZING_GROW(0)}}})
                {
                    CLAY(CLAY_ID("CompScrollBarHandle"),
                         {.floating = {.attachTo = CLAY_ATTACH_TO_PARENT,
                                       .offset = {.y = thumb_y},
                                       .zIndex = 1,
                                       .attachPoints = {.element = CLAY_ATTACH_POINT_LEFT_TOP,
                                                        .parent = CLAY_ATTACH_POINT_LEFT_TOP}},
                          .layout = {.sizing = {.width = CLAY_SIZING_FIXED((float)SCROLLBAR_WIDTH),
                                                .height = CLAY_SIZING_FIXED(thumb_h)}},
                          .backgroundColor = Clay_PointerOver(Clay_GetElementId(CLAY_STRING("CompScrollBarHandle")))
                                                 ? COLOR_SCROLLBAR_HOVER
                                                 : COLOR_SCROLLBAR,
                          .cornerRadius = CLAY_CORNER_RADIUS((float)SCROLLBAR_WIDTH / 2.0f)})
                    {
                    }
                }
            }
        }
        PicoComplete_Render(app);
    }
}

void PicoComposer_DrawOverlay(PicoApp *app)
{
    PicoComposer *c = &app->composer;
    ComposerView v = GetComposerView(app);
    if (!v.found)
    {
        return;
    }

    BeginScissorMode((int)v.clip.x, (int)v.clip.y, (int)v.clip.width, (int)v.clip.height);

    if (PicoComposer_HasSelection(app) && c->text)
    {
        int sel_from = SelFrom(c);
        int sel_to = SelTo(c);
        Color fill = {(unsigned char)COLOR_SELECTION.r, (unsigned char)COLOR_SELECTION.g, (unsigned char)COLOR_SELECTION.b,
                      (unsigned char)COLOR_SELECTION.a};
        for (int i = 0; i < v.line_count; i++)
        {
            int start = v.lines[i].start;
            int end = start + v.lines[i].length;
            int range_lo = start;
            int range_hi = end;
            if (v.lines[i].length == 0 && start > 0)
            {
                range_lo = start - 1;
            }
            if (sel_from >= range_hi || sel_to <= range_lo)
            {
                continue;
            }
            float y = v.origin_y + (float)i * v.line_height + v.scroll_y;
            if (v.lines[i].length == 0)
            {
                DrawRectangle((int)v.origin_x, (int)y, 6, (int)v.line_height, fill);
                continue;
            }
            int a = sel_from > start ? sel_from : start;
            int b = sel_to < end ? sel_to : end;
            if (a > b)
            {
                a = b;
            }
            float x0 = MeasureSlice(app->fonts[FONT_REGULAR], c->text, start, a - start, COMPOSER_FONT_SIZE);
            float x1 = MeasureSlice(app->fonts[FONT_REGULAR], c->text, start, b - start, COMPOSER_FONT_SIZE);
            DrawRectangle((int)(v.origin_x + x0), (int)y, (int)(x1 - x0 < 2 ? 2 : x1 - x0), (int)v.line_height, fill);
        }
    }

    if (((int)(GetTime() * 2.0) & 1) == 0)
    {
        float x, y, h;
        CaretPos(app, &x, &y, &h);
        Color caret = {(unsigned char)COLOR_CURSOR.r, (unsigned char)COLOR_CURSOR.g, (unsigned char)COLOR_CURSOR.b, 255};
        DrawRectangle((int)x, (int)y, 2, (int)h, caret);
    }

    EndScissorMode();
}

static void ComposerAfterLayout(PicoApp *app)
{
    ComposerView v = GetComposerView(app);
    if (v.wrap_width > 10)
    {
        s_wrap_width = v.wrap_width;
    }
    Clay_ScrollContainerData scroll = Clay_GetScrollContainerData(Clay_GetElementId(CLAY_STRING("ComposerScroll")));
    app->composer_overflow =
        scroll.found && scroll.contentDimensions.height > scroll.scrollContainerDimensions.height + 0.5f;
    if (!PicoUi_ModalOpen(app))
    {
        if (!PicoComplete_HandlePointer(app))
        {
            PicoComposer_HandlePointer(app);
        }
    }
    EnsureCaretVisible(app);
}

static void UpdateComposerScrollbarDrag(PicoApp *app)
{
    PicoScrollbar *drag = &app->composer_scrollbar;
    Clay_Vector2 mouse = {.x = GetMousePosition().x, .y = GetMousePosition().y};
    if (!IsMouseButtonDown(0))
    {
        drag->mouse_down = false;
    }
    if (IsMouseButtonDown(0) && !drag->mouse_down &&
        Clay_PointerOver(Clay_GetElementId(CLAY_STRING("CompScrollBarHandle"))))
    {
        Clay_ScrollContainerData data = Clay_GetScrollContainerData(Clay_GetElementId(CLAY_STRING("ComposerScroll")));
        if (data.found && data.scrollPosition)
        {
            drag->click_origin = mouse;
            drag->position_origin = *data.scrollPosition;
            drag->mouse_down = true;
        }
    }
    else if (drag->mouse_down)
    {
        Clay_ScrollContainerData data = Clay_GetScrollContainerData(Clay_GetElementId(CLAY_STRING("ComposerScroll")));
        if (data.found && data.scrollPosition && data.contentDimensions.height > 0)
        {
            float ratio = data.contentDimensions.height / data.scrollContainerDimensions.height;
            data.scrollPosition->y = drag->position_origin.y + (drag->click_origin.y - mouse.y) * ratio;
        }
    }
}

static void ComposerFrame(PicoApp *app, float dt)
{
    (void)dt;
    PicoComposer_HandleInput(app);
    if (!PicoUi_ModalOpen(app))
    {
        UpdateComposerScrollbarDrag(app);
    }
}

static void ComposerInit(PicoApp *app)
{
    pico_add_view(app, PICO_SLOT_COMPOSER, 0, PicoComposer_Render);
    pico_add_hook(app, PICO_HOOK_AFTER_LAYOUT, ComposerAfterLayout);
    pico_add_hook(app, PICO_HOOK_AFTER_RENDER, PicoComposer_DrawOverlay);
}

PicoExt pico_ext_composer(void)
{
    return (PicoExt){
        .abi = PICO_EXT_ABI,
        .name = "composer",
        .description = "Prompt input",
        .init = ComposerInit,
        .on_frame = ComposerFrame,
    };
}
