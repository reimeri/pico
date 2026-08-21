#include "chat_sel.h"

#include "pico/app.h"

#include "raylib.h"

#include <stdlib.h>
#include <string.h>

#define SEL_MAX_WRAP_LINES 256

typedef struct SelBuf {
    char *text;
    int len;
    int cap;
} SelBuf;

typedef struct SelHit {
    int msg;
    int start;
    int length;
    uint16_t font_id;
    uint16_t font_size;
    uint16_t line_height;
    Clay_TextElementConfigWrapMode wrap;
} SelHit;

typedef struct WrapLine {
    int start;
    int length;
} WrapLine;

static SelBuf *s_msgs;
static int s_msg_n;
static int s_msg_cap;
static int s_cur_msg = -1;

static SelHit *s_hits;
static int s_hit_count;
static int s_hit_cap;

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

static float MeasureN(Font font, float size, const char *s, int n)
{
    static char *buf;
    static int cap;
    if (n <= 0)
    {
        return 0;
    }
    if (n + 1 > cap)
    {
        int next = cap == 0 ? 64 : cap;
        while (next < n + 1)
        {
            next *= 2;
        }
        char *grown = (char *)realloc(buf, (size_t)next);
        if (!grown)
        {
            return 0;
        }
        buf = grown;
        cap = next;
    }
    memcpy(buf, s, (size_t)n);
    buf[n] = '\0';
    return MeasureTextEx(font, buf, size, 0).x;
}

static void BufReserve(SelBuf *b, int extra)
{
    if (b->len + extra + 1 <= b->cap)
    {
        return;
    }
    int cap = b->cap == 0 ? 64 : b->cap;
    while (cap < b->len + extra + 1)
    {
        cap *= 2;
    }
    char *next = (char *)realloc(b->text, (size_t)cap);
    if (!next)
    {
        return;
    }
    b->text = next;
    b->cap = cap;
}

static void BufAppend(SelBuf *b, const char *s, int n)
{
    if (!s || n <= 0)
    {
        return;
    }
    BufReserve(b, n);
    if (!b->text)
    {
        return;
    }
    memcpy(b->text + b->len, s, (size_t)n);
    b->len += n;
    b->text[b->len] = '\0';
}

static SelBuf *CurBuf(void)
{
    if (s_cur_msg < 0 || s_cur_msg >= s_msg_n)
    {
        return NULL;
    }
    return &s_msgs[s_cur_msg];
}

static Color ClayToRay(Clay_Color c)
{
    return (Color){(unsigned char)c.r, (unsigned char)c.g, (unsigned char)c.b, (unsigned char)c.a};
}

static int WrapRun(Font font, float font_size, const char *s, int len, float max_w, bool wrap,
                   WrapLine *lines, int max_lines)
{
    if (max_lines <= 0)
    {
        return 0;
    }
    if (!s || len <= 0)
    {
        lines[0].start = 0;
        lines[0].length = 0;
        return 1;
    }
    if (!wrap || max_w <= 1)
    {
        lines[0].start = 0;
        lines[0].length = len;
        return 1;
    }

    int n = 0;
    int i = 0;
    while (i < len && n < max_lines)
    {
        if (s[i] == '\n')
        {
            lines[n].start = i;
            lines[n].length = 0;
            n++;
            i++;
            continue;
        }
        int line_start = i;
        float width = 0;
        int last_break = -1;
        while (i < len && s[i] != '\n')
        {
            int next = Utf8Next(s, len, i);
            float cw = MeasureN(font, font_size, s + i, next - i);
            if (width + cw > max_w && i > line_start)
            {
                int end = last_break > line_start ? last_break : i;
                while (end > line_start && s[end - 1] == ' ')
                {
                    end--;
                }
                lines[n].start = line_start;
                lines[n].length = end - line_start;
                n++;
                i = last_break > line_start ? last_break : i;
                while (i < len && s[i] == ' ')
                {
                    i++;
                }
                break;
            }
            width += cw;
            if (s[i] == ' ' || s[i] == '\t')
            {
                last_break = next;
            }
            i = next;
            if (i >= len || s[i] == '\n')
            {
                lines[n].start = line_start;
                lines[n].length = i - line_start;
                n++;
                break;
            }
        }
    }
    return n > 0 ? n : 1;
}

static float LineHeight(const SelHit *hit, Font font, float box_h, int nlines)
{
    if (nlines > 0 && box_h > 1)
    {
        return box_h / (float)nlines;
    }
    if (hit->line_height > 0)
    {
        return (float)hit->line_height;
    }
    Vector2 sample = MeasureTextEx(font, "Hg", (float)hit->font_size, 0);
    return sample.y > 1 ? sample.y : (float)hit->font_size;
}

static int OffsetOnLine(Font font, float font_size, const char *s, int start, int length, float x)
{
    if (x <= 0 || length <= 0)
    {
        return start;
    }
    float width = 0;
    int i = start;
    int end = start + length;
    while (i < end)
    {
        int next = Utf8Next(s, end, i);
        float cw = MeasureN(font, font_size, s + i, next - i);
        if (width + cw * 0.5f >= x)
        {
            return i;
        }
        width += cw;
        i = next;
    }
    return end;
}

void PicoChatSel_BeginFrame(int message_count)
{
    if (message_count < 0)
    {
        message_count = 0;
    }
    if (message_count > s_msg_cap)
    {
        SelBuf *next = (SelBuf *)realloc(s_msgs, (size_t)message_count * sizeof(SelBuf));
        if (!next)
        {
            return;
        }
        for (int i = s_msg_cap; i < message_count; i++)
        {
            memset(&next[i], 0, sizeof(SelBuf));
        }
        s_msgs = next;
        s_msg_cap = message_count;
    }
    s_msg_n = message_count;
    for (int i = 0; i < s_msg_n; i++)
    {
        s_msgs[i].len = 0;
        if (s_msgs[i].text)
        {
            s_msgs[i].text[0] = '\0';
        }
    }
    s_hit_count = 0;
    s_cur_msg = -1;
}

void PicoChatSel_SetMessage(int msg)
{
    s_cur_msg = (msg >= 0 && msg < s_msg_n) ? msg : -1;
}

void PicoChatSel_Break(void)
{
    SelBuf *b = CurBuf();
    if (!b)
    {
        return;
    }
    if (b->len == 0 || b->text[b->len - 1] == '\n')
    {
        return;
    }
    BufAppend(b, "\n", 1);
}

void PicoChatSel_Glue(const char *s)
{
    SelBuf *b = CurBuf();
    if (!b || !s || !s[0])
    {
        return;
    }
    BufAppend(b, s, (int)strlen(s));
}

void PicoChatSel_Text(Clay_String text, Clay_TextElementConfig config)
{
    if (s_cur_msg < 0 || text.length <= 0 || !text.chars)
    {
        CLAY_TEXT(text, config);
        return;
    }
    if (s_hit_count >= s_hit_cap)
    {
        int cap = s_hit_cap == 0 ? 128 : s_hit_cap * 2;
        SelHit *next = (SelHit *)realloc(s_hits, (size_t)cap * sizeof(SelHit));
        if (!next)
        {
            CLAY_TEXT(text, config);
            return;
        }
        s_hits = next;
        s_hit_cap = cap;
    }

    SelBuf *b = CurBuf();
    int start = b ? b->len : 0;
    if (b)
    {
        BufAppend(b, text.chars, text.length);
    }

    int id = s_hit_count;
    s_hits[id].msg = s_cur_msg;
    s_hits[id].start = start;
    s_hits[id].length = text.length;
    s_hits[id].font_id = config.fontId;
    s_hits[id].font_size = config.fontSize;
    s_hits[id].line_height = config.lineHeight;
    s_hits[id].wrap = config.wrapMode;
    s_hit_count++;

    CLAY(CLAY_IDI("ChatRun", id), {})
    {
        CLAY_TEXT(text, config);
    }
}

bool PicoChatSel_HasSelection(const PicoApp *app)
{
    return app && app->chat_sel.msg >= 0 && app->chat_sel.anchor != app->chat_sel.cursor;
}

void PicoChatSel_Clear(PicoApp *app)
{
    if (!app)
    {
        return;
    }
    app->chat_sel.msg = -1;
    app->chat_sel.anchor = 0;
    app->chat_sel.cursor = 0;
    app->chat_sel.mouse_selecting = false;
    app->chat_sel.dragging = false;
    app->chat_sel.pressed_tool = false;
}

void PicoChatSel_Copy(PicoApp *app)
{
    if (!PicoChatSel_HasSelection(app))
    {
        return;
    }
    int msg = app->chat_sel.msg;
    if (msg < 0 || msg >= s_msg_n || !s_msgs[msg].text)
    {
        return;
    }
    int from = app->chat_sel.anchor < app->chat_sel.cursor ? app->chat_sel.anchor : app->chat_sel.cursor;
    int to = app->chat_sel.anchor > app->chat_sel.cursor ? app->chat_sel.anchor : app->chat_sel.cursor;
    if (from < 0)
    {
        from = 0;
    }
    if (to > s_msgs[msg].len)
    {
        to = s_msgs[msg].len;
    }
    if (to <= from)
    {
        return;
    }
    int n = to - from;
    char *copy = (char *)malloc((size_t)n + 1);
    if (!copy)
    {
        return;
    }
    memcpy(copy, s_msgs[msg].text + from, (size_t)n);
    copy[n] = '\0';
    SetClipboardText(copy);
    free(copy);
}

void PicoChatSel_Clamp(PicoApp *app)
{
    if (!app || app->chat_sel.msg < 0)
    {
        return;
    }
    if (app->chat_sel.msg >= s_msg_n)
    {
        PicoChatSel_Clear(app);
        return;
    }
    int len = s_msgs[app->chat_sel.msg].len;
    if (app->chat_sel.anchor > len)
    {
        app->chat_sel.anchor = len;
    }
    if (app->chat_sel.cursor > len)
    {
        app->chat_sel.cursor = len;
    }
    if (app->chat_sel.anchor < 0)
    {
        app->chat_sel.anchor = 0;
    }
    if (app->chat_sel.cursor < 0)
    {
        app->chat_sel.cursor = 0;
    }
}

static int HitOffset(PicoApp *app, const SelHit *hit, Clay_BoundingBox box, float x, float y)
{
    SelBuf *b = (hit->msg >= 0 && hit->msg < s_msg_n) ? &s_msgs[hit->msg] : NULL;
    if (!b || !b->text || hit->start < 0 || hit->start > b->len)
    {
        return hit->start;
    }
    int available = b->len - hit->start;
    int len = hit->length < available ? hit->length : available;
    const char *s = b->text + hit->start;
    Font font = Pico_FontAt(hit->font_id, hit->font_size);
    float size = (float)hit->font_size;
    bool wrap = hit->wrap != CLAY_TEXT_WRAP_NONE;
    WrapLine lines[SEL_MAX_WRAP_LINES];
    int nlines = WrapRun(font, size, s, len, wrap ? box.width : 0, wrap, lines, SEL_MAX_WRAP_LINES);
    float lh = LineHeight(hit, font, box.height, nlines);

    int li = 0;
    if (lh > 1)
    {
        li = (int)((y - box.y) / lh);
    }
    if (li < 0)
    {
        return hit->start;
    }
    if (li >= nlines)
    {
        return hit->start + len;
    }
    return hit->start + OffsetOnLine(font, size, s, lines[li].start, lines[li].length, x - box.x);
}

int PicoChatSel_OffsetAtPoint(PicoApp *app, float x, float y, int lock_msg, int *out_msg)
{
    int best_msg = -1;
    int best_off = 0;
    float best_dist = 1e9f;
    bool inside = false;

    for (int h = 0; h < s_hit_count; h++)
    {
        if (lock_msg >= 0 && s_hits[h].msg != lock_msg)
        {
            continue;
        }
        Clay_ElementData el = Clay_GetElementData(CLAY_IDI("ChatRun", h));
        if (!el.found)
        {
            continue;
        }
        Clay_BoundingBox box = el.boundingBox;
        bool in = x >= box.x && x <= box.x + box.width && y >= box.y && y <= box.y + box.height;
        float dx = 0;
        float dy = 0;
        if (x < box.x)
        {
            dx = box.x - x;
        }
        else if (x > box.x + box.width)
        {
            dx = x - (box.x + box.width);
        }
        if (y < box.y)
        {
            dy = box.y - y;
        }
        else if (y > box.y + box.height)
        {
            dy = y - (box.y + box.height);
        }
        float dist = dx * dx + dy * dy;
        if (inside && !in)
        {
            continue;
        }
        if (in && !inside)
        {
            inside = true;
            best_dist = dist;
            best_msg = s_hits[h].msg;
            best_off = HitOffset(app, &s_hits[h], box, x, y);
            continue;
        }
        if (dist <= best_dist)
        {
            best_dist = dist;
            best_msg = s_hits[h].msg;
            best_off = HitOffset(app, &s_hits[h], box, x, y);
        }
    }

    if (out_msg)
    {
        *out_msg = best_msg;
    }
    return best_off;
}

bool PicoChatSel_PointerOverText(void)
{
    for (int h = 0; h < s_hit_count; h++)
    {
        if (Clay_PointerOver(CLAY_IDI("ChatRun", h)))
        {
            return true;
        }
    }
    return false;
}

void PicoChatSel_DrawOverlay(PicoApp *app)
{
    if (!PicoChatSel_HasSelection(app) || !app->fonts)
    {
        return;
    }
    int msg = app->chat_sel.msg;
    if (msg < 0 || msg >= s_msg_n || !s_msgs[msg].text)
    {
        return;
    }
    int from = app->chat_sel.anchor < app->chat_sel.cursor ? app->chat_sel.anchor : app->chat_sel.cursor;
    int to = app->chat_sel.anchor > app->chat_sel.cursor ? app->chat_sel.anchor : app->chat_sel.cursor;
    Clay_ElementData scroll = Clay_GetElementData(Clay_GetElementId(CLAY_STRING("ChatScroll")));
    if (scroll.found)
    {
        Clay_BoundingBox clip = scroll.boundingBox;
        BeginScissorMode((int)clip.x, (int)clip.y, (int)clip.width, (int)clip.height);
    }
    Color fill = ClayToRay(COLOR_SELECTION);

    for (int h = 0; h < s_hit_count; h++)
    {
        SelHit *hit = &s_hits[h];
        if (hit->msg != msg)
        {
            continue;
        }
        int hs = hit->start;
        int he = hit->start + hit->length;
        if (he <= from || hs >= to)
        {
            continue;
        }
        Clay_ElementData el = Clay_GetElementData(CLAY_IDI("ChatRun", h));
        if (!el.found)
        {
            continue;
        }
        Clay_BoundingBox box = el.boundingBox;
        Font font = Pico_FontAt(hit->font_id, hit->font_size);
        float size = (float)hit->font_size;
        const char *s = s_msgs[msg].text + hit->start;
        int len = hit->length;
        bool wrap = hit->wrap != CLAY_TEXT_WRAP_NONE;
        WrapLine lines[SEL_MAX_WRAP_LINES];
        int nlines = WrapRun(font, size, s, len, wrap ? box.width : 0, wrap, lines, SEL_MAX_WRAP_LINES);
        float lh = LineHeight(hit, font, box.height, nlines);
        int local_from = from > hs ? from - hs : 0;
        int local_to = to < he ? to - hs : len;

        for (int li = 0; li < nlines; li++)
        {
            int ls = lines[li].start;
            int le = ls + lines[li].length;
            if (le <= local_from || ls >= local_to)
            {
                continue;
            }
            int a = local_from > ls ? local_from : ls;
            int b = local_to < le ? local_to : le;
            float x0 = MeasureN(font, size, s + ls, a - ls);
            float x1 = MeasureN(font, size, s + ls, b - ls);
            float w = x1 - x0;
            if (w < 2)
            {
                w = 2;
            }
            DrawRectangle((int)(box.x + x0), (int)(box.y + (float)li * lh), (int)w, (int)(lh > 2 ? lh : 2), fill);
        }
    }

    if (scroll.found)
    {
        EndScissorMode();
    }
}
