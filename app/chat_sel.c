#include "chat_sel.h"
#include "host_internal.h"

#include "pico/app.h"
#include "text_range.h"

#include "raylib.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

typedef struct SelBuf {
    char *text;
    int len;
    int cap;
    int first_hit;
    int end_hit;
} SelBuf;

typedef struct WrapLine {
    int start;
    int length;
} WrapLine;

typedef struct SelHit {
    int msg;
    Clay_ElementId horizontal_clip;
    bool temporary_clip;
    int start;
    int length;
    uint16_t font_id;
    uint16_t font_size;
    uint16_t line_height;
    Clay_TextElementConfigWrapMode wrap;
    float *advances;
    int advance_capacity;
    WrapLine *lines;
    int line_capacity;
    int line_count;
    Clay_BoundingBox box;
    float line_height_px;
} SelHit;

static SelBuf *s_msgs;
static int s_msg_n;
static int s_msg_cap;
static int s_cur_msg = -1;
static PicoChatSearch *s_search;
static Clay_ElementId s_horizontal_clip;
static bool s_temporary_clip;

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
    if (!b->text || b->cap < b->len + n + 1)
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

static bool ReserveLines(WrapLine **lines, int *capacity, int needed)
{
    if (needed <= *capacity) return true;
    int cap = *capacity ? *capacity * 2 : 8;
    while (cap < needed) cap *= 2;
    WrapLine *next = realloc(*lines, (size_t)cap * sizeof(*next));
    if (!next) return false;
    *lines = next;
    *capacity = cap;
    return true;
}

static int WrapRun(const float *advances, const char *s, int len, float max_w, bool wrap,
                   WrapLine **storage, int *capacity)
{
    if (!ReserveLines(storage, capacity, 1)) return 0;
    WrapLine *lines = *storage;
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
    while (i < len)
    {
        if (!ReserveLines(storage, capacity, n + 1)) return 0;
        lines = *storage;
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
            float cw = advances[next] - advances[i];
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
    Vector2 sample = MeasureTextEx(font, "Hg", Pico_FontPx(hit->font_size), 0);
    return sample.y > 1 ? sample.y : Pico_FontPx(hit->font_size);
}

static int OffsetOnLine(const float *advances, const char *s, int start, int length, float x)
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
        float cw = advances[next] - advances[i];
        if (width + cw * 0.5f >= x)
        {
            return i;
        }
        width += cw;
        i = next;
    }
    return end;
}

void PicoChatSel_Free(void)
{
    for (int i = 0; i < s_msg_cap; i++) free(s_msgs[i].text);
    for (int i = 0; i < s_hit_cap; i++)
    {
        free(s_hits[i].lines);
        free(s_hits[i].advances);
    }
    free(s_msgs);
    free(s_hits);
    s_msgs = NULL;
    s_hits = NULL;
    s_msg_n = s_msg_cap = s_hit_count = s_hit_cap = 0;
    s_cur_msg = -1;
    s_search = NULL;
}

void PicoChatSel_SetSearch(PicoChatSearch *search)
{
    s_search = search;
}

void PicoChatSel_SetHorizontalClip(Clay_ElementId id, bool temporary)
{
    s_horizontal_clip = id;
    s_temporary_clip = temporary;
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
        s_msgs[i].first_hit = s_msgs[i].end_hit = 0;
        if (s_msgs[i].text)
        {
            s_msgs[i].text[0] = '\0';
        }
    }
    s_search = NULL;
    s_hit_count = 0;
    s_cur_msg = -1;
    s_horizontal_clip = (Clay_ElementId){0};
    s_temporary_clip = false;
}

void PicoChatSel_SetMessage(int msg)
{
    if (s_search && s_cur_msg >= 0 && s_cur_msg < s_msg_n)
    {
        PicoChatSearch_SetText(s_search, s_cur_msg, s_msgs[s_cur_msg].text);
    }
    s_cur_msg = (msg >= 0 && msg < s_msg_n) ? msg : -1;
    if (s_cur_msg >= 0) s_msgs[s_cur_msg].first_hit = s_msgs[s_cur_msg].end_hit = s_hit_count;
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
        memset(next + s_hit_cap, 0, (size_t)(cap - s_hit_cap) * sizeof(*next));
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
    s_hits[id].horizontal_clip = s_horizontal_clip;
    s_hits[id].temporary_clip = s_temporary_clip;
    s_hits[id].start = start;
    s_hits[id].length = text.length;
    s_hits[id].font_id = config.fontId;
    s_hits[id].font_size = config.fontSize;
    s_hits[id].line_height = config.lineHeight;
    s_hits[id].wrap = config.wrapMode;
    s_hits[id].line_count = 0;
    s_hit_count++;
    if (b) b->end_hit = s_hit_count;

    CLAY(CLAY_IDI("ChatRun", id), {})
    {
        CLAY_TEXT(text, config);
    }
}

bool PicoChatSel_HasSelection(const PicoHost *app)
{
    return app && app->chat_sel.msg >= 0 && app->chat_sel.anchor != app->chat_sel.cursor;
}

void PicoChatSel_Clear(PicoHost *app)
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
    app->chat_sel.pressed_group = false;
    app->chat_sel.granularity = 1;
    app->chat_sel.unit_from = 0;
    app->chat_sel.unit_to = 0;
    PicoClickSeq_Reset(&app->chat_sel.click_seq);
}

void PicoChatSel_Copy(PicoHost *app)
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

void PicoChatSel_Clamp(PicoHost *app)
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

static bool MsgText(int msg, const char **text, int *len)
{
    if (msg < 0 || msg >= s_msg_n || !s_msgs[msg].text)
    {
        return false;
    }
    *text = s_msgs[msg].text;
    *len = s_msgs[msg].len;
    return true;
}

static void UnitRange(const char *text, int len, int pos, int granularity, int *from, int *to)
{
    if (granularity >= 3)
    {
        PicoText_ParaRange(text, len, pos, from, to);
    }
    else
    {
        PicoText_WordRange(text, len, pos, from, to);
    }
}

void PicoChatSel_SelectUnitAt(PicoHost *app, int msg, int pos, int granularity)
{
    if (!app)
    {
        return;
    }
    if (granularity < 1)
    {
        granularity = 1;
    }
    app->chat_sel.msg = msg;
    app->chat_sel.granularity = granularity;
    const char *text = NULL;
    int len = 0;
    if (granularity <= 1 || !MsgText(msg, &text, &len))
    {
        app->chat_sel.anchor = pos;
        app->chat_sel.cursor = pos;
        app->chat_sel.unit_from = pos;
        app->chat_sel.unit_to = pos;
        return;
    }
    int from = pos;
    int to = pos;
    UnitRange(text, len, pos, granularity, &from, &to);
    app->chat_sel.unit_from = from;
    app->chat_sel.unit_to = to;
    app->chat_sel.anchor = from;
    app->chat_sel.cursor = to;
}

void PicoChatSel_ExtendUnitTo(PicoHost *app, int pos)
{
    if (!app)
    {
        return;
    }
    int granularity = app->chat_sel.granularity;
    if (granularity <= 1)
    {
        app->chat_sel.cursor = pos;
        return;
    }
    const char *text = NULL;
    int len = 0;
    if (!MsgText(app->chat_sel.msg, &text, &len))
    {
        app->chat_sel.cursor = pos;
        return;
    }
    int from = pos;
    int to = pos;
    UnitRange(text, len, pos, granularity, &from, &to);
    int span_from = 0;
    int span_to = 0;
    PicoText_UnionRange(app->chat_sel.unit_from, app->chat_sel.unit_to, from, to, &span_from, &span_to);
    if (pos >= app->chat_sel.unit_from)
    {
        app->chat_sel.anchor = app->chat_sel.unit_from;
        app->chat_sel.cursor = span_to;
    }
    else
    {
        app->chat_sel.anchor = app->chat_sel.unit_to;
        app->chat_sel.cursor = span_from;
    }
}

static bool PrepareHit(SelHit *hit, Clay_BoundingBox box)
{
    if (hit->line_count && memcmp(&box, &hit->box, sizeof(box)) == 0) return true;
    SelBuf *b = &s_msgs[hit->msg];
    Font font = Pico_FontAt(hit->font_id, hit->font_size);
    bool wrap = hit->wrap != CLAY_TEXT_WRAP_NONE;
    if (hit->length + 1 > hit->advance_capacity)
    {
        float *next = realloc(hit->advances, (size_t)(hit->length + 1) * sizeof(*next));
        if (!next) return false;
        hit->advances = next;
        hit->advance_capacity = hit->length + 1;
    }
    const char *text = b->text + hit->start;
    hit->advances[0] = 0;
    for (int i = 0; i < hit->length;)
    {
        int next = Utf8Next(text, hit->length, i);
        float width = MeasureN(font, Pico_FontPx(hit->font_size), text + i, next - i);
        for (int j = i + 1; j < next; j++) hit->advances[j] = hit->advances[i];
        hit->advances[next] = hit->advances[i] + width;
        i = next;
    }
    /* Prefix advances make a dense one-character query linear in run length,
     * instead of measuring each increasingly long prefix for every match. */
    hit->line_count = WrapRun(hit->advances, text, hit->length, wrap ? box.width : 0, wrap,
                              &hit->lines, &hit->line_capacity);
    hit->box = box;
    hit->line_height_px = LineHeight(hit, font, box.height, hit->line_count);
    return hit->line_count > 0;
}

static int HitOffset(SelHit *hit, Clay_BoundingBox box, float x, float y)
{
    if (!PrepareHit(hit, box)) return hit->start;
    int li = hit->line_height_px > 1 ? (int)((y - box.y) / hit->line_height_px) : 0;
    if (li < 0) return hit->start;
    if (li >= hit->line_count) return hit->start + hit->length;
    return hit->start + OffsetOnLine(hit->advances,
                                     s_msgs[hit->msg].text + hit->start,
                                     hit->lines[li].start, hit->lines[li].length, x - box.x);
}

int PicoChatSel_OffsetAtPoint(PicoHost *app, float x, float y, int lock_msg, int *out_msg)
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
            best_off = HitOffset(&s_hits[h], box, x, y);
            continue;
        }
        if (dist <= best_dist)
        {
            best_dist = dist;
            best_msg = s_hits[h].msg;
            best_off = HitOffset(&s_hits[h], box, x, y);
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

void PicoChatSel_VisitRange(int msg, int from, int to, PicoChatRangeFn visit, void *user)
{
    if (!visit || msg < 0 || msg >= s_msg_n || !s_msgs[msg].text || to <= from) return;
    SelBuf *buffer = &s_msgs[msg];
    /* Runs are ordered by byte range. A common one-character query must not
     * rescan the entire message for each match. */
    int lo = buffer->first_hit, hi = buffer->end_hit;
    while (lo < hi)
    {
        int mid = lo + (hi - lo) / 2;
        if (s_hits[mid].start + s_hits[mid].length <= from) lo = mid + 1;
        else hi = mid;
    }
    bool found = false;
    for (int h = lo; h < buffer->end_hit && s_hits[h].start < to; h++)
    {
        SelHit *hit = &s_hits[h];
        Clay_ElementData el = Clay_GetElementData(CLAY_IDI("ChatRun", h));
        if (!el.found || !PrepareHit(hit, el.boundingBox)) continue;
        Clay_BoundingBox box = el.boundingBox;
        int local_from = from > hit->start ? from - hit->start : 0;
        int local_to = to - hit->start;
        /* Likewise skip preceding wrapped lines within a long Clay text run. */
        int a = 0, b = hit->line_count;
        while (a < b)
        {
            int mid = a + (b - a) / 2;
            if (hit->lines[mid].start + hit->lines[mid].length <= local_from) a = mid + 1;
            else b = mid;
        }
        for (int li = a; li < hit->line_count && hit->lines[li].start < local_to; li++)
        {
            int ls = hit->lines[li].start;
            int le = ls + hit->lines[li].length;
            int start = local_from > ls ? local_from : ls;
            int end = local_to < le ? local_to : le;
            float x0 = hit->advances[start] - hit->advances[ls];
            float x1 = hit->advances[end] - hit->advances[ls];
            visit((Clay_BoundingBox){box.x + x0, box.y + (float)li * hit->line_height_px,
                                    fmaxf(2, x1 - x0), hit->line_height_px}, hit->horizontal_clip, hit->temporary_clip, user);
            found = true;
        }
    }
    /* A soft-wrap separator has no glyph. Give whitespace-only hits a caret
     * at the neighboring run instead of counting an unreachable result. */
    if (!found && buffer->first_hit < buffer->end_hit)
    {
        int h = lo > buffer->first_hit ? lo - 1 : lo;
        if (h >= buffer->end_hit) h = buffer->end_hit - 1;
        SelHit *hit = &s_hits[h];
        Clay_ElementData el = Clay_GetElementData(CLAY_IDI("ChatRun", h));
        if (el.found && PrepareHit(hit, el.boundingBox))
        {
            int li = from >= hit->start ? hit->line_count - 1 : 0;
            WrapLine line = hit->lines[li];
            float x = from >= hit->start ? hit->advances[line.start + line.length] - hit->advances[line.start] : 0;
            visit((Clay_BoundingBox){el.boundingBox.x + x, el.boundingBox.y + li * hit->line_height_px,
                                    2, hit->line_height_px}, hit->horizontal_clip, hit->temporary_clip, user);
        }
    }
}

static void DrawSelectionRange(Clay_BoundingBox box, Clay_ElementId horizontal_clip, bool temporary, void *user)
{
    (void)user;
    (void)temporary;
    Clay_ElementData scroll = Clay_GetElementData(CLAY_ID("ChatScroll"));
    if (!scroll.found) return;
    Clay_BoundingBox clip = scroll.boundingBox;
    Clay_ElementData horizontal = Clay_GetElementData(horizontal_clip);
    if (horizontal.found)
    {
        float right = fminf(clip.x + clip.width, horizontal.boundingBox.x + horizontal.boundingBox.width);
        clip.x = fmaxf(clip.x, horizontal.boundingBox.x);
        clip.width = fmaxf(0, right - clip.x);
    }
    BeginScissorMode((int)ceilf(clip.x), (int)ceilf(clip.y), (int)clip.width, (int)clip.height);
    DrawRectangle((int)box.x, (int)box.y, (int)box.width, (int)box.height, ClayToRay(COLOR_SELECTION));
    EndScissorMode();
}

void PicoChatSel_DrawOverlay(PicoHost *app)
{
    if (!PicoChatSel_HasSelection(app) || !app->fonts) return;
    int from = app->chat_sel.anchor < app->chat_sel.cursor ? app->chat_sel.anchor : app->chat_sel.cursor;
    int to = app->chat_sel.anchor > app->chat_sel.cursor ? app->chat_sel.anchor : app->chat_sel.cursor;
    PicoChatSel_VisitRange(app->chat_sel.msg, from, to, DrawSelectionRange, NULL);
}
