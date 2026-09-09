#include "chat_find.h"
#include "host_internal.h"
#include "agent_internal.h"
#include "chat_sel.h"
#include "text_range.h"
#include "richtext.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static Clay_String String(const char *s)
{
    return (Clay_String){.chars = s ? s : "", .length = s ? (int)strlen(s) : 0};
}

static Clay_TextElementConfig InputConfig(void)
{
    return (Clay_TextElementConfig){.fontId = FONT_REGULAR, .fontSize = PICO_FONT_UI,
                                    .textColor = COLOR_TEXT, .wrapMode = CLAY_TEXT_WRAP_NONE};
}

static float TextWidth(const char *s, int n)
{
    Clay_TextElementConfig config = InputConfig();
    return Pico_MeasureTextUtf8((Clay_StringSlice){.chars = s, .length = n, .baseChars = s},
                               &config, NULL).width;
}

typedef struct PicoFindScrollRestore {
    struct PicoFindScrollRestore *next;
    Clay_ElementId id;
    float x;
} PicoFindScrollRestore;

static void RestoreTemporaryScrolls(PicoChatFind *f)
{
    while (f->temporary_scrolls)
    {
        PicoFindScrollRestore *entry = f->temporary_scrolls;
        if (Clay_GetCurrentContext())
        {
            Clay_ScrollContainerData data = Clay_GetScrollContainerData(entry->id);
            if (data.found && data.scrollPosition) data.scrollPosition->x = entry->x;
        }
        f->temporary_scrolls = entry->next;
        free(entry);
    }
}

static void RememberTemporaryScroll(PicoChatFind *f, Clay_ElementId id, float x)
{
    for (PicoFindScrollRestore *p = f->temporary_scrolls; p; p = p->next)
        if (p->id.id == id.id) return;
    PicoFindScrollRestore *entry = malloc(sizeof(*entry));
    if (!entry) return;
    *entry = (PicoFindScrollRestore){.next = f->temporary_scrolls, .id = id, .x = x};
    f->temporary_scrolls = entry;
}

void PicoChatFind_Reset(PicoHost *app)
{
    if (!app) return;
    RestoreTemporaryScrolls(&app->find);
    PicoChatSearch_Free(&app->find.search);
    free(app->find.query);
    memset(&app->find, 0, sizeof(app->find));
    app->find.search.active = -1;
}

void PicoChatFind_Sync(PicoHost *app)
{
    PicoAgent *agent = PicoHost_SelectedAgent(app);
    PicoChatFind *f = &app->find;
    uint64_t id = agent ? agent->id : 0;
    const char *session = agent ? agent->session_id : "";
    if (f->agent_id != id || (f->session_id[0] && strcmp(f->session_id, session) != 0) ||
        (agent && agent->message_count < f->search.message_count))
    {
        PicoChatFind_Reset(app);
        f->agent_id = id;
    }
    /* Assigning a durable ID to the current draft is not a conversation switch. */
    snprintf(f->session_id, sizeof(f->session_id), "%s", session);
    PicoChatSearch_Resize(&f->search, agent ? agent->message_count : 0);
}

void PicoChatFind_Open(PicoHost *app)
{
    if (!app || PicoUi_ModalOpen(app) || !PicoHost_SelectedAgent(app)) return;
    PicoChatFind_Sync(app);
    PicoChatFind *f = &app->find;
    bool was_open = f->open;
    f->open = f->focused = f->claimed_input = true;
    f->anchor = 0;
    f->cursor = f->length;
    if (!was_open)
    {
        PicoChatSearch_Query(&f->search, f->query);
        PicoChatSearch_Refresh(&f->search);
        f->nearest = f->length > 0;
    }
}

void PicoChatFind_Close(PicoHost *app)
{
    PicoChatFind *f = &app->find;
    RestoreTemporaryScrolls(f);
    f->open = f->focused = f->dragging = f->nearest = f->reveal = false;
    f->pressed_button = 0;
    f->claimed_input = true; /* Closing must not leak Escape/Enter this frame. */
    PicoChatSearch_Query(&f->search, "");
    PicoChatSearch_Refresh(&f->search);
}

static bool Reserve(PicoChatFind *f, int length)
{
    if (length + 1 <= f->capacity) return true;
    int capacity = f->capacity ? f->capacity : 64;
    while (capacity <= length) capacity *= 2;
    char *next = realloc(f->query, (size_t)capacity);
    if (!next) return false;
    f->query = next;
    f->capacity = capacity;
    return true;
}

static void QueryChanged(PicoChatFind *f)
{
    RestoreTemporaryScrolls(f);
    PicoChatSearch_Query(&f->search, f->open ? f->query : "");
    f->nearest = f->open && f->length > 0;
    f->reveal = false;
}

void PicoChatFind_SetQuery(PicoHost *app, const char *query)
{
    PicoChatFind *f = &app->find;
    if (!query) query = "";
    /* This is a single-line editor. Pasted line/control separators become
     * spaces, so a query can never cross a logical block boundary. */
    int n = (int)strlen(query);
    char *copy = malloc((size_t)n + 1);
    if (!copy) return;
    for (int i = 0; i < n; i++) copy[i] = (unsigned char)query[i] < 32 ? ' ' : query[i];
    copy[n] = 0;
    if (Reserve(f, n))
    {
        memcpy(f->query, copy, (size_t)n + 1);
        f->length = f->cursor = f->anchor = n;
        QueryChanged(f);
    }
    free(copy);
}

void PicoChatFind_Navigate(PicoHost *app, int direction)
{
    PicoChatFind *f = &app->find;
    if (!f->open) return;
    PicoChatSearch_Refresh(&f->search);
    PicoChatSearch_Step(&f->search, direction);
    f->nearest = false;
    f->reveal = f->search.count > 0;
}

bool PicoChatFind_BlocksInput(const PicoHost *app)
{
    return app && (app->find.claimed_input || (app->find.open && app->find.focused));
}

static bool PointerOverId(Clay_ElementId id)
{
    Clay_ElementData element = Clay_GetElementData(id);
    Vector2 point = GetMousePosition();
    Clay_BoundingBox box = element.boundingBox;
    return element.found && point.x >= box.x && point.y >= box.y &&
           point.x < box.x + box.width && point.y < box.y + box.height;
}

bool PicoChatFind_PointerOver(const PicoHost *app)
{
    return app && !PicoUi_ModalOpen((PicoHost *)app) &&
           (app->find.claimed_pointer || (app->find.open && PointerOverId(CLAY_ID("ChatFind"))));
}

static void ReplaceSelection(PicoChatFind *f, const char *text, int length)
{
    int from = f->cursor < f->anchor ? f->cursor : f->anchor;
    int to = f->cursor > f->anchor ? f->cursor : f->anchor;
    int next_length = f->length - (to - from) + length;
    if (!Reserve(f, next_length)) return;
    memmove(f->query + from + length, f->query + to, (size_t)(f->length - to));
    if (length) memcpy(f->query + from, text, (size_t)length);
    f->query[next_length] = 0;
    f->length = next_length;
    f->cursor = f->anchor = from + length;
    QueryChanged(f);
}

static void Move(PicoChatFind *f, int position, bool shift)
{
    f->cursor = position;
    if (!shift) f->anchor = position;
}

static bool Key(int key)
{
    return IsKeyPressed(key) || IsKeyPressedRepeat(key);
}

static int PointerOffset(PicoChatFind *f, float x)
{
    Clay_ElementData input = Clay_GetElementData(CLAY_ID("ChatFindInput"));
    x -= input.boundingBox.x + 6 - f->input_scroll;
    const char *s = f->query ? f->query : "";
    for (int i = 0; i < f->length;)
    {
        int next = PicoText_Utf8Next(s, f->length, i);
        if (x < (TextWidth(s, i) + TextWidth(s, next)) * 0.5f) return i;
        i = next;
    }
    return f->length;
}

void PicoChatFind_HandleInput(PicoHost *app)
{
    PicoChatFind_Sync(app);
    PicoChatFind *f = &app->find;
    f->claimed_input = false;
    f->claimed_pointer = false;
    if (PicoUi_ModalOpen(app)) { f->dragging = false; f->pressed_button = 0; return; }
    bool ctrl = IsKeyDown(KEY_LEFT_CONTROL) || IsKeyDown(KEY_RIGHT_CONTROL);
    bool shift = IsKeyDown(KEY_LEFT_SHIFT) || IsKeyDown(KEY_RIGHT_SHIFT);
    if (ctrl && Pico_ShortcutPressed('f')) PicoChatFind_Open(app);
    if (!f->open) return;
    f->claimed_pointer = PointerOverId(CLAY_ID("ChatFind")) &&
                         (IsMouseButtonDown(MOUSE_BUTTON_LEFT) || IsMouseButtonReleased(MOUSE_BUTTON_LEFT));
    if (IsKeyPressed(KEY_ESCAPE)) { PicoChatFind_Close(app); return; }

    if (IsMouseButtonPressed(MOUSE_BUTTON_LEFT))
    {
        f->pressed_button = 0;
        if (PointerOverId(CLAY_ID("ChatFindPrevious"))) f->pressed_button = -1;
        else if (PointerOverId(CLAY_ID("ChatFindNext"))) f->pressed_button = 1;
        else if (PointerOverId(CLAY_ID("ChatFindClose"))) f->pressed_button = 2;
        if (PointerOverId(CLAY_ID("ChatFindInput")))
        {
            f->focused = f->dragging = true;
            Move(f, PointerOffset(f, GetMousePosition().x), shift);
        }
        else if (PointerOverId(CLAY_ID("Composer"))) f->focused = false;
        else if (f->pressed_button) f->focused = true;
    }
    if (f->dragging && IsMouseButtonDown(MOUSE_BUTTON_LEFT))
        f->cursor = PointerOffset(f, GetMousePosition().x);
    if (IsMouseButtonReleased(MOUSE_BUTTON_LEFT))
    {
        if (f->pressed_button == 2 && PointerOverId(CLAY_ID("ChatFindClose")))
            PicoChatFind_Close(app);
        else if ((f->pressed_button == -1 && PointerOverId(CLAY_ID("ChatFindPrevious"))) ||
                 (f->pressed_button == 1 && PointerOverId(CLAY_ID("ChatFindNext"))))
            PicoChatFind_Navigate(app, f->pressed_button);
        f->pressed_button = 0;
        f->dragging = false;
    }
    if (!f->open || !f->focused) return;
    f->claimed_input = true;
    const char *s = f->query ? f->query : "";
    if (ctrl && Pico_ShortcutPressed('a')) { f->anchor = 0; f->cursor = f->length; }
    if (ctrl && (Pico_ShortcutPressed('c') || Pico_ShortcutPressed('x')))
    {
        int from = f->cursor < f->anchor ? f->cursor : f->anchor;
        int to = f->cursor > f->anchor ? f->cursor : f->anchor;
        if (to > from)
        {
            char *copy = malloc((size_t)(to - from) + 1);
            if (copy)
            {
                memcpy(copy, s + from, (size_t)(to - from)); copy[to - from] = 0;
                SetClipboardText(copy); free(copy);
                if (Pico_ShortcutPressed('x')) ReplaceSelection(f, "", 0);
            }
        }
    }
    if (ctrl && Pico_ShortcutPressed('v'))
    {
        const char *clip = GetClipboardText();
        if (clip)
        {
            char *copy = strdup(clip);
            if (copy)
            {
                for (char *p = copy; *p; p++) if ((unsigned char)*p < 32) *p = ' ';
                ReplaceSelection(f, copy, (int)strlen(copy)); free(copy);
            }
        }
    }
    s = f->query ? f->query : "";
    if (Key(KEY_LEFT))
        Move(f, !shift && f->cursor != f->anchor ? (f->cursor < f->anchor ? f->cursor : f->anchor) :
                ctrl ? PicoText_PrevWord(s, f->cursor) : PicoText_Utf8Prev(s, f->cursor), shift);
    if (Key(KEY_RIGHT))
        Move(f, !shift && f->cursor != f->anchor ? (f->cursor > f->anchor ? f->cursor : f->anchor) :
                ctrl ? PicoText_NextWord(s, f->length, f->cursor) : PicoText_Utf8Next(s, f->length, f->cursor), shift);
    if (Key(KEY_HOME)) Move(f, 0, shift);
    if (Key(KEY_END)) Move(f, f->length, shift);
    if (Key(KEY_BACKSPACE))
    {
        if (f->cursor == f->anchor) f->anchor = ctrl ? PicoText_PrevWord(s, f->cursor) : PicoText_Utf8Prev(s, f->cursor);
        ReplaceSelection(f, "", 0);
    }
    if (Key(KEY_DELETE))
    {
        s = f->query ? f->query : "";
        if (f->cursor == f->anchor) f->anchor = ctrl ? PicoText_NextWord(s, f->length, f->cursor) : PicoText_Utf8Next(s, f->length, f->cursor);
        ReplaceSelection(f, "", 0);
    }
    if (IsKeyPressed(KEY_ENTER) || IsKeyPressed(KEY_KP_ENTER)) PicoChatFind_Navigate(app, shift ? -1 : 1);
    int cp;
    while ((cp = GetCharPressed()) != 0)
    {
        if (ctrl || cp < 32 || cp == 127) continue;
        char bytes[4];
        int n = PicoText_Utf8Encode(cp, bytes);
        if (n) ReplaceSelection(f, bytes, n);
    }
}

static void Button(const char *id, const char *label, bool enabled)
{
    Clay_ElementId element = Clay_GetElementId(String(id));
    Clay_Color background = enabled && Clay_PointerOver(element) ? COLOR_CODE_BG : COLOR_COMPOSER_BG;
    CLAY(element, {.layout = {.sizing = {.width = CLAY_SIZING_FIXED(28), .height = CLAY_SIZING_FIXED(28)},
                              .childAlignment = {.x = CLAY_ALIGN_X_CENTER, .y = CLAY_ALIGN_Y_CENTER}},
                   .backgroundColor = background, .cornerRadius = CLAY_CORNER_RADIUS(4)})
    {
        Clay_TextElementConfig config = InputConfig();
        config.textColor = enabled ? COLOR_TEXT : COLOR_MUTED;
        CLAY_TEXT(String(label), CLAY_TEXT_CONFIG(config));
    }
}

void PicoChatFind_Render(PicoHost *app)
{
    PicoChatFind *f = &app->find;
    if (!f->open || PicoUi_ModalOpen(app)) return;
    PicoChatSearch_Refresh(&f->search);
    if (f->search.failed) snprintf(f->counter, sizeof(f->counter), "Unavailable");
    else if (f->length) snprintf(f->counter, sizeof(f->counter), "%d of %d", f->search.active + 1, f->search.count);
    else snprintf(f->counter, sizeof(f->counter), "0 of 0");
    float width = fminf(440, fmaxf(160, Clay_GetLayoutDimensions().width - 24));
    float field_width = fmaxf(20, width - 200);
    const char *query = f->query ? f->query : "";
    float caret = TextWidth(query, f->cursor);
    if (caret < f->input_scroll) f->input_scroll = caret;
    if (caret > f->input_scroll + field_width - 14) f->input_scroll = caret - field_width + 14;
    f->input_scroll = fmaxf(0, fminf(f->input_scroll, TextWidth(query, f->length)));
    CLAY(CLAY_ID("ChatFind"),
         {.floating = {.attachTo = CLAY_ATTACH_TO_ROOT, .zIndex = 30,
                       .attachPoints = {.element = CLAY_ATTACH_POINT_RIGHT_TOP, .parent = CLAY_ATTACH_POINT_RIGHT_TOP},
                       .offset = {.x = -12, .y = 12}},
          .layout = {.layoutDirection = CLAY_LEFT_TO_RIGHT, .padding = CLAY_PADDING_ALL(6), .childGap = 4,
                     .sizing = {.width = CLAY_SIZING_FIXED(width), .height = CLAY_SIZING_FIXED(40)},
                     .childAlignment = {.y = CLAY_ALIGN_Y_CENTER}},
          .backgroundColor = COLOR_COMPOSER_BG, .cornerRadius = CLAY_CORNER_RADIUS(6)})
    {
        CLAY(CLAY_ID("ChatFindInput"),
             {.layout = {.padding = {6, 6, 0, 0},
                         .sizing = {.width = CLAY_SIZING_GROW(20), .height = CLAY_SIZING_FIXED(28)},
                         .childAlignment = {.y = CLAY_ALIGN_Y_CENTER}},
              .clip = {.horizontal = true, .vertical = true, .childOffset = {.x = -f->input_scroll}},
              .backgroundColor = COLOR_CODE_BG, .cornerRadius = CLAY_CORNER_RADIUS(4)})
        {
            Clay_TextElementConfig config = InputConfig();
            if (!f->length) config.textColor = COLOR_MUTED;
            CLAY_TEXT(String(f->length ? query : "Find in chat"), CLAY_TEXT_CONFIG(config));
        }
        CLAY(CLAY_ID("ChatFindCount"), {.layout = {.sizing = {.width = CLAY_SIZING_FIXED(88)}}})
        {
            Clay_TextElementConfig config = InputConfig();
            config.fontSize = PICO_FONT_CAPTION;
            config.textColor = COLOR_MUTED;
            CLAY_TEXT(String(f->counter), CLAY_TEXT_CONFIG(config));
        }
        Button("ChatFindPrevious", "↑", f->search.count > 0);
        Button("ChatFindNext", "↓", f->search.count > 0);
        Button("ChatFindClose", "×", true);
    }
}

void PicoChatFind_DrawInput(PicoHost *app)
{
    PicoChatFind *f = &app->find;
    if (!f->open || !f->focused || PicoUi_ModalOpen(app)) return;
    Clay_ElementData input = Clay_GetElementData(CLAY_ID("ChatFindInput"));
    if (!input.found) return;
    Clay_BoundingBox box = input.boundingBox;
    BeginScissorMode((int)box.x, (int)box.y, (int)box.width, (int)box.height);
    const char *s = f->query ? f->query : "";
    float x = box.x + 6 - f->input_scroll;
    float a = TextWidth(s, f->anchor), b = TextWidth(s, f->cursor);
    if (a != b) DrawRectangle((int)(x + fminf(a, b)), (int)box.y + 3, (int)fabsf(a - b), (int)box.height - 6,
                              (Color){100, 150, 240, 90});
    if (fmod(GetTime(), 1.0) < 0.6) DrawRectangle((int)(x + b), (int)box.y + 4, 1, (int)box.height - 8,
                                                (Color){(unsigned char)COLOR_TEXT.r, (unsigned char)COLOR_TEXT.g,
                                                        (unsigned char)COLOR_TEXT.b, 255});
    EndScissorMode();
}

typedef struct RangeBounds {
    bool found;
    Clay_BoundingBox box;
    Clay_ElementId horizontal;
    bool temporary;
    float center;
    float distance;
} RangeBounds;

static void Bounds(Clay_BoundingBox box, Clay_ElementId horizontal, bool temporary, void *user)
{
    RangeBounds *b = user;
    float distance = fabsf(box.y + box.height * 0.5f - b->center);
    if (!b->found)
    {
        b->box = box;
        b->horizontal = horizontal;
        b->temporary = temporary;
        b->distance = distance;
        b->found = true;
    }
    else
    {
        float right = fmaxf(b->box.x + b->box.width, box.x + box.width);
        float bottom = fmaxf(b->box.y + b->box.height, box.y + box.height);
        b->box.x = fminf(b->box.x, box.x);
        b->box.y = fminf(b->box.y, box.y);
        b->box.width = right - b->box.x;
        b->box.height = bottom - b->box.y;
        b->distance = fminf(b->distance, distance);
    }
}

void PicoChatFind_MountTargets(PicoHost *app, PicoTranscriptVirtual *cache, float gap)
{
    PicoChatFind *f = &app->find;
    if (!f->open || (!f->nearest && !f->reveal)) return;
    PicoChatSearch_Refresh(&f->search);
    if (!f->nearest)
    {
        if (f->search.active >= 0 && f->search.active < f->search.count)
            PicoTranscriptVirtual_ForceMount(cache, f->search.matches[f->search.active].message);
        return;
    }
    /* Only the matching message containing the center and its nearest matching
     * neighbors can contain the closest hit. Measure those, not every result. */
    Clay_ScrollContainerData scroll = Clay_GetScrollContainerData(CLAY_ID("ChatScroll"));
    float center = scroll.found && scroll.scrollPosition
                       ? -scroll.scrollPosition->y + scroll.scrollContainerDimensions.height * 0.5f : 0;
    int before = -1, after = -1;
    float top = 8; /* ChatContent's top padding, not a message-height estimate. */
    for (int i = 0; i < cache->count && i < f->search.message_count; i++)
    {
        float bottom = top + cache->heights[i];
        if (f->search.messages[i].count)
        {
            if (bottom < center) before = i;
            else if (top > center) { if (after < 0) after = i; }
            else PicoTranscriptVirtual_ForceMount(cache, i);
        }
        top = bottom + gap;
    }
    PicoTranscriptVirtual_ForceMount(cache, before);
    PicoTranscriptVirtual_ForceMount(cache, after);
}

bool PicoChatFind_Reveal(PicoHost *app)
{
    PicoChatFind *f = &app->find;
    if (!f->open || PicoUi_ModalOpen(app)) return false;
    PicoChatSearch_Refresh(&f->search);
    if (!f->nearest && !f->reveal) return false;
    if (!f->search.count) { f->nearest = f->reveal = false; return false; }
    Clay_ElementData viewport = Clay_GetElementData(CLAY_ID("ChatScroll"));
    Clay_ScrollContainerData scroll = Clay_GetScrollContainerData(CLAY_ID("ChatScroll"));
    if (!viewport.found || !scroll.found || !scroll.scrollPosition) return false;
    Clay_BoundingBox clip = viewport.boundingBox;
    float center = clip.y + clip.height * 0.5f;
    int previous_active = f->search.active;
    if (f->nearest)
    {
        int best = -1;
        float distance = INFINITY;
        for (int i = 0; i < f->search.count; i++)
        {
            PicoChatMatch match = f->search.matches[i];
            RangeBounds bounds = {.center = center};
            PicoChatSel_VisitRange(match.message, match.from, match.to, Bounds, &bounds);
            if (bounds.found && bounds.distance < distance)
            {
                best = i;
                distance = bounds.distance;
            }
        }
        if (best < 0) return false;
        f->search.active = best;
    }
    PicoChatMatch match = f->search.matches[f->search.active];
    RangeBounds bounds = {.center = center};
    PicoChatSel_VisitRange(match.message, match.from, match.to, Bounds, &bounds);
    if (!bounds.found) return false; /* Force mounting completes next layout. */
    f->nearest = f->reveal = false;
    bool changed = app->chat_follow_bottom || previous_active != f->search.active;
    app->chat_follow_bottom = false;
    Clay_BoundingBox box = bounds.box;
    if (box.y < clip.y || box.y + box.height > clip.y + clip.height)
    {
        float target_center = box.y + fminf(box.height, clip.height) * 0.5f;
        float next = scroll.scrollPosition->y + center - target_center;
        float minimum = fminf(0, scroll.scrollContainerDimensions.height - scroll.contentDimensions.height);
        next = fmaxf(minimum, fminf(0, next));
        changed |= fabsf(next - scroll.scrollPosition->y) > 0.01f;
        scroll.scrollPosition->y = next;
    }
    Clay_ElementData horizontal = Clay_GetElementData(bounds.horizontal);
    Clay_ScrollContainerData hscroll = Clay_GetScrollContainerData(bounds.horizontal);
    if (horizontal.found && hscroll.found && hscroll.scrollPosition)
    {
        Clay_BoundingBox hclip = horizontal.boundingBox;
        if (bounds.temporary) RememberTemporaryScroll(f, bounds.horizontal, hscroll.scrollPosition->x);
        float next = hscroll.scrollPosition->x;
        if (box.x < hclip.x || box.x + box.width > hclip.x + hclip.width)
            next += hclip.x + hclip.width * 0.5f - (box.x + fminf(box.width, hclip.width) * 0.5f);
        next = fmaxf(fminf(0, hscroll.scrollContainerDimensions.width - hscroll.contentDimensions.width), fminf(0, next));
        changed |= fabsf(next - hscroll.scrollPosition->x) > 0.01f;
        hscroll.scrollPosition->x = next;
    }
    return changed;
}

typedef struct Highlight {
    Clay_BoundingBox viewport;
    Clay_ElementData find;
    Color color;
} Highlight;

static void DrawMatch(Clay_BoundingBox box, Clay_ElementId horizontal, bool temporary, void *user)
{
    (void)temporary;
    Highlight *h = user;
    Clay_BoundingBox clip = h->viewport;
    Clay_ElementData hc = Clay_GetElementData(horizontal);
    if (hc.found)
    {
        float right = fminf(clip.x + clip.width, hc.boundingBox.x + hc.boundingBox.width);
        clip.x = fmaxf(clip.x, hc.boundingBox.x);
        clip.width = fmaxf(0, right - clip.x);
    }
    float x = fmaxf(box.x, clip.x), y = fmaxf(box.y, clip.y);
    float right = fminf(box.x + box.width, clip.x + clip.width);
    float bottom = fminf(box.y + box.height, clip.y + clip.height);
    if (right <= x || bottom <= y) return;
    /* This overlay is drawn after Clay. Never tint the floating find box. */
    if (h->find.found)
    {
        Clay_BoundingBox f = h->find.boundingBox;
        float fx = fmaxf(x, f.x), fy = fmaxf(y, f.y);
        float fr = fminf(right, f.x + f.width), fb = fminf(bottom, f.y + f.height);
        if (fr > fx && fb > fy)
        {
            DrawRectangleRec((Rectangle){x, y, right - x, fy - y}, h->color);
            DrawRectangleRec((Rectangle){x, fb, right - x, bottom - fb}, h->color);
            DrawRectangleRec((Rectangle){x, fy, fx - x, fb - fy}, h->color);
            DrawRectangleRec((Rectangle){fr, fy, right - fr, fb - fy}, h->color);
            return;
        }
    }
    DrawRectangleRec((Rectangle){x, y, right - x, bottom - y}, h->color);
}

void PicoChatFind_DrawMatches(PicoHost *app)
{
    PicoChatFind *f = &app->find;
    if (!f->open || PicoUi_ModalOpen(app)) return;
    Clay_ElementData viewport = Clay_GetElementData(CLAY_ID("ChatScroll"));
    if (!viewport.found) return;
    Highlight h = {.viewport = viewport.boundingBox, .find = Clay_GetElementData(CLAY_ID("ChatFind"))};
    for (int i = 0; i < f->search.count; i++)
    {
        PicoChatMatch match = f->search.matches[i];
        h.color = i == f->search.active ? (Color){255, 155, 45, 135} : (Color){245, 210, 60, 80};
        PicoChatSel_VisitRange(match.message, match.from, match.to, DrawMatch, &h);
    }
}
