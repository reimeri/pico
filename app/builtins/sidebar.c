#define _DEFAULT_SOURCE
#define _POSIX_C_SOURCE 200809L

#include "clarification.h"
#include "../agent_internal.h"
#include "host_internal.h"

#include "pico/plugin.h"
#include "agent.h"
#include "builtins/chat.h"
#include "docs_path.h"
#include "overlay.h"
#include "session.h"
#include "settings.h"
#include "builtins/sidebar.h"
#include "tinyfiledialogs.h"
#include "text_range.h"
#include "text_field_ui.h"

#include "clay/clay.h"

#include <math.h>
#include <pthread.h>
#include <utf8proc.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdatomic.h>

#ifdef PICO_SESSION_TEST_HOOKS
extern bool PicoSession_TestHook(const char *stage);
#endif

#define SIDEBAR_SCAN_SEC 0.5
#define SIDEBAR_RECONCILE_SEC 60.0
#define SIDEBAR_SESSION_PAGE 10
#define SIDEBAR_ROW_PAD_X 6
#define SIDEBAR_ROW_GAP 6
#define SIDEBAR_FOLDER_ICON 17
#define SIDEBAR_SESSION_DOT 8
#define SIDEBAR_SESSION_IDLE_DOT 4
#define SIDEBAR_DRAG_THRESHOLD 4.0f
#define SIDEBAR_LABEL_CHUNK_SIZE 32
#define SIDEBAR_LABEL_CAPACITY (PICO_SESSION_TITLE_MAX_BYTES + 4)

typedef struct SidebarLabelChunk
{
    struct SidebarLabelChunk *next;
    char text[SIDEBAR_LABEL_CHUNK_SIZE][SIDEBAR_LABEL_CAPACITY];
} SidebarLabelChunk;

typedef PicoCatalogPage SidebarWsUi;

typedef struct SidebarScanWorker
{
    atomic_bool cancelled;
    uint64_t serial;
    uint64_t inbox_epoch;
    uint64_t project_epoch;
    char token_before[PICO_CATALOG_CHANGE_TOKEN_MAX];
    char token_after[PICO_CATALOG_CHANGE_TOKEN_MAX];
    bool token_before_valid;
    bool token_after_valid;
    PicoCatalogWorkspace *workspaces;
    int workspace_count;
    PicoCatalogPage *pages;
    int page_count;
    PicoSessionRef *live_titles;
    int live_title_count;
    bool failed;
    bool reconcile;
} SidebarScanWorker;

/* Local presentation wins over snapshots started before its save completed.
 * Failed saves stay local for this sidebar lifetime, without retries. */
typedef struct SidebarCollapseEdit
{
    struct SidebarCollapseEdit *next;
    char path[4096];
    bool collapsed;
    bool pending;
    uint64_t accept_scan;
} SidebarCollapseEdit;

typedef struct SidebarCollapseSave
{
    atomic_bool cancelled;
    char path[4096];
    bool collapsed;
    bool saved;
} SidebarCollapseSave;

typedef struct SidebarLiveAgent
{
    PicoAgentId id;
    char path[4096], session_id[40];
} SidebarLiveAgent;

typedef enum InboxSection { INBOX_NEEDS_INPUT, INBOX_WORKING, INBOX_COMPLETED, INBOX_SECTION_COUNT } InboxSection;

typedef struct InboxVisit
{
    const char *path;
    char title[PICO_SESSION_TITLE_MAX_BYTES + 1];
    char session_id[40];
    PicoAgentId live_id;
    int ws_index;
    int slot;
    bool unseen;
    bool worktree;
    bool missing;
    int index;
    double activity;
    InboxSection section;
} InboxVisit;

typedef struct SidebarState
{
    PicoHost *host;
    double wall_clock_offset;
    uint64_t inbox_layout_signature;
    double inbox_transition_until;
    SidebarLiveAgent live[PICO_MAX_TOTAL_AGENTS];
    int live_count;
    bool live_snapshot_valid;
    PicoSessionRef *live_titles;
    int live_title_count;
    PicoAgentId drafts[PICO_MAX_CATALOG_WORKSPACES][PICO_MAX_TOTAL_AGENTS];
    int draft_counts[PICO_MAX_CATALOG_WORKSPACES];
    InboxVisit *inbox_rows;
    int inbox_count, inbox_capacity;
    bool inbox_valid;
    PicoCatalogWorkspace *workspaces;
    int workspace_count;
    SidebarWsUi *ui;
    int ui_count;
    bool want_folder;
    bool folder_painted;
    bool dirty;
    bool catalog_scanned;
    bool reconcile_pending;
    double last_scan;
    double last_reconcile;
    char catalog_change_token[PICO_CATALOG_CHANGE_TOKEN_MAX];
    bool catalog_change_token_valid;
    uint64_t scan_serial;
    uint64_t inbox_epoch;
    uint64_t project_epoch;
    SidebarScanWorker *scan_worker;
    SidebarCollapseEdit *collapse_edits;
    SidebarCollapseSave *collapse_save;
    Texture2D folder_collapsed;
    Texture2D folder_expanded;
    Texture2D archive_open;
    Texture2D archive_closed;
    Texture2D pen_icon;
    bool stash_expanded;
    bool edit_open;
    bool delete_confirm;
    bool edit_pointer_latched;
    int edit_latch_button;
    bool session_menu_open;
    bool session_delete_confirm;
    bool session_menu_latched;
    int session_menu_latch_button;
    char session_menu_path[4096];
    char session_menu_id[40];
    char edit_path[4096];
    char edit_name[PICO_CATALOG_NAME_MAX];
    PicoTextField edit_field;
    char delete_label[384];
    Texture2D settings_icon;
    Texture2D settings_icon_hover;
    Texture2D worktree_icon;
    bool icons_tried;
    bool drag_press_pending;
    int drag_source_index;
    Vector2 drag_press_pos;
    bool is_dragging;
    int drag_target_index;
    uint64_t order_persist_generation;
    bool order_unsaved;
    float row_width;
    SidebarLabelChunk *label_chunks;
    SidebarLabelChunk *label_chunk;
    int label_used;
} SidebarState;

static Clay_String CStr(const char *s)
{
    if (!s)
    {
        s = "";
    }
    return (Clay_String){.length = (int32_t)strlen(s), .chars = s};
}

static float SidebarTextWidth(const char *text, int length)
{
    Clay_TextElementConfig config = {.fontId = FONT_REGULAR, .fontSize = PICO_FONT_UI, .wrapMode = CLAY_TEXT_WRAP_NONE};
    return Pico_MeasureTextUtf8((Clay_StringSlice){.chars = text, .length = length},
                                &config, NULL)
        .width;
}

/* Text commands refer to these strings until Clay_EndLayout and the draw complete.
 * Never realloc a buffer containing a previously emitted title. Reuse the
 * chunks on the next layout pass, after the prior command array is finished. */
static char *SidebarNextLabel(SidebarState *s)
{
    if (!s->label_chunk || s->label_used == SIDEBAR_LABEL_CHUNK_SIZE)
    {
        SidebarLabelChunk *next = s->label_chunk ? s->label_chunk->next : s->label_chunks;
        if (!next)
        {
            next = calloc(1, sizeof(*next));
            if (!next)
                return NULL;
            if (s->label_chunk)
                s->label_chunk->next = next;
            else
                s->label_chunks = next;
        }
        s->label_chunk = next;
        s->label_used = 0;
    }
    return s->label_chunk->text[s->label_used++];
}

static Clay_String SidebarDisplayTitle(SidebarState *s, const char *title, float width)
{
    static const char ellipsis[] = "\xE2\x80\xA6";
    Clay_String full = CStr(title);
    if (full.length <= 0)
        return full;
    if (!(width > 0.0f))
        return CLAY_STRING("");

    /* Catalog names need not be valid UTF-8. Replace malformed bytes in the
     * display copy so a long accepted name does not disappear when shortened. */
    char normalized[SIDEBAR_LABEL_CAPACITY];
    bool repaired = false;
    for (int pos = 0; pos < full.length && pos < SIDEBAR_LABEL_CAPACITY - 1;)
    {
        utf8proc_int32_t codepoint;
        utf8proc_ssize_t step = utf8proc_iterate(
            (const utf8proc_uint8_t *)full.chars + pos, full.length - pos, &codepoint);
        if (step <= 0)
        {
            if (!repaired)
            {
                memcpy(normalized, full.chars, (size_t)full.length + 1);
                repaired = true;
            }
            normalized[pos++] = '?';
        }
        else
            pos += (int)step;
    }
    if (repaired)
        full.chars = normalized;
    /* Leave space for rounded glyph placement and scissor-free painting. */
    width -= 2.0f;
    if (!(width > 0.0f))
        return CLAY_STRING("");
    if (SidebarTextWidth(full.chars, full.length) <= width)
    {
        if (!repaired)
            return full;
        char *copy = SidebarNextLabel(s);
        if (!copy)
            return CLAY_STRING("");
        memcpy(copy, full.chars, (size_t)full.length + 1);
        return (Clay_String){.chars = copy, .length = full.length};
    }
    if (SidebarTextWidth(ellipsis, 3) > width)
        return CLAY_STRING("");
    char *display = SidebarNextLabel(s);
    if (!display)
        return CLAY_STRING("");

    /* A codepoint boundary can still split a combining character or emoji
     * sequence. Shorten on displayed-character (grapheme) boundaries. */
    int boundaries[SIDEBAR_LABEL_CAPACITY] = {0};
    int count = 0, pos = 0;
    utf8proc_int32_t previous = 0, state = 0, codepoint;
    while (pos < full.length && count < SIDEBAR_LABEL_CAPACITY - 2)
    {
        utf8proc_ssize_t step = utf8proc_iterate(
            (const utf8proc_uint8_t *)full.chars + pos, full.length - pos, &codepoint);
        if (step <= 0)
            return CLAY_STRING("");
        if (pos > 0 && utf8proc_grapheme_break_stateful(previous, codepoint, &state))
        {
            boundaries[++count] = pos;
        }
        pos += (int)step;
        previous = codepoint;
    }
    boundaries[++count] = pos;
    int low = 0, high = count;
    while (low < high)
    {
        int mid = low + (high - low + 1) / 2;
        int bytes = boundaries[mid];
        if (bytes + 4 > SIDEBAR_LABEL_CAPACITY)
        {
            high = mid - 1;
            continue;
        }
        memcpy(display, full.chars, (size_t)bytes);
        memcpy(display + bytes, ellipsis, 3);
        display[bytes + 3] = '\0';
        if (SidebarTextWidth(display, bytes + 3) <= width)
            low = mid;
        else
            high = mid - 1;
    }
    int bytes = low ? boundaries[low] : 0;
    memcpy(display, full.chars, (size_t)bytes);
    memcpy(display + bytes, ellipsis, 3);
    display[bytes + 3] = '\0';
    return (Clay_String){.chars = display, .length = bytes + 3};
}

static float SidebarLabelWidth(float row_width, float leading_width, float trailing_width)
{
    float width = row_width - 2.0f * SIDEBAR_ROW_PAD_X - leading_width - SIDEBAR_ROW_GAP;
    if (trailing_width > 0.0f)
        width -= SIDEBAR_ROW_GAP + trailing_width;
    return width > 0.0f ? width : 0.0f;
}

static int ClampShown(int shown, int total)
{
    if (total <= SIDEBAR_SESSION_PAGE)
    {
        return total;
    }
    if (shown < SIDEBAR_SESSION_PAGE)
    {
        shown = SIDEBAR_SESSION_PAGE;
    }
    if (shown > total)
    {
        shown = total;
    }
    return shown;
}

static int ShownForIndex(SidebarState *s, int index, int total)
{
    int shown;
    if (!s || index < 0 || index >= s->ui_count)
    {
        return ClampShown(SIDEBAR_SESSION_PAGE, total);
    }
    shown = ClampShown(s->ui[index].shown, total);
    s->ui[index].shown = shown;
    return shown;
}

static void AdjustShown(SidebarState *s, int index, int delta, int total)
{
    if (!s || index < 0 || index >= s->ui_count)
    {
        return;
    }
    s->ui[index].shown = ClampShown(s->ui[index].shown + delta, total);
    s->host->ui_relayout_requested = true;
}

static int PrevShownForPath(const SidebarWsUi *ui, int ui_count, const char *path)
{
    int i;
    if (!ui || !path || !path[0])
    {
        return SIDEBAR_SESSION_PAGE;
    }
    for (i = 0; i < ui_count; i++)
    {
        if (strcmp(ui[i].path, path) == 0)
        {
            return ui[i].shown;
        }
    }
    return SIDEBAR_SESSION_PAGE;
}

static int SidebarWorkspaceOrderCmp(const void *a, const void *b)
{
    const PicoCatalogWorkspace *x = (const PicoCatalogWorkspace *)a;
    const PicoCatalogWorkspace *y = (const PicoCatalogWorkspace *)b;
    if (x->order != y->order)
    {
        return x->order < y->order ? -1 : 1;
    }
    return strcmp(x->name, y->name);
}

static void SidebarPreserveWorkspaceOrder(PicoCatalogWorkspace *next, int next_count,
                                          const PicoCatalogWorkspace *previous,
                                          int previous_count)
{
    int i;
    int j;
    if (!next || next_count <= 0 || !previous || previous_count <= 0)
    {
        return;
    }
    for (i = 0; i < next_count; i++)
    {
        next[i].order = previous_count;
        for (j = 0; j < previous_count; j++)
        {
            if (strcmp(next[i].path, previous[j].path) == 0)
            {
                next[i].order = j;
                break;
            }
        }
    }
    qsort(next, (size_t)next_count, sizeof(*next), SidebarWorkspaceOrderCmp);
}

static void SidebarAdoptCatalog(SidebarState *s, PicoCatalogWorkspace *next, int n,
                                bool token_before_valid, const char *token_before,
                                bool token_after_valid, const char *token_after)
{
    SidebarWsUi *next_ui = NULL;
    SidebarWsUi *prev_ui;
    int prev_ui_count;
    int i;
    if (!s)
    {
        PicoCatalog_Free(next, n);
        return;
    }
    if (s->order_unsaved)
    {
        SidebarPreserveWorkspaceOrder(next, n, s->workspaces, s->workspace_count);
    }
    prev_ui = s->ui;
    prev_ui_count = s->ui_count;
    if (n > 0)
    {
        next_ui = (SidebarWsUi *)calloc((size_t)n, sizeof(SidebarWsUi));
        if (next_ui)
        {
            for (i = 0; i < n; i++)
            {
                snprintf(next_ui[i].path, sizeof(next_ui[i].path), "%s", next[i].path);
                next_ui[i].shown = PrevShownForPath(prev_ui, prev_ui_count, next[i].path);
            }
        }
    }
    s->live_snapshot_valid = false;
    s->inbox_valid = false;
    PicoCatalog_Free(s->workspaces, s->workspace_count);
    free(prev_ui);
    pico_host_request_redraw(s->host);
    s->workspaces = next;
    s->workspace_count = n;
    s->ui = next_ui;
    s->ui_count = next_ui ? n : 0;
    s->catalog_change_token_valid = token_after_valid;
    if (token_after_valid && token_after)
    {
        snprintf(s->catalog_change_token, sizeof(s->catalog_change_token), "%s", token_after);
    }
    s->dirty = token_after_valid &&
               (!token_before_valid || !token_before || strcmp(token_before, token_after) != 0);
    s->catalog_scanned = true;
    s->last_scan = GetTime();
}

static bool SidebarCopyLiveTitleTargets(SidebarState *s, SidebarScanWorker *worker)
{
    int count = pico_agent_count(s->host);
    if (count == 0) return true;
    worker->live_titles = calloc((size_t)count, sizeof(*worker->live_titles));
    if (!worker->live_titles) return false;
    for (int i = 0; i < count; i++)
    {
        PicoAgentInfo info;
        if (!pico_agent_info(s->host, i, &info)) continue;
        const PicoAgent *agent = PicoHost_FindAgentConst(s->host, info.id);
        if (!PicoAgent_IsUserMain(agent) || !info.session_id[0]) continue;
        PicoSessionRef *title = &worker->live_titles[worker->live_title_count++];
        snprintf(title->checkout, sizeof(title->checkout), "%s", PicoAgent_WorkspacePath(agent));
        snprintf(title->id, sizeof(title->id), "%s", info.session_id);
    }
    return true;
}

static void *SidebarScanRun(void *arg)
{
    SidebarScanWorker *worker = (SidebarScanWorker *)arg;
    worker->token_before_valid = PicoCatalog_ReadChangeToken(worker->token_before);
    if (!atomic_load(&worker->cancelled))
    {
        worker->workspace_count = worker->reconcile ? PicoCatalog_ScanGroupedPaged(&worker->workspaces, &worker->cancelled,
                                                                                   worker->pages, worker->page_count)
                                                    : PicoCatalog_ReadGroupedPaged(&worker->workspaces, &worker->cancelled,
                                                                                   worker->pages, worker->page_count);
    }
    if (worker->workspace_count < 0 ||
        (!atomic_load(&worker->cancelled) &&
         !PicoCatalog_ReadSessionTitles(worker->live_titles, worker->live_title_count, &worker->cancelled)))
        worker->failed = true;
    worker->token_after_valid = PicoCatalog_ReadChangeToken(worker->token_after);
    return NULL;
}

static void SidebarScanCancel(void *arg)
{
    atomic_store(&((SidebarScanWorker *)arg)->cancelled, true);
}

static void SidebarScanDestroy(void *arg)
{
    SidebarScanWorker *worker = (SidebarScanWorker *)arg;
    PicoCatalog_Free(worker->workspaces, worker->workspace_count);
    free(worker->pages);
    free(worker->live_titles);
    free(worker);
}

/* Equality is about sidebar-visible data, not file-stat/index bookkeeping.
 * An unchanged reconciliation must not replace labels, paging or scroll state. */
static bool SidebarCatalogEqual(const SidebarState *s, const SidebarScanWorker *worker)
{
    if (!s->catalog_scanned || s->workspace_count != worker->workspace_count ||
        s->live_title_count != worker->live_title_count)
        return false;
    for (int i = 0; i < s->live_title_count; i++)
    {
        const PicoSessionRef *a = &s->live_titles[i], *b = &worker->live_titles[i];
        if (strcmp(a->checkout, b->checkout) || strcmp(a->id, b->id) || strcmp(a->title, b->title))
            return false;
    }
    for (int i = 0; i < s->workspace_count; i++)
    {
        const PicoCatalogWorkspace *a = &s->workspaces[i], *b = &worker->workspaces[i];
        if (strcmp(a->path, b->path) || strcmp(a->name, b->name) ||
            a->missing != b->missing || a->collapsed != b->collapsed ||
            a->stashed != b->stashed || a->has_more_sessions != b->has_more_sessions ||
            a->session_count != b->session_count || a->inbox_count != b->inbox_count)
            return false;
        for (int j = 0; j < a->session_count + a->inbox_count; j++)
        {
            const PicoCatalogSession *x = j < a->session_count ? &a->sessions[j] : &a->inbox_sessions[j - a->session_count];
            const PicoCatalogSession *y = j < b->session_count ? &b->sessions[j] : &b->inbox_sessions[j - b->session_count];
            if (strcmp(x->id, y->id) || strcmp(x->title, y->title) ||
                strcmp(x->model, y->model) || strcmp(x->effort, y->effort) ||
                strcmp(x->checkout_path, y->checkout_path) ||
                strcmp(x->checkout_name, y->checkout_name) || x->worktree != y->worktree ||
                x->missing_checkout != y->missing_checkout ||
                x->unseen_complete != y->unseen_complete || x->inbox_pending != y->inbox_pending ||
                x->kind != y->kind || x->mtime != y->mtime || x->mtime_nsec != y->mtime_nsec)
                return false;
        }
    }
    return true;
}

static void SidebarFinishRefresh(SidebarState *s, SidebarScanWorker *worker)
{
    /* Failed maintenance leaves the last successful snapshot on screen. Do
     * not spin a failed reconciliation on every frame; retry on its interval. */
    if (worker->reconcile)
    {
        s->last_reconcile = GetTime();
        s->reconcile_pending = false;
    }
    if (worker->failed)
    {
        if (!worker->reconcile)
            s->dirty = true;
        return;
    }
    /* A refresh already in flight at click/save time may contain old state. */
    SidebarCollapseEdit **link = &s->collapse_edits;
    while (*link)
    {
        SidebarCollapseEdit *edit = *link;
        if (edit->accept_scan && worker->serial >= edit->accept_scan)
        {
            *link = edit->next;
            free(edit);
            continue;
        }
        for (int i = 0; i < worker->workspace_count; i++)
            if (strcmp(worker->workspaces[i].path, edit->path) == 0)
                worker->workspaces[i].collapsed = edit->collapsed;
        link = &edit->next;
    }
    PicoProjectPreference **pref_link = &s->host->project_preferences;
    while (*pref_link)
    {
        PicoProjectPreference *pref = *pref_link;
        bool covered = false;
        for (int i = 0; i < worker->workspace_count; i++)
        {
            PicoCatalogWorkspace *ws = &worker->workspaces[i];
            if (strcmp(ws->path, pref->path)) continue;
            covered = !pref->pending && pref->completed_epoch <= worker->project_epoch &&
                      !strcmp(ws->name, pref->name) && ws->stashed == pref->stashed;
            if (!covered)
            {
                snprintf(ws->name, sizeof(ws->name), "%s", pref->name);
                ws->stashed = pref->stashed;
            }
        }
        if (covered) { *pref_link = pref->next; free(pref); }
        else pref_link = &pref->next;
    }
    bool refresh_again = s->dirty;
    if (s->order_unsaved)
        SidebarPreserveWorkspaceOrder(worker->workspaces, worker->workspace_count,
                                      s->workspaces, s->workspace_count);
    if (!SidebarCatalogEqual(s, worker))
    {
        SidebarAdoptCatalog(s, worker->workspaces, worker->workspace_count,
                            worker->token_before_valid, worker->token_before,
                            worker->token_after_valid, worker->token_after);
        worker->workspaces = NULL;
        worker->workspace_count = 0;
        free(s->live_titles);
        s->live_titles = worker->live_titles;
        s->live_title_count = worker->live_title_count;
        worker->live_titles = NULL;
        worker->live_title_count = 0;
    }
    else
    {
        s->catalog_change_token_valid = worker->token_after_valid;
        if (worker->token_after_valid)
            snprintf(s->catalog_change_token, sizeof(s->catalog_change_token), "%s", worker->token_after);
        s->dirty = worker->token_after_valid &&
                   (!worker->token_before_valid || strcmp(worker->token_before, worker->token_after));
        s->last_scan = GetTime();
    }
    s->dirty = s->dirty || refresh_again;
    PicoCatalog_InboxSnapshotApplied(s->host, worker->inbox_epoch, s->workspaces, s->workspace_count);
}

static void SidebarScanCompleted(PicoHost *host, void *arg)
{
    SidebarScanWorker *worker = (SidebarScanWorker *)arg;
    SidebarState *s = host ? (SidebarState *)PicoPlugins_HostState(host, "sidebar") : NULL;
    if (s && s->scan_worker == worker)
        s->scan_worker = NULL;
    if (!s || s->scan_serial != worker->serial || atomic_load(&worker->cancelled))
        return;
    SidebarFinishRefresh(s, worker);
}

static void SidebarRefreshSync(SidebarState *s, bool reconcile)
{
    SidebarScanWorker worker = {.reconcile = reconcile, .pages = s->ui, .page_count = s->ui_count,
                                .inbox_epoch = PicoCatalog_InboxEpoch(s->host),
                                .project_epoch = s->host->project_preference_epoch};
    if (!SidebarCopyLiveTitleTargets(s, &worker)) worker.failed = true;
    else SidebarScanRun(&worker);
    SidebarFinishRefresh(s, &worker);
    PicoCatalog_Free(worker.workspaces, worker.workspace_count);
    free(worker.live_titles);
}

static bool SidebarHostCanTask(const PicoHost *host)
{
    return host && host->ask_id_mu_ready && !host->terminal_shutdown;
}

static void SidebarStartRefresh(SidebarState *s, bool reconcile)
{
    SidebarScanWorker *worker;
    if (!s || !s->host)
    {
        return;
    }
    if (s->scan_worker)
    {
        s->dirty = true;
        return;
    }
    s->scan_serial++;
    if (s->scan_serial == 0)
    {
        s->scan_serial = 1;
    }
    s->dirty = false;
    s->last_scan = GetTime();
    if (!SidebarHostCanTask(s->host))
    {
        SidebarRefreshSync(s, reconcile);
        return;
    }
    worker = (SidebarScanWorker *)calloc(1, sizeof(*worker));
    if (!worker)
    {
        s->dirty = true;
        return;
    }
    worker->serial = s->scan_serial;
    worker->inbox_epoch = PicoCatalog_InboxEpoch(s->host);
    worker->project_epoch = s->host->project_preference_epoch;
    worker->reconcile = reconcile;
    worker->page_count = s->ui_count;
    if (worker->page_count)
    {
        worker->pages = malloc((size_t)worker->page_count * sizeof(*worker->pages));
        if (!worker->pages)
        {
            free(worker);
            s->dirty = true;
            return;
        }
        memcpy(worker->pages, s->ui, (size_t)worker->page_count * sizeof(*worker->pages));
    }
    if (!SidebarCopyLiveTitleTargets(s, worker))
    {
        SidebarScanDestroy(worker);
        s->dirty = true;
        return;
    }
    if (!PicoHost_StartTaskCompleted(s->host, SidebarScanRun, worker, SidebarScanCancel,
                                     SidebarScanCompleted, SidebarScanDestroy))
    {
        SidebarScanDestroy(worker);
        s->dirty = true;
        return;
    }
    s->scan_worker = worker;
}

static bool SidebarCatalogChanged(SidebarState *s)
{
    char token[PICO_CATALOG_CHANGE_TOKEN_MAX];
    if (!s || !PicoCatalog_ReadChangeToken(token))
    {
        return true;
    }
    return !s->catalog_change_token_valid ||
           strcmp(s->catalog_change_token, token) != 0;
}

static bool FolderDialogGraphic(void)
{
    tinyfd_forceConsole = 0;
    (void)tinyfd_selectFolderDialog("tinyfd_query", NULL);
    if (!tinyfd_response[0])
    {
        return false;
    }
    return strcmp(tinyfd_response, "dialog") != 0 && strcmp(tinyfd_response, "whiptail") != 0 &&
           strcmp(tinyfd_response, "basicinput") != 0 && strcmp(tinyfd_response, "no_solution") != 0;
}

static bool ClearFolderRequest(SidebarState *s)
{
    if (!s)
    {
        return false;
    }
    if (s->want_folder && s->host && !pico_ui_modal_pop(s->host, "sidebar-folder"))
    {
        return false;
    }
    s->want_folder = false;
    s->folder_painted = false;
    return true;
}

static void RequestAddWorkspace(PicoHost *host, SidebarState *s)
{
    if (!host || !s)
    {
        return;
    }
    if (!FolderDialogGraphic())
    {
        PicoOverlay_Notify(host, "Folder dialog unavailable. Install zenity or kdialog.");
        return;
    }
    if (!s->want_folder && !pico_ui_modal_push(host, "sidebar-folder"))
    {
        return;
    }
    s->want_folder = true;
    s->folder_painted = false;
}

static void NotifyOpenResult(PicoHost *host, PicoResult result)
{
    if (result == PICO_OK || result == PICO_ALREADY_OPEN)
    {
        return;
    }
    if (result == PICO_LIMIT)
    {
        PicoOverlay_Notify(host, "Too many workspaces are open.");
        return;
    }
    if (result == PICO_BUSY)
    {
        PicoOverlay_Notify(host, "That workspace is closing.");
        return;
    }
    PicoOverlay_Notify(host, "Could not open that workspace.");
}

static PicoWorkspaceId OpenLiveWorkspace(PicoHost *host, const char *path, PicoResult *out_opened)
{
    PicoWorkspaceId id = 0;
    PicoResult opened;
    if (out_opened)
    {
        *out_opened = PICO_INVALID;
    }
    if (!host || !path || !path[0])
    {
        return 0;
    }
    opened = pico_workspace_open(host, path, &id);
    if (out_opened)
    {
        *out_opened = opened;
    }
    if (opened != PICO_OK && opened != PICO_ALREADY_OPEN)
    {
        NotifyOpenResult(host, opened);
        return 0;
    }
    PicoCatalog_Ensure(path);
    return id;
}

static PicoAgentId LiveMainAgent(PicoHost *host, const char *ws_path, const char *session_id)
{
    if (!host || !ws_path || !ws_path[0]) return 0;
    SidebarState *s = PicoPlugins_HostState(host, "sidebar");
    if (s && s->live_snapshot_valid)
    {
        for (int i = 0; i < s->live_count; i++)
        {
            SidebarLiveAgent *live = &s->live[i];
            if (!strcmp(live->path, ws_path) &&
                !strcmp(live->session_id, session_id ? session_id : "")) return live->id;
        }
        return 0;
    }
    for (int w = 0; w < host->workspace_count; w++)
    {
        PicoWorkspace *ws = host->workspaces[w];
        if (!ws || strcmp(ws->path, ws_path)) continue;
        for (int i = 0; i < ws->count; i++)
        {
            PicoAgent *agent = ws->agents[i];
            if (PicoAgent_IsUserMain(agent) &&
                !strcmp(agent->session_id, session_id ? session_id : "")) return agent->id;
        }
    }
    return 0;
}

static bool CatalogHasSession(const PicoCatalogWorkspace *ws, const char *checkout_path,
                              const char *session_id)
{
    int i;
    if (!ws || !session_id || !session_id[0])
    {
        return false;
    }
    for (i = 0; i < ws->inbox_count; i++)
    {
        if (strcmp(ws->inbox_sessions[i].id, session_id) == 0 && checkout_path &&
            strcmp(ws->inbox_sessions[i].checkout_path, checkout_path) == 0)
        {
            return true;
        }
    }
    return false;
}

static const PicoSessionRef *SidebarLiveTitle(const SidebarState *s, const char *path, const char *id)
{
    for (int i = 0; i < s->live_title_count; i++)
        if (!strcmp(s->live_titles[i].checkout, path) && !strcmp(s->live_titles[i].id, id))
            return &s->live_titles[i];
    return NULL;
}

static int NewestDraftFirst(const void *a, const void *b)
{
    PicoAgentId x = *(const PicoAgentId *)a, y = *(const PicoAgentId *)b;
    /* Host agent IDs are allocated monotonically at creation, across workspaces. */
    return x > y ? -1 : x < y ? 1 : 0;
}

static void SidebarBuildLiveSnapshot(SidebarState *s)
{
    s->live_count = 0;
    memset(s->draft_counts, 0, sizeof(s->draft_counts));
    for (int w = 0; w < s->host->workspace_count; w++)
    {
        PicoWorkspace *workspace = s->host->workspaces[w];
        if (!workspace) continue;
        const char *project = workspace->project_path[0] ? workspace->project_path : workspace->path;
        int ws_index = -1;
        for (int j = 0; j < s->workspace_count; j++)
            if (!strcmp(s->workspaces[j].path, project)) { ws_index = j; break; }
        for (int i = 0; i < workspace->count && s->live_count < PICO_MAX_TOTAL_AGENTS; i++)
        {
            PicoAgent *agent = workspace->agents[i];
            if (!PicoAgent_IsUserMain(agent)) continue;
            SidebarLiveAgent *live = &s->live[s->live_count++];
            live->id = agent->id;
            snprintf(live->path, sizeof(live->path), "%s", workspace->path);
            snprintf(live->session_id, sizeof(live->session_id), "%s", agent->session_id);
            /* A saved session missing from this page is not a draft. Its
             * visibility and order belong exclusively to catalog pagination. */
            if (ws_index >= 0 && ws_index < PICO_MAX_CATALOG_WORKSPACES && !agent->session_id[0])
                s->drafts[ws_index][s->draft_counts[ws_index]++] = agent->id;
            if (agent->session_id[0] && !SidebarLiveTitle(s, workspace->path, agent->session_id))
                s->dirty = true;
        }
    }
    for (int i = 0; i < s->workspace_count && i < PICO_MAX_CATALOG_WORKSPACES; i++)
        if (s->draft_counts[i] > 1)
            qsort(s->drafts[i], (size_t)s->draft_counts[i], sizeof(s->drafts[i][0]), NewestDraftFirst);
    s->live_snapshot_valid = true;
    s->inbox_valid = false;
}

static void SelectAgent(PicoHost *host, PicoAgentId id)
{
    PicoChat_InspectClose();
    (void)pico_agent_select(host, id);
}

static void ToggleCollapsed(SidebarState *s, int index);

static void ExpandWorkspace(SidebarState *s, int index)
{
    PicoCatalogWorkspace *ws;
    if (!s || index < 0 || index >= s->workspace_count)
    {
        return;
    }
    ws = &s->workspaces[index];
    if (!ws->collapsed)
    {
        return;
    }
    ToggleCollapsed(s, index);
}

static void NewSessionInWorkspace(PicoHost *host, SidebarState *s, int index)
{
    PicoCatalogWorkspace *ws;
    PicoWorkspaceId id;
    PicoAgentCreateOptions options;
    PicoAgentId agent_id = 0;
    PicoAgentId live;
    PicoResult created;
    if (!s || index < 0 || index >= s->workspace_count)
    {
        return;
    }
    ws = &s->workspaces[index];
    ExpandWorkspace(s, index);
    live = 0;
    /* A project picker reuses an empty draft, never a working or nonempty
     * id-less conversation. Refresh even when the sidebar itself is hidden. */
    SidebarBuildLiveSnapshot(s);
    for (int i = 0; i < s->live_count; i++)
    {
        PicoAgent *candidate = PicoHost_FindAgent(host, s->live[i].id);
        if (candidate && !strcmp(s->live[i].path, ws->path) && !candidate->session_id[0] &&
            candidate->message_count == 0 && !PicoAgent_IsBusy(candidate))
        {
            live = candidate->id;
            break;
        }
    }
    if (live)
    {
        SelectAgent(host, live);
        return;
    }
    id = OpenLiveWorkspace(host, ws->path, NULL);
    if (!id)
    {
        return;
    }
    memset(&options, 0, sizeof(options));
    options.kind = PICO_AGENT_MAIN;
    options.session_start = PICO_SESSION_NEW;
    options.select = true;
    PicoChat_InspectClose();
    created = pico_main_agent_create(host, id, &options, &agent_id);
    if (created != PICO_OK)
    {
        if (created == PICO_LIMIT)
        {
            PicoOverlay_Notify(host, "Too many agents are open.");
        }
        else
        {
            PicoOverlay_Notify(host, "Could not create a session.");
        }
        return;
    }
    s->dirty = true;
}

static void OpenCatalogSession(PicoHost *host, SidebarState *s, const char *path, const char *session_id)
{
    PicoAgentId live;
    PicoWorkspaceId id;
    PicoResult created;
    live = LiveMainAgent(host, path, session_id);
    if (live)
    {
        PicoSession_LoadCancel(host);
        SelectAgent(host, live);
        return;
    }
    id = OpenLiveWorkspace(host, path, NULL);
    if (!id)
    {
        return;
    }
    PicoChat_InspectClose();
    created = PicoSession_LoadAsync(host, id, 0, session_id, false, false, false, false);
    if (created != PICO_OK)
    {
        if (created == PICO_LIMIT)
        {
            PicoOverlay_Notify(host, "Too many agents are open.");
        }
        else if (created == PICO_SESSION_INVALID)
        {
            PicoOverlay_Notify(host, "Could not open that session.");
        }
        else
        {
            PicoOverlay_Notify(host, "Could not open that session.");
        }
        return;
    }
    if (s)
    {
        s->dirty = true;
    }
}

/* Core tasks can outlive a sidebar reload. Serialize their writes as well as
 * cancelling retired saves that have not started, so an old generation cannot
 * overwrite a newer click. This mutex is never acquired by the UI thread. */
static pthread_mutex_t g_collapse_save_mu = PTHREAD_MUTEX_INITIALIZER;

static void SidebarCollapseCancel(void *arg)
{
    atomic_store(&((SidebarCollapseSave *)arg)->cancelled, true);
}

static void *SidebarCollapseRun(void *arg)
{
    SidebarCollapseSave *save = arg;
    pthread_mutex_lock(&g_collapse_save_mu);
    if (!atomic_load(&save->cancelled))
        save->saved = PicoCatalog_SetCollapsed(save->path, save->collapsed) == 0;
    pthread_mutex_unlock(&g_collapse_save_mu);
    return NULL;
}

static void SidebarCollapseCompleted(PicoHost *host, void *arg)
{
    SidebarCollapseSave *save = arg;
    SidebarState *s = PicoPlugins_HostState(host, "sidebar");
    if (!s || s->collapse_save != save)
        return;
    s->collapse_save = NULL;
    for (SidebarCollapseEdit *edit = s->collapse_edits; edit; edit = edit->next)
    {
        if (strcmp(edit->path, save->path) == 0 && !edit->pending && save->saved)
        {
            /* Only a snapshot started after this completion can release the
             * local override. Newer clicks still pending keep theirs. */
            edit->accept_scan = s->scan_serial + 1;
            s->dirty = true;
            break;
        }
    }
}

static void SidebarStartCollapseSave(SidebarState *s)
{
    if (s->collapse_save)
        return;
    for (SidebarCollapseEdit *edit = s->collapse_edits; edit; edit = edit->next)
    {
        if (!edit->pending)
            continue;
        edit->pending = false;
        SidebarCollapseSave *save = calloc(1, sizeof(*save));
        if (!save)
            return;
        snprintf(save->path, sizeof(save->path), "%s", edit->path);
        save->collapsed = edit->collapsed;
        /* One save at a time keeps rapid toggles ordered. Never fall back to
         * synchronous I/O when tasks are unavailable. */
        if (!SidebarHostCanTask(s->host) ||
            !PicoHost_StartTaskCompleted(s->host, SidebarCollapseRun, save, SidebarCollapseCancel,
                                         SidebarCollapseCompleted, free))
        {
            free(save);
            return;
        }
        s->collapse_save = save;
        return;
    }
}

static void ToggleCollapsed(SidebarState *s, int index)
{
    if (!s || index < 0 || index >= s->workspace_count)
        return;
    PicoCatalogWorkspace *ws = &s->workspaces[index];
    ws->collapsed = !ws->collapsed;
    /* Input runs after layout; rebuild before presenting the changed rows. */
    s->host->ui_relayout_requested = true;

    SidebarCollapseEdit *edit = s->collapse_edits;
    while (edit && strcmp(edit->path, ws->path) != 0)
        edit = edit->next;
    if (!edit)
    {
        edit = calloc(1, sizeof(*edit));
        if (!edit)
            return;
        snprintf(edit->path, sizeof(edit->path), "%s", ws->path);
        edit->next = s->collapse_edits;
        s->collapse_edits = edit;
    }
    edit->collapsed = ws->collapsed;
    edit->pending = true;
    edit->accept_scan = 0;
    SidebarStartCollapseSave(s);
}

static Clay_Color RowFill(bool selected, bool hovered)
{
    if (selected)
    {
        return (Clay_Color){52, 52, 62, 255};
    }
    if (hovered)
    {
        return (Clay_Color){42, 42, 50, 255};
    }
    return (Clay_Color){0, 0, 0, 0};
}

static void ResourcePath(const char *relative, char *out, size_t cap)
{
    if (!Pico_DataPath(relative, out, cap))
    {
        snprintf(out, cap, "%s", relative);
    }
}

static Texture2D LoadFolderIcon(const char *relative, Clay_Color tint)
{
    char path[4096];
    Image img;
    Texture2D tex = {0};
    ResourcePath(relative, path, sizeof(path));
    img = LoadImage(path);
    if (!img.data)
    {
        return tex;
    }
    /* Clay draws a filled rect for backgroundColor even on image elements. */
    ImageColorTint(&img, (Color){(unsigned char)tint.r, (unsigned char)tint.g,
                                 (unsigned char)tint.b, (unsigned char)tint.a});
    tex = LoadTextureFromImage(img);
    UnloadImage(img);
    if (tex.id != 0)
    {
        SetTextureFilter(tex, TEXTURE_FILTER_BILINEAR);
    }
    return tex;
}

static void EnsureFolderIcons(SidebarState *s)
{
    if (!s || s->icons_tried || !IsWindowReady())
    {
        return;
    }
    s->icons_tried = true;
    s->folder_collapsed = LoadFolderIcon("resources/folder-collapsed.png", COLOR_MUTED);
    s->folder_expanded = LoadFolderIcon("resources/folder-expanded.png", COLOR_MUTED);
    s->pen_icon = LoadFolderIcon("resources/pen.png", COLOR_TEXT);
    s->settings_icon = LoadFolderIcon("resources/settings.png", COLOR_MUTED);
    s->settings_icon_hover = LoadFolderIcon("resources/settings.png", COLOR_TEXT);
    s->worktree_icon = LoadFolderIcon("resources/worktree.png", COLOR_MUTED);
    s->archive_open = LoadFolderIcon("resources/archive_open.png", COLOR_MUTED);
    s->archive_closed = LoadFolderIcon("resources/archive_closed.png", COLOR_MUTED);
}

static void UnloadFolderIcons(SidebarState *s)
{
    if (!s)
    {
        return;
    }
    if (s->folder_collapsed.id != 0)
    {
        UnloadTexture(s->folder_collapsed);
        memset(&s->folder_collapsed, 0, sizeof(s->folder_collapsed));
    }
    if (s->folder_expanded.id != 0)
    {
        UnloadTexture(s->folder_expanded);
        memset(&s->folder_expanded, 0, sizeof(s->folder_expanded));
    }
    if (s->pen_icon.id != 0)
        UnloadTexture(s->pen_icon);
    if (s->settings_icon.id != 0)
    {
        UnloadTexture(s->settings_icon);
        memset(&s->settings_icon, 0, sizeof(s->settings_icon));
    }
    if (s->settings_icon_hover.id != 0)
    {
        UnloadTexture(s->settings_icon_hover);
        memset(&s->settings_icon_hover, 0, sizeof(s->settings_icon_hover));
    }
    if (s->worktree_icon.id != 0)
    {
        UnloadTexture(s->worktree_icon);
        memset(&s->worktree_icon, 0, sizeof(s->worktree_icon));
    }
    if (s->archive_open.id != 0)
    {
        UnloadTexture(s->archive_open);
        memset(&s->archive_open, 0, sizeof(s->archive_open));
    }
    if (s->archive_closed.id != 0)
    {
        UnloadTexture(s->archive_closed);
        memset(&s->archive_closed, 0, sizeof(s->archive_closed));
    }
}

static void RenderGlyph(const char *glyph, Clay_Color color)
{
    CLAY_TEXT(CStr(glyph), CLAY_TEXT_CONFIG({.fontId = FONT_REGULAR,
                                             .fontSize = PICO_FONT_UI,
                                             .textColor = color,
                                             .wrapMode = CLAY_TEXT_WRAP_NONE}));
}

static void RenderFolderIcon(Texture2D *tex, const char *fallback)
{
    float size = Pico_FontPx(SIDEBAR_FOLDER_ICON);
    if (tex && tex->id != 0)
    {
        CLAY_AUTO_ID({.layout = {.sizing = {.width = CLAY_SIZING_FIXED(size),
                                            .height = CLAY_SIZING_FIXED(size)}},
                      .image = {.imageData = tex}})
        {
        }
        return;
    }
    RenderGlyph(fallback, COLOR_MUTED);
}

static void RenderSettingsIcon(SidebarState *s, bool hovered)
{
    Texture2D *tex = hovered && s->settings_icon_hover.id != 0 ? &s->settings_icon_hover
                                                               : &s->settings_icon;
    float size = Pico_FontPx(SIDEBAR_FOLDER_ICON);
    if (tex->id != 0)
    {
        CLAY_AUTO_ID({.layout = {.sizing = {.width = CLAY_SIZING_FIXED(size),
                                            .height = CLAY_SIZING_FIXED(size)}},
                      .image = {.imageData = tex}})
        {
        }
        return;
    }
    RenderGlyph("*", hovered ? COLOR_TEXT : COLOR_MUTED);
}

typedef enum SidebarDotKind
{
    SIDEBAR_DOT_IDLE = 0,
    SIDEBAR_DOT_RUNNING,
    SIDEBAR_DOT_WAITING_USER,
    SIDEBAR_DOT_DONE,
    SIDEBAR_DOT_ERROR,
} SidebarDotKind;

static bool AgentOrDescendantAskOpen(PicoHost *host, PicoAgentId id)
{
    const PicoAgent *agent;
    int n, i;
    if (!host || !id)
        return false;
    agent = PicoHost_FindAgentConst(host, id);
    if (agent && PicoAgent_AskUiOpen(agent))
        return true;
    n = pico_agent_count(host);
    for (i = 0; i < n; i++)
    {
        PicoAgentInfo info;
        PicoAgentId cur;
        int depth;
        if (!pico_agent_info(host, i, &info) || info.id == id)
            continue;
        cur = info.parent_id;
        for (depth = 0; cur && depth < 8; depth++)
        {
            if (cur == id)
            {
                const PicoAgent *child = PicoHost_FindAgentConst(host, info.id);
                if (child && PicoAgent_AskUiOpen(child))
                    return true;
                break;
            }
            {
                const PicoAgent *node = PicoHost_FindAgentConst(host, cur);
                if (!node)
                    break;
                cur = node->parent_id;
            }
        }
    }
    return false;
}

static SidebarDotKind SessionDotKind(PicoHost *host, const char *ws_path, const char *session_id,
                                     PicoAgentId live_id, bool catalog_unseen)
{
    PicoAgent *agent;
    if (!live_id)
    {
        live_id = LiveMainAgent(host, ws_path, session_id);
    }
    if (!live_id)
    {
        return catalog_unseen ? SIDEBAR_DOT_DONE : SIDEBAR_DOT_IDLE;
    }
    agent = PicoHost_FindAgent(host, live_id);
    if (!agent)
    {
        return catalog_unseen ? SIDEBAR_DOT_DONE : SIDEBAR_DOT_IDLE;
    }
    if (agent->state != PICO_AGENT_ERROR && AgentOrDescendantAskOpen(host, agent->id))
        return SIDEBAR_DOT_WAITING_USER;
    switch (agent->state)
    {
    case PICO_AGENT_ERROR:
        return SIDEBAR_DOT_ERROR;
    case PICO_AGENT_TOOL_WAIT:
        if (PicoAgent_AskUiOpen(agent))
        {
            return SIDEBAR_DOT_WAITING_USER;
        }
        return SIDEBAR_DOT_RUNNING;
    case PICO_AGENT_LLM_WAIT:
    case PICO_AGENT_COMPACT_WAIT:
        return SIDEBAR_DOT_RUNNING;
    case PICO_AGENT_IDLE:
    default:
        return agent->unseen_complete ? SIDEBAR_DOT_DONE : SIDEBAR_DOT_IDLE;
    }
}

static void RenderSessionSpinner(int row_id, float slot)
{
    const int segments = 8;
    const float two_pi = 6.28318530718f;
    float theta = (float)GetTime() * two_pi * 1.5f;
    float size = slot * 0.23f;
    /* Floating segments may extend beyond the slot without changing row layout. */
    float diameter = Pico_FontPx(11);
    float radius = (diameter - size) * 0.5f;
    CLAY(CLAY_IDI("SidebarSessSpinner", row_id),
         {.layout = {.sizing = {.width = CLAY_SIZING_FIXED(slot),
                                .height = CLAY_SIZING_FIXED(slot)}}})
    {
        for (int i = 0; i < segments; i++)
        {
            float angle = theta - (float)i * two_pi / (float)segments;
            Clay_Color color = COLOR_STATUS_OFF;
            color.a *= 1.0f - 0.85f * (float)i / (float)(segments - 1);
            CLAY(CLAY_IDI("SidebarSessSpinSegment", row_id * segments + i),
                 {.floating = {.attachTo = CLAY_ATTACH_TO_PARENT,
                               .pointerCaptureMode = CLAY_POINTER_CAPTURE_MODE_PASSTHROUGH,
                               .attachPoints = {.element = CLAY_ATTACH_POINT_CENTER_CENTER,
                                                .parent = CLAY_ATTACH_POINT_CENTER_CENTER},
                               .offset = {.x = radius * cosf(angle), .y = radius * sinf(angle)}},
                  .layout = {.sizing = {.width = CLAY_SIZING_FIXED(size),
                                        .height = CLAY_SIZING_FIXED(size)}},
                  .backgroundColor = color,
                  .cornerRadius = CLAY_CORNER_RADIUS(size * 0.5f)}) {}
        }
    }
}

static void RenderSessionDot(SidebarState *s, SidebarDotKind kind, int row_id, bool worktree,
                             bool loading, bool unloaded)
{
    float gutter = Pico_FontPx(SIDEBAR_FOLDER_ICON);
    float slot = Pico_FontPx(SIDEBAR_SESSION_DOT);
    float size = Pico_FontPx(kind == SIDEBAR_DOT_IDLE ? SIDEBAR_SESSION_IDLE_DOT
                                                      : SIDEBAR_SESSION_DOT);
    Clay_Color color = COLOR_STATUS_OFF;
    switch (kind)
    {
    case SIDEBAR_DOT_ERROR:
        color = COLOR_STATUS_ERR;
        break;
    case SIDEBAR_DOT_WAITING_USER:
        color = COLOR_STATUS_RUN;
        break;
    case SIDEBAR_DOT_RUNNING:
    {
        float pulse = 0.5f + 0.5f * sinf((float)GetTime() * 6.28318530718f * 1.25f);
        color = COLOR_STATUS_ON;
        color.a = 90.0f + 165.0f * pulse;
        break;
    }
    case SIDEBAR_DOT_DONE:
        color = COLOR_STATUS_DONE;
        break;
    case SIDEBAR_DOT_IDLE:
    default:
        break;
    }
    if (unloaded)
        color.a *= 0.65f;
    CLAY_AUTO_ID({.layout = {.sizing = {.width = CLAY_SIZING_FIXED(gutter),
                                        .height = CLAY_SIZING_FIXED(slot)},
                             .childAlignment = {.x = CLAY_ALIGN_X_CENTER,
                                                .y = CLAY_ALIGN_Y_CENTER}}})
    {
        if (loading)
        {
            RenderSessionSpinner(row_id, slot);
        }
        else if (worktree && kind == SIDEBAR_DOT_IDLE && s && s->worktree_icon.id != 0)
        {
            CLAY(CLAY_IDI("SidebarSessDot", row_id),
                 {.layout = {.sizing = {.width = CLAY_SIZING_FIXED(gutter),
                                        .height = CLAY_SIZING_FIXED(gutter)}},
                  .image = {.imageData = &s->worktree_icon}}) {}
        }
        else
        {
            CLAY(CLAY_IDI("SidebarSessDot", row_id),
                 {.layout = {.sizing = {.width = CLAY_SIZING_FIXED(size),
                                        .height = CLAY_SIZING_FIXED(size)}},
                  .backgroundColor = color,
                  .cornerRadius = CLAY_CORNER_RADIUS(size * 0.5f)}) {}
        }
    }
}

static bool SessionIsSelected(PicoHost *host, const char *ws_path, const char *session_id,
                              PicoAgentId live_id)
{
    PicoAgentId selected = pico_agent_active(host);
    PicoAgentInfo info;
    const char *agent_ws;
    if (!pico_agent_find(host, selected, &info) || info.kind != PICO_AGENT_MAIN)
    {
        return false;
    }
    agent_ws = PicoAgent_WorkspacePath(PicoHost_FindAgentConst(host, selected));
    if (!agent_ws || strcmp(agent_ws, ws_path) != 0)
    {
        return false;
    }
    if (live_id)
    {
        return selected == live_id;
    }
    return session_id && session_id[0] && strcmp(info.session_id, session_id) == 0;
}

static int SessionRowId(int ws_index, int session_index)
{
    return ws_index * 20000 + session_index;
}

bool PicoSidebar_DragMoved(float press_x, float press_y, float mouse_x, float mouse_y)
{
    float dx = mouse_x - press_x;
    float dy = mouse_y - press_y;
    return dx * dx + dy * dy >= SIDEBAR_DRAG_THRESHOLD * SIDEBAR_DRAG_THRESHOLD;
}

int PicoSidebar_DragTarget(const float *midpoints, int count, int source, float mouse_y)
{
    int target = source;
    int k;
    if (!midpoints || count <= 0 || source < 0 || source >= count)
    {
        return -1;
    }
    if (mouse_y < midpoints[source])
    {
        for (k = source - 1; k >= 0 && mouse_y < midpoints[k]; k--)
        {
            target = k;
        }
    }
    else
    {
        for (k = source + 1; k < count && mouse_y > midpoints[k]; k++)
        {
            target = k;
        }
    }
    return target;
}

int PicoSidebar_DropTarget(const float *midpoints, int count, int source, float mouse_y,
                           bool pointer_over_list)
{
    return pointer_over_list
               ? PicoSidebar_DragTarget(midpoints, count, source, mouse_y)
               : -1;
}

/* Reorderable (non-stashed) workspace indices in display order. */
static int SidebarDraggableSlots(const SidebarState *s, int *slots)
{
    int n = 0;
    for (int i = 0; i < s->workspace_count && n < PICO_MAX_CATALOG_WORKSPACES; i++)
        if (!s->workspaces[i].stashed)
            slots[n++] = i;
    return n;
}

static int SidebarComputeDragTarget(SidebarState *s, float mouse_y, bool pointer_over_list)
{
    float mids[PICO_MAX_CATALOG_WORKSPACES];
    int slots[PICO_MAX_CATALOG_WORKSPACES];
    int n, source = -1;
    if (!s || s->drag_source_index < 0 || s->drag_source_index >= s->workspace_count ||
        s->workspaces[s->drag_source_index].stashed)
        return -1;
    n = SidebarDraggableSlots(s, slots);
    for (int k = 0; k < n; k++)
    {
        Clay_ElementData el = Clay_GetElementData(CLAY_IDI("SidebarWs", slots[k]));
        if (!el.found)
            return s->drag_source_index;
        mids[k] = el.boundingBox.y + el.boundingBox.height * 0.5f;
        if (slots[k] == s->drag_source_index)
            source = k;
    }
    int target = PicoSidebar_DropTarget(mids, n, source, mouse_y, pointer_over_list);
    return target < 0 ? -1 : slots[target];
}

static void SidebarReorderWorkspaces(SidebarState *s, int from, int to)
{
    PicoCatalogWorkspace moved_ws;
    SidebarWsUi moved_ui;
    bool has_ui;
    int k;
    if (!s || from < 0 || to < 0 || from >= s->workspace_count || to >= s->workspace_count || from == to)
    {
        return;
    }
    if (s->workspaces[from].stashed || s->workspaces[to].stashed)
        return;
    int slots[PICO_MAX_CATALOG_WORKSPACES];
    int n = SidebarDraggableSlots(s, slots);
    int from_slot = -1, to_slot = -1;
    for (int i = 0; i < n; i++)
    {
        if (slots[i] == from)
            from_slot = i;
        if (slots[i] == to)
            to_slot = i;
    }
    if (from_slot < 0 || to_slot < 0)
        return;
    moved_ws = s->workspaces[from];
    memset(&moved_ui, 0, sizeof(moved_ui));
    has_ui = s->ui && from < s->ui_count && to < s->ui_count;
    if (has_ui)
        moved_ui = s->ui[from];
    if (from_slot < to_slot)
    {
        for (int i = from_slot; i < to_slot; i++)
        {
            s->workspaces[slots[i]] = s->workspaces[slots[i + 1]];
            if (has_ui)
                s->ui[slots[i]] = s->ui[slots[i + 1]];
        }
    }
    else
    {
        for (int i = from_slot; i > to_slot; i--)
        {
            s->workspaces[slots[i]] = s->workspaces[slots[i - 1]];
            if (has_ui)
                s->ui[slots[i]] = s->ui[slots[i - 1]];
        }
    }
    s->workspaces[to] = moved_ws;
    if (has_ui)
        s->ui[to] = moved_ui;
    for (k = 0; k < s->workspace_count; k++)
    {
        s->workspaces[k].order = k;
    }
    s->order_persist_generation = PicoCatalog_EnqueueOrder(s->host, s->workspaces,
                                                           s->workspace_count);
    if (s->order_persist_generation == 0)
    {
        s->order_unsaved = true;
    }
}

static void RenderDropIndicator(void)
{
    CLAY_AUTO_ID({.layout = {.sizing = {.width = CLAY_SIZING_PERCENT(1),
                                        .height = CLAY_SIZING_FIXED(2)}},
                  .backgroundColor = (Clay_Color){80, 140, 255, 255},
                  .cornerRadius = CLAY_CORNER_RADIUS(1)})
    {
    }
}

static void RenderWorkspaceRow(PicoHost *host, SidebarState *s, const PicoCatalogWorkspace *ws, int index)
{
    Clay_ElementId row_id = CLAY_IDI("SidebarWs", index);
    Clay_ElementId plus_id = CLAY_IDI("SidebarPlus", index);
    Clay_ElementId edit_id = CLAY_IDI("SidebarEdit", index);
    bool edit_hovered = Clay_PointerOver(edit_id);
    bool is_dragged = s && s->is_dragging && s->drag_source_index == index;
    bool plus_hovered = !is_dragged && Clay_PointerOver(plus_id);
    bool hovered = !is_dragged && (Clay_PointerOver(row_id) || plus_hovered);
    Clay_Color fill = is_dragged ? (Clay_Color){35, 35, 42, 100} : RowFill(false, hovered);
    Clay_Color text_col = is_dragged ? (Clay_Color){120, 120, 135, 120} : COLOR_TEXT;
    float plus_width = hovered && !is_dragged
                           ? 8.0f + SidebarTextWidth("+", 1)
                           : 0.0f;
    Clay_String display = SidebarDisplayTitle(
        s, ws->name, SidebarLabelWidth(s->row_width, Pico_FontPx(SIDEBAR_FOLDER_ICON), plus_width));

    CLAY(row_id, {.layout = {.layoutDirection = CLAY_LEFT_TO_RIGHT,
                             .childAlignment = {.y = CLAY_ALIGN_Y_CENTER},
                             .padding = {SIDEBAR_ROW_PAD_X, SIDEBAR_ROW_PAD_X, 4, 4},
                             .childGap = SIDEBAR_ROW_GAP,
                             .sizing = {.width = CLAY_SIZING_PERCENT(1)}},
                  .backgroundColor = fill,
                  .cornerRadius = CLAY_CORNER_RADIUS(6)})
    {
        CLAY(edit_id, {.layout = {.sizing = {.width = CLAY_SIZING_FIXED(Pico_FontPx(SIDEBAR_FOLDER_ICON)),
                                             .height = CLAY_SIZING_FIXED(Pico_FontPx(SIDEBAR_FOLDER_ICON))}}})
        {
            RenderFolderIcon(edit_hovered ? &s->pen_icon : ws->collapsed ? &s->folder_collapsed
                                                                         : &s->folder_expanded,
                             edit_hovered ? "*" : ws->collapsed ? ">"
                                                                : "v");
        }
        CLAY_AUTO_ID({.layout = {.sizing = {.width = CLAY_SIZING_GROW(0)}}})
        {
            CLAY_TEXT(display,
                      CLAY_TEXT_CONFIG({.fontId = FONT_REGULAR,
                                        .fontSize = PICO_FONT_UI,
                                        .textColor = text_col,
                                        .wrapMode = CLAY_TEXT_WRAP_NONE}));
        }
        if (hovered && !is_dragged)
        {
            CLAY(plus_id, {.layout = {.padding = {4, 4, 0, 0}}})
            {
                RenderGlyph("+", plus_hovered ? COLOR_TEXT : COLOR_MUTED);
            }
        }
    }
    (void)host;
}

static void RenderSessionRow(PicoHost *host, SidebarState *s, const char *ws_path,
                             const char *title, const char *session_id, PicoAgentId live_id,
                             int row_id, bool catalog_unseen, bool worktree,
                             bool missing_checkout, bool inbox_row)
{
    bool selected = SessionIsSelected(host, ws_path, session_id, live_id);
    PicoAgentId row_live_id = live_id ? live_id : LiveMainAgent(host, ws_path, session_id);
    const char *target_ws = NULL;
    const char *target_id = NULL;
    bool loading = PicoSession_LoadTarget(host, &target_ws, &target_id) &&
                   ws_path && session_id && session_id[0] &&
                   strcmp(ws_path, target_ws) == 0 && strcmp(session_id, target_id) == 0;
    /* Keep load tracking on the project-list id. Inbox rows use a separate element. */
    int dot_id = inbox_row ? row_id + 10000000 : row_id;
    Clay_ElementId id = inbox_row ? CLAY_IDI("SidebarInbox", row_id) : CLAY_IDI("SidebarSess", row_id);
    bool hovered = Clay_PointerOver(id);
    Clay_String display = SidebarDisplayTitle(
        s, title && title[0] ? title : "Untitled",
        SidebarLabelWidth(s->row_width, Pico_FontPx(SIDEBAR_FOLDER_ICON), 0.0f));
    if (loading && !inbox_row)
    {
        host->session_load_row_rendered = host->session_load_row_id != row_id ||
                                          host->session_load_row_was_visible;
        host->session_load_row_id = row_id;
        host->session_load_row_known = true;
    }
    CLAY(id, {.layout = {.layoutDirection = CLAY_LEFT_TO_RIGHT,
                         .childAlignment = {.y = CLAY_ALIGN_Y_CENTER},
                         .padding = {SIDEBAR_ROW_PAD_X, SIDEBAR_ROW_PAD_X, 3, 3},
                         .childGap = SIDEBAR_ROW_GAP,
                         .sizing = {.width = CLAY_SIZING_PERCENT(1)}},
              .backgroundColor = RowFill(selected, hovered),
              .cornerRadius = CLAY_CORNER_RADIUS(6)})
    {
        RenderSessionDot(s, SessionDotKind(host, ws_path, session_id, row_live_id, catalog_unseen),
                         dot_id, worktree, loading, !row_live_id);
        CLAY_AUTO_ID({.layout = {.sizing = {.width = CLAY_SIZING_GROW(0)}}})
        {
            CLAY_TEXT(display,
                      CLAY_TEXT_CONFIG({.fontId = FONT_REGULAR,
                                        .fontSize = PICO_FONT_UI,
                                        .textColor = missing_checkout ? COLOR_STATUS_ERR
                                                                      : (selected ? COLOR_TEXT : COLOR_MUTED),
                                        .wrapMode = CLAY_TEXT_WRAP_NONE}));
        }
    }
}

static bool AgentInCatalogProject(const PicoAgent *agent, const PicoCatalogWorkspace *ws);
static PicoAgentId DraftAt(SidebarState *s, const PicoCatalogWorkspace *ws, int draft_index);
static int CountDrafts(SidebarState *s, const PicoCatalogWorkspace *ws);

typedef struct SidebarPin
{
    bool found;
    PicoAgentId live_id;
    int row_id;
    const char *title;
    const char *session_id;
    const char *checkout_path;
    bool worktree;
    bool missing_checkout;
    bool unseen_complete;
} SidebarPin;

static SidebarPin FindSelectedSidebarRow(PicoHost *host, SidebarState *s, const PicoCatalogWorkspace *ws, int ws_index)
{
    SidebarPin pin = {0};
    PicoAgentInfo info;
    int drafts;
    int j;
    if (!host || !ws)
    {
        return pin;
    }
    drafts = CountDrafts(s, ws);
    int total = drafts + ws->session_count + (ws->has_more_sessions ? SIDEBAR_SESSION_PAGE : 0);
    int shown = ShownForIndex(s, ws_index, total);
    for (j = 0; j < drafts && j < shown; j++)
    {
        PicoAgentId draft = DraftAt(s, ws, j);
        if (!pico_agent_find(host, draft, &info))
        {
            continue;
        }
        PicoAgent *selected_agent = PicoHost_FindAgent(host, draft);
        if (!SessionIsSelected(host, PicoAgent_WorkspacePath(selected_agent), info.session_id, draft))
        {
            continue;
        }
        pin.found = true;
        pin.live_id = draft;
        pin.row_id = SessionRowId(ws_index, j);
        pin.title = "New session";
        pin.session_id = "";
        PicoAgent *live_agent = PicoHost_FindAgent(host, draft);
        pin.checkout_path = PicoAgent_WorkspacePath(live_agent);
        pin.worktree = live_agent && live_agent->workspace && live_agent->workspace->worktree;
        pin.unseen_complete = false;
        return pin;
    }
    for (j = 0; j < ws->session_count && drafts + j < shown; j++)
    {
        if (!SessionIsSelected(host, ws->sessions[j].checkout_path, ws->sessions[j].id, 0))
        {
            continue;
        }
        pin.found = true;
        pin.live_id = 0;
        pin.row_id = SessionRowId(ws_index, drafts + j);
        pin.title = ws->sessions[j].title;
        pin.session_id = ws->sessions[j].id;
        pin.checkout_path = ws->sessions[j].checkout_path;
        pin.worktree = ws->sessions[j].worktree;
        pin.missing_checkout = ws->sessions[j].missing_checkout;
        pin.unseen_complete = ws->sessions[j].unseen_complete;
        return pin;
    }
    return pin;
}

static void RenderPinnedSelected(PicoHost *host, SidebarState *s, const PicoCatalogWorkspace *ws, int ws_index)
{
    SidebarPin pin = FindSelectedSidebarRow(host, s, ws, ws_index);
    if (!pin.found)
    {
        return;
    }
    RenderSessionRow(host, s, pin.checkout_path, pin.title, pin.session_id, pin.live_id,
                     pin.row_id, pin.unseen_complete, pin.worktree, pin.missing_checkout, false);
}

static bool OpenPinnedSelected(PicoHost *host, SidebarState *s, const PicoCatalogWorkspace *ws,
                               int ws_index)
{
    SidebarPin pin = FindSelectedSidebarRow(host, s, ws, ws_index);
    if (!pin.found || !Clay_PointerOver(CLAY_IDI("SidebarSess", pin.row_id)))
    {
        return false;
    }
    if (pin.live_id)
    {
        SelectAgent(host, pin.live_id);
        return true;
    }
    OpenCatalogSession(host, s, pin.checkout_path, pin.session_id);
    return true;
}

static void RenderMoreLessLabel(Clay_ElementId id, Clay_String label)
{
    bool hovered = Clay_PointerOver(id);
    CLAY(id, {.layout = {.padding = {6, 6, 2, 2}},
              .backgroundColor = RowFill(false, hovered),
              .cornerRadius = CLAY_CORNER_RADIUS(6)})
    {
        CLAY_TEXT(label, CLAY_TEXT_CONFIG({.fontId = FONT_REGULAR,
                                           .fontSize = PICO_FONT_CAPTION,
                                           .textColor = COLOR_MUTED,
                                           .wrapMode = CLAY_TEXT_WRAP_NONE}));
    }
}

static void RenderMoreLessRow(int ws_index, int shown, int total)
{
    bool more = shown < total;
    bool less = shown > SIDEBAR_SESSION_PAGE;
    if (!more && !less)
    {
        return;
    }
    CLAY(CLAY_IDI("SidebarMoreLess", ws_index),
         {.layout = {.layoutDirection = CLAY_LEFT_TO_RIGHT,
                     .childAlignment = {.y = CLAY_ALIGN_Y_CENTER},
                     .padding = {SIDEBAR_ROW_PAD_X - 6, SIDEBAR_ROW_PAD_X - 6, 2, 2},
                     .childGap = SIDEBAR_ROW_GAP,
                     .sizing = {.width = CLAY_SIZING_PERCENT(1)}}})
    {
        if (more)
        {
            float gutter = Pico_FontPx(SIDEBAR_FOLDER_ICON);
            CLAY_AUTO_ID({.layout = {.sizing = {.width = CLAY_SIZING_FIXED(gutter)}}}) {}
            RenderMoreLessLabel(CLAY_IDI("SidebarMore", ws_index), CLAY_STRING("More"));
        }
        CLAY_AUTO_ID({.layout = {.sizing = {.width = CLAY_SIZING_GROW(0)}}}) {}
        if (less)
        {
            RenderMoreLessLabel(CLAY_IDI("SidebarLess", ws_index), CLAY_STRING("Less"));
        }
    }
}

static void RenderDrafts(PicoHost *host, SidebarState *s, const PicoCatalogWorkspace *ws,
                         int ws_index, int max_drafts)
{
    for (int draft = 0; draft < max_drafts; draft++)
    {
        PicoAgentId id = DraftAt(s, ws, draft);
        PicoAgent *agent = PicoHost_FindAgent(host, id);
        if (!agent) continue;
        RenderSessionRow(host, s, PicoAgent_WorkspacePath(agent), "New session",
                         agent->session_id, id, SessionRowId(ws_index, draft), false,
                         agent->workspace && agent->workspace->worktree, false, false);
    }
}

typedef int (*InboxVisitFn)(PicoHost *host, SidebarState *s, const InboxVisit *row, int index, void *ctx);

static bool InboxPendingValue(PicoHost *host, const char *ws_path, const char *session_id,
                              bool catalog_inbox, PicoAgentId live_id)
{
    PicoAgent *agent;
    if (!live_id)
        live_id = LiveMainAgent(host, ws_path, session_id);
    agent = live_id ? PicoHost_FindAgent(host, live_id) : NULL;
    if (agent)
        return agent->inbox_pending;
    return catalog_inbox && !PicoCatalog_InboxAcknowledged(host, ws_path, session_id);
}

static InboxSection InboxSectionFor(PicoHost *host, const InboxVisit *row)
{
    SidebarDotKind dot = SessionDotKind(host, row->path, row->session_id, row->live_id, row->unseen);
    if (dot == SIDEBAR_DOT_WAITING_USER || dot == SIDEBAR_DOT_ERROR) return INBOX_NEEDS_INPUT;
    return dot == SIDEBAR_DOT_RUNNING ? INBOX_WORKING : INBOX_COMPLETED;
}

static bool InboxVisible(PicoHost *host, const char *ws_path, const char *session_id,
                         bool catalog_unseen, bool catalog_inbox, PicoAgentId live_id)
{
    SidebarDotKind dot = SessionDotKind(host, ws_path, session_id, live_id, catalog_unseen);
    if (dot == SIDEBAR_DOT_RUNNING || dot == SIDEBAR_DOT_WAITING_USER || dot == SIDEBAR_DOT_ERROR)
        return true;
    if (!live_id && PicoCatalog_InboxAcknowledged(host, ws_path, session_id)) return false;
    return dot == SIDEBAR_DOT_DONE || InboxPendingValue(host, ws_path, session_id, catalog_inbox, live_id);
}

static int EnumerateInbox(PicoHost *host, SidebarState *s, InboxVisitFn fn, void *ctx)
{
    int index = 0;
    int i, j;
    if (!host || !s)
        return 0;
    for (i = 0; i < s->workspace_count; i++)
    {
        const PicoCatalogWorkspace *ws = &s->workspaces[i];
        int drafts = CountDrafts(s, ws);
        /* Inbox attention is independent of project-list pagination. Include
         * off-page live sessions, but never turn them into project-list drafts. */
        for (j = 0; j < s->live_count; j++)
        {
            PicoAgentId id = s->live[j].id;
            PicoAgent *agent = PicoHost_FindAgent(host, id);
            if (!AgentInCatalogProject(agent, ws)) continue;
            const char *path = PicoAgent_WorkspacePath(agent);
            if (CatalogHasSession(ws, path, agent->session_id) ||
                !InboxVisible(host, path, agent->session_id, agent->unseen_complete, agent->inbox_pending, id))
                continue;
            if (fn)
            {
                InboxVisit row = {0};
                const PicoSessionRef *title = SidebarLiveTitle(s, path, agent->session_id);
                row.path = path;
                const char *label = "New session";
                if (agent->session_id[0])
                    label = !title ? "Loading session…" : title->title[0] ? title->title : "Untitled";
                snprintf(row.title, sizeof(row.title), "%s", label);
                snprintf(row.session_id, sizeof(row.session_id), "%s", agent->session_id);
                row.live_id = id;
                row.ws_index = i;
                row.slot = -1;
                for (int k = 0; k < drafts; k++)
                    if (DraftAt(s, ws, k) == id) { row.slot = k; break; }
                row.unseen = agent->unseen_complete;
                row.activity = s->wall_clock_offset + (agent->inbox_activity > 0 ? agent->inbox_activity : agent->last_activity);
                row.worktree = agent->workspace && agent->workspace->worktree;
                if (fn(host, s, &row, index, ctx))
                    return index + 1;
            }
            index++;
        }
        for (j = 0; j < ws->inbox_count; j++)
        {
            const PicoCatalogSession *session = &ws->inbox_sessions[j];
            PicoAgentId live = LiveMainAgent(host, session->checkout_path, session->id);
            InboxVisit row;
            if (!InboxVisible(host, session->checkout_path, session->id, session->unseen_complete,
                              session->inbox_pending, live))
                continue;
            if (fn)
            {
                memset(&row, 0, sizeof(row));
                row.path = session->checkout_path;
                snprintf(row.title, sizeof(row.title), "%s", session->title[0] ? session->title : "Untitled");
                snprintf(row.session_id, sizeof(row.session_id), "%s", session->id);
                row.live_id = live;
                row.ws_index = i;
                row.slot = drafts + j;
                row.unseen = session->unseen_complete;
                PicoAgent *agent = live ? PicoHost_FindAgent(host, live) : NULL;
                row.activity = (double)session->mtime + session->mtime_nsec / 1e9;
                if (agent && agent->inbox_activity > 0)
                    row.activity = fmax(row.activity, s->wall_clock_offset + agent->inbox_activity);
                row.worktree = session->worktree;
                row.missing = session->missing_checkout;
                if (fn(host, s, &row, index, ctx))
                    return index + 1;
            }
            index++;
        }
    }
    return index;
}

static int CacheInboxRow(PicoHost *host, SidebarState *s, const InboxVisit *row, int index, void *ctx)
{
    (void)ctx;
    if (s->inbox_count == s->inbox_capacity)
    {
        int capacity = s->inbox_capacity ? s->inbox_capacity * 2 : 16;
        InboxVisit *rows = realloc(s->inbox_rows, (size_t)capacity * sizeof(*rows));
        if (!rows) return 1;
        s->inbox_rows = rows;
        s->inbox_capacity = capacity;
    }
    InboxVisit *cached = &s->inbox_rows[s->inbox_count++];
    *cached = *row;
    cached->index = index;
    cached->section = InboxSectionFor(host, row);
    return 0;
}

static int InboxNewestFirst(const void *a, const void *b)
{
    const InboxVisit *x = a, *y = b;
    if (x->activity != y->activity) return x->activity > y->activity ? -1 : 1;
    int path = strcmp(x->path, y->path);
    if (path) return path;
    int session = strcmp(x->session_id, y->session_id);
    if (session) return session;
    return x->live_id > y->live_id ? -1 : x->live_id < y->live_id;
}

/* Identity follows the session through reordering, catalog refresh and group changes. */
static Clay_ElementId InboxSessionId(const InboxVisit *row)
{
    uint32_t session = row->session_id[0] ? Clay_GetElementId(CStr(row->session_id)).id : (uint32_t)row->live_id;
    return CLAY_IDI("SidebarInboxSession", Clay_GetElementIdWithIndex(CStr(row->path), session).id);
}

static int VisitInbox(PicoHost *host, SidebarState *s, InboxVisitFn fn, void *ctx)
{
    if (!s->inbox_valid)
    {
        s->inbox_count = 0;
        EnumerateInbox(host, s, CacheInboxRow, NULL);
        if (s->inbox_count > 1)
            qsort(s->inbox_rows, (size_t)s->inbox_count, sizeof(*s->inbox_rows), InboxNewestFirst);
        s->inbox_valid = true;
    }
    for (int i = 0; fn && i < s->inbox_count; i++)
        if (fn(host, s, &s->inbox_rows[i], s->inbox_rows[i].index, ctx)) return i + 1;
    return s->inbox_count;
}

static int RenderInboxRow(PicoHost *host, SidebarState *s, const InboxVisit *row, int index, void *ctx)
{
    if (row->section != *(InboxSection *)ctx) return 0;
    Clay_ElementId id = InboxSessionId(row);
    bool selected = SessionIsSelected(host, row->path, row->session_id, row->live_id);
    float width = SidebarLabelWidth(s->row_width, Pico_FontPx(SIDEBAR_FOLDER_ICON), 0);
    Clay_String title = SidebarDisplayTitle(s, row->title, width);
    Clay_String project = SidebarDisplayTitle(s, s->workspaces[row->ws_index].name, width);
    float height = Pico_FontPx(PICO_FONT_UI) + Pico_FontPx(PICO_FONT_CAPTION) + 14;
    const char *loading_path = NULL, *loading_id = NULL;
    bool loading = PicoSession_LoadTarget(host, &loading_path, &loading_id) &&
                   !strcmp(loading_path, row->path) && !strcmp(loading_id, row->session_id);
    CLAY(id, {.layout = {.sizing = {.width = CLAY_SIZING_PERCENT(1), .height = CLAY_SIZING_FIXED(height)}},
              .transition = {.handler = Clay_EaseOut, .duration = 0.18f,
                             .properties = CLAY_TRANSITION_PROPERTY_Y}})
    {
        CLAY(CLAY_IDI("SidebarInbox", index), {.layout = {.layoutDirection = CLAY_LEFT_TO_RIGHT,
                             .childAlignment = {.y = CLAY_ALIGN_Y_CENTER},
                             .padding = {SIDEBAR_ROW_PAD_X, SIDEBAR_ROW_PAD_X, 5, 5},
                             .childGap = SIDEBAR_ROW_GAP,
                             .sizing = {.width = CLAY_SIZING_PERCENT(1), .height = CLAY_SIZING_FIXED(height)}},
                  .backgroundColor = RowFill(selected, Clay_PointerOver(id)),
                  .cornerRadius = CLAY_CORNER_RADIUS(6)})
        {
            RenderSessionDot(s, SessionDotKind(host, row->path, row->session_id, row->live_id, row->unseen),
                             (int)(id.id & 0x7fffffff), row->worktree, loading, !row->live_id);
            CLAY_AUTO_ID({.layout = {.layoutDirection = CLAY_TOP_TO_BOTTOM, .childGap = 4,
                                     .sizing = {.width = CLAY_SIZING_GROW(0)}}})
            {
                CLAY_TEXT(title, CLAY_TEXT_CONFIG({.fontId = FONT_REGULAR, .fontSize = PICO_FONT_UI,
                    .textColor = row->missing ? COLOR_STATUS_ERR : COLOR_TEXT, .wrapMode = CLAY_TEXT_WRAP_NONE}));
                CLAY_TEXT(project, CLAY_TEXT_CONFIG({.fontId = FONT_REGULAR, .fontSize = PICO_FONT_CAPTION,
                    .textColor = COLOR_MUTED, .wrapMode = CLAY_TEXT_WRAP_NONE}));
            }
        }
    }
    return 0;
}

typedef struct SidebarInboxAckTargets
{
    PicoCatalogInboxTarget *items;
    int count;
    int capacity;
    bool failed;
} SidebarInboxAckTargets;

static int AckInboxIdle(PicoHost *host, SidebarState *s, const InboxVisit *row, int index, void *ctx)
{
    (void)index;
    if (InboxSectionFor(host, row) != INBOX_COMPLETED) return 0;
    if (row->live_id)
    {
        PicoAgent *agent = PicoHost_FindAgent(host, row->live_id);
        if (agent)
        {
            PicoSession_SetUnseenComplete(host, agent, false);
            PicoSession_SetInboxPending(host, agent, false);
        }
        s->dirty = true;
    }
    else
    {
        SidebarInboxAckTargets *targets = ctx;
        if (targets->count == targets->capacity)
        {
            int capacity = targets->capacity ? targets->capacity * 2 : 8;
            PicoCatalogInboxTarget *items = realloc(targets->items, (size_t)capacity * sizeof(*items));
            if (!items) { targets->failed = true; return 0; }
            targets->items = items;
            targets->capacity = capacity;
        }
        PicoCatalogInboxTarget *target = &targets->items[targets->count++];
        snprintf(target->checkout_path, sizeof(target->checkout_path), "%s", row->path);
        snprintf(target->session_id, sizeof(target->session_id), "%s", row->session_id);
    }
    return 0;
}

static void AcknowledgeInbox(PicoHost *host, SidebarState *s)
{
    SidebarInboxAckTargets targets = {0};
    VisitInbox(host, s, AckInboxIdle, &targets);
    if (targets.count && !PicoCatalog_EnqueueInboxAcknowledgements(host, targets.items, targets.count))
        targets.failed = true;
    free(targets.items);
    if (targets.failed)
        PicoOverlay_Notify(host, "Could not queue some inbox acknowledgements. Those rows remain in the inbox.");
    /* The checkmark runs after layout; show accepted acknowledgements now. */
    host->ui_relayout_requested = true;
}

static void RevealInboxSession(PicoHost *host, SidebarState *s, const InboxVisit *row)
{
    const PicoCatalogWorkspace *ws;
    int total, shown, needed;
    if (!s || row->ws_index < 0 || row->ws_index >= s->workspace_count)
        return;
    ws = &s->workspaces[row->ws_index];
    if (ws->stashed)
        s->stash_expanded = true;
    ExpandWorkspace(s, row->ws_index);
    /* Selecting a saved Inbox item does not bypass project pagination, even
     * when a larger previously fetched page still contains its catalog row. */
    if (row->session_id[0] || row->slot < 0) return;
    total = CountDrafts(s, ws) + ws->session_count +
            (ws->has_more_sessions ? SIDEBAR_SESSION_PAGE : 0);
    needed = row->slot + 1;
    shown = ShownForIndex(s, row->ws_index, total);
    if (shown < needed && row->ws_index < s->ui_count)
    {
        s->ui[row->ws_index].shown = ClampShown(needed, total);
        host->ui_relayout_requested = true;
    }
}

static int ClickInboxRow(PicoHost *host, SidebarState *s, const InboxVisit *row, int index, void *ctx)
{
    (void)index;
    if (!Clay_PointerOver(InboxSessionId(row)))
        return 0;
    if (row->live_id)
        SelectAgent(host, row->live_id);
    else
        OpenCatalogSession(host, s, row->path, row->session_id);
    RevealInboxSession(host, s, row);
    /* VisitInbox returns the row count, not whether a click was consumed. */
    if (ctx)
        *(bool *)ctx = true;
    return 1;
}

static void RenderInbox(PicoHost *host, SidebarState *s)
{
    int counts[INBOX_SECTION_COUNT] = {0};
    VisitInbox(host, s, NULL, NULL);
    uint64_t signature = 0;
    for (int i = 0; i < s->inbox_count; i++)
    {
        counts[s->inbox_rows[i].section]++;
        signature = (signature ^ InboxSessionId(&s->inbox_rows[i]).id) * UINT64_C(1099511628211);
        signature = (signature ^ s->inbox_rows[i].section) * UINT64_C(1099511628211);
    }
    if (signature != s->inbox_layout_signature)
    {
        s->inbox_layout_signature = signature;
        s->inbox_transition_until = GetTime() + 0.18;
    }
    const char *titles[] = {"Needs input", "Working", "Completed"};
    for (InboxSection section = 0; section < INBOX_SECTION_COUNT; section++)
    {
        /* Clay borrows even unshortened labels until the layout is consumed. */
        char *label = SidebarNextLabel(s);
        if (!label) continue;
        snprintf(label, SIDEBAR_LABEL_CAPACITY, "%s · %d", titles[section], counts[section]);
        Clay_String title = SidebarDisplayTitle(s, label, s->row_width - 48);
        CLAY(CLAY_IDI("SidebarInboxSection", section),
             {.layout = {.layoutDirection = CLAY_LEFT_TO_RIGHT,
                         .childAlignment = {.y = CLAY_ALIGN_Y_CENTER},
                         .padding = {SIDEBAR_ROW_PAD_X, SIDEBAR_ROW_PAD_X, 10, 6},
                         .sizing = {.width = CLAY_SIZING_PERCENT(1)}}})
        {
            CLAY_AUTO_ID({.layout = {.sizing = {.width = CLAY_SIZING_GROW(0)}}})
            {
                CLAY_TEXT(title, CLAY_TEXT_CONFIG({.fontId = FONT_REGULAR, .fontSize = PICO_FONT_UI,
                                                    .textColor = COLOR_MUTED, .wrapMode = CLAY_TEXT_WRAP_NONE}));
            }
            if (section == INBOX_COMPLETED)
            {
                CLAY(CLAY_ID("SidebarInboxDone"), {.layout = {.padding = {4, 4, 0, 0}}})
                {
                    RenderGlyph("✓", counts[section] && Clay_PointerOver(CLAY_ID("SidebarInboxDone"))
                                        ? COLOR_TEXT : COLOR_MUTED);
                }
            }
        }
        VisitInbox(host, s, RenderInboxRow, &section);
    }
}

static bool PointerOverInbox(PicoHost *host, SidebarState *s)
{
    if (!host->sidebar_inbox_mode) return false;
    if (Clay_PointerOver(CLAY_ID("SidebarInboxDone"))) return true;
    VisitInbox(host, s, NULL, NULL);
    for (int i = 0; i < s->inbox_count; i++)
        if (Clay_PointerOver(InboxSessionId(&s->inbox_rows[i]))) return true;
    return false;
}

bool PicoSidebar_RenderProjectPicker(PicoHost *host)
{
    SidebarState *s = PicoPlugins_HostState(host, "sidebar");
    if (!s || !host->sidebar_inbox_mode || PicoHost_SelectedAgent(host)) return false;
    EnsureFolderIcons(s);
    CLAY(CLAY_ID("InboxProjectPicker"),
         {.layout = {.layoutDirection = CLAY_TOP_TO_BOTTOM, .childGap = 12,
                     .sizing = {.width = CLAY_SIZING_GROW(0, 480)}}})
    {
        CLAY_TEXT(CLAY_STRING("New session"), CLAY_TEXT_CONFIG({.fontId = FONT_BOLD,
                    .fontSize = PICO_FONT_TITLE, .textColor = COLOR_TEXT}));
        CLAY_TEXT(CLAY_STRING("Choose a project to get started"), CLAY_TEXT_CONFIG({.fontId = FONT_REGULAR,
                    .fontSize = PICO_FONT_UI, .textColor = COLOR_MUTED}));
        for (int i = 0; i < s->workspace_count; i++)
        {
            const PicoCatalogWorkspace *ws = &s->workspaces[i];
            if (ws->stashed) continue;
            Clay_ElementId id = CLAY_IDI("InboxProject", i);
            CLAY(id, {.layout = {.layoutDirection = CLAY_LEFT_TO_RIGHT, .childGap = 12,
                                 .childAlignment = {.y = CLAY_ALIGN_Y_CENTER}, .padding = CLAY_PADDING_ALL(12),
                                 .sizing = {.width = CLAY_SIZING_GROW(0)}},
                      .backgroundColor = RowFill(false, Clay_PointerOver(id)), .cornerRadius = CLAY_CORNER_RADIUS(8)})
            {
                RenderFolderIcon(&s->folder_collapsed, ">");
                CLAY_AUTO_ID({.layout = {.sizing = {.width = CLAY_SIZING_GROW(0)}}})
                {
                    CLAY_TEXT(CStr(ws->name), CLAY_TEXT_CONFIG({.fontId = FONT_REGULAR,
                                .fontSize = PICO_FONT_UI, .textColor = COLOR_TEXT}));
                }
                RenderGlyph("+", COLOR_MUTED);
            }
        }
        CLAY(CLAY_ID("InboxAddProject"), {.layout = {.padding = CLAY_PADDING_ALL(12),
                                                      .sizing = {.width = CLAY_SIZING_GROW(0)}},
              .backgroundColor = RowFill(false, Clay_PointerOver(CLAY_ID("InboxAddProject"))),
              .cornerRadius = CLAY_CORNER_RADIUS(8)})
        {
            RenderGlyph("+ Add project", COLOR_MUTED);
        }
    }
    return true;
}

static void PicoSidebar_Render(PicoHost *host, void *state)
{
    SidebarState *s = state ? (SidebarState *)state : (SidebarState *)PicoPlugins_HostState(host, "sidebar");
    int i;
    int j;
    int drafts;
    int total;
    int shown;
    bool add_hover;
    if (!s)
    {
        return;
    }
    SidebarBuildLiveSnapshot(s);
    EnsureFolderIcons(s);
    s->row_width = PicoHost_SidebarContentWidth();
    s->label_chunk = NULL;
    s->label_used = 0;

    add_hover = Clay_PointerOver(Clay_GetElementId(CLAY_STRING("SidebarAddWs")));
    float add_width = 8.0f + SidebarTextWidth("+", 1);
    Clay_String header = SidebarDisplayTitle(s, host->sidebar_inbox_mode ? "Inbox" : "Projects",
                                             s->row_width - 2.0f * SIDEBAR_ROW_PAD_X - SIDEBAR_ROW_GAP - add_width);
    CLAY(CLAY_ID("SidebarRoot"),
         {.layout = {.layoutDirection = CLAY_TOP_TO_BOTTOM,
                     .childGap = 6,
                     .sizing = {.width = CLAY_SIZING_PERCENT(1), .height = CLAY_SIZING_PERCENT(1)}}})
    {
        CLAY(CLAY_ID("SidebarProjectsHeader"),
             {.layout = {.layoutDirection = CLAY_LEFT_TO_RIGHT,
                         .childAlignment = {.y = CLAY_ALIGN_Y_CENTER},
                         .padding = {SIDEBAR_ROW_PAD_X, SIDEBAR_ROW_PAD_X, 4, 4},
                         .childGap = SIDEBAR_ROW_GAP,
                         .sizing = {.width = CLAY_SIZING_PERCENT(1)}}})
        {
            CLAY(CLAY_ID("SidebarMode"), {.layout = {.layoutDirection = CLAY_LEFT_TO_RIGHT, .childGap = 6,
                                                      .sizing = {.width = CLAY_SIZING_GROW(0)}}})
            {
                CLAY_TEXT(header,
                          CLAY_TEXT_CONFIG({.fontId = FONT_REGULAR,
                                            .fontSize = PICO_FONT_UI,
                                            .textColor = Clay_PointerOver(CLAY_ID("SidebarMode")) ? COLOR_TEXT : COLOR_MUTED,
                                            .wrapMode = CLAY_TEXT_WRAP_NONE}));
                RenderGlyph("⌄", COLOR_MUTED);
            }
            CLAY(CLAY_ID("SidebarAddWs"), {.layout = {.padding = {4, 4, 0, 0}}})
            {
                RenderGlyph("+", add_hover ? COLOR_TEXT : COLOR_MUTED);
            }
        }

        CLAY(CLAY_ID("SidebarScroll"),
             {.layout = {.layoutDirection = CLAY_TOP_TO_BOTTOM,
                         .childGap = 2,
                         .sizing = {.width = CLAY_SIZING_PERCENT(1), .height = CLAY_SIZING_GROW(0)}},
              .clip = {.vertical = true, .horizontal = true, .childOffset = Clay_GetScrollOffset()}})
        {
            if (host->sidebar_inbox_mode) RenderInbox(host, s);
            for (int section = 0; !host->sidebar_inbox_mode && section < 2; section++)
            {
                if (section == 1)
                {
                    bool any = false;
                    for (int k = 0; k < s->workspace_count; k++)
                        if (s->workspaces[k].stashed)
                            any = true;
                    if (!any)
                        break;
                    Clay_ElementId stash_id = CLAY_ID("SidebarStashedHeader");
                    CLAY(stash_id, {.layout = {.layoutDirection = CLAY_LEFT_TO_RIGHT,
                                               .childAlignment = {.y = CLAY_ALIGN_Y_CENTER},
                                               .childGap = SIDEBAR_ROW_GAP,
                                               .padding = {SIDEBAR_ROW_PAD_X, 4, 8, 4},
                                               .sizing = {.width = CLAY_SIZING_PERCENT(1)}}})
                    {
                        RenderFolderIcon(s->stash_expanded ? &s->archive_open : &s->archive_closed,
                                         s->stash_expanded ? "v" : ">");
                        CLAY_TEXT(CLAY_STRING("Stashed"),
                                  CLAY_TEXT_CONFIG({.fontId = FONT_REGULAR, .fontSize = PICO_FONT_UI, .textColor = Clay_PointerOver(stash_id) ? COLOR_TEXT : COLOR_MUTED}));
                    }
                    if (!s->stash_expanded)
                        break;
                }
                for (i = 0; i < s->workspace_count; i++)
                {
                    if (s->workspaces[i].stashed != (section == 1))
                        continue;
                    if (s->is_dragging && s->drag_target_index != s->drag_source_index)
                    {
                        if (s->drag_target_index < s->drag_source_index && s->drag_target_index == i)
                        {
                            RenderDropIndicator();
                        }
                    }
                    const PicoCatalogWorkspace *ws = &s->workspaces[i];
                    RenderWorkspaceRow(host, s, ws, i);
                    if (ws->collapsed)
                    {
                        RenderPinnedSelected(host, s, ws, i);
                    }
                    else
                    {
                        drafts = CountDrafts(s, ws);
                        total = drafts + ws->session_count + (ws->has_more_sessions ? SIDEBAR_SESSION_PAGE : 0);
                        shown = ShownForIndex(s, i, total);
                        RenderDrafts(host, s, ws, i, shown < drafts ? shown : drafts);
                        for (j = 0; j < shown - drafts && j < ws->session_count; j++)
                        {
                            RenderSessionRow(host, s, ws->sessions[j].checkout_path, ws->sessions[j].title,
                                             ws->sessions[j].id, 0, SessionRowId(i, drafts + j),
                                             ws->sessions[j].unseen_complete, ws->sessions[j].worktree,
                                             ws->sessions[j].missing_checkout, false);
                        }
                        RenderMoreLessRow(i, shown, total);
                    }
                    if (s->is_dragging && s->drag_target_index != s->drag_source_index)
                    {
                        if (s->drag_target_index > s->drag_source_index && s->drag_target_index == i)
                        {
                            RenderDropIndicator();
                        }
                    }
                }
            }
        }


        if (s->is_dragging && s->drag_source_index >= 0 && s->drag_source_index < s->workspace_count)
        {
            Vector2 mouse = GetMousePosition();
            const PicoCatalogWorkspace *drag_ws = &s->workspaces[s->drag_source_index];
            Clay_String preview = SidebarDisplayTitle(
                s, drag_ws->name,
                SidebarLabelWidth(180.0f, Pico_FontPx(SIDEBAR_FOLDER_ICON), 0.0f));
            CLAY(CLAY_ID("SidebarDragPreview"),
                 {.floating = {.attachTo = CLAY_ATTACH_TO_ROOT,
                               .offset = {.x = mouse.x + 12.0f, .y = mouse.y - 12.0f},
                               .zIndex = 50,
                               .pointerCaptureMode = CLAY_POINTER_CAPTURE_MODE_PASSTHROUGH},
                  .layout = {.layoutDirection = CLAY_LEFT_TO_RIGHT,
                             .childAlignment = {.y = CLAY_ALIGN_Y_CENTER},
                             .padding = {SIDEBAR_ROW_PAD_X, SIDEBAR_ROW_PAD_X, 4, 4},
                             .childGap = SIDEBAR_ROW_GAP,
                             .sizing = {.width = CLAY_SIZING_FIXED(180)}},
                  .backgroundColor = (Clay_Color){42, 42, 50, 230},
                  .cornerRadius = CLAY_CORNER_RADIUS(6),
                  .border = {.width = {1, 1, 1, 1}, .color = (Clay_Color){80, 140, 255, 200}}})
            {
                RenderFolderIcon(drag_ws->collapsed ? &s->folder_collapsed : &s->folder_expanded,
                                 drag_ws->collapsed ? ">" : "v");
                CLAY_AUTO_ID({.layout = {.sizing = {.width = CLAY_SIZING_GROW(0)}}})
                {
                    CLAY_TEXT(preview,
                              CLAY_TEXT_CONFIG({.fontId = FONT_REGULAR,
                                                .fontSize = PICO_FONT_UI,
                                                .textColor = COLOR_TEXT,
                                                .wrapMode = CLAY_TEXT_WRAP_NONE}));
                }
            }
        }

        {
            bool settings_hover = Clay_PointerOver(Clay_GetElementId(CLAY_STRING("SidebarSettings")));
            float icon = Pico_FontPx(SIDEBAR_FOLDER_ICON);
            float row_h = icon + 12.0f;
            CLAY(CLAY_ID("SidebarSettingsRow"),
                 {.layout = {.layoutDirection = CLAY_LEFT_TO_RIGHT,
                             .childAlignment = {.x = CLAY_ALIGN_X_LEFT, .y = CLAY_ALIGN_Y_CENTER},
                             .sizing = {.width = CLAY_SIZING_PERCENT(1), .height = CLAY_SIZING_FIXED(row_h)}}})
            {
                CLAY(CLAY_ID("SidebarSettings"),
                     {.layout = {.layoutDirection = CLAY_LEFT_TO_RIGHT,
                                 .childAlignment = {.x = CLAY_ALIGN_X_CENTER, .y = CLAY_ALIGN_Y_CENTER},
                                 .padding = {6, 6, 4, 4},
                                 .sizing = {.width = CLAY_SIZING_FIXED(icon + 12.0f),
                                            .height = CLAY_SIZING_FIXED(icon + 8.0f)}}})
                {
                    RenderSettingsIcon(s, settings_hover);
                }
            }
        }
    }
}

static int EditWorkspaceIndex(const SidebarState *s)
{
    if (!s || !s->edit_open)
        return -1;
    for (int i = 0; i < s->workspace_count; i++)
        if (!strcmp(s->workspaces[i].path, s->edit_path))
            return i;
    return -1;
}

static bool CloseWorkspaceEdit(SidebarState *s)
{
    if (!s || !s->edit_open)
        return true;
    if (!pico_ui_modal_pop(s->host, "sidebar-workspace-edit"))
        return false; /* buried under another modal; retry once it is top again */
    s->edit_open = false;
    s->delete_confirm = false;
    PicoTextField_Unbind(&s->edit_field);
    if (s->host)
        s->host->ui_relayout_requested = true;
    return true;
}

static bool CloseSessionMenu(SidebarState *s)
{
    if (!s || !s->session_menu_open)
        return true;
    if (!pico_ui_modal_pop(s->host, "sidebar-session-menu"))
        return false;
    s->session_menu_open = false;
    s->session_delete_confirm = false;
    s->session_menu_latched = false;
    if (s->host)
        s->host->ui_relayout_requested = true;
    return true;
}

static void OpenWorkspaceEdit(SidebarState *s, int index, int latch_button)
{
    if (!s || index < 0 || index >= s->workspace_count)
        return;
    if (s->session_menu_open && !CloseSessionMenu(s))
        return;
    if (!s->edit_open && !pico_ui_modal_push(s->host, "sidebar-workspace-edit"))
        return;
    s->edit_open = true;
    s->delete_confirm = false;
    s->edit_pointer_latched = true; /* opening press must not close the new popup */
    s->edit_latch_button = latch_button;
    snprintf(s->edit_path, sizeof(s->edit_path), "%s", s->workspaces[index].path);
    snprintf(s->edit_name, sizeof(s->edit_name), "%s", s->workspaces[index].name);
    PicoTextField_Bind(&s->edit_field, s->edit_name, sizeof(s->edit_name));
    snprintf(s->delete_label, sizeof(s->delete_label),
             "Delete saved sessions from %s and ALL its checkouts and worktrees? Project folders remain untouched.",
             s->workspaces[index].name);
    if (s->host)
        s->host->ui_relayout_requested = true;
}

static void RenderEditAction(Clay_ElementId id, const char *label, bool danger)
{
    CLAY(id, {.layout = {.padding = {7, 7, 5, 5},
                         .sizing = {.width = CLAY_SIZING_PERCENT(1)}},
              .backgroundColor = Clay_PointerOver(id) ? (Clay_Color){52, 52, 62, 255} : (Clay_Color){0, 0, 0, 0},
              .cornerRadius = CLAY_CORNER_RADIUS(4)})
    {
        CLAY_TEXT(CStr(label), CLAY_TEXT_CONFIG({.fontId = FONT_REGULAR,
                                                 .fontSize = PICO_FONT_UI,
                                                 .textColor = danger ? (Clay_Color){235, 105, 105, 255} : COLOR_TEXT,
                                                 .wrapMode = CLAY_TEXT_WRAP_WORDS}));
    }
}

static void RenderWorkspaceEdit(PicoHost *host, void *state)
{
    SidebarState *s = state ? state : PicoPlugins_HostState(host, "sidebar");
    int i = EditWorkspaceIndex(s);
    Clay_ElementData row;
    float x, y, height, expected;
    bool short_view;
    if (i < 0 || !pico_ui_modal_is_top(host, "sidebar-workspace-edit"))
        return;
    row = Clay_GetElementData(CLAY_IDI("SidebarWs", i));
    if (!row.found)
        return;
    x = row.boundingBox.x;
    y = row.boundingBox.y + row.boundingBox.height + 3;
    if (x + 244 > GetScreenWidth())
        x = GetScreenWidth() - 244;
    if (x < 4)
        x = 4;
    /* The confirmation is taller and fonts can be scaled by the user. */
    expected = (s->delete_confirm ? 300.0f : 200.0f) * Pico_FontPx(PICO_FONT_UI) / PICO_FONT_UI;
    height = expected;
    short_view = height > GetScreenHeight() - 8;
    if (short_view)
        height = GetScreenHeight() - 8;
    if (y + height > GetScreenHeight())
        y = row.boundingBox.y - height - 3;
    if (y < 4)
        y = 4;
    CLAY(CLAY_ID("SidebarEditPopup"),
         {.floating = {.attachTo = CLAY_ATTACH_TO_ROOT, .offset = {.x = x, .y = y}, .zIndex = 55},
          .layout = {.layoutDirection = CLAY_TOP_TO_BOTTOM, .childGap = 5, .padding = {10, 10, 10, 10}, .sizing = {.width = CLAY_SIZING_FIXED(240), .height = short_view ? CLAY_SIZING_FIXED(height) : CLAY_SIZING_FIT(0)}},
          .clip = {.vertical = short_view, .childOffset = Clay_GetScrollOffset()},
          .backgroundColor = COLOR_CONTENT_BG,
          .cornerRadius = CLAY_CORNER_RADIUS(7),
          .border = {.width = {1, 1, 1, 1}, .color = COLOR_MUTED}})
    {
        CLAY_TEXT(CLAY_STRING("Project name "), CLAY_TEXT_CONFIG({.fontId = FONT_REGULAR,
                                                                  .fontSize = PICO_FONT_UI,
                                                                  .textColor = COLOR_MUTED}));
        CLAY(CLAY_ID("SidebarEditName"),
             {.layout = {.layoutDirection = CLAY_LEFT_TO_RIGHT,
                         .padding = {7, 7, 6, 6},
                         .sizing = {.width = CLAY_SIZING_PERCENT(1)}},
              .clip = {.horizontal = true, .childOffset = {.x = -s->edit_field.scroll_x}},
              .backgroundColor = (Clay_Color){35, 35, 42, 255},
              .cornerRadius = CLAY_CORNER_RADIUS(4)})
        {
            Clay_TextElementConfig field_config = PicoTextField_Config(FONT_REGULAR);
            if (!s->edit_name[0])
                field_config.textColor = COLOR_MUTED;
            CLAY_TEXT(CStr(s->edit_name[0] ? s->edit_name : " "),
                      CLAY_TEXT_CONFIG(field_config));
        }
        RenderEditAction(CLAY_ID("SidebarEditSave"), "Save name", false);
        RenderEditAction(CLAY_ID("SidebarEditStash"), s->workspaces[i].stashed ? "Restore to Projects" : "Stash project", false);
        if (!s->delete_confirm)
            RenderEditAction(CLAY_ID("SidebarEditDelete"), "Delete workspace...", true);
        else
        {
            CLAY_TEXT(CStr(s->delete_label),
                      CLAY_TEXT_CONFIG({.fontId = FONT_REGULAR, .fontSize = PICO_FONT_CAPTION, .textColor = COLOR_TEXT, .wrapMode = CLAY_TEXT_WRAP_WORDS}));
            RenderEditAction(CLAY_ID("SidebarEditConfirm"), "Delete all saved history", true);
            RenderEditAction(CLAY_ID("SidebarEditCancelDelete"), "Cancel", false);
        }
    }
}

typedef struct SidebarProjectSave {
    char path[4096], name[PICO_CATALOG_NAME_MAX];
    bool rename, stashed, saved;
} SidebarProjectSave;

static void SidebarProjectSaveRun(void *arg)
{
    SidebarProjectSave *save = arg;
    save->saved = (save->rename ? PicoCatalog_SetProjectName(save->path, save->name)
                               : PicoCatalog_SetProjectStashed(save->path, save->stashed)) == 0;
}

static void SidebarProjectSaveCompleted(PicoHost *host, void *arg)
{
    SidebarProjectSave *save = arg;
    PicoProjectPreference *pref = host->project_preferences;
    while (pref && strcmp(pref->path, save->path)) pref = pref->next;
    if (!pref) return;
    if (save->saved)
    {
        if (save->rename) snprintf(pref->confirmed_name, sizeof(pref->confirmed_name), "%s", save->name);
        else pref->confirmed_stashed = save->stashed;
    }
    pref->pending--;
    if (!pref->pending)
    {
        snprintf(pref->name, sizeof(pref->name), "%s", pref->confirmed_name);
        pref->stashed = pref->confirmed_stashed;
        pref->completed_epoch = ++host->project_preference_epoch;
    }
    SidebarState *s = PicoPlugins_HostState(host, "sidebar");
    if (s)
    {
        for (int i = 0; i < s->workspace_count; i++)
            if (!strcmp(s->workspaces[i].path, pref->path))
            {
                snprintf(s->workspaces[i].name, sizeof(s->workspaces[i].name), "%s", pref->name);
                s->workspaces[i].stashed = pref->stashed;
            }
        s->dirty = true;
    }
    if (!save->saved) pico_status_warn(host, "Could not save the project preference. Unsaved changes were restored.");
}

static bool SidebarSaveProject(SidebarState *s, int index, bool rename)
{
    PicoCatalogWorkspace *ws = &s->workspaces[index];
    if (rename)
    {
        size_t n = strlen(s->edit_name);
        if (!n || n >= PICO_CATALOG_NAME_MAX) return false;
        for (size_t i = 0; i < n; i++) if ((unsigned char)s->edit_name[i] < 32) return false;
    }
    PicoProjectPreference *pref = s->host->project_preferences;
    while (pref && strcmp(pref->path, ws->path)) pref = pref->next;
    if (!pref)
    {
        pref = calloc(1, sizeof(*pref));
        if (!pref) return false;
        snprintf(pref->path, sizeof(pref->path), "%s", ws->path);
        snprintf(pref->name, sizeof(pref->name), "%s", ws->name);
        snprintf(pref->confirmed_name, sizeof(pref->confirmed_name), "%s", ws->name);
        pref->stashed = pref->confirmed_stashed = ws->stashed;
        pref->next = s->host->project_preferences;
        s->host->project_preferences = pref;
    }
    SidebarProjectSave *save = calloc(1, sizeof(*save));
    if (!save) return false;
    snprintf(save->path, sizeof(save->path), "%s", ws->path);
    snprintf(save->name, sizeof(save->name), "%s", s->edit_name);
    save->rename = rename;
    save->stashed = !ws->stashed;
    if (!PicoHost_EnqueueUiPersist(s->host, save, SidebarProjectSaveRun, SidebarProjectSaveCompleted, free))
    { free(save); return false; }
    pref->pending++;
    if (rename) snprintf(pref->name, sizeof(pref->name), "%s", save->name);
    else pref->stashed = save->stashed;
    snprintf(ws->name, sizeof(ws->name), "%s", pref->name);
    ws->stashed = pref->stashed;
    s->host->ui_relayout_requested = true;
    return true;
}

static void WorkspaceEditInput(PicoHost *host, SidebarState *s)
{
    int i = EditWorkspaceIndex(s);
    if (i < 0)
    {
        CloseWorkspaceEdit(s);
        return;
    }
    if (!pico_ui_modal_is_top(host, "sidebar-workspace-edit"))
        return;
    host->hovered_text = Clay_PointerOver(CLAY_ID("SidebarEditName"));
    if (IsKeyPressed(KEY_ESCAPE))
    {
        CloseWorkspaceEdit(s);
        return;
    }
    if (!s->delete_confirm)
    {
        if (PicoTextField_HandleKeys(&s->edit_field))
            pico_host_request_redraw(host);
    }
    else
    {
        while (GetCharPressed() != 0)
        {
        } /* the queue is global; discard typing while confirming */
    }
    {
        Clay_ElementData name_box = Clay_GetElementData(CLAY_ID("SidebarEditName"));
        if (name_box.found)
            PicoTextField_KeepCaretVisible(&s->edit_field, name_box.boundingBox.width - 14.0f,
                                           FONT_REGULAR);
    }
    if (s->edit_pointer_latched)
    {
        if (IsMouseButtonDown(s->edit_latch_button))
            return;
        s->edit_pointer_latched = false;
    }
    PicoTextField_HandlePointer(&s->edit_field, CLAY_ID("SidebarEditName"), 7.0f, FONT_REGULAR);
    if (!IsMouseButtonPressed(MOUSE_BUTTON_LEFT) && !IsMouseButtonPressed(MOUSE_BUTTON_RIGHT))
        return;
    s->edit_pointer_latched = true;
    s->edit_latch_button = IsMouseButtonPressed(MOUSE_BUTTON_RIGHT) ? MOUSE_BUTTON_RIGHT : MOUSE_BUTTON_LEFT;
    if (Clay_PointerOver(CLAY_ID("SidebarEditName")))
        return; /* the field consumed the press */
    if (Clay_PointerOver(CLAY_ID("SidebarEditSave")) && !s->delete_confirm)
    {
        if (!SidebarSaveProject(s, i, true))
            PicoOverlay_Notify(host, "Enter a name to rename the project.");
        else
        {
            s->dirty = true;
            CloseWorkspaceEdit(s);
        }
    }
    else if (Clay_PointerOver(CLAY_ID("SidebarEditStash")) && !s->delete_confirm)
    {
        if (!SidebarSaveProject(s, i, false))
            PicoOverlay_Notify(host, "Could not update the project.");
        else
        {
            s->dirty = true;
            CloseWorkspaceEdit(s);
        }
    }
    else if (Clay_PointerOver(CLAY_ID("SidebarEditDelete")) && !s->delete_confirm)
    {
        s->delete_confirm = true;
        host->ui_relayout_requested = true;
    }
    else if (Clay_PointerOver(CLAY_ID("SidebarEditCancelDelete")))
        s->delete_confirm = false;
    else if (Clay_PointerOver(CLAY_ID("SidebarEditConfirm")) && s->delete_confirm)
    {
        bool live = false;
        for (int k = 0; k < pico_agent_count(host); k++)
        {
            PicoAgentInfo info;
            if (pico_agent_info(host, k, &info) &&
                AgentInCatalogProject(PicoHost_FindAgent(host, info.id), &s->workspaces[i]))
                live = true;
        }
        if (live)
            PicoOverlay_Notify(host, "Close all agents in this project before deleting its history.");
        else if (PicoCatalog_DeleteProject(host, s->edit_path) != 0)
            PicoOverlay_Notify(host, "Could not delete all saved project history.");
        else
        {
            s->dirty = true;
            CloseWorkspaceEdit(s);
        }
    }
    else if (!Clay_PointerOver(CLAY_ID("SidebarEditPopup")))
        CloseWorkspaceEdit(s);
}

static void SidebarDrawEditOverlay(PicoHost *host, const PicoHookEvent *event, void *state)
{
    SidebarState *s = state ? (SidebarState *)state : (SidebarState *)PicoPlugins_HostState(host, "sidebar");
    (void)event;
    if (!s || !s->edit_open || !pico_ui_modal_is_top(host, "sidebar-workspace-edit"))
        return;
    PicoTextField_Draw(&s->edit_field, CLAY_ID("SidebarEditName"), 7.0f, 6.0f, FONT_REGULAR);
    pico_host_request_redraw_after(host, PicoTextField_NextBlink(&s->edit_field));
}

static void RenderFolderModal(PicoHost *host, void *state)
{
    SidebarState *s = state ? (SidebarState *)state : (SidebarState *)PicoPlugins_HostState(host, "sidebar");
    float sw;
    float sh;
    float card_w;
    if (!s || !s->want_folder)
    {
        return;
    }
    sw = (float)GetScreenWidth();
    sh = (float)GetScreenHeight();
    card_w = sw < 520.0f ? sw - 48.0f : 420.0f;
    if (card_w < 260.0f)
    {
        card_w = 260.0f;
    }
    CLAY(CLAY_ID("SidebarFolderDim"),
         {.floating = {.attachTo = CLAY_ATTACH_TO_ROOT,
                       .zIndex = 43,
                       .attachPoints = {.element = CLAY_ATTACH_POINT_LEFT_TOP,
                                        .parent = CLAY_ATTACH_POINT_LEFT_TOP}},
          .layout = {.layoutDirection = CLAY_TOP_TO_BOTTOM,
                     .childAlignment = {.x = CLAY_ALIGN_X_CENTER, .y = CLAY_ALIGN_Y_CENTER},
                     .sizing = {.width = CLAY_SIZING_FIXED(sw), .height = CLAY_SIZING_FIXED(sh)}},
          .backgroundColor = {0, 0, 0, 140}})
    {
        CLAY(CLAY_ID("SidebarFolderCard"),
             {.layout = {.layoutDirection = CLAY_TOP_TO_BOTTOM,
                         .padding = {20, 20, 16, 16},
                         .childGap = 8,
                         .sizing = {.width = CLAY_SIZING_FIXED(card_w)}},
              .backgroundColor = COLOR_CONTENT_BG,
              .cornerRadius = CLAY_CORNER_RADIUS(8)})
        {
            CLAY_TEXT(CLAY_STRING("Select a workspace folder"),
                      CLAY_TEXT_CONFIG({.fontId = FONT_BOLD, .fontSize = PICO_FONT_TITLE, .textColor = COLOR_TEXT}));
            CLAY_TEXT(CLAY_STRING("To continue choose a workspace folder in the opened file dialog."),
                      CLAY_TEXT_CONFIG({.fontId = FONT_REGULAR,
                                        .fontSize = PICO_FONT_UI,
                                        .textColor = COLOR_TEXT,
                                        .wrapMode = CLAY_TEXT_WRAP_WORDS}));
        }
    }
    s->folder_painted = true;
}

static bool AgentInCatalogProject(const PicoAgent *agent, const PicoCatalogWorkspace *ws)
{
    if (!agent || !agent->workspace || !ws)
        return false;
    const char *project = agent->workspace->project_path[0]
                              ? agent->workspace->project_path
                              : agent->workspace->path;
    return strcmp(project, ws->path) == 0;
}

static int CountDrafts(SidebarState *s, const PicoCatalogWorkspace *ws)
{
    int index = (int)(ws - s->workspaces);
    return index >= 0 && index < s->workspace_count && index < PICO_MAX_CATALOG_WORKSPACES
               ? s->draft_counts[index] : 0;
}

static PicoAgentId DraftAt(SidebarState *s, const PicoCatalogWorkspace *ws, int draft_index)
{
    int index = (int)(ws - s->workspaces);
    return draft_index >= 0 && draft_index < CountDrafts(s, ws)
               ? s->drafts[index][draft_index] : 0;
}

static bool SidebarPointerOverClickable(PicoHost *host, SidebarState *s)
{
    int i;
    int j;
    int drafts;
    int total;
    int shown;
    if (Clay_PointerOver(CLAY_ID("SidebarMode")) ||
        Clay_PointerOver(CLAY_ID("InboxAddProject")) || PointerOverInbox(host, s) ||
        Clay_PointerOver(CLAY_ID("SidebarStashedHeader")) ||
        Clay_PointerOver(Clay_GetElementId(CLAY_STRING("SidebarAddWs"))) ||
        Clay_PointerOver(Clay_GetElementId(CLAY_STRING("SidebarSettings"))))
    {
        return true;
    }
    for (i = 0; i < s->workspace_count; i++)
        if (Clay_PointerOver(CLAY_IDI("InboxProject", i))) return true;
    if (host->sidebar_inbox_mode) return false;
    for (i = 0; i < s->workspace_count; i++)
    {
        const PicoCatalogWorkspace *ws = &s->workspaces[i];
        if (Clay_PointerOver(CLAY_IDI("SidebarEdit", i)) ||
            Clay_PointerOver(CLAY_IDI("SidebarPlus", i)) || Clay_PointerOver(CLAY_IDI("SidebarWs", i)))
        {
            return true;
        }
        if (ws->collapsed)
        {
            SidebarPin pin = FindSelectedSidebarRow(host, s, ws, i);
            if (pin.found && Clay_PointerOver(CLAY_IDI("SidebarSess", pin.row_id)))
            {
                return true;
            }
            continue;
        }
        drafts = CountDrafts(s, ws);
        total = drafts + ws->session_count + (ws->has_more_sessions ? SIDEBAR_SESSION_PAGE : 0);
        shown = ShownForIndex(s, i, total);
        if (Clay_PointerOver(CLAY_IDI("SidebarMore", i)) || Clay_PointerOver(CLAY_IDI("SidebarLess", i)))
        {
            return true;
        }
        for (j = 0; j < drafts && j < shown; j++)
        {
            if (Clay_PointerOver(CLAY_IDI("SidebarSess", SessionRowId(i, j))))
            {
                return true;
            }
        }
        for (j = 0; j < ws->session_count && drafts + j < shown; j++)
        {
            if (Clay_PointerOver(CLAY_IDI("SidebarSess", SessionRowId(i, drafts + j))))
            {
                return true;
            }
        }
    }
    return false;
}

typedef struct SidebarSessionHit
{
    bool found;
    bool has_id;
    char path[4096];
    char id[40];
    int row_id;
    Clay_ElementId element;
} SidebarSessionHit;

static bool FillSessionHit(SidebarSessionHit *hit, const char *path, const char *id, int row_id)
{
    if (!hit)
        return false;
    memset(hit, 0, sizeof(*hit));
    hit->found = true;
    hit->row_id = row_id;
    hit->element = CLAY_IDI("SidebarSess", row_id);
    hit->has_id = id && id[0];
    if (path)
        snprintf(hit->path, sizeof(hit->path), "%s", path);
    if (hit->has_id)
        snprintf(hit->id, sizeof(hit->id), "%s", id);
    return true;
}

/* pointer_only returns the row under the cursor. Otherwise it returns the visible
 * row for want_path + want_id, which is how the open menu stays anchored. */
static bool VisitVisibleSessions(PicoHost *host, SidebarState *s, bool pointer_only,
                                 const char *want_path, const char *want_id, SidebarSessionHit *out)
{
    int i;
    int j;
    int drafts;
    int total;
    int shown;
    if (!host || !s)
        return false;
    if (host->sidebar_inbox_mode)
    {
        VisitInbox(host, s, NULL, NULL);
        for (i = 0; i < s->inbox_count; i++)
        {
            const InboxVisit *row = &s->inbox_rows[i];
            Clay_ElementId id = InboxSessionId(row);
            if (pointer_only ? !Clay_PointerOver(id) :
                (!want_id || strcmp(row->session_id, want_id) ||
                 (want_path && strcmp(row->path, want_path)) || !Clay_GetElementData(id).found)) continue;
            FillSessionHit(out, row->path, row->session_id, row->index);
            out->element = id;
            return true;
        }
        return false;
    }
    for (i = 0; i < s->workspace_count; i++)
    {
        const PicoCatalogWorkspace *ws = &s->workspaces[i];
        if (ws->collapsed)
        {
            SidebarPin pin = FindSelectedSidebarRow(host, s, ws, i);
            char live_id[40] = {0};
            const char *path = pin.checkout_path;
            const char *sid = pin.session_id;
            PicoAgent *agent;
            if (!pin.found)
                continue;
            if (pin.live_id)
            {
                agent = PicoHost_FindAgent(host, pin.live_id);
                if (agent && agent->session_id[0])
                {
                    snprintf(live_id, sizeof(live_id), "%s", agent->session_id);
                    sid = live_id;
                }
                if (agent)
                    path = PicoAgent_WorkspacePath(agent);
            }
            if (pointer_only)
            {
                if (!Clay_PointerOver(CLAY_IDI("SidebarSess", pin.row_id)))
                    continue;
                return FillSessionHit(out, path, sid, pin.row_id);
            }
            if (!want_id || !want_id[0] || !sid || strcmp(sid, want_id) ||
                (want_path && path && strcmp(path, want_path)) ||
                !Clay_GetElementData(CLAY_IDI("SidebarSess", pin.row_id)).found)
                continue;
            return FillSessionHit(out, path, sid, pin.row_id);
        }
        drafts = CountDrafts(s, ws);
        total = drafts + ws->session_count + (ws->has_more_sessions ? SIDEBAR_SESSION_PAGE : 0);
        shown = ShownForIndex(s, i, total);
        for (j = 0; j < drafts && j < shown; j++)
        {
            PicoAgentId draft = DraftAt(s, ws, j);
            PicoAgent *agent = PicoHost_FindAgent(host, draft);
            const char *path = agent ? PicoAgent_WorkspacePath(agent) : NULL;
            const char *sid = agent ? agent->session_id : "";
            int row_id = SessionRowId(i, j);
            if (pointer_only)
            {
                if (!Clay_PointerOver(CLAY_IDI("SidebarSess", row_id)))
                    continue;
                return FillSessionHit(out, path, sid, row_id);
            }
            if (!want_id || !want_id[0] || !sid || strcmp(sid, want_id) ||
                (want_path && path && strcmp(path, want_path)) ||
                !Clay_GetElementData(CLAY_IDI("SidebarSess", row_id)).found)
                continue;
            return FillSessionHit(out, path, sid, row_id);
        }
        for (j = 0; j < ws->session_count && drafts + j < shown; j++)
        {
            const PicoCatalogSession *session = &ws->sessions[j];
            int row_id = SessionRowId(i, drafts + j);
            if (pointer_only)
            {
                if (!Clay_PointerOver(CLAY_IDI("SidebarSess", row_id)))
                    continue;
                return FillSessionHit(out, session->checkout_path, session->id, row_id);
            }
            if (!want_id || strcmp(session->id, want_id) ||
                (want_path && strcmp(session->checkout_path, want_path)) ||
                !Clay_GetElementData(CLAY_IDI("SidebarSess", row_id)).found)
                continue;
            return FillSessionHit(out, session->checkout_path, session->id, row_id);
        }
    }
    return false;
}

static int WorkspaceUnderPointer(const SidebarState *s)
{
    int i;
    if (!s || s->host->sidebar_inbox_mode)
        return -1;
    for (i = 0; i < s->workspace_count; i++)
    {
        if (Clay_PointerOver(CLAY_IDI("SidebarWs", i)) ||
            Clay_PointerOver(CLAY_IDI("SidebarEdit", i)) ||
            Clay_PointerOver(CLAY_IDI("SidebarPlus", i)))
            return i;
    }
    return -1;
}

static void OpenSessionMenu(SidebarState *s, const char *path, const char *id, int latch_button)
{
    if (!s || !path || !id || !id[0])
        return;
    if (s->edit_open && !CloseWorkspaceEdit(s))
        return;
    if (!s->session_menu_open && !pico_ui_modal_push(s->host, "sidebar-session-menu"))
        return;
    s->session_menu_open = true;
    s->session_delete_confirm = false;
    s->session_menu_latched = true;
    s->session_menu_latch_button = latch_button;
    snprintf(s->session_menu_path, sizeof(s->session_menu_path), "%s", path);
    snprintf(s->session_menu_id, sizeof(s->session_menu_id), "%s", id);
    if (s->host)
        s->host->ui_relayout_requested = true;
}

static bool SidebarApplyRightClick(PicoHost *host, SidebarState *s)
{
    int ws;
    SidebarSessionHit hit;
    if (!host || !s)
        return false;
    ws = WorkspaceUnderPointer(s);
    if (ws >= 0)
    {
        OpenWorkspaceEdit(s, ws, MOUSE_BUTTON_RIGHT);
        return true;
    }
    memset(&hit, 0, sizeof(hit));
    if (!VisitVisibleSessions(host, s, true, NULL, NULL, &hit))
        return false;
    if (!hit.has_id)
    {
        if (s->session_menu_open)
            CloseSessionMenu(s);
        else if (s->edit_open)
            CloseWorkspaceEdit(s);
        if (s->host)
            s->host->ui_relayout_requested = true;
        return true;
    }
    OpenSessionMenu(s, hit.path, hit.id, MOUSE_BUTTON_RIGHT);
    return true;
}

static void RenderSessionMenu(PicoHost *host, void *state)
{
    SidebarState *s = state ? state : PicoPlugins_HostState(host, "sidebar");
    SidebarSessionHit hit;
    Clay_ElementData row;
    float x;
    float y;
    float height;
    float expected;
    bool short_view;
    if (!s || !s->session_menu_open || !pico_ui_modal_is_top(host, "sidebar-session-menu"))
        return;
    memset(&hit, 0, sizeof(hit));
    if (!VisitVisibleSessions(host, s, false, s->session_menu_path, s->session_menu_id, &hit))
        return;
    row = Clay_GetElementData(hit.element);
    if (!row.found)
        return;
    x = row.boundingBox.x;
    y = row.boundingBox.y + row.boundingBox.height + 3;
    if (x + 244 > GetScreenWidth())
        x = GetScreenWidth() - 244;
    if (x < 4)
        x = 4;
    expected = (s->session_delete_confirm ? 280.0f : 180.0f) * Pico_FontPx(PICO_FONT_UI) / PICO_FONT_UI;
    height = expected;
    short_view = height > GetScreenHeight() - 8;
    if (short_view)
        height = GetScreenHeight() - 8;
    if (y + height > GetScreenHeight())
        y = row.boundingBox.y - height - 3;
    if (y < 4)
        y = 4;
    CLAY(CLAY_ID("SidebarSessionPopup"),
         {.floating = {.attachTo = CLAY_ATTACH_TO_ROOT, .offset = {.x = x, .y = y}, .zIndex = 55},
          .layout = {.layoutDirection = CLAY_TOP_TO_BOTTOM,
                     .childGap = 5,
                     .padding = {10, 10, 10, 10},
                     .sizing = {.width = CLAY_SIZING_FIXED(240),
                                .height = short_view ? CLAY_SIZING_FIXED(height) : CLAY_SIZING_FIT(0)}},
          .clip = {.vertical = short_view, .childOffset = Clay_GetScrollOffset()},
          .backgroundColor = COLOR_CONTENT_BG,
          .cornerRadius = CLAY_CORNER_RADIUS(7),
          .border = {.width = {1, 1, 1, 1}, .color = COLOR_MUTED}})
    {
        if (!s->session_delete_confirm)
        {
            RenderEditAction(CLAY_ID("SidebarSessionCopy"), "Copy session id", false);
            RenderEditAction(CLAY_ID("SidebarSessionDelete"), "Delete session...", true);
        }
        else
        {
            CLAY_TEXT(CLAY_STRING("Delete this session and its subagent sessions? The project folder stays."),
                      CLAY_TEXT_CONFIG({.fontId = FONT_REGULAR,
                                        .fontSize = PICO_FONT_CAPTION,
                                        .textColor = COLOR_TEXT,
                                        .wrapMode = CLAY_TEXT_WRAP_WORDS}));
            RenderEditAction(CLAY_ID("SidebarSessionConfirm"), "Delete session", true);
            RenderEditAction(CLAY_ID("SidebarSessionCancel"), "Cancel", false);
        }
    }
}

static void SessionMenuInput(PicoHost *host, SidebarState *s)
{
    SidebarSessionHit hit;
    bool over_action;
    if (!s || !s->session_menu_open)
        return;
    if (!pico_ui_modal_is_top(host, "sidebar-session-menu"))
        return;
    memset(&hit, 0, sizeof(hit));
    if (!VisitVisibleSessions(host, s, false, s->session_menu_path, s->session_menu_id, &hit))
    {
        CloseSessionMenu(s);
        return;
    }
    over_action = Clay_PointerOver(CLAY_ID("SidebarSessionCopy")) ||
                  Clay_PointerOver(CLAY_ID("SidebarSessionDelete")) ||
                  Clay_PointerOver(CLAY_ID("SidebarSessionConfirm")) ||
                  Clay_PointerOver(CLAY_ID("SidebarSessionCancel"));
    if (over_action)
        host->hovered_clickable = true;
    if (IsKeyPressed(KEY_ESCAPE))
    {
        CloseSessionMenu(s);
        host->ui_relayout_requested = true;
        return;
    }
    if (s->session_menu_latched)
    {
        if (IsMouseButtonDown(s->session_menu_latch_button))
            return;
        s->session_menu_latched = false;
    }
    if (!IsMouseButtonPressed(MOUSE_BUTTON_LEFT) && !IsMouseButtonPressed(MOUSE_BUTTON_RIGHT))
        return;
    s->session_menu_latched = true;
    s->session_menu_latch_button = IsMouseButtonPressed(MOUSE_BUTTON_RIGHT) ? MOUSE_BUTTON_RIGHT
                                                                            : MOUSE_BUTTON_LEFT;
    if (Clay_PointerOver(CLAY_ID("SidebarSessionCopy")) && !s->session_delete_confirm)
    {
        SetClipboardText(s->session_menu_id);
        PicoOverlay_Notify(host, "Copied session id.");
        CloseSessionMenu(s);
        host->ui_relayout_requested = true;
    }
    else if (Clay_PointerOver(CLAY_ID("SidebarSessionDelete")) && !s->session_delete_confirm)
    {
        s->session_delete_confirm = true;
        host->ui_relayout_requested = true;
    }
    else if (Clay_PointerOver(CLAY_ID("SidebarSessionCancel")) && s->session_delete_confirm)
    {
        s->session_delete_confirm = false;
        host->ui_relayout_requested = true;
    }
    else if (Clay_PointerOver(CLAY_ID("SidebarSessionConfirm")) && s->session_delete_confirm)
    {
        PicoResult deleted = PicoCatalog_DeleteSession(host, s->session_menu_path, s->session_menu_id);
        if (deleted == PICO_BUSY)
            PicoOverlay_Notify(host, "Wait until the session is idle before deleting it.");
        else if (deleted != PICO_OK)
            PicoOverlay_Notify(host, "Could not delete that session.");
        else
        {
            s->dirty = true;
            CloseSessionMenu(s);
            host->ui_relayout_requested = true;
        }
    }
    else if (!Clay_PointerOver(CLAY_ID("SidebarSessionPopup")))
    {
        CloseSessionMenu(s);
        host->ui_relayout_requested = true;
    }
}

typedef struct SidebarModeSave { bool inbox, saved; } SidebarModeSave;

static void SidebarModeSaveRun(void *state)
{
    SidebarModeSave *save = state;
    save->saved = PicoSettings_WriteSidebarInboxMode(save->inbox);
}

static void SidebarModeSaveDone(PicoHost *host, void *state)
{
    if (!((SidebarModeSave *)state)->saved)
        PicoOverlay_Notify(host, "Could not save the sidebar mode. It will only apply until restart.");
}

static void SidebarToggleMode(PicoHost *host)
{
    host->sidebar_inbox_mode = !host->sidebar_inbox_mode;
    Clay_ScrollContainerData scroll = Clay_GetScrollContainerData(CLAY_ID("SidebarScroll"));
    if (scroll.found) *scroll.scrollPosition = (Clay_Vector2){0};
    SidebarModeSave *save = calloc(1, sizeof(*save));
    if (save) save->inbox = host->sidebar_inbox_mode;
    if (!save || !PicoHost_EnqueueUiPersist(host, save, SidebarModeSaveRun, SidebarModeSaveDone, free))
    {
        free(save);
        PicoOverlay_Notify(host, "Could not queue the sidebar mode preference.");
    }
    host->ui_relayout_requested = true;
}

static void SidebarAfterLayout(PicoHost *host, const PicoHookEvent *event, void *state)
{
    SidebarState *s = state ? (SidebarState *)state : (SidebarState *)PicoPlugins_HostState(host, "sidebar");
    int i;
    int j;
    int drafts;
    int total;
    int shown;
    (void)event;
    if (!s)
    {
        return;
    }
    if (s->edit_open)
    {
        if (pico_ui_modal_is_top(host, "sidebar-workspace-edit") &&
            IsMouseButtonPressed(MOUSE_BUTTON_RIGHT) && SidebarApplyRightClick(host, s))
            return;
        WorkspaceEditInput(host, s);
        return;
    }
    if (s->session_menu_open)
    {
        if (pico_ui_modal_is_top(host, "sidebar-session-menu") &&
            IsMouseButtonPressed(MOUSE_BUTTON_RIGHT) && SidebarApplyRightClick(host, s))
            return;
        SessionMenuInput(host, s);
        return;
    }
    if (s->want_folder || PicoUi_ModalOpen(host) || IsKeyPressed(KEY_ESCAPE))
    {
        s->drag_press_pending = false;
        s->is_dragging = false;
        s->drag_source_index = -1;
        s->drag_target_index = -1;
        host->ui_drag_active = false;
        return;
    }

    if (s->is_dragging)
    {
        host->hovered_drag = true;
    }
    else if (SidebarPointerOverClickable(host, s))
    {
        host->hovered_clickable = true;
    }

    if (s->drag_press_pending)
    {
        if (IsMouseButtonDown(MOUSE_BUTTON_LEFT))
        {
            Vector2 mouse = GetMousePosition();
            if (!s->is_dragging &&
                PicoSidebar_DragMoved(s->drag_press_pos.x, s->drag_press_pos.y,
                                      mouse.x, mouse.y))
            {
                s->is_dragging = true;
            }
            if (s->is_dragging)
            {
                host->hovered_drag = true;
                s->drag_target_index = SidebarComputeDragTarget(
                    s, mouse.y,
                    Clay_PointerOver(Clay_GetElementId(CLAY_STRING("SidebarScroll"))));
                return;
            }
        }
        else
        {
            if (s->is_dragging)
            {
                Vector2 mouse = GetMousePosition();
                s->drag_target_index = SidebarComputeDragTarget(
                    s, mouse.y,
                    Clay_PointerOver(Clay_GetElementId(CLAY_STRING("SidebarScroll"))));
                if (s->drag_target_index >= 0 && s->drag_target_index < s->workspace_count &&
                    s->drag_target_index != s->drag_source_index)
                {
                    SidebarReorderWorkspaces(s, s->drag_source_index, s->drag_target_index);
                }
                s->is_dragging = false;
                s->drag_press_pending = false;
                s->drag_source_index = -1;
                s->drag_target_index = -1;
                host->ui_drag_active = false;
                return;
            }
            else
            {
                int clicked = s->drag_source_index;
                s->drag_press_pending = false;
                s->drag_source_index = -1;
                s->drag_target_index = -1;
                host->ui_drag_active = false;
                if (clicked >= 0 && clicked < s->workspace_count &&
                    Clay_PointerOver(CLAY_IDI("SidebarWs", clicked)))
                {
                    ToggleCollapsed(s, clicked);
                    return;
                }
            }
        }
    }

    if (IsMouseButtonPressed(MOUSE_BUTTON_RIGHT))
    {
        if (!s->is_dragging && !s->drag_press_pending)
            SidebarApplyRightClick(host, s);
        return;
    }
    if (!IsMouseButtonPressed(MOUSE_BUTTON_LEFT))
    {
        return;
    }
    if (Clay_PointerOver(CLAY_ID("SidebarMode")))
    {
        SidebarToggleMode(host);
        return;
    }
    if (host->sidebar_inbox_mode && !PicoHost_SelectedAgent(host))
    {
        if (Clay_PointerOver(CLAY_ID("InboxAddProject")))
        {
            RequestAddWorkspace(host, s);
            return;
        }
        for (int k = 0; k < s->workspace_count; k++)
            if (!s->workspaces[k].stashed && Clay_PointerOver(CLAY_IDI("InboxProject", k)))
            {
                NewSessionInWorkspace(host, s, k);
                return;
            }
    }
    if (host->sidebar_inbox_mode && Clay_PointerOver(CLAY_ID("SidebarInboxDone")))
    {
        AcknowledgeInbox(host, s);
        return;
    }
    if (host->sidebar_inbox_mode)
    {
        bool inbox_row_clicked = false;
        VisitInbox(host, s, ClickInboxRow, &inbox_row_clicked);
        if (inbox_row_clicked)
            return;
    }
    if (Clay_PointerOver(Clay_GetElementId(CLAY_STRING("SidebarAddWs"))))
    {
        if (host->sidebar_inbox_mode)
        {
            PicoChat_InspectClose();
            PicoHost_ClearAgentSelection(host);
        }
        else RequestAddWorkspace(host, s);
        return;
    }
    if (Clay_PointerOver(Clay_GetElementId(CLAY_STRING("SidebarSettings"))))
    {
        PicoSettingsUi_Open(host);
        return;
    }
    if (host->sidebar_inbox_mode) return;
    if (Clay_PointerOver(CLAY_ID("SidebarStashedHeader")))
    {
        s->stash_expanded = !s->stash_expanded;
        host->ui_relayout_requested = true;
        return;
    }
    for (i = 0; i < s->workspace_count; i++)
    {
        PicoCatalogWorkspace *ws = &s->workspaces[i];
        if (Clay_PointerOver(CLAY_IDI("SidebarEdit", i)))
        {
            OpenWorkspaceEdit(s, i, MOUSE_BUTTON_LEFT);
            return;
        }
        if (Clay_PointerOver(CLAY_IDI("SidebarPlus", i)))
        {
            NewSessionInWorkspace(host, s, i);
            return;
        }
        if (Clay_PointerOver(CLAY_IDI("SidebarWs", i)))
        {
            if (ws->stashed)
            {
                ToggleCollapsed(s, i);
                return;
            }
            s->drag_press_pending = true;
            s->drag_source_index = i;
            s->drag_press_pos = GetMousePosition();
            s->is_dragging = false;
            s->drag_target_index = i;
            host->ui_drag_active = true;
            return;
        }
        if (ws->collapsed)
        {
            if (OpenPinnedSelected(host, s, ws, i))
            {
                return;
            }
            continue;
        }
        drafts = CountDrafts(s, ws);
        total = drafts + ws->session_count + (ws->has_more_sessions ? SIDEBAR_SESSION_PAGE : 0);
        shown = ShownForIndex(s, i, total);
        if (Clay_PointerOver(CLAY_IDI("SidebarMore", i)))
        {
            AdjustShown(s, i, SIDEBAR_SESSION_PAGE, total);
            s->dirty = true;
            return;
        }
        if (Clay_PointerOver(CLAY_IDI("SidebarLess", i)))
        {
            AdjustShown(s, i, -SIDEBAR_SESSION_PAGE, total);
            return;
        }
        for (j = 0; j < drafts && j < shown; j++)
        {
            PicoAgentId draft = DraftAt(s, ws, j);
            if (Clay_PointerOver(CLAY_IDI("SidebarSess", SessionRowId(i, j))))
            {
                SelectAgent(host, draft);
                return;
            }
        }
        for (j = 0; j < ws->session_count && drafts + j < shown; j++)
        {
            if (Clay_PointerOver(CLAY_IDI("SidebarSess", SessionRowId(i, drafts + j))))
            {
                OpenCatalogSession(host, s, ws->sessions[j].checkout_path, ws->sessions[j].id);
                return;
            }
        }
    }
}

static void SidebarOnFrame(PicoHost *host, void *state, float dt)
{
    SidebarState *s = state ? (SidebarState *)state : (SidebarState *)PicoPlugins_HostState(host, "sidebar");
    const PicoAgent *selected;
    (void)dt;
    if (!s)
    {
        return;
    }
    if (s->inbox_transition_until > 0)
    {
        pico_host_request_redraw(host);
        if (GetTime() >= s->inbox_transition_until) s->inbox_transition_until = 0;
    }
    SidebarStartCollapseSave(s);
    if (s->project_epoch != host->project_preference_epoch)
    {
        s->project_epoch = host->project_preference_epoch;
        s->dirty = true;
    }
    uint64_t inbox_epoch = PicoCatalog_InboxEpoch(host);
    if (s->inbox_epoch != inbox_epoch)
    {
        s->inbox_epoch = inbox_epoch;
        s->dirty = true;
    }
    selected = PicoHost_SelectedAgentConst(host);
    for (int i = 0; i < pico_agent_count(host); i++)
    {
        PicoAgentInfo info;
        if (pico_agent_info(host, i, &info) &&
            PicoAgent_IsBusy(PicoHost_FindAgent(host, info.id)))
        {
            pico_host_request_redraw(host);
            break;
        }
    }
    if (s->order_persist_generation != 0)
    {
        PicoCatalogPersistStatus status = PicoCatalog_OrderPersistStatus(
            host, s->order_persist_generation);
        if (status == PICO_CATALOG_PERSIST_SUCCEEDED)
        {
            s->order_persist_generation = 0;
            s->order_unsaved = false;
            s->dirty = true;
        }
        else if (status == PICO_CATALOG_PERSIST_FAILED)
        {
            s->order_persist_generation = 0;
            s->order_unsaved = true;
        }
    }
    double now = GetTime();
    bool poll_due = now - s->last_scan >= SIDEBAR_SCAN_SEC;
#ifdef PICO_SESSION_TEST_HOOKS
    poll_due = poll_due || PicoSession_TestHook("sidebar_poll_due");
#endif
    bool reconcile_due = now - s->last_reconcile >= SIDEBAR_RECONCILE_SEC;
    bool snapshot_due = s->dirty || !s->catalog_scanned ||
                        (poll_due && SidebarCatalogChanged(s));
    bool need_refresh = snapshot_due || s->reconcile_pending || (poll_due && reconcile_due);
    if (!s->is_dragging && !s->drag_press_pending &&
        s->order_persist_generation == 0 && need_refresh)
    {
        if (!s->scan_worker)
        {
            SidebarStartRefresh(s, !snapshot_due);
        }
    }
    else if (poll_due)
    {
        s->last_scan = now;
    }
    if (!s->want_folder || !s->folder_painted || !pico_ui_modal_is_top(host, "sidebar-folder"))
    {
        return;
    }
    if (!FolderDialogGraphic())
    {
        ClearFolderRequest(s);
        PicoOverlay_Notify(host, "Folder dialog unavailable. Install zenity or kdialog.");
        return;
    }
    {
        const char *start = PicoAgent_WorkspacePath(selected);
        char *path;
        if (!start || !start[0])
        {
            start = NULL;
        }
        path = tinyfd_selectFolderDialog("Add workspace", start);
        ClearFolderRequest(s);
        if (path && path[0])
        {
            if (PicoCatalog_Ensure(path) != 0)
            {
                PicoOverlay_Notify(host, "Could not add that workspace.");
            }
            else
            {
                PicoHost_ChangeWorkspace(host, PicoHost_SelectedWorkspace(host), path);
                s->dirty = true;
            }
        }
    }
}

static int SidebarInit(PicoHost *host, void **state_out)
{
    SidebarState *s = (SidebarState *)calloc(1, sizeof(SidebarState));
    if (!s)
    {
        return 1;
    }
    s->host = host;
    struct timespec wall;
    clock_gettime(CLOCK_REALTIME, &wall);
    s->wall_clock_offset = wall.tv_sec + wall.tv_nsec / 1e9 - PicoClock_Monotonic();
    if (!host->sidebar_mode_loaded)
    {
        host->sidebar_inbox_mode = PicoSettings_ReadSidebarInboxMode();
        host->sidebar_mode_loaded = true;
    }
    s->dirty = true;
    s->reconcile_pending = true;
    if (state_out)
    {
        *state_out = s;
    }
    pico_host_add_view(host, PICO_SLOT_SIDEBAR, 0, PicoSidebar_Render);
    pico_host_add_view(host, PICO_SLOT_OVERLAY, 41, RenderFolderModal);
    pico_host_add_view(host, PICO_SLOT_OVERLAY, 42, RenderWorkspaceEdit);
    pico_host_add_view(host, PICO_SLOT_OVERLAY, 43, RenderSessionMenu);
    pico_host_add_hook(host, PICO_HOOK_AFTER_LAYOUT, SidebarAfterLayout);
    pico_host_add_hook(host, PICO_HOOK_AFTER_RENDER, SidebarDrawEditOverlay);
    return 0;
}

static void SidebarShutdown(PicoHost *host, void *state)
{
    SidebarState *s = (SidebarState *)state;
    if (!s)
    {
        return;
    }
    (void)ClearFolderRequest(s);
    (void)CloseWorkspaceEdit(s);
    (void)CloseSessionMenu(s);
    if (host)
    {
        host->ui_drag_active = false;
    }
    UnloadFolderIcons(s);
    if (s->collapse_save)
        SidebarCollapseCancel(s->collapse_save);
    if (s->scan_worker)
    {
        s->dirty = true;
        return;
    }
    s->scan_serial++;
    while (s->collapse_edits)
    {
        SidebarCollapseEdit *edit = s->collapse_edits;
        s->collapse_edits = edit->next;
        free(edit);
    }
    PicoCatalog_Free(s->workspaces, s->workspace_count);
    free(s->ui);
    free(s->inbox_rows);
    free(s->live_titles);
    for (SidebarLabelChunk *chunk = s->label_chunks; chunk;)
    {
        SidebarLabelChunk *next = chunk->next;
        free(chunk);
        chunk = next;
    }
    free(s);
}

PicoExt pico_ext_sidebar(void)
{
    return (PicoExt){
        .abi = PICO_EXT_ABI,
        .name = "sidebar",
        .description = "Workspace and session list",
        .host_init = SidebarInit,
        .host_shutdown = SidebarShutdown,
        .host_on_frame = SidebarOnFrame,
    };
}
