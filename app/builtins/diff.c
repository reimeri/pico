#define _GNU_SOURCE
#define _POSIX_C_SOURCE 200809L

#include "pico/plugin.h"
#include "diff_model.h"
#include "diff_virtual.h"
#include "highlight.h"
#include "hl_colors.h"
#include "scrollbar.h"
#include "host_internal.h"
#include "diff_ui.h"
#include "settings_ui.h"
#include <math.h>

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
#define DIFF_INACTIVE_POLL_SECONDS 30

typedef struct DiffWorkerCtx
{
    int refcount; /* 1 for DiffState, 1 for worker thread */
    pthread_mutex_t lock;
    pthread_cond_t wake;
    DiffModel *pending; /* worker -> main */
    char workspace[4096];
    bool thread_started;
    bool thread_stop;
    bool active; /* selected workspace or open panel, published by main thread */
    bool refresh_requested;
    pthread_t thread;
    /* Worker-thread-only capture dedup: signature of the last published
     * model. Unchanged captures are dropped before highlighting. */
    uint64_t last_sig;
    bool has_sig;
} DiffWorkerCtx;

typedef struct ReviewFile {
    char *path;
    bool collapsed;
    int *expanded_gaps; /* old-image start lines remain stable across working-tree edits */
    int gap_count;
    char adds[24], dels[24];
} ReviewFile;
typedef struct ReviewRow {
    int file, row, hidden, gap_line;
    int offset_units; /* file headers occupy two source-row strides */
    char gutter[32];
    char caption[128];
} ReviewRow;

typedef struct DiffState
{
    PicoWorkspace *workspace;
    DiffWorkerCtx *worker;
    DiffModel *model; /* main thread only */
    bool open;
    bool files_menu;
    bool review_failed;
    bool focused, was_visible, resizing, pointer_claimed;
    int mounted_first, mounted_end;
    float width, resize_x, resize_width;
    ReviewFile *files;
    int file_count;
    ReviewRow *rows;
    int row_count, total_units;
    float scroll_y, scroll_x;
    bool restore_scroll;
    char summary[128];
    PicoScrollbar scrollbar;
    bool overflow;
    char chip_adds[32];
    char chip_dels[32];
    char chip_files[32];
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
        bool active = w->active;
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
            pico_host_wakeup(NULL);
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
        deadline.tv_sec += active ? DIFF_POLL_SECONDS : DIFF_INACTIVE_POLL_SECONDS;
        pthread_mutex_lock(&w->lock);
        while (!w->thread_stop)
        {
            if (pthread_cond_timedwait(&w->wake, &w->lock, &deadline) != 0 ||
                w->refresh_requested)
            {
                w->refresh_requested = false;
                break; /* timeout or explicit refresh */
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

static float ReviewStride(void) { return Pico_FontPx(PICO_FONT_UI) + 4.0f; }

static float ReviewOffset(const DiffState *s, int row)
{
    if (row < 0) return 0;
    return (row < s->row_count ? s->rows[row].offset_units : s->total_units) * ReviewStride();
}

/* Locate the row containing an offset, including the lower half of a header. */
static int ReviewAt(const DiffState *s, float offset)
{
    int low = 0, high = s->row_count;
    while (low < high) {
        int mid = low + (high - low) / 2;
        if (ReviewOffset(s, mid) <= offset) low = mid + 1;
        else high = mid;
    }
    return s->row_count ? (low > 0 ? low - 1 : 0) : -1;
}

static void FreeReviewFiles(DiffState *s)
{
    for (int i = 0; i < s->file_count; i++) { free(s->files[i].path); free(s->files[i].expanded_gaps); }
    free(s->files); s->files = NULL; s->file_count = 0;
}

static bool GapExpanded(const ReviewFile *file, int old_line)
{
    for (int i = 0; i < file->gap_count; i++) if (file->expanded_gaps[i] == old_line) return true;
    return false;
}

static void AppendReviewRow(DiffState *s, int fi, int ri, int hidden, int gap_line, bool emit)
{
    int index = s->row_count++;
    int offset = s->total_units;
    DiffRow *r = &s->model->files[fi].rows[ri];
    s->total_units += r->kind == ROW_HEADER ? 2 : 1;
    if (!emit) return;
    ReviewRow *v = &s->rows[index];
    *v = (ReviewRow){.file=fi, .row=ri, .hidden=hidden, .gap_line=gap_line, .offset_units=offset};
    if (hidden) snprintf(v->caption, sizeof(v->caption), "… %d unchanged lines · expand context", hidden);
    else if (r->kind == ROW_HEADER) snprintf(v->caption, sizeof(v->caption), "%s", s->files[fi].collapsed ? "▸" : "▾");
    else {
        char old[12] = "", new[12] = "";
        if (r->old_line) snprintf(old, sizeof(old), "%d", r->old_line);
        if (r->new_line) snprintf(new, sizeof(new), "%d", r->new_line);
        snprintf(v->gutter, sizeof(v->gutter), "%5s %5s", old, new);
    }
}

static void BuildReviewPass(DiffState *s, bool emit)
{
    s->row_count = 0; s->total_units = 0;
    for (int fi = 0; fi < s->file_count; fi++) {
        DiffFile *f = &s->model->files[fi];
        snprintf(s->files[fi].adds, sizeof(s->files[fi].adds), "+%d", f->adds);
        snprintf(s->files[fi].dels, sizeof(s->files[fi].dels), "−%d", f->dels);
        for (int ri = 0; ri < f->row_count;) {
            if (s->files[fi].collapsed && ri) break;
            if (f->rows[ri].kind != ROW_CTX) {
                AppendReviewRow(s, fi, ri++, 0, 0, emit); continue;
            }
            int start = ri, end = ri;
            while (end < f->row_count && f->rows[end].kind == ROW_CTX) end++;
            int gap_line = f->rows[start].old_line;
            bool before_change = start > 0 && (f->rows[start-1].kind == ROW_ADD || f->rows[start-1].kind == ROW_DEL);
            bool after_change = end < f->row_count && (f->rows[end].kind == ROW_ADD || f->rows[end].kind == ROW_DEL);
            int prefix = before_change ? 3 : 0, suffix = after_change ? 3 : 0;
            if (GapExpanded(&s->files[fi], gap_line) || end - start <= prefix + suffix) {
                while (ri < end) AppendReviewRow(s, fi, ri++, 0, 0, emit);
                continue;
            }
            for (int n = 0; n < prefix; n++) AppendReviewRow(s, fi, ri++, 0, 0, emit);
            int hidden = end - ri - suffix;
            AppendReviewRow(s, fi, ri, hidden, gap_line, emit);
            ri += hidden;
            while (ri < end) AppendReviewRow(s, fi, ri++, 0, 0, emit);
        }
    }
}

static void BuildReview(DiffState *s)
{
    /* Count the visible document first: collapsed context must not allocate
     * a ReviewRow for every unchanged source line. */
    BuildReviewPass(s, false);
    ReviewRow *rows = s->row_count ? calloc((size_t)s->row_count, sizeof(*rows)) : NULL;
    s->review_failed = s->row_count && !rows;
    free(s->rows); s->rows = rows;
    if (s->review_failed) { s->row_count = 0; s->total_units = 0; return; }
    BuildReviewPass(s, true);
}

static void AdoptReview(DiffState *s, DiffModel *fresh)
{
    /* Anchor by file and source text, not the old absolute scroll offset.
     * If that source disappears, choose the closest surviving source line. */
    Clay_ScrollContainerData scroll = Clay_GetCurrentContext() ?
        Clay_GetScrollContainerData(CLAY_ID("DiffScroll")) : (Clay_ScrollContainerData){0};
    if (s->workspace && PicoHost_SelectedWorkspace(s->workspace->host) == s->workspace &&
        !s->restore_scroll && PicoDiff_IsOpen(s->workspace->host) && scroll.found && scroll.scrollPosition) {
        s->scroll_y = scroll.scrollPosition->y; s->scroll_x = scroll.scrollPosition->x;
    }
    int anchor = ReviewAt(s, -s->scroll_y);
    ReviewRow *v = anchor >= 0 && anchor < s->row_count ? &s->rows[anchor] : NULL;
    DiffRow *old = v ? &s->model->files[v->file].rows[v->row] : NULL;
    const char *path = v ? s->files[v->file].path : NULL;
    ReviewFile *files = fresh->file_count ? calloc((size_t)fresh->file_count, sizeof(*files)) : NULL;
    if (fresh->file_count && !files) { PicoDiffModel_Free(fresh); return; }
    for (int i = 0; i < fresh->file_count; i++) {
        files[i].path = strndup(fresh->files[i].label, (size_t)fresh->files[i].label_len);
        if (!files[i].path) {
            for (int j = 0; j < i; j++) { free(files[j].path); free(files[j].expanded_gaps); }
            free(files); PicoDiffModel_Free(fresh); return;
        }
        for (int j = 0; j < s->file_count; j++) if (!strcmp(files[i].path, s->files[j].path)) {
            files[i].collapsed = s->files[j].collapsed;
            if (s->files[j].gap_count) {
                files[i].expanded_gaps = malloc((size_t)s->files[j].gap_count * sizeof(int));
                if (files[i].expanded_gaps) {
                    files[i].gap_count = s->files[j].gap_count;
                    memcpy(files[i].expanded_gaps, s->files[j].expanded_gaps, (size_t)files[i].gap_count * sizeof(int));
                }
            }
            break;
        }
    }
    int target_file = -1, target_row = 0, best = 2147483647;
    if (path) for (int i = 0; i < fresh->file_count; i++) if (!strcmp(path, files[i].path)) {
        target_file = i;
        for (int j = 0; j < fresh->files[i].row_count; j++) {
            DiffRow *r = &fresh->files[i].rows[j];
            if (r->kind == old->kind && r->len == old->len && !memcmp(r->text, old->text, (size_t)r->len)) {
                int distance = abs(r->new_line - old->new_line) + abs(r->old_line - old->old_line);
                if (distance < best) { target_row = j; best = distance; }
            }
        }
        if (best == 2147483647) for (int j = 0; j < fresh->files[i].row_count; j++) {
            DiffRow *r = &fresh->files[i].rows[j];
            int distance = abs(r->new_line - old->new_line) + abs(r->old_line - old->old_line);
            if (distance < best) { target_row = j; best = distance; }
        }
        break;
    }
    float residue = s->scroll_y + ReviewOffset(s, anchor);
    FreeReviewFiles(s); s->files = files; s->file_count = fresh->file_count;
    PicoDiffModel_Free(s->model); s->model = fresh;
    BuildReview(s);
    if (target_file >= 0) for (int i = 0; i < s->row_count; i++) {
        ReviewRow *r = &s->rows[i];
        if (r->file == target_file && (r->row == target_row ||
            (r->hidden && target_row >= r->row && target_row < r->row + r->hidden))) {
            s->scroll_y = -ReviewOffset(s, i) + residue; break;
        }
    }
    s->restore_scroll = true;
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
        AdoptReview(s, fresh);
        if (s->workspace) {
            s->workspace->host->ui_relayout_requested = true;
            pico_host_request_redraw(s->workspace->host);
        }
        fresh = s->model;
        if (!fresh) return;

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
           (s->model->file_count > 0 || s->model->untracked > 0);
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
    snprintf(s->chip_files, sizeof(s->chip_files), "%d %s", s->model->file_count,
             s->model->file_count == 1 ? "file" : "files");
    bool counts_only = s->model->adds == 0 && s->model->dels == 0;
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
        if (counts_only)
        {
            CLAY_TEXT(CStr(s->chip_files), CLAY_TEXT_CONFIG({.fontId = FONT_REGULAR,
                         .fontSize = PICO_FONT_CAPTION, .textColor = hovered ? COLOR_TEXT : COLOR_MUTED,
                         .wrapMode = CLAY_TEXT_WRAP_NONE}));
        }
        else
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
}

/* ------------------------------------------------------------------ */
/* Docked panel                                                        */
/* ------------------------------------------------------------------ */

bool PicoDiff_IsOpen(const PicoHost *app)
{
    const PicoWorkspace *ws = PicoHost_SelectedWorkspaceConst(app);
    DiffState *s = (DiffState *)PicoPlugins_WorkspaceState(ws, "diff");
    return s && s->open && !PicoSettingsUi_IsOpen(app);
}

static bool ClosePanel(DiffState *s)
{
    if (!s) return true;
    Clay_ScrollContainerData scroll = Clay_GetCurrentContext() ?
        Clay_GetScrollContainerData(CLAY_ID("DiffScroll")) : (Clay_ScrollContainerData){0};
    if (s->workspace && PicoHost_SelectedWorkspace(s->workspace->host) == s->workspace &&
        !s->restore_scroll && PicoDiff_IsOpen(s->workspace->host) && scroll.found && scroll.scrollPosition) {
        s->scroll_y = scroll.scrollPosition->y; s->scroll_x = scroll.scrollPosition->x;
    }
    s->open = false; s->focused = false;
    if (s->resizing && s->workspace) s->workspace->host->ui_drag_active = false;
    s->resizing = false;
    if (s->workspace) s->workspace->host->ui_relayout_requested = true;
    return true;
}

void PicoDiffUi_Close(PicoHost *host)
{
    ClosePanel(PicoPlugins_WorkspaceState(PicoHost_SelectedWorkspace(host), "diff"));
}

void PicoDiffUi_Open(PicoHost *host)
{
    DiffState *s = PicoPlugins_WorkspaceState(PicoHost_SelectedWorkspace(host), "diff");
    if (!s) return;
    if (!PicoSettingsUi_CloseForChanges(host)) return;
    s->open = true; s->focused = true; s->restore_scroll = true;
    host->ui_relayout_requested = true;
    pico_host_request_redraw(host);
    if (s->worker) {
        pthread_mutex_lock(&s->worker->lock);
        s->worker->active = true; s->worker->refresh_requested = true;
        pthread_cond_signal(&s->worker->wake);
        pthread_mutex_unlock(&s->worker->lock);
    }
}

float PicoDiffUi_ShellWidth(const PicoHost *host)
{
    if (!PicoDiff_IsOpen(host)) return 0;
    DiffState *s = PicoPlugins_WorkspaceState(PicoHost_SelectedWorkspaceConst(host), "diff");
    return fmaxf(0, fminf(PicoHost_RightColumnWidth(host) * 0.65f,
                         fmaxf(300, s->width > 0 ? s->width : 700)));
}

static bool ReviewPointerIn(const char *id)
{
    Clay_ElementData e = Clay_GetElementData(Clay_GetElementId(CStr(id)));
    return e.found && CheckCollisionPointRec(GetMousePosition(),
        (Rectangle){e.boundingBox.x, e.boundingBox.y, e.boundingBox.width, e.boundingBox.height});
}

bool PicoDiffUi_BlocksKeyboard(const PicoHost *host)
{
    DiffState *s = PicoPlugins_WorkspaceState(PicoHost_SelectedWorkspaceConst(host), "diff");
    return s && ((PicoDiff_IsOpen(host) && s->focused) || s->pointer_claimed);
}

bool PicoDiffUi_BlocksPointer(const PicoHost *host)
{
    DiffState *s = PicoPlugins_WorkspaceState(PicoHost_SelectedWorkspaceConst(host), "diff");
    return s && s->pointer_claimed;
}

bool PicoDiffUi_ResizeHovered(const PicoHost *host)
{
    DiffState *s = PicoPlugins_WorkspaceState(PicoHost_SelectedWorkspaceConst(host), "diff");
    return s && PicoDiff_IsOpen(host) && host->window_focused && !host->window_minimized &&
        !PicoUi_ModalOpen(host) && (s->resizing || (!host->ui_drag_active && ReviewPointerIn("DiffResizeHandle")));
}

bool PicoDiffUi_ScrollHovered(PicoHost *host, Clay_Vector2 wheel)
{
    if (!PicoDiff_IsOpen(host) || PicoUi_ModalOpen(host)) return false;
    Clay_ElementId id;
    if (Clay_PointerOver(CLAY_ID("DiffFiles"))) id = CLAY_ID("DiffFiles");
    else if (Clay_PointerOver(CLAY_ID("DiffScroll"))) id = CLAY_ID("DiffScroll");
    else return false;
    /* Filename clipping creates nested horizontal containers. Route the wheel
     * to the owning list/document rather than letting those labels consume it. */
    Clay_ScrollContainerData data = Clay_GetScrollContainerData(id);
    if (data.found && data.scrollPosition) {
        data.scrollPosition->y = fmaxf(-fmaxf(0, data.contentDimensions.height - data.scrollContainerDimensions.height),
                                      fminf(0, data.scrollPosition->y + wheel.y * 10));
        if (data.config.horizontal)
            data.scrollPosition->x = fmaxf(-fmaxf(0, data.contentDimensions.width - data.scrollContainerDimensions.width),
                                          fminf(0, data.scrollPosition->x + wheel.x * 10));
        if (wheel.x || wheel.y) pico_host_request_redraw(host);
    }
    return true;
}

void PicoDiffUi_Input(PicoHost *host)
{
    DiffState *s = PicoPlugins_WorkspaceState(PicoHost_SelectedWorkspace(host), "diff");
    if (!s) return;
    bool click = IsMouseButtonPressed(MOUSE_BUTTON_LEFT);
    if (!IsMouseButtonDown(MOUSE_BUTTON_LEFT) || click) s->pointer_claimed = false;
    if (!PicoDiff_IsOpen(host) || !host->window_focused || host->window_minimized || PicoUi_ModalOpen(host)) {
        if (s->resizing) { s->resizing = false; host->ui_drag_active = false; }
        return;
    }
    if (s->resizing || (click && PicoDiffUi_ResizeHovered(host))) {
        if (!s->resizing) {
            s->resizing = true; s->resize_x = GetMousePosition().x;
            s->resize_width = PicoDiffUi_ShellWidth(host); host->ui_drag_active = true;
        }
        s->focused = s->pointer_claimed = true;
        if (!IsKeyPressed(KEY_ESCAPE)) s->width = fminf(PicoHost_RightColumnWidth(host) * 0.65f,
            fmaxf(300, s->resize_width + s->resize_x - GetMousePosition().x));
        if (!IsMouseButtonDown(MOUSE_BUTTON_LEFT) || IsKeyPressed(KEY_ESCAPE)) {
            s->resizing = false; host->ui_drag_active = false;
        }
        host->ui_relayout_requested = true; pico_host_request_redraw(host);
        return;
    }
    if (click) {
        s->focused = ReviewPointerIn("ChangesPane") || ReviewPointerIn("DiffFilesMenu");
        if (!s->focused) s->files_menu = false;
    }
    /* Read-only review owns typing while focused, never the chat draft. */
    if (s->focused) while (GetCharPressed()) {}
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

static void ReviewText(Clay_String text, Clay_Color color)
{
    CLAY_TEXT(text, CLAY_TEXT_CONFIG({.fontId=FONT_MONO, .fontSize=PICO_FONT_UI,
                                    .textColor=color, .wrapMode=CLAY_TEXT_WRAP_NONE}));
}

static void ReviewLabel(Clay_String text, Clay_Color color)
{
    CLAY_TEXT(text, CLAY_TEXT_CONFIG({.fontId=FONT_REGULAR, .fontSize=PICO_FONT_UI,
                                    .textColor=color, .wrapMode=CLAY_TEXT_WRAP_NONE}));
}

static void ReviewButton(const char *id, const char *text)
{
    CLAY(Clay_GetElementId(CStr(id)), {.layout={.padding={6,6,3,3}},
         .backgroundColor=COLOR_CODE_BG, .cornerRadius=CLAY_CORNER_RADIUS(4)}) {
        ReviewLabel(CStr(text), COLOR_TEXT);
    }
}

static void RenderFileIdentity(DiffState *s, int index, bool header)
{
    ReviewFile *file = &s->files[index];
    const char *slash = strrchr(file->path, '/');
    const char *name = slash ? slash + 1 : file->path;
    CLAY_AUTO_ID({.layout={.childGap=8, .sizing={.width=CLAY_SIZING_GROW(0)}},
                  .clip={.horizontal=true}}) {
        CLAY_TEXT(CStr(name), CLAY_TEXT_CONFIG({.fontId=header ? FONT_BOLD : FONT_REGULAR,
            .fontSize=PICO_FONT_UI, .textColor=COLOR_TEXT, .wrapMode=CLAY_TEXT_WRAP_NONE}));
        if (slash) ReviewLabel(Slice(file->path, (int)(slash - file->path)), COLOR_MUTED);
    }
    ReviewLabel(CStr(file->adds), COLOR_DIFF_ADD);
    ReviewLabel(CStr(file->dels), COLOR_DIFF_DEL);
}

static void RenderFiles(DiffState *s, float width)
{
    CLAY(CLAY_ID("DiffFiles"), {.layout={.layoutDirection=CLAY_TOP_TO_BOTTOM,
        .sizing={.width=CLAY_SIZING_FIXED(width), .height=CLAY_SIZING_PERCENT(1)}, .childGap=4},
        .clip={.vertical=true, .childOffset=Clay_GetScrollOffset()}}) {
        for (int i = 0; i < s->file_count; i++) {
            CLAY(CLAY_IDI("DiffFileEntry", i), {.layout={.childGap=4, .padding={4,4,4,4},
                .sizing={.width=CLAY_SIZING_GROW(0)}}}) {
                CLAY(CLAY_IDI("DiffFileCollapse", i), {.layout={.padding={2,2,0,0}}}) {
                    ReviewText(s->files[i].collapsed ? CLAY_STRING("▸") : CLAY_STRING("▾"), COLOR_MUTED);
                }
                CLAY(CLAY_IDI("DiffFile", i), {.layout={.childGap=4,
                    .sizing={.width=CLAY_SIZING_GROW(0)}}}) { RenderFileIdentity(s, i, false); }
            }
        }
    }
}

void PicoDiffUi_Layout(PicoHost *host)
{
    DiffState *s = PicoPlugins_WorkspaceState(PicoHost_SelectedWorkspace(host), "diff");
    if (!s || !PicoDiff_IsOpen(host)) return;
    float width = PicoDiffUi_ShellWidth(host);
    bool wide = width >= 700;
    snprintf(s->summary, sizeof(s->summary), "%d files  +%d −%d%s", s->file_count,
             s->model ? s->model->adds : 0, s->model ? s->model->dels : 0,
             s->model && s->model->partial ? " · partial capture" : "");
    CLAY(CLAY_ID("ChangesPane"), {.layout={.layoutDirection=CLAY_TOP_TO_BOTTOM,
        .padding={10,10,10,10}, .childGap=8,
        .sizing={.width=CLAY_SIZING_FIXED(width), .height=CLAY_SIZING_PERCENT(1)}},
        .backgroundColor=COLOR_CONTENT_BG, .cornerRadius=CLAY_CORNER_RADIUS(6)}) {
        CLAY(CLAY_ID("DiffResizeHandle"), {
            .floating={.attachTo=CLAY_ATTACH_TO_PARENT, .offset={-4,0}, .zIndex=8},
            .layout={.sizing={.width=CLAY_SIZING_FIXED(8), .height=CLAY_SIZING_PERCENT(1)}}}) {}
        CLAY(CLAY_ID("ChangesToolbar"), {.layout={.childGap=6,
            .sizing={.width=CLAY_SIZING_GROW(0)}}}) {
            ReviewLabel(CLAY_STRING("Changes"), COLOR_TEXT);
            CLAY_AUTO_ID({.layout={.sizing={.width=CLAY_SIZING_GROW(0)}}}) {}
            ReviewButton("DiffPrevious", "↑"); ReviewButton("DiffNext", "↓");
            ReviewButton("DiffClose", "×");
        }
        ReviewLabel(CLAY_STRING("Workspace vs HEAD"), COLOR_MUTED);
        CLAY(CLAY_ID("DiffSummary"), {.layout={.sizing={.width=CLAY_SIZING_GROW(0),
            .height=CLAY_SIZING_FIXED(Pico_FontPx(PICO_FONT_UI))}}, .clip={.horizontal=true}}) {
            ReviewLabel(CStr(s->summary), COLOR_MUTED);
        }
        CLAY(CLAY_ID("DiffViewControls"), {.layout={.childGap=8}}) {
            if (!wide) ReviewButton("DiffFilesToggle", "Files ▾");
        }
        CLAY(CLAY_ID("ChangesContent"), {.layout={.childGap=8,
            .sizing={.width=CLAY_SIZING_GROW(0), .height=CLAY_SIZING_GROW(0)}}}) {
            if (wide) RenderFiles(s, fminf(320, width * 0.42f));
            else if (s->files_menu) {
                CLAY(CLAY_ID("DiffFilesMenu"), {
                    .floating={.attachTo=CLAY_ATTACH_TO_PARENT, .zIndex=12},
                    .layout={.sizing={.width=CLAY_SIZING_FIXED(fmaxf(0,width-20)), .height=CLAY_SIZING_FIXED(240)}},
                    .backgroundColor=COLOR_CODE_BG}) { RenderFiles(s, fmaxf(0,width-20)); }
            }
            CLAY(CLAY_ID("ChangesDocument"), {.layout={.layoutDirection=CLAY_TOP_TO_BOTTOM,
                .sizing={.width=CLAY_SIZING_GROW(0), .height=CLAY_SIZING_PERCENT(1)}}}) {
                Clay_ScrollContainerData scroll = Clay_GetCurrentContext() ?
        Clay_GetScrollContainerData(CLAY_ID("DiffScroll")) : (Clay_ScrollContainerData){0};
                float top = scroll.found && scroll.scrollPosition ? -scroll.scrollPosition->y : -s->scroll_y;
                int current = ReviewAt(s, top);
                if (current < 0) current = 0;
                if (current >= s->row_count) current = s->row_count - 1;
                CLAY(CLAY_ID("DiffSticky"), {.layout={.padding={10,10,0,0}, .childGap=6,
                    .childAlignment={.y=CLAY_ALIGN_Y_CENTER},
                    .sizing={.width=CLAY_SIZING_GROW(0), .height=CLAY_SIZING_FIXED(2 * ReviewStride())}},
                    .clip={.horizontal=true}, .backgroundColor=COLOR_CODE_BG,
                    .border={.color=COLOR_TABLE_BORDER, .width={.top=1, .bottom=1}}}) {
                    if (current >= 0) {
                        RenderFileIdentity(s, s->rows[current].file, true);
                    }
                }
                CLAY(CLAY_ID("DiffScrollRow"), {.layout={.childGap=SCROLLBAR_GAP,
                    .sizing={.width=CLAY_SIZING_GROW(0), .height=CLAY_SIZING_GROW(0)}}}) {
                    CLAY(CLAY_ID("DiffScroll"), {.layout={.layoutDirection=CLAY_TOP_TO_BOTTOM,
                        .sizing={.width=CLAY_SIZING_GROW(0), .height=CLAY_SIZING_GROW(0)}},
                        .clip={.vertical=true, .horizontal=true, .childOffset=Clay_GetScrollOffset()}}) {
                        if (!s->row_count) ReviewText(CStr(!s->model ? "Loading changes…" :
                            s->review_failed ? "Unable to allocate review rows" :
                            !s->model->is_repo ? "Not a Git repository" : "No changes"), COLOR_MUTED);
                        int first, end; float before, after;
                        PicoDiffWindow_Range(s->total_units, top,
                            scroll.found ? scroll.scrollContainerDimensions.height : 600,
                            ReviewStride(), 0, DIFF_ROW_OVERSCAN, &first, &end, &before, &after);
                        first = s->row_count ? ReviewAt(s, first * ReviewStride()) : 0;
                        end = end > 0 ? ReviewAt(s, (end - 1) * ReviewStride()) + 1 : 0;
                        before = ReviewOffset(s, first);
                        after = ReviewOffset(s, s->row_count) - ReviewOffset(s, end);
                        s->mounted_first = first; s->mounted_end = end;
                        if (before > 0) CLAY(CLAY_ID("DiffTopPad"), {.layout={.sizing={.height=CLAY_SIZING_FIXED(before)}}}) {}
                        for (int i = first; i < end; i++) {
                            ReviewRow *v = &s->rows[i];
                            DiffFile *f = &s->model->files[v->file]; DiffRow *r = &f->rows[v->row];
                            bool header = r->kind == ROW_HEADER;
                            CLAY(CLAY_IDI("DiffReviewRow", i), {.layout={.childGap=6,
                                .padding=header ? (Clay_Padding){10,10,0,0} : (Clay_Padding){6,6,2,2},
                                .childAlignment={.y=CLAY_ALIGN_Y_CENTER},
                                .sizing={.width=CLAY_SIZING_GROW(r->kind == ROW_HEADER ? 0 : s->row_min_width + 110),
                                         .height=CLAY_SIZING_FIXED((header ? 2 : 1) * ReviewStride())}},
                                .backgroundColor=header ? COLOR_CODE_BG : RowBg(r->kind),
                                .border={.color=COLOR_TABLE_BORDER, .width={.top=header ? 1 : 0}}}) {
                                if (v->hidden) ReviewText(CStr(v->caption), COLOR_MUTED);
                                else if (r->kind == ROW_HEADER) {
                                    ReviewText(CStr(v->caption), COLOR_MUTED);
                                    RenderFileIdentity(s, v->file, true);
                                } else if (r->kind == ROW_CTX || r->kind == ROW_ADD || r->kind == ROW_DEL) {
                                    ReviewText(CStr(v->gutter), COLOR_MUTED);
                                    ReviewText(r->kind == ROW_ADD ? CLAY_STRING("+") :
                                               r->kind == ROW_DEL ? CLAY_STRING("−") : CLAY_STRING(" "), RowSignFg(r->kind));
                                    RenderRowText(r, r->kind == ROW_DEL ? f->old_spans : f->new_spans,
                                                  r->kind == ROW_DEL ? f->old_count : f->new_count, RowFg(r->kind));
                                } else ReviewText(Slice(r->text, r->len), COLOR_MUTED);
                            }
                        }
                        if (after > 0) CLAY(CLAY_ID("DiffBottomPad"), {.layout={.sizing={.height=CLAY_SIZING_FIXED(after)}}}) {}
                    }
                    if (s->overflow) PicoScrollbar_Render(CLAY_STRING("DiffScroll"), CLAY_STRING("DiffScrollTrack"), CLAY_STRING("DiffScrollHandle"));
                }
            }
        }
    }
}

static void JumpTo(DiffState *s, int index)
{
    s->scroll_y = -ReviewOffset(s, index); s->restore_scroll = true;
    s->workspace->host->ui_relayout_requested = true;
}

bool PicoDiffUi_AfterLayout(PicoHost *host)
{
    DiffState *s = PicoPlugins_WorkspaceState(PicoHost_SelectedWorkspace(host), "diff");
    if (!s) return false;
    Clay_ScrollContainerData scroll = Clay_GetCurrentContext() ?
        Clay_GetScrollContainerData(CLAY_ID("DiffScroll")) : (Clay_ScrollContainerData){0};
    bool changed = false;
    if (PicoDiff_IsOpen(host) && scroll.found && scroll.scrollPosition &&
        scroll.scrollContainerDimensions.height > 0 && Clay_GetElementData(CLAY_ID("DiffScroll")).found) {
        if (s->restore_scroll) {
            float max = fmaxf(0, scroll.contentDimensions.height - scroll.scrollContainerDimensions.height);
            scroll.scrollPosition->y = fmaxf(-max, fminf(0, s->scroll_y));
            scroll.scrollPosition->x = s->scroll_x;
            s->restore_scroll = false; changed = true;
        }
        s->scroll_y = scroll.scrollPosition->y; s->scroll_x = scroll.scrollPosition->x;
        s->overflow = PicoScrollbar_Overflows(CLAY_STRING("DiffScroll"));
    }
    return changed;
}

static void DiffHostAfterLayout(PicoHost *host, const PicoHookEvent *event, void *state)
{
    (void)event; (void)state;
    DiffState *s = PicoPlugins_WorkspaceState(PicoHost_SelectedWorkspace(host), "diff");
    if (!s) return;
    if (PicoUi_ModalOpen(host) || !host->window_focused || host->window_minimized) return;
    bool click = IsMouseButtonPressed(MOUSE_BUTTON_LEFT);
    if (Clay_PointerOver(CLAY_ID("FooterDiff"))) {
        host->hovered_clickable = true;
        if (click) PicoDiffUi_Open(host);
    }
    if (!PicoDiff_IsOpen(host) || s->pointer_claimed) return;
    const char *buttons[] = {"DiffClose", "DiffFilesToggle", "DiffPrevious", "DiffNext", "DiffSticky"};
    for (int b = 0; b < 5; b++) if (Clay_PointerOver(Clay_GetElementId(CStr(buttons[b])))) {
        host->hovered_clickable = true;
        if (!click) continue;
        if (b == 0) PicoDiffUi_Close(host);
        else if (b == 1) { s->files_menu = !s->files_menu; host->ui_relayout_requested = true; }
        else if (b == 4) {
            int at = ReviewAt(s, -s->scroll_y);
            if (at >= 0 && at < s->row_count) {
                int fi = s->rows[at].file;
                s->files[fi].collapsed = !s->files[fi].collapsed;
                BuildReview(s);
                for (int i = 0; i < s->row_count; i++) if (s->rows[i].file == fi) { JumpTo(s, i); break; }
            }
        } else {
            int at = ReviewAt(s, -s->scroll_y);
            if (at < 0) at = 0;
            if (at >= s->row_count) at = s->row_count - 1;
            if (at < 0) continue;
            int fi = s->rows[at].file, ri = s->rows[at].row;
            int step = b == 2 ? -1 : 1;
            bool found = false;
            while (!found && fi >= 0 && fi < s->file_count) {
                DiffFile *f = &s->model->files[fi];
                for (ri += step; ri >= 0 && ri < f->row_count; ri += step) {
                    DiffRowKind kind = f->rows[ri].kind;
                    DiffRowKind prev = ri ? f->rows[ri-1].kind : ROW_HEADER;
                    if ((kind == ROW_ADD || kind == ROW_DEL) && prev != ROW_ADD && prev != ROW_DEL) { found = true; break; }
                }
                if (!found) { fi += step; if (fi >= 0 && fi < s->file_count) ri = step > 0 ? -1 : s->model->files[fi].row_count; }
            }
            if (found) {
                s->files[fi].collapsed = false; BuildReview(s);
                for (int i = 0; i < s->row_count; i++) if (s->rows[i].file == fi && s->rows[i].row == ri) { JumpTo(s, i); break; }
            }
        }
    }
    for (int fi = 0; fi < s->file_count; fi++) if (Clay_PointerOver(CLAY_IDI("DiffFileCollapse", fi))) {
        host->hovered_clickable = true;
        if (click) { s->files[fi].collapsed = !s->files[fi].collapsed; BuildReview(s); host->ui_relayout_requested = true; return; }
    }
    for (int fi = 0; fi < s->file_count; fi++) if (Clay_PointerOver(CLAY_IDI("DiffFile", fi))) {
        host->hovered_clickable = true;
        if (click) {
            s->files[fi].collapsed = false; s->files_menu = false; BuildReview(s);
            for (int i = 0; i < s->row_count; i++) if (s->rows[i].file == fi) { JumpTo(s, i); break; }
            return;
        }
    }
    for (int i = s->mounted_first; i < s->mounted_end && i < s->row_count; i++) if (Clay_PointerOver(CLAY_IDI("DiffReviewRow", i))) {
        ReviewRow v = s->rows[i];
        if (!v.hidden && s->model->files[v.file].rows[v.row].kind != ROW_HEADER) continue;
        host->hovered_clickable = true;
        if (click) {
            if (v.hidden) {
                ReviewFile *file = &s->files[v.file];
                int *gaps = realloc(file->expanded_gaps, (size_t)(file->gap_count + 1) * sizeof(int));
                if (!gaps) return;
                file->expanded_gaps = gaps; gaps[file->gap_count++] = v.gap_line;
            }
            else s->files[v.file].collapsed = !s->files[v.file].collapsed;
            BuildReview(s); JumpTo(s, i); return;
        }
    }
}

static void DiffWorkspaceOnFrame(PicoWorkspace *workspace, void *state, float dt)
{
    (void)dt;
    DiffState *s = (DiffState *)state;
    if (!s) return;
    if (s->worker)
    {
        bool active = s->open || (workspace &&
            PicoHost_SelectedWorkspace(workspace->host) == workspace);
        pthread_mutex_lock(&s->worker->lock);
        if (active && !s->worker->active)
        {
            s->worker->refresh_requested = true;
            pthread_cond_signal(&s->worker->wake);
        }
        s->worker->active = active;
        pthread_mutex_unlock(&s->worker->lock);
    }
    bool visible = workspace && PicoHost_SelectedWorkspace(workspace->host) == workspace && PicoDiff_IsOpen(workspace->host);
    if (visible && !s->was_visible) s->restore_scroll = true;
    s->was_visible = visible;
    if (!visible) {
        if (s->resizing) workspace->host->ui_drag_active = false;
        s->resizing = false; s->pointer_claimed = false;
    }
    AdoptPending(s);
    if (!visible || PicoUi_ModalOpen(workspace->host))
    {
        return;
    }
    PicoScrollbar_UpdateDrag(&s->scrollbar, CLAY_STRING("DiffScroll"), CLAY_STRING("DiffScrollHandle"));

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
    (void)ClosePanel(s);
    FreeReviewFiles(s);
    free(s->rows);
    free(s);
}

PicoExt pico_ext_diff(void)
{
    return (PicoExt){
        .abi = PICO_EXT_ABI,
        .name = "diff",
        .description = "Workspace Changes review panel and footer summary",
        .host_init = DiffHostInit,
        .workspace_init = DiffWorkspaceInit,
        .workspace_shutdown = DiffWorkspaceShutdown,
        .workspace_on_frame = DiffWorkspaceOnFrame,
    };
}
