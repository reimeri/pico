#define _GNU_SOURCE
#define _POSIX_C_SOURCE 200809L

#include "pico/plugin.h"
#include "diff_model.h"
#include "diff_virtual.h"
#include "highlight.h"
#include "hl_colors.h"
#include "scrollbar.h"
#include "host_internal.h"

#include "clay/clay.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* ------------------------------------------------------------------ */
/* Model: parsed files and rows                                        */
/* ------------------------------------------------------------------ */

/* ------------------------------------------------------------------ */
/* Git capture (worker thread)                                         */
/* ------------------------------------------------------------------ */

#define DIFF_POLL_SECONDS 2

typedef struct DiffWorkerCtx
{
    int refcount; /* 1 for DiffState, 1 for worker thread */
    pthread_mutex_t lock;
    pthread_cond_t wake;
    DiffModel *pending; /* worker -> main */
    char workspace[4096];
    bool thread_started;
    bool thread_stop;
    pthread_t thread;
    /* Worker-thread-only capture dedup: signature of the last published
     * model. Unchanged captures are dropped before highlighting. */
    uint64_t last_sig;
    bool has_sig;
} DiffWorkerCtx;

typedef struct DiffState
{
    PicoWorkspace *workspace;
    DiffWorkerCtx *worker;
    DiffModel *model; /* main thread only */
    bool open;
    PicoScrollbar scrollbar;
    bool overflow;
    char chip_adds[32];
    char chip_dels[32];
    float row_min_width; /* keeps horizontal scroll extent stable while virtualized */
} DiffState;

static void DiffWorkerCtx_Release(DiffWorkerCtx *w)
{
    if (!w)
    {
        return;
    }
    pthread_mutex_lock(&w->lock);
    int r = --w->refcount;
    pthread_mutex_unlock(&w->lock);
    if (r == 0)
    {
        PicoDiffModel_Free(w->pending);
        pthread_mutex_destroy(&w->lock);
        pthread_cond_destroy(&w->wake);
        free(w);
    }
}

static void *DiffThreadMain(void *arg)
{
    DiffWorkerCtx *w = (DiffWorkerCtx *)arg;
    for (;;)
    {
        pthread_mutex_lock(&w->lock);
        bool stop = w->thread_stop;
        char ws[4096];
        snprintf(ws, sizeof(ws), "%s", w->workspace);
        pthread_mutex_unlock(&w->lock);
        if (stop)
        {
            break;
        }

        DiffModel *fresh = PicoDiffModel_Capture(ws);
        if (fresh)
        {
            uint64_t sig = PicoDiffModel_Signature(fresh);
            if (w->has_sig && sig == w->last_sig)
            {
                /* Nothing changed: keep the current model and skip the
                 * highlight pass entirely. */
                PicoDiffModel_Free(fresh);
                fresh = NULL;
            }
            else
            {
                PicoDiffModel_HighlightAll(fresh);
                w->last_sig = sig;
                w->has_sig = true;
            }
        }

        pthread_mutex_lock(&w->lock);
        DiffModel *old = w->pending;
        if (fresh)
        {
            w->pending = fresh;
        }
        stop = w->thread_stop;
        pthread_mutex_unlock(&w->lock);
        if (fresh)
        {
            PicoDiffModel_Free(old);
        }
        if (stop)
        {
            break;
        }

        /* Interruptible poll interval: StopThread signals wake so shutdown
         * does not wait out a sleep. Absolute deadline against CLOCK_REALTIME
         * (the condvar's default clock). */
        struct timespec deadline;
        clock_gettime(CLOCK_REALTIME, &deadline);
        deadline.tv_sec += DIFF_POLL_SECONDS;
        pthread_mutex_lock(&w->lock);
        while (!w->thread_stop)
        {
            if (pthread_cond_timedwait(&w->wake, &w->lock, &deadline) != 0)
            {
                break; /* timeout: poll again */
            }
        }
        stop = w->thread_stop;
        pthread_mutex_unlock(&w->lock);
        if (stop)
        {
            break;
        }
    }
    DiffWorkerCtx_Release(w);
    return NULL;
}

static void StartThread(DiffState *s, const char *ws_path)
{
    if (!s)
    {
        return;
    }
    if (!s->worker)
    {
        s->worker = (DiffWorkerCtx *)calloc(1, sizeof(DiffWorkerCtx));
        if (!s->worker)
        {
            return;
        }
        s->worker->refcount = 1;
        pthread_mutex_init(&s->worker->lock, NULL);
        pthread_cond_init(&s->worker->wake, NULL);
    }
    DiffWorkerCtx *w = s->worker;
    if (w->thread_started)
    {
        return;
    }
    snprintf(w->workspace, sizeof(w->workspace), "%s", ws_path ? ws_path : ".");
    w->thread_started = true;
    pthread_mutex_lock(&w->lock);
    w->refcount++;
    pthread_mutex_unlock(&w->lock);
    if (pthread_create(&w->thread, NULL, DiffThreadMain, w) != 0)
    {
        w->thread_started = false;
        pthread_mutex_lock(&w->lock);
        w->refcount--;
        pthread_mutex_unlock(&w->lock);
    }
}

static void StopThread(DiffState *s)
{
    if (!s || !s->worker)
    {
        return;
    }
    DiffWorkerCtx *w = s->worker;
    s->worker = NULL;
    if (!w->thread_started)
    {
        DiffWorkerCtx_Release(w);
        return;
    }
    pthread_mutex_lock(&w->lock);
    w->thread_stop = true;
    pthread_cond_signal(&w->wake);
    pthread_mutex_unlock(&w->lock);

    /* Shutdown callbacks run on the main thread and must never wait for a
     * worker. The worker owns its second context reference until it exits. */
    pthread_detach(w->thread);
    DiffWorkerCtx_Release(w);
}

static void AdoptPending(DiffState *s)
{
    if (!s || !s->worker)
    {
        return;
    }
    DiffWorkerCtx *w = s->worker;
    pthread_mutex_lock(&w->lock);
    DiffModel *fresh = w->pending;
    w->pending = NULL;
    const char *root = s->workspace ? s->workspace->path : "";
    if (strncmp(w->workspace, root, sizeof(w->workspace)) != 0)
    {
        snprintf(w->workspace, sizeof(w->workspace), "%s", root);
    }
    pthread_mutex_unlock(&w->lock);

    /* A capture that started under a previous workspace is never displayed,
     * even if it was published after the workspace switched back. */
    if (fresh && strncmp(fresh->workspace, root, sizeof(fresh->workspace)) != 0)
    {
        PicoDiffModel_Free(fresh);
        return;
    }
    if (fresh)
    {
        PicoDiffModel_Free(s->model);
        s->model = fresh;

        int max_len = 0;
        for (int fi = 0; fi < fresh->file_count; fi++)
        {
            for (int ri = 0; ri < fresh->files[fi].row_count; ri++)
            {
                if (fresh->files[fi].rows[ri].len > max_len)
                {
                    max_len = fresh->files[fi].rows[ri].len;
                }
            }
        }
        Clay_TextElementConfig cfg = {0};
        cfg.fontId = FONT_MONO;
        cfg.fontSize = PICO_FONT_UI;
        Clay_Dimensions glyph = Pico_MeasureTextUtf8(
            (Clay_StringSlice){.length = 1, .chars = "0", .baseChars = "0"}, &cfg, NULL);
        float advance = glyph.width > 0.0f ? glyph.width : 8.0f;
        /* +1 for the sign column; 6 childGap, 16 horizontal padding. */
        s->row_min_width = (float)(max_len + 1) * advance + 6.0f + 16.0f;
    }
}

/* Rows are single-line WRAP_NONE text: height is exactly the font px plus
 * the row's 1+1 vertical padding (raylib measures single-line height as the
 * font size). childGap between rows in DiffScroll. */
#define DIFF_ROW_GAP 2
/* Extra rows mounted above/below the viewport to cover the one-frame lag of
 * scroll container data and fast scrolls. */
#define DIFF_ROW_OVERSCAN 8

/* ------------------------------------------------------------------ */
/* Footer chip                                                         */
/* ------------------------------------------------------------------ */

static Clay_String CStr(const char *str)
{
    if (!str)
    {
        str = "";
    }
    return (Clay_String){.length = (int32_t)strlen(str), .chars = str};
}

static Clay_String Slice(const char *str, int len)
{
    return (Clay_String){.length = (int32_t)len, .chars = str};
}

static bool HasChanges(const DiffState *s)
{
    return s && s->model && s->model->is_repo &&
           (s->model->adds > 0 || s->model->dels > 0 || s->model->untracked > 0);
}

void PicoDiff_RenderChip(PicoHost *app)
{
    PicoWorkspace *ws = PicoHost_SelectedWorkspace(app);
    DiffState *s = (DiffState *)PicoPlugins_WorkspaceState(ws, "diff");
    if (!s || !HasChanges(s))
    {
        return;
    }
    snprintf(s->chip_adds, sizeof(s->chip_adds), "+%d", s->model->adds);
    snprintf(s->chip_dels, sizeof(s->chip_dels), "-%d", s->model->dels);
    bool hovered = Clay_PointerOver(CLAY_ID("FooterDiff"));

    CLAY_TEXT(CLAY_STRING("  ·  "), CLAY_TEXT_CONFIG({.fontId = FONT_REGULAR,
                                                      .fontSize = PICO_FONT_CAPTION,
                                                      .textColor = COLOR_MUTED,
                                                      .wrapMode = CLAY_TEXT_WRAP_NONE}));
    CLAY(CLAY_ID("FooterDiff"),
         {.layout = {.layoutDirection = CLAY_LEFT_TO_RIGHT,
                     .childGap = 6,
                     .sizing = {.width = CLAY_SIZING_FIT(0), .height = CLAY_SIZING_FIT(0)}}})
    {
        CLAY_TEXT(CStr(s->chip_adds), CLAY_TEXT_CONFIG({.fontId = FONT_REGULAR,
                                                        .fontSize = PICO_FONT_CAPTION,
                                                        .textColor = hovered ? COLOR_DIFF_ADD_TEXT
                                                                             : COLOR_DIFF_ADD,
                                                        .wrapMode = CLAY_TEXT_WRAP_NONE}));
        CLAY_TEXT(CStr(s->chip_dels), CLAY_TEXT_CONFIG({.fontId = FONT_REGULAR,
                                                        .fontSize = PICO_FONT_CAPTION,
                                                        .textColor = hovered ? COLOR_DIFF_DEL_TEXT
                                                                             : COLOR_DIFF_DEL,
                                                        .wrapMode = CLAY_TEXT_WRAP_NONE}));
    }
}

/* ------------------------------------------------------------------ */
/* Modal                                                               */
/* ------------------------------------------------------------------ */

bool PicoDiff_IsOpen(const PicoHost *app)
{
    const PicoWorkspace *ws = PicoHost_SelectedWorkspaceConst(app);
    DiffState *s = (DiffState *)PicoPlugins_WorkspaceState(ws, "diff");
    return s ? s->open : false;
}

static bool CloseModal(DiffState *s)
{
    if (!s || !s->open)
    {
        return true;
    }
    PicoHost *host = s->workspace ? s->workspace->host : NULL;
    if (host && !pico_ui_modal_pop(host, "diff"))
    {
        return false;
    }
    s->open = false;
    memset(&s->scrollbar, 0, sizeof(s->scrollbar));
    return true;
}

static void OpenModal(DiffState *s, PicoHost *app)
{
    if (s && !s->open && pico_ui_modal_push(app, "diff"))
    {
        s->open = true;
    }
}

static Clay_Color RowBg(DiffRowKind kind)
{
    switch (kind)
    {
    case ROW_ADD:
        return COLOR_DIFF_ADD_BG;
    case ROW_DEL:
        return COLOR_DIFF_DEL_BG;
    default:
        return (Clay_Color){0, 0, 0, 0};
    }
}

static Clay_Color RowFg(DiffRowKind kind)
{
    switch (kind)
    {
    case ROW_HEADER:
        return COLOR_TEXT;
    case ROW_HUNK:
        return COLOR_LINK;
    /* Context, add, and delete rows share one text palette so syntax
     * highlighting matches across the hunk. Add/delete only tint the
     * background (see RowBg). */
    default:
        return COLOR_MUTED;
    }
}

static Clay_Color RowSignFg(DiffRowKind kind)
{
    switch (kind)
    {
    case ROW_ADD:
        return COLOR_DIFF_ADD_TEXT;
    case ROW_DEL:
        return COLOR_DIFF_DEL_TEXT;
    default:
        return RowFg(kind);
    }
}

/* First span that may overlap [start, ...): spans are sorted by start. */
static int SpanLowerBound(const PicoHlSpan *spans, int count, int start)
{
    int lo = 0, hi = count;
    while (lo < hi)
    {
        int mid = lo + (hi - lo) / 2;
        if (spans[mid].end <= start)
        {
            lo = mid + 1;
        }
        else
        {
            hi = mid;
        }
    }
    return lo;
}

/* Row text with syntax spans overlaid on the row's base color; spans index
 * the row's image at img_off. */
static void RenderRowText(const DiffRow *row, const PicoHlSpan *spans, int span_count,
                          Clay_Color base)
{
    if (row->img_off < 0 || span_count == 0 || row->len == 0)
    {
        CLAY_TEXT(Slice(row->text, row->len),
                  CLAY_TEXT_CONFIG({.fontId = FONT_MONO,
                                    .fontSize = PICO_FONT_UI,
                                    .textColor = base,
                                    .wrapMode = CLAY_TEXT_WRAP_NONE}));
        return;
    }
    int row_end = row->img_off + row->len;
    int cursor = row->img_off;
    int si = SpanLowerBound(spans, span_count, cursor);
    while (cursor < row_end)
    {
        if (si >= span_count || spans[si].start >= row_end)
        {
            CLAY_TEXT(Slice(row->text + (cursor - row->img_off), row_end - cursor),
                      CLAY_TEXT_CONFIG({.fontId = FONT_MONO,
                                        .fontSize = PICO_FONT_UI,
                                        .textColor = base,
                                        .wrapMode = CLAY_TEXT_WRAP_NONE}));
            break;
        }
        PicoHlSpan span = spans[si];
        int s = span.start > cursor ? span.start : cursor;
        int e = span.end < row_end ? span.end : row_end;
        if (s > cursor)
        {
            CLAY_TEXT(Slice(row->text + (cursor - row->img_off), s - cursor),
                      CLAY_TEXT_CONFIG({.fontId = FONT_MONO,
                                        .fontSize = PICO_FONT_UI,
                                        .textColor = base,
                                        .wrapMode = CLAY_TEXT_WRAP_NONE}));
        }
        if (e > s)
        {
            CLAY_TEXT(Slice(row->text + (s - row->img_off), e - s),
                      CLAY_TEXT_CONFIG({.fontId = FONT_MONO,
                                        .fontSize = PICO_FONT_UI,
                                        .textColor = PicoHlClassColor(span.class),
                                        .wrapMode = CLAY_TEXT_WRAP_NONE}));
        }
        cursor = e;
        if (span.end <= row_end)
        {
            si++;
        }
    }
}

static void RenderRow(int index, const DiffFile *f, const DiffRow *row, float min_width)
{
    const char *sign = " ";
    if (row->kind == ROW_ADD)
    {
        sign = "+";
    }
    else if (row->kind == ROW_DEL)
    {
        sign = "-";
    }
    /* min_width keeps the horizontal scroll extent anchored to the widest row
     * in the model; otherwise it would shrink to the widest mounted row while
     * virtualized. Monospace advance is an estimate: wide glyphs can exceed
     * it, which only clips horizontal reach slightly. */
    CLAY(CLAY_IDI("DiffRow", index),
         {.layout = {.layoutDirection = CLAY_LEFT_TO_RIGHT,
                     .childGap = 6,
                     .padding = {8, 8, 1, 1},
                     .sizing = {.width = CLAY_SIZING_GROW((int)min_width)}},
          .backgroundColor = RowBg(row->kind)})
    {
        if (row->kind == ROW_HEADER)
        {
            CLAY_TEXT(Slice(row->text, row->len),
                      CLAY_TEXT_CONFIG({.fontId = FONT_BOLD,
                                        .fontSize = PICO_FONT_UI,
                                        .textColor = RowFg(row->kind),
                                        .wrapMode = CLAY_TEXT_WRAP_NONE}));
        }
        else
        {
            CLAY_TEXT(CStr(sign), CLAY_TEXT_CONFIG({.fontId = FONT_MONO,
                                                    .fontSize = PICO_FONT_UI,
                                                    .textColor = RowSignFg(row->kind),
                                                    .wrapMode = CLAY_TEXT_WRAP_NONE}));
            const PicoHlSpan *spans = row->kind == ROW_DEL ? f->old_spans : f->new_spans;
            int span_count = row->kind == ROW_DEL ? f->old_count : f->new_count;
            RenderRowText(row, spans, span_count, RowFg(row->kind));
        }
    }
}

static void DiffModalRender(PicoWorkspace *workspace, PicoAgentId selected_agent_id, void *state)
{
    (void)selected_agent_id;
    DiffState *s = (DiffState *)state;
    if (!s)
    {
        s = (DiffState *)PicoPlugins_WorkspaceState(workspace, "diff");
    }
    if (!s || !s->open)
    {
        return;
    }
    float sw = (float)GetScreenWidth();
    float sh = (float)GetScreenHeight();
    float card_w = sw - 96.0f;
    if (card_w < 420.0f)
    {
        card_w = sw - 32.0f;
    }
    if (card_w > 980.0f)
    {
        card_w = 980.0f;
    }
    float card_h = sh * 0.8f;
    if (card_h < 280.0f)
    {
        card_h = 280.0f;
    }

    if (HasChanges(s))
    {
        snprintf(s->chip_adds, sizeof(s->chip_adds), "+%d", s->model->adds);
        snprintf(s->chip_dels, sizeof(s->chip_dels), "-%d", s->model->dels);
    }

    CLAY(CLAY_ID("DiffModalDim"),
         {.floating = {.attachTo = CLAY_ATTACH_TO_ROOT,
                       .zIndex = 40,
                       .attachPoints = {.element = CLAY_ATTACH_POINT_LEFT_TOP,
                                        .parent = CLAY_ATTACH_POINT_LEFT_TOP}},
          .layout = {.layoutDirection = CLAY_TOP_TO_BOTTOM,
                     .childAlignment = {.x = CLAY_ALIGN_X_CENTER, .y = CLAY_ALIGN_Y_CENTER},
                     .sizing = {.width = CLAY_SIZING_FIXED(sw), .height = CLAY_SIZING_FIXED(sh)}},
          .backgroundColor = {0, 0, 0, 140}})
    {
        CLAY(CLAY_ID("DiffModalCard"),
             {.layout = {.layoutDirection = CLAY_TOP_TO_BOTTOM,
                         .padding = {20, 20, 16, 16},
                         .childGap = 12,
                         .sizing = {.width = CLAY_SIZING_FIXED(card_w),
                                    .height = CLAY_SIZING_FIXED(card_h)}},
              .backgroundColor = COLOR_CONTENT_BG,
              .cornerRadius = CLAY_CORNER_RADIUS(8)})
        {
            CLAY(CLAY_ID("DiffModalTitle"),
                 {.layout = {.layoutDirection = CLAY_LEFT_TO_RIGHT,
                             .childGap = 6,
                             .childAlignment = {.y = CLAY_ALIGN_Y_CENTER},
                             .sizing = {.width = CLAY_SIZING_FIT(0),
                                        .height = CLAY_SIZING_FIT(0)}}})
            {
                CLAY_TEXT(CLAY_STRING("Changes"),
                          CLAY_TEXT_CONFIG({.fontId = FONT_BOLD,
                                            .fontSize = PICO_FONT_TITLE,
                                            .textColor = COLOR_TEXT,
                                            .wrapMode = CLAY_TEXT_WRAP_NONE}));
                if (HasChanges(s))
                {
                    CLAY_TEXT(CLAY_STRING("·"),
                              CLAY_TEXT_CONFIG({.fontId = FONT_BOLD,
                                                .fontSize = PICO_FONT_TITLE,
                                                .textColor = COLOR_TEXT,
                                                .wrapMode = CLAY_TEXT_WRAP_NONE}));
                    CLAY_TEXT(CStr(s->chip_adds),
                              CLAY_TEXT_CONFIG({.fontId = FONT_BOLD,
                                                .fontSize = PICO_FONT_TITLE,
                                                .textColor = COLOR_DIFF_ADD_TEXT,
                                                .wrapMode = CLAY_TEXT_WRAP_NONE}));
                    CLAY_TEXT(CStr(s->chip_dels),
                              CLAY_TEXT_CONFIG({.fontId = FONT_BOLD,
                                                .fontSize = PICO_FONT_TITLE,
                                                .textColor = COLOR_DIFF_DEL_TEXT,
                                                .wrapMode = CLAY_TEXT_WRAP_NONE}));
                }
            }

            CLAY(CLAY_ID("DiffScrollRow"),
                 {.layout = {.layoutDirection = CLAY_LEFT_TO_RIGHT,
                             .childGap = SCROLLBAR_GAP,
                             .sizing = {.width = CLAY_SIZING_GROW(0),
                                        .height = CLAY_SIZING_GROW(0)}}})
            {
                CLAY(CLAY_ID("DiffScroll"),
                     {.layout = {.layoutDirection = CLAY_TOP_TO_BOTTOM,
                                 .childGap = DIFF_ROW_GAP,
                                 .sizing = {.width = CLAY_SIZING_GROW(0),
                                            .height = CLAY_SIZING_GROW(0)}},
                      .clip = {.vertical = true,
                               .horizontal = true,
                               .childOffset = Clay_GetScrollOffset()}})
                {
                    if (!s->model || s->model->file_count == 0)
                    {
                        CLAY_TEXT(CLAY_STRING("No changes"),
                                  CLAY_TEXT_CONFIG({.fontId = FONT_REGULAR,
                                                    .fontSize = PICO_FONT_UI,
                                                    .textColor = COLOR_MUTED,
                                                    .wrapMode = CLAY_TEXT_WRAP_NONE}));
                    }
                    else
                    {
                        /* Mount only the visible window. ow height is exact (single-line text at a
                         * fixed px size + 2px row padding), and the pads keep
                         * total content height identical to a mounted list so
                         * scroll range and scrollbar thumb do not change. */
                        int total_rows = 0;
                        for (int fi = 0; fi < s->model->file_count; fi++)
                        {
                            total_rows += s->model->files[fi].row_count;
                        }

                        Clay_ScrollContainerData scroll =
                            Clay_GetScrollContainerData(Clay_GetElementId(CLAY_STRING("DiffScroll")));
                        float viewport_h =
                            scroll.found ? scroll.scrollContainerDimensions.height : card_h;
                        float scroll_top =
                            scroll.found && scroll.scrollPosition ? -scroll.scrollPosition->y : 0.0f;

                        float row_h = Pico_FontPx(PICO_FONT_UI) + 2.0f;
                        int first, end;
                        float top_pad, bottom_pad;
                        PicoDiffWindow_Range(total_rows, scroll_top, viewport_h,
                                             row_h + (float)DIFF_ROW_GAP, (float)DIFF_ROW_GAP,
                                             DIFF_ROW_OVERSCAN, &first, &end, &top_pad, &bottom_pad);

                        if (first > 0)
                        {
                            CLAY(CLAY_ID("DiffTopPad"),
                                 {.layout = {.sizing = {.width = CLAY_SIZING_GROW(0),
                                                        .height = CLAY_SIZING_FIXED(top_pad)}}})
                            {
                            }
                        }
                        int row_index = 0;
                        for (int fi = 0; fi < s->model->file_count; fi++)
                        {
                            DiffFile *f = &s->model->files[fi];
                            for (int ri = 0; ri < f->row_count; ri++)
                            {
                                if (row_index >= first && row_index < end)
                                {
                                    RenderRow(row_index, f, &f->rows[ri], s->row_min_width);
                                }
                                row_index++;
                            }
                        }
                        if (end < total_rows)
                        {
                            CLAY(CLAY_ID("DiffBottomPad"),
                                 {.layout = {.sizing = {.width = CLAY_SIZING_GROW(0),
                                                        .height = CLAY_SIZING_FIXED(bottom_pad)}}})
                            {
                            }
                        }
                    }
                }
                if (s->overflow)
                {
                    PicoScrollbar_Render(CLAY_STRING("DiffScroll"), CLAY_STRING("DiffScrollTrack"),
                                         CLAY_STRING("DiffScrollHandle"));
                }
            }
        }
    }
}

/* ------------------------------------------------------------------ */
/* Input                                                               */
/* ------------------------------------------------------------------ */

static void DiffHostAfterLayout(PicoHost *app, const PicoHookEvent *event, void *state)
{
    (void)state;
    (void)event;
    if (!app)
    {
        return;
    }
    PicoWorkspace *workspace = PicoHost_SelectedWorkspace(app);
    DiffState *s = (DiffState *)PicoPlugins_WorkspaceState(workspace, "diff");
    if (!s)
    {
        return;
    }
    if (s->open)
    {
        if (!pico_ui_modal_is_top(app, "diff"))
        {
            return;
        }
        s->overflow = PicoScrollbar_Overflows(CLAY_STRING("DiffScroll"));
        app->hovered_clickable = false;
        if (!IsMouseButtonPressed(MOUSE_BUTTON_LEFT))
        {
            return;
        }
        if (Clay_PointerOver(Clay_GetElementId(CLAY_STRING("DiffModalCard"))))
        {
            return;
        }
        if (Clay_PointerOver(Clay_GetElementId(CLAY_STRING("DiffModalDim"))))
        {
            CloseModal(s);
        }
        return;
    }

    if (!HasChanges(s) || PicoUi_ModalOpen(app))
    {
        return;
    }
    if (Clay_PointerOver(Clay_GetElementId(CLAY_STRING("FooterDiff"))))
    {
        app->hovered_clickable = true;
        if (IsMouseButtonPressed(MOUSE_BUTTON_LEFT))
        {
            OpenModal(s, app);
        }
    }
}

static void DiffWorkspaceOnFrame(PicoWorkspace *workspace, void *state, float dt)
{
    (void)workspace;
    (void)dt;
    DiffState *s = (DiffState *)state;
    if (!s)
    {
        return;
    }
    AdoptPending(s);
    if (!s->open)
    {
        return;
    }
    PicoScrollbar_UpdateDrag(&s->scrollbar, CLAY_STRING("DiffScroll"), CLAY_STRING("DiffScrollHandle"));
    if (IsKeyPressed(KEY_ESCAPE))
    {
        CloseModal(s);
    }
}

/* ------------------------------------------------------------------ */
/* Extension                                                           */
/* ------------------------------------------------------------------ */

static int DiffHostInit(PicoHost *host, void **state_out)
{
    (void)state_out;
    pico_host_add_hook(host, PICO_HOOK_AFTER_LAYOUT, DiffHostAfterLayout);
    return 0;
}

static int DiffWorkspaceInit(PicoWorkspace *workspace, void **state_out)
{
    DiffState *s = (DiffState *)calloc(1, sizeof(DiffState));
    if (!s)
    {
        return 1;
    }
    s->workspace = workspace;
    if (state_out)
    {
        *state_out = s;
    }
    pico_workspace_add_view(workspace, PICO_SLOT_OVERLAY, 30, DiffModalRender);
    StartThread(s, workspace ? workspace->path : ".");
    return 0;
}

static void DiffWorkspaceShutdown(PicoWorkspace *workspace, void *state)
{
    (void)workspace;
    DiffState *s = (DiffState *)state;
    if (!s)
    {
        return;
    }
    StopThread(s);
    PicoDiffModel_Free(s->model);
    s->model = NULL;
    (void)CloseModal(s);
    free(s);
}

PicoExt pico_ext_diff(void)
{
    return (PicoExt){
        .abi = PICO_EXT_ABI,
        .name = "diff",
        .description = "Git working-tree changes in the footer",
        .host_init = DiffHostInit,
        .workspace_init = DiffWorkspaceInit,
        .workspace_shutdown = DiffWorkspaceShutdown,
        .workspace_on_frame = DiffWorkspaceOnFrame,
    };
}
