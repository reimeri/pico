#define _GNU_SOURCE
#define _POSIX_C_SOURCE 200809L

#include "diff_model.h"

#include "diff_lines.h"
#include "highlight.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void PicoDiffModel_Free(DiffModel *m)
{
    if (!m)
    {
        return;
    }
    for (int i = 0; i < m->file_count; i++)
    {
        free(m->files[i].rows);
        free(m->files[i].old_spans);
        free(m->files[i].new_spans);
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
    DiffFile *f = &m->files[m->file_count];
    memset(f, 0, sizeof(*f));
    m->file_count++;
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
    f->rows[f->row_count++] = (DiffRow){.kind = kind, .text = text, .len = len, .img_off = -1};
}

/* ------------------------------------------------------------------ */
/* Signature                                                           */
/* ------------------------------------------------------------------ */

/* FNV-1a over every byte the UI can observe. */
static void SigBytes(uint64_t *h, const void *data, size_t len)
{
    const unsigned char *p = data;
    for (size_t i = 0; i < len; i++)
    {
        *h = (*h ^ p[i]) * 1099511628211ULL;
    }
}

static void SigInt(uint64_t *h, long v)
{
    SigBytes(h, &v, sizeof(v));
}

uint64_t PicoDiffModel_Signature(const DiffModel *m)
{
    uint64_t h = 1469598103934665603ULL;
    if (!m)
    {
        return h;
    }
    SigBytes(&h, m->workspace, strlen(m->workspace));
    SigInt(&h, m->is_repo);
    SigInt(&h, m->adds);
    SigInt(&h, m->dels);
    SigInt(&h, m->untracked);
    for (int i = 0; i < m->file_count; i++)
    {
        const DiffFile *f = &m->files[i];
        SigInt(&h, f->label_len);
        if (f->label)
        {
            SigBytes(&h, f->label, (size_t)f->label_len);
        }
        SigInt(&h, f->row_count);
        for (int r = 0; r < f->row_count; r++)
        {
            SigInt(&h, f->rows[r].kind);
            SigInt(&h, f->rows[r].len);
            if (f->rows[r].text && f->rows[r].len > 0)
            {
                SigBytes(&h, f->rows[r].text, (size_t)f->rows[r].len);
            }
        }
    }
    return h;
}

/* ------------------------------------------------------------------ */
/* Syntax highlighting                                                 */
/* ------------------------------------------------------------------ */

/* Rebuilds the file's pre-image (context + deleted) and post-image (context +
 * added) as temporary strings, highlights both, and keeps the span lists.
 * Runs once per file; spans index image offsets recorded in DiffRow.img_off.
 * Approximate by design: an image spliced from hunks is not the full file. */
/* Highlighting is a visual aid: cap image size so a huge or generated diff
 * cannot turn it into a stall, and so image offsets always fit in int. */
#define DIFF_HL_MAX_IMAGE (4 * 1024 * 1024)

static void FileBuildHighlight(DiffFile *f)
{
    if (f->hl_built)
    {
        return;
    }
    f->hl_built = true;
    if (!f->label || f->label_len <= 0)
    {
        return;
    }
    char label[1024];
    int n = f->label_len < (int)sizeof(label) - 1 ? f->label_len : (int)sizeof(label) - 1;
    memcpy(label, f->label, (size_t)n);
    label[n] = '\0';
    const PicoHlLang *lang = PicoHl_LangForPath(label);
    if (!lang)
    {
        return;
    }

    size_t old_cap = 1, new_cap = 1;
    for (int i = 0; i < f->row_count; i++)
    {
        DiffRowKind kind = f->rows[i].kind;
        if (kind == ROW_CTX || kind == ROW_DEL)
        {
            old_cap += (size_t)f->rows[i].len + 1;
        }
        if (kind == ROW_CTX || kind == ROW_ADD)
        {
            new_cap += (size_t)f->rows[i].len + 1;
        }
    }
    if (old_cap > DIFF_HL_MAX_IMAGE || new_cap > DIFF_HL_MAX_IMAGE)
    {
        return;
    }
    char *old_img = (char *)malloc(old_cap);
    char *new_img = (char *)malloc(new_cap);
    if (!old_img || !new_img)
    {
        free(old_img);
        free(new_img);
        return;
    }
    int old_off = 0, new_off = 0;
    for (int i = 0; i < f->row_count; i++)
    {
        DiffRow *r = &f->rows[i];
        if (r->kind == ROW_CTX || r->kind == ROW_DEL)
        {
            memcpy(old_img + old_off, r->text, (size_t)r->len);
            old_img[old_off + r->len] = '\n';
            if (r->kind == ROW_DEL)
            {
                r->img_off = old_off;
            }
            old_off += r->len + 1;
        }
        if (r->kind == ROW_CTX || r->kind == ROW_ADD)
        {
            memcpy(new_img + new_off, r->text, (size_t)r->len);
            new_img[new_off + r->len] = '\n';
            r->img_off = new_off;
            new_off += r->len + 1;
        }
    }
    old_img[old_off] = '\0';
    new_img[new_off] = '\0';

    f->old_count = PicoHl_Count(lang, old_img);
    f->old_spans = (PicoHlSpan *)malloc((size_t)f->old_count * sizeof(PicoHlSpan));
    if (f->old_spans)
    {
        f->old_count = PicoHl_Fill(lang, old_img, f->old_spans, f->old_count);
    }
    else
    {
        f->old_count = 0;
    }
    f->new_count = PicoHl_Count(lang, new_img);
    f->new_spans = (PicoHlSpan *)malloc((size_t)f->new_count * sizeof(PicoHlSpan));
    if (f->new_spans)
    {
        f->new_count = PicoHl_Fill(lang, new_img, f->new_spans, f->new_count);
    }
    else
    {
        f->new_count = 0;
    }
    free(old_img);
    free(new_img);
}

void PicoDiffModel_HighlightAll(DiffModel *m)
{
    if (!m)
    {
        return;
    }
    for (int i = 0; i < m->file_count; i++)
    {
        FileBuildHighlight(&m->files[i]);
    }
}

/* ------------------------------------------------------------------ */
/* Git capture                                                         */
/* ------------------------------------------------------------------ */

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

DiffModel *PicoDiffModel_Capture(const char *ws)
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
