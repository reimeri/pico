#include "pico/plugin.h"
#include "host_internal.h"

#include "clay/clay.h"
#include "complete_internal.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>

typedef struct CompleteState {
    bool open;
    bool dismissed;
    int dismissed_start;
    int dismissed_len;
    int selected;
    int token_start;
    int token_end;
    bool token_active;
    int active_token_start;
    char active_trigger;
    uint64_t token_id;
    int count;
    PicoCompleteItem items[PICO_MAX_COMPLETE_ITEMS];
} CompleteState;

static CompleteState *GetCompleteState(void)
{
    static CompleteState *complete_instance;
    if (!complete_instance)
    {
        complete_instance = (CompleteState *)calloc(1, sizeof(CompleteState));
    }
    return complete_instance;
}

#define g_complete (*GetCompleteState())

void PicoComplete_Close(void)
{
    g_complete.open = false;
    g_complete.count = 0;
    g_complete.selected = 0;
}

bool PicoComplete_IsOpen(void)
{
    return g_complete.open;
}

void PicoComplete_BeforeEdit(int from, int to)
{
    if (g_complete.token_active && from <= g_complete.active_token_start &&
        to > g_complete.active_token_start)
    {
        g_complete.token_active = false;
    }
}

uint64_t PicoComplete_TokenId(void)
{
    return g_complete.token_id;
}

static void TrackToken(int start, char trigger)
{
    if (g_complete.token_active && g_complete.active_token_start == start &&
        g_complete.active_trigger == trigger)
    {
        return;
    }
    g_complete.token_active = true;
    g_complete.active_token_start = start;
    g_complete.active_trigger = trigger;
    g_complete.token_id++;
    if (g_complete.token_id == 0)
    {
        g_complete.token_id++;
    }
}

static void Dismiss(PicoHost *app)
{
    g_complete.dismissed = true;
    g_complete.dismissed_start = g_complete.token_start;
    g_complete.dismissed_len = app->composer.length;
    PicoComplete_Close();
}

static bool IsSpaceByte(char c)
{
    return c == ' ' || c == '\t' || c == '\n' || c == '\r';
}

static bool IsPathByte(unsigned char c)
{
    return c > 32 && c != '@';
}

static const PicoCompleter *FindCompleter(const PicoHost *app, char trigger, bool bol)
{
    for (int i = 0; i < app->completer_count; i++)
    {
        if (app->completers[i].trigger == trigger && app->completers[i].bol_only == bol)
        {
            return &app->completers[i];
        }
    }
    for (int i = 0; i < app->completer_count; i++)
    {
        if (app->completers[i].trigger == trigger)
        {
            return &app->completers[i];
        }
    }
    return NULL;
}

static bool ScanToken(const PicoHost *app, int *start, int *end, const PicoCompleter **out)
{
    const PicoComposer *c = &app->composer;
    if (!c->text || c->length <= 0 || c->cursor <= 0)
    {
        return false;
    }
    const char *s = c->text;
    int cur = c->cursor;
    if (cur > c->length)
    {
        cur = c->length;
    }
    if (s[0] == '/')
    {
        const PicoCompleter *comp = FindCompleter(app, '/', true);
        if (comp)
        {
            int i = 0;
            while (i < cur && s[i] != '\n')
            {
                i++;
            }
            if (cur <= i)
            {
                *start = 0;
                *end = cur;
                *out = comp;
                return true;
            }
        }
    }
    int i = cur;
    while (i > 0 && IsPathByte((unsigned char)s[i - 1]))
    {
        i--;
    }
    if (i > 0 && s[i - 1] == '@' && (i == 1 || IsSpaceByte(s[i - 2])))
    {
        const PicoCompleter *comp = FindCompleter(app, '@', false);
        if (comp)
        {
            *start = i - 1;
            *end = cur;
            *out = comp;
            return true;
        }
    }
    return false;
}

static int Fold(int c)
{
    return (c >= 'A' && c <= 'Z') ? c - 'A' + 'a' : c;
}

static bool PrefixMatch(const char *s, const char *prefix)
{
    if (!prefix || !prefix[0])
    {
        return true;
    }
    while (*prefix)
    {
        if (Fold((unsigned char)*s) != Fold((unsigned char)*prefix))
        {
            return false;
        }
        s++;
        prefix++;
    }
    return true;
}

static int RankItem(const PicoCompleteItem *item, const char *prefix)
{
    if (!prefix || !prefix[0])
    {
        return 1;
    }
    if (PrefixMatch(item->insert[0] ? item->insert : item->label, prefix) || PrefixMatch(item->label, prefix))
    {
        return 0;
    }
    return 1;
}

static void SortItems(PicoCompleteItem *items, int n, const char *prefix)
{
    for (int i = 0; i < n; i++)
    {
        int best = i;
        int best_rank = RankItem(&items[i], prefix);
        for (int j = i + 1; j < n; j++)
        {
            int r = RankItem(&items[j], prefix);
            if (r < best_rank)
            {
                best = j;
                best_rank = r;
            }
        }
        if (best != i)
        {
            PicoCompleteItem tmp = items[i];
            items[i] = items[best];
            items[best] = tmp;
        }
    }
}

void PicoComplete_Refresh(PicoHost *app)
{
    const PicoCompleter *comp = NULL;
    int start = 0;
    int end = 0;
    if (!ScanToken(app, &start, &end, &comp) || !comp || (!comp->host_query && !comp->workspace_query))
    {
        PicoComplete_Close();
        g_complete.dismissed = false;
        g_complete.token_active = false;
        return;
    }
    TrackToken(start, app->composer.text[start]);
    if (g_complete.dismissed && start == g_complete.dismissed_start &&
        app->composer.length == g_complete.dismissed_len)
    {
        PicoComplete_Close();
        return;
    }
    g_complete.dismissed = false;
    char prefix[512];
    int plen = end - start - 1;
    if (plen < 0)
    {
        plen = 0;
    }
    if (plen >= (int)sizeof(prefix))
    {
        plen = (int)sizeof(prefix) - 1;
    }
    memcpy(prefix, app->composer.text + start + 1, (size_t)plen);
    prefix[plen] = '\0';

    PicoCompleteItem raw[PICO_MAX_COMPLETE_ITEMS];
    int n = 0;
    if (comp->host_query)
    {
        n = comp->host_query(app, prefix, raw, PICO_MAX_COMPLETE_ITEMS, comp->state);
    }
    else if (comp->workspace_query && comp->workspace)
    {
        n = comp->workspace_query(comp->workspace, prefix, raw, PICO_MAX_COMPLETE_ITEMS, comp->state);
    }
    if (n < 0)
    {
        n = 0;
    }
    if (n > PICO_MAX_COMPLETE_ITEMS)
    {
        n = PICO_MAX_COMPLETE_ITEMS;
    }
    if (n == 0)
    {
        PicoComplete_Close();
        return;
    }
    SortItems(raw, n, prefix);
    g_complete.open = true;
    g_complete.token_start = start;
    g_complete.token_end = end;
    g_complete.count = n;
    memcpy(g_complete.items, raw, (size_t)n * sizeof(PicoCompleteItem));
    if (g_complete.selected >= n)
    {
        g_complete.selected = n - 1;
    }
    if (g_complete.selected < 0)
    {
        g_complete.selected = 0;
    }
}

static void Accept(PicoHost *app)
{
    if (!g_complete.open || g_complete.count <= 0)
    {
        return;
    }
    int sel = g_complete.selected;
    if (sel < 0 || sel >= g_complete.count)
    {
        sel = 0;
    }
    const PicoCompleteItem *item = &g_complete.items[sel];
    const PicoCompleter *comp = NULL;
    int start = g_complete.token_start;
    int end = g_complete.token_end;
    ScanToken(app, &start, &end, &comp);
    if (comp && comp->host_accept && comp->host_accept(app, item, comp->state))
    {
        PicoComplete_Refresh(app);
        return;
    }
    const char *ins = item->insert[0] ? item->insert : item->label;
    PicoComposer_ReplaceRange(app, start, end, ins);
    PicoComplete_Refresh(app);
}

bool PicoComplete_HandleKeys(PicoHost *app)
{
    if (g_complete.open && IsKeyPressed(KEY_ESCAPE))
    {
        Dismiss(app);
        return true;
    }
    PicoComplete_Refresh(app);
    if (!g_complete.open)
    {
        return false;
    }
    if (IsKeyPressed(KEY_UP) || IsKeyPressedRepeat(KEY_UP))
    {
        if (g_complete.selected > 0)
        {
            g_complete.selected--;
        }
        return true;
    }
    if (IsKeyPressed(KEY_DOWN) || IsKeyPressedRepeat(KEY_DOWN))
    {
        if (g_complete.selected + 1 < g_complete.count)
        {
            g_complete.selected++;
        }
        return true;
    }
    if (IsKeyPressed(KEY_TAB))
    {
        Accept(app);
        return true;
    }
    if ((IsKeyPressed(KEY_ENTER) || IsKeyPressed(KEY_KP_ENTER)) &&
        !(IsKeyDown(KEY_LEFT_SHIFT) || IsKeyDown(KEY_RIGHT_SHIFT)))
    {
        char before[4096];
        snprintf(before, sizeof(before), "%s", app->composer.text ? app->composer.text : "");
        Accept(app);
        const char *after = app->composer.text ? app->composer.text : "";
        if (strcmp(before, after) == 0)
        {
            PicoComplete_Close();
            PicoHost_Submit(app);
        }
        return true;
    }
    return false;
}

static int HoveredItem(void)
{
    for (int i = 0; i < g_complete.count; i++)
    {
        if (Clay_PointerOver(CLAY_IDI("CompleteItem", i)))
        {
            return i;
        }
    }
    return -1;
}

static void SelectHoveredItem(void)
{
    Vector2 delta = GetMouseDelta();
    if (delta.x == 0.0f && delta.y == 0.0f)
    {
        return;
    }
    int hovered = HoveredItem();
    if (hovered >= 0)
    {
        g_complete.selected = hovered;
    }
}

bool PicoComplete_HandlePointer(PicoHost *app)
{
    if (!g_complete.open)
    {
        return false;
    }
    SelectHoveredItem();
    if (!IsMouseButtonPressed(MOUSE_BUTTON_LEFT))
    {
        return Clay_PointerOver(Clay_GetElementId(CLAY_STRING("CompletePopup")));
    }
    int hovered = HoveredItem();
    if (hovered >= 0)
    {
        g_complete.selected = hovered;
        Accept(app);
        return true;
    }
    if (Clay_PointerOver(Clay_GetElementId(CLAY_STRING("CompletePopup"))))
    {
        return true;
    }
    Dismiss(app);
    return false;
}

void PicoComplete_Render(PicoHost *app)
{
    (void)app;
    if (!g_complete.open || g_complete.count <= 0)
    {
        return;
    }
    SelectHoveredItem();
    CLAY(CLAY_ID("CompletePopup"),
         {.floating = {.attachTo = CLAY_ATTACH_TO_PARENT,
                       .zIndex = 25,
                       .attachPoints = {.element = CLAY_ATTACH_POINT_LEFT_BOTTOM,
                                        .parent = CLAY_ATTACH_POINT_LEFT_TOP},
                       .offset = {.y = -6}},
          .layout = {.layoutDirection = CLAY_TOP_TO_BOTTOM,
                     .padding = {6, 6, 6, 6},
                     .childGap = 2,
                     .sizing = {.width = CLAY_SIZING_GROW(0)}},
          .backgroundColor = COLOR_CONTENT_BG,
          .cornerRadius = CLAY_CORNER_RADIUS(6)})
    {
        for (int i = 0; i < g_complete.count; i++)
        {
            Clay_Color bg = i == g_complete.selected ? COLOR_CODE_BG : COLOR_CONTENT_BG;
            CLAY(CLAY_IDI("CompleteItem", i),
                 {.layout = {.layoutDirection = CLAY_LEFT_TO_RIGHT,
                             .childGap = 8,
                             .padding = {8, 8, 4, 4},
                             .childAlignment = {.y = CLAY_ALIGN_Y_CENTER},
                             .sizing = {.width = CLAY_SIZING_GROW(0)}},
                  .backgroundColor = bg,
                  .cornerRadius = CLAY_CORNER_RADIUS(4)})
            {
                Clay_String label = {.length = (int32_t)strlen(g_complete.items[i].label),
                                     .chars = g_complete.items[i].label};
                CLAY_AUTO_ID({.layout = {.sizing = {.width = CLAY_SIZING_GROW(0)}},
                              .clip = {.horizontal = true}})
                {
                    CLAY_TEXT(label, CLAY_TEXT_CONFIG({.fontId = FONT_MONO,
                                                       .fontSize = 14,
                                                       .textColor = COLOR_TEXT,
                                                       .wrapMode = CLAY_TEXT_WRAP_NONE}));
                }
                if (g_complete.items[i].detail[0])
                {
                    Clay_String detail = {.length = (int32_t)strlen(g_complete.items[i].detail),
                                          .chars = g_complete.items[i].detail};
                    CLAY_TEXT(detail, CLAY_TEXT_CONFIG({.fontId = FONT_REGULAR,
                                                        .fontSize = 13,
                                                        .textColor = COLOR_MUTED,
                                                        .wrapMode = CLAY_TEXT_WRAP_NONE}));
                }
            }
        }
    }
}
