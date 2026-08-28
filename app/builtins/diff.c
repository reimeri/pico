#define _GNU_SOURCE /* pthread_timedjoin_np */
#define _POSIX_C_SOURCE 200809L

#include "pico/plugin.h"
#include "diff_lines.h"
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

typedef enum DiffRowKind {
    ROW_HEADER = 0, /* file section header */
    ROW_HUNK,       /* @@ ... @@ */
    ROW_CTX,
    ROW_ADD,
    ROW_DEL,
    ROW_NOTE,       /* untracked file we could not render (empty/binary/oversized) */
} DiffRowKind;

typedef struct DiffRow {
    DiffRowKind kind;
    const char *text; /* borrowed from the owning model buffer */
    int len;
} DiffRow;

typedef struct DiffFile {
    const char *label; /* borrowed from the owning model buffer */
    int label_len;
    DiffRow *rows;     /* malloc'd */
    int row_count;
    int row_cap;
} DiffFile;

/* Borrowed storage the model keeps alive for rows/labels outside `patch`. */
typedef struct DiffStash {
    char *buf;
    struct DiffStash *next;
} DiffStash;

typedef struct DiffModel {
    DiffFile *files;
    int file_count;
    int file_cap;
    int adds;
    int dels;
    int untracked;  /* files with no tracked counterpart (empty/binary/oversized included) */
    bool is_repo;
    char workspace[4096]; /* captured from; AdoptPending drops models for other workspaces */
    char *patch;    /* malloc'd; tracked file labels and rows borrow from this */
    DiffStash *stash; /* malloc'd buffers for untracked file labels/rows */
} DiffModel;

static void DiffModel_Free(DiffModel *m)
{
    if (!m)
    {
        return;
    }
    for (int i = 0; i < m->file_count; i++)
    {
        free(m->files[i].rows);
    }
    free(m->files);
    free(m->patch);
    while (m->stash)
    {
        DiffStash *next = m->stash->next;
        free(m->stash->buf);
        free(m->stash);
        m->stash = next;
    }
    free(m);
}

/* Takes ownership of `buf` on success. */
static bool ModelStash(DiffModel *m, char *buf)
{
    DiffStash *s = malloc(sizeof(*s));
    if (!s)
    {
        return false;
    }
    s->buf = buf;
    s->next = m->stash;
    m->stash = s;
    return true;
}

static DiffFile *ModelAddFile(DiffModel *m)
{
    if (m->file_count == m->file_cap)
    {
        int cap = m->file_cap ? m->file_cap * 2 : 8;
        DiffFile *next = realloc(m->files, (size_t)cap * sizeof(*next));
        if (!next)
        {
            return NULL;
        }
        m->files = next;
        m->file_cap = cap;
    }
    DiffFile *f = &m->files[m->file_count++];
    memset(f, 0, sizeof(*f));
    return f;
}

static void FilePushRow(DiffFile *f, DiffRowKind kind, const char *text, int len)
{
    if (f->row_count == f->row_cap)
    {
        int cap = f->row_cap ? f->row_cap * 2 : 64;
        DiffRow *next = realloc(f->rows, (size_t)cap * sizeof(*next));
        if (!next)
        {
            return;
        }
        f->rows = next;
        f->row_cap = cap;
    }
    f->rows[f->row_count++] = (DiffRow){.kind = kind, .text = text, .len = len};
}

/* ------------------------------------------------------------------ */
/* Git capture (worker thread)                                         */
/* ------------------------------------------------------------------ */

#define DIFF_POLL_SECONDS 2

typedef struct DiffWorkerCtx {
    int refcount; /* 1 for DiffState, 1 for worker thread */
    pthread_mutex_t lock;
    pthread_cond_t wake;
    DiffModel *pending; /* worker -> main */
    char workspace[4096];
    bool thread_started;
    bool thread_stop;
    pthread_t thread;
} DiffWorkerCtx;

typedef struct DiffState {
    PicoWorkspace *workspace;
    DiffWorkerCtx *worker;
    DiffModel *model;   /* main thread only */
    bool open;
    PicoScrollbar scrollbar;
    bool overflow;
    char chip_adds[32];
    char chip_dels[32];
    char title[128];
} DiffState;

static char *ShellQuote(const char *path)
{
    size_t len = 2; /* surrounding quotes */
    for (const char *p = path; *p; p++)
    {
        len += *p == '\'' ? 4 : 1;
    }
    char *out = malloc(len + 1);
    if (!out)
    {
        return NULL;
    }
    char *w = out;
    *w++ = '\'';
    for (const char *p = path; *p; p++)
    {
        if (*p == '\'')
        {
            memcpy(w, "'\\''", 4);
            w += 4;
        }
        else
        {
            *w++ = *p;
        }
    }
    *w++ = '\'';
    *w = '\0';
    return out;
}

/* Runs `git -C <ws> <args>`; returns malloc'd NUL-terminated stdout or NULL. */
static char *GitRun(const char *ws, const char *args)
{
    char *quoted = ShellQuote(ws);
    if (!quoted)
    {
        return NULL;
    }
    size_t cap = strlen(quoted) + strlen(args) + 32;
    char *cmd = malloc(cap);
    if (!cmd)
    {
        free(quoted);
        return NULL;
    }
    snprintf(cmd, cap, "git -C %s %s 2>/dev/null", quoted, args);
    free(quoted);

    FILE *fp = popen(cmd, "r");
    free(cmd);
    if (!fp)
    {
        return NULL;
    }
    size_t len = 0;
    size_t buf_cap = 1 << 16;
    char *buf = malloc(buf_cap);
    if (!buf)
    {
        pclose(fp);
        return NULL;
    }
    size_t n;
    while ((n = fread(buf + len, 1, buf_cap - len, fp)) > 0)
    {
        len += n;
        if (len == buf_cap)
        {
            buf_cap *= 2;
            char *next = realloc(buf, buf_cap);
            if (!next)
            {
                free(buf);
                pclose(fp);
                return NULL;
            }
            buf = next;
        }
    }
    int status = pclose(fp);
    if (status != 0)
    {
        free(buf);
        return NULL;
    }
    buf[len] = '\0';
    return buf;
}

static bool IsRepo(const char *ws)
{
    char *out = GitRun(ws, "rev-parse --is-inside-work-tree");
    if (!out)
    {
        return false;
    }
    bool yes = strncmp(out, "true", 4) == 0;
    free(out);
    return yes;
}

/* Well-known empty tree; diff base when HEAD is unborn (no commits yet). */
#define DIFF_EMPTY_TREE "4b825dc642cb6eb9a060e54bf8d69288fbee4904"

/* Ref to diff against: HEAD when it exists, else the empty tree. */
static void DiffBase(const char *ws, char *out, size_t cap)
{
    char *head = GitRun(ws, "rev-parse --verify HEAD");
    if (head)
    {
        free(head);
        snprintf(out, cap, "HEAD");
    }
    else
    {
        snprintf(out, cap, "%s", DIFF_EMPTY_TREE);
    }
}

/* One line of `git diff --numstat`; binary lines ("-") are skipped. */
static void ParseNumstatLine(const char *line, int *adds, int *dels)
{
    if (*line == '-')
    {
        return;
    }
    char *end;
    long a = strtol(line, &end, 10);
    if (end == line)
    {
        return;
    }
    long d = strtol(end, &end, 10);
    *adds += (int)a;
    *dels += (int)d;
}

/* Parse `git diff -U3` output. File labels and rows borrow from `patch`. */
static void ParseUnified(DiffModel *m, char *patch)
{
    DiffFile *cur = NULL;
    const char *line = patch;
    while (*line)
    {
        const char *nl = strchr(line, '\n');
        int len = nl ? (int)(nl - line) : (int)strlen(line);

        if (strncmp(line, "diff --git ", 11) == 0)
        {
            cur = ModelAddFile(m);
        }
        else if (cur && strncmp(line, "+++ b/", 6) == 0)
        {
            cur->label = line + 6;
            cur->label_len = len - 6;
        }
        else if (cur && strncmp(line, "+++ /dev/null", 13) == 0)
        {
            /* Deleted file: label comes from the preceding "--- a/" line. */
        }
        else if (cur && !cur->label && strncmp(line, "--- a/", 6) == 0)
        {
            cur->label = line + 6;
            cur->label_len = len - 6;
        }
        else if (cur && cur->label)
        {
            if (len >= 2 && line[0] == '@' && line[1] == '@')
            {
                FilePushRow(cur, ROW_HUNK, line, len);
            }
            else if (line[0] == '+')
            {
                FilePushRow(cur, ROW_ADD, line + 1, len - 1);
            }
            else if (line[0] == '-')
            {
                FilePushRow(cur, ROW_DEL, line + 1, len - 1);
            }
            else if (line[0] == ' ')
            {
                FilePushRow(cur, ROW_CTX, line + 1, len - 1);
            }
            else if (line[0] == '\\')
            {
                FilePushRow(cur, ROW_HUNK, line, len);
            }
        }
        line += len + (nl ? 1 : 0);
    }

    /* Emit a header row per labeled file, before its first content row. */
    for (int i = 0; i < m->file_count; i++)
    {
        DiffFile *f = &m->files[i];
        if (!f->label || f->row_count == 0)
        {
            continue;
        }
        FilePushRow(f, ROW_HEADER, f->label, f->label_len);
        memmove(f->rows + 1, f->rows, (size_t)(f->row_count - 1) * sizeof(*f->rows));
        f->rows[0] = (DiffRow){.kind = ROW_HEADER, .text = f->label, .len = f->label_len};
    }
}

static char *Strndup(const char *s, int len)
{
    char *out = malloc((size_t)len + 1);
    if (!out)
    {
        return NULL;
    }
    memcpy(out, s, (size_t)len);
    out[len] = '\0';
    return out;
}

/* Untracked files appear as fully-added via a Myers diff against empty.
 * Empty, binary, and oversized files are listed with a note instead. */
static void AddUntracked(DiffModel *m, const char *ws, const char *path, int path_len)
{
    m->untracked++;

    char full[4096];
    int wrote = snprintf(full, sizeof(full), "%s/%.*s", ws, path_len, path);
    if (wrote <= 0 || (size_t)wrote >= sizeof(full))
    {
        return;
    }
    FILE *fp = fopen(full, "rb");
    if (!fp)
    {
        return;
    }
    if (fseek(fp, 0, SEEK_END) != 0)
    {
        fclose(fp);
        return;
    }
    long size = ftell(fp);
    if (fseek(fp, 0, SEEK_SET) != 0)
    {
        fclose(fp);
        return;
    }

    char *content = NULL;
    const char *note = NULL;
    if (size <= 0)
    {
        note = "(empty file)";
    }
    else if (size > (1 << 20))
    {
        note = "(file too large to diff)";
    }
    else
    {
        content = malloc((size_t)size + 1);
        if (content)
        {
            size_t got = fread(content, 1, (size_t)size, fp);
            content[got] = '\0';
            if (memchr(content, '\0', got))
            {
                free(content);
                content = NULL;
                note = "(binary file)";
            }
        }
    }
    fclose(fp);

    PicoDiffLines lines = {0};
    if (content && !PicoDiff_Lines("", content, &lines))
    {
        free(content);
        return;
    }

    char *label = Strndup(path, path_len);
    DiffFile *f = ModelAddFile(m);
    /* Stash before use so every published row borrows model-owned memory.
     * Once stashed, the model owns the buffer; never free it locally. */
    bool label_stashed = label && ModelStash(m, label);
    bool content_stashed = !content || ModelStash(m, content);
    if (!f || !label_stashed || !content_stashed)
    {
        PicoDiff_LinesFree(&lines);
        if (!label_stashed)
        {
            free(label);
        }
        if (!content_stashed)
        {
            free(content);
        }
        return;
    }
    f->label = label;
    f->label_len = path_len;
    FilePushRow(f, ROW_HEADER, label, path_len);
    if (note)
    {
        FilePushRow(f, ROW_NOTE, note, (int)strlen(note));
        PicoDiff_LinesFree(&lines);
        return;
    }
    for (int i = 0; i < lines.count; i++)
    {
        if (lines.lines[i].op == PICO_DIFF_ADD)
        {
            FilePushRow(f, ROW_ADD, lines.lines[i].text, lines.lines[i].len);
            m->adds++;
        }
    }
    PicoDiff_LinesFree(&lines);
}

static void AddUntrackedFiles(DiffModel *m, const char *ws)
{
    char *out = GitRun(ws, "ls-files --others --exclude-standard -z");
    if (!out)
    {
        return;
    }
    const char *p = out;
    while (*p)
    {
        int len = (int)strlen(p);
        AddUntracked(m, ws, p, len);
        p += len + 1;
    }
    free(out);
}

static DiffModel *Capture(const char *ws)
{
    DiffModel *m = calloc(1, sizeof(*m));
    if (!m)
    {
        return NULL;
    }
    snprintf(m->workspace, sizeof(m->workspace), "%s", ws);
    if (!IsRepo(ws))
    {
        m->is_repo = false;
        return m;
    }
    m->is_repo = true;

    char base[64];
    DiffBase(ws, base, sizeof(base));
    char args[128];

    snprintf(args, sizeof(args), "diff --no-color --numstat %s", base);
    char *numstat = GitRun(ws, args);
    if (numstat)
    {
        const char *line = numstat;
        while (*line)
        {
            const char *nl = strchr(line, '\n');
            int len = nl ? (int)(nl - line) : (int)strlen(line);
            ParseNumstatLine(line, &m->adds, &m->dels);
            line += len + (nl ? 1 : 0);
        }
        free(numstat);
    }

    snprintf(args, sizeof(args), "diff --no-color -U3 %s", base);
    m->patch = GitRun(ws, args);
    if (m->patch)
    {
        ParseUnified(m, m->patch);
    }

    AddUntrackedFiles(m, ws);
    return m;
}

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
        DiffModel_Free(w->pending);
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

        DiffModel *fresh = Capture(ws);

        pthread_mutex_lock(&w->lock);
        DiffModel *old = w->pending;
        w->pending = fresh;
        stop = w->thread_stop;
        pthread_mutex_unlock(&w->lock);
        DiffModel_Free(old);
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

    struct timespec deadline;
    clock_gettime(CLOCK_REALTIME, &deadline);
    deadline.tv_nsec += 300 * 1000 * 1000;
    if (deadline.tv_nsec >= 1000 * 1000 * 1000)
    {
        deadline.tv_sec++;
        deadline.tv_nsec -= 1000 * 1000 * 1000;
    }
    if (pthread_timedjoin_np(w->thread, NULL, &deadline) == 0)
    {
        w->thread_started = false;
        w->thread_stop = false;
    }
    else
    {
        pthread_detach(w->thread);
    }
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
        DiffModel_Free(fresh);
        return;
    }
    if (fresh)
    {
        DiffModel_Free(s->model);
        s->model = fresh;
    }
}

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
                                                        .fontSize = 13,
                                                        .textColor = COLOR_MUTED,
                                                        .wrapMode = CLAY_TEXT_WRAP_NONE}));
    CLAY(CLAY_ID("FooterDiff"),
         {.layout = {.layoutDirection = CLAY_LEFT_TO_RIGHT,
                     .childGap = 6,
                     .sizing = {.width = CLAY_SIZING_FIT(0), .height = CLAY_SIZING_FIT(0)}}})
    {
        CLAY_TEXT(CStr(s->chip_adds), CLAY_TEXT_CONFIG({.fontId = FONT_REGULAR,
                                                        .fontSize = 13,
                                                        .textColor = hovered ? COLOR_DIFF_ADD_TEXT
                                                                             : COLOR_DIFF_ADD,
                                                        .wrapMode = CLAY_TEXT_WRAP_NONE}));
        CLAY_TEXT(CStr(s->chip_dels), CLAY_TEXT_CONFIG({.fontId = FONT_REGULAR,
                                                        .fontSize = 13,
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
        case ROW_ADD:
            return COLOR_DIFF_ADD_TEXT;
        case ROW_DEL:
            return COLOR_DIFF_DEL_TEXT;
        default:
            return COLOR_MUTED;
    }
}

static void RenderRow(int index, const DiffRow *row)
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
    CLAY(CLAY_IDI("DiffRow", index),
         {.layout = {.layoutDirection = CLAY_LEFT_TO_RIGHT,
                     .childGap = 6,
                     .padding = {8, 8, 1, 1},
                     .sizing = {.width = CLAY_SIZING_GROW(0)}},
          .backgroundColor = RowBg(row->kind)})
    {
        if (row->kind == ROW_HEADER)
        {
            CLAY_TEXT(Slice(row->text, row->len),
                       CLAY_TEXT_CONFIG({.fontId = FONT_BOLD,
                                         .fontSize = 13,
                                         .textColor = RowFg(row->kind),
                                         .wrapMode = CLAY_TEXT_WRAP_NONE}));
        }
        else
        {
            CLAY_TEXT(CStr(sign), CLAY_TEXT_CONFIG({.fontId = FONT_MONO,
                                                    .fontSize = 13,
                                                    .textColor = RowFg(row->kind),
                                                    .wrapMode = CLAY_TEXT_WRAP_NONE}));
            CLAY_TEXT(Slice(row->text, row->len),
                       CLAY_TEXT_CONFIG({.fontId = FONT_MONO,
                                         .fontSize = 13,
                                         .textColor = RowFg(row->kind),
                                         .wrapMode = CLAY_TEXT_WRAP_NONE}));
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
    PicoHost *app = workspace ? workspace->host : NULL;

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
        snprintf(s->title, sizeof(s->title), "Changes  ·  +%d -%d", s->model->adds, s->model->dels);
    }
    else
    {
        snprintf(s->title, sizeof(s->title), "Changes");
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
            CLAY_TEXT(CStr(s->title),
                      CLAY_TEXT_CONFIG({.fontId = FONT_BOLD, .fontSize = 18, .textColor = COLOR_TEXT}));

            CLAY(CLAY_ID("DiffScrollRow"),
                 {.layout = {.layoutDirection = CLAY_LEFT_TO_RIGHT,
                             .childGap = SCROLLBAR_GAP,
                             .sizing = {.width = CLAY_SIZING_GROW(0),
                                        .height = CLAY_SIZING_GROW(0)}}})
            {
                CLAY(CLAY_ID("DiffScroll"),
                     {.layout = {.layoutDirection = CLAY_TOP_TO_BOTTOM,
                                 .childGap = 2,
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
                                                    .fontSize = 14,
                                                    .textColor = COLOR_MUTED,
                                                    .wrapMode = CLAY_TEXT_WRAP_NONE}));
                    }
                    else
                    {
                        int row_index = 0;
                        for (int fi = 0; fi < s->model->file_count; fi++)
                        {
                            DiffFile *f = &s->model->files[fi];
                            for (int ri = 0; ri < f->row_count; ri++)
                            {
                                RenderRow(row_index++, &f->rows[ri]);
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
    DiffModel_Free(s->model);
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
