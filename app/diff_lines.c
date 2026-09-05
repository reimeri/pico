#include "diff_lines.h"

#include <stdlib.h>
#include <string.h>

static bool LinesPush(PicoDiffLines *d, PicoDiffOp op, const char *text, int len)
{
    if (d->count == d->cap)
    {
        int cap = d->cap ? d->cap * 2 : 64;
        PicoDiffLine *next = realloc(d->lines, (size_t)cap * sizeof(*next));
        if (!next)
        {
            return false;
        }
        d->lines = next;
        d->cap = cap;
    }
    d->lines[d->count++] = (PicoDiffLine){.op = op, .text = text, .len = len};
    return true;
}

void PicoDiff_LinesFree(PicoDiffLines *d)
{
    if (!d)
    {
        return;
    }
    free(d->lines);
    d->lines = NULL;
    d->count = 0;
    d->cap = 0;
}

/* Split NUL-terminated text into borrowed line slices (newline excluded).
 * A trailing newline does not produce an empty final line. */
static int SplitLines(const char *text, const char ***out_ptrs, int **out_lens)
{
    int count = 0;
    for (const char *p = text; *p; p++)
    {
        if (*p == '\n')
        {
            count++;
        }
    }
    const char **ptrs = malloc((size_t)(count + 1) * sizeof(*ptrs));
    int *lens = malloc((size_t)(count + 1) * sizeof(*lens));
    if (!ptrs || !lens)
    {
        free(ptrs);
        free(lens);
        return -1;
    }
    int i = 0;
    const char *start = text;
    for (const char *p = text;; p++)
    {
        if (*p == '\n')
        {
            ptrs[i] = start;
            lens[i] = (int)(p - start);
            i++;
            start = p + 1;
        }
        else if (*p == '\0')
        {
            /* A final segment only counts as a line when it is non-empty:
             * a trailing newline terminates the last line, it does not add one. */
            if (p != start)
            {
                ptrs[i] = start;
                lens[i] = (int)(p - start);
                i++;
            }
            break;
        }
    }
    *out_ptrs = ptrs;
    *out_lens = lens;
    return i;
}

bool PicoDiff_Lines(const char *old_text, const char *new_text, PicoDiffLines *out)
{
    const char **a_ptrs = NULL;
    const char **b_ptrs = NULL;
    int *a_lens = NULL;
    int *b_lens = NULL;
    int *trace = NULL;
    int *v = NULL;
    PicoDiffLines rev = {0};
    bool ok = false;

    int n = SplitLines(old_text ? old_text : "", &a_ptrs, &a_lens);
    int m = SplitLines(new_text ? new_text : "", &b_ptrs, &b_lens);
    if (n < 0 || m < 0)
    {
        goto done;
    }
    if (n == 0 && m == 0)
    {
        ok = true;
        goto done;
    }

    /* Empty-sided diffs need no search or trace. */
    if (n == 0 || m == 0)
    {
        for (int i = 0; i < (n ? n : m); i++)
        {
            if (!LinesPush(out, n ? PICO_DIFF_DEL : PICO_DIFF_ADD,
                           n ? a_ptrs[i] : b_ptrs[i], n ? a_lens[i] : b_lens[i]))
            {
                PicoDiff_LinesFree(out);
                goto done;
            }
        }
        ok = true;
        goto done;
    }

    int max = n + m;
    int width = 2 * max + 1;
    v = calloc((size_t)width, sizeof(*v));
    if (!v)
    {
        goto done;
    }

    /* Forward pass: keep a copy of the V array after each D. Slot 0 of each
     * trace row is repurposed during backtrack only; V[k] lives at k + max. */
    int d_found = -1;
    for (int d = 0; d <= max; d++)
    {
        /* Keep pathological replacements bounded. The caller can display a
         * coarse diff when finding a shortest script exceeds this budget. */
        if ((size_t)(d + 1) > (32u * 1024u * 1024u) / sizeof(*trace) / (size_t)width)
        {
            goto done;
        }
        int *next_trace = realloc(trace, (size_t)(d + 1) * (size_t)width * sizeof(*trace));
        if (!next_trace)
        {
            goto done;
        }
        trace = next_trace;
        memcpy(trace + (size_t)d * (size_t)width, v, (size_t)width * sizeof(*trace));

        for (int k = -d; k <= d; k += 2)
        {
            int x;
            if (k == -d || (k != d && v[max + k - 1] < v[max + k + 1]))
            {
                x = v[max + k + 1];
            }
            else
            {
                x = v[max + k - 1] + 1;
            }
            int y = x - k;
            while (x < n && y < m && a_lens[x] == b_lens[y] &&
                   memcmp(a_ptrs[x], b_ptrs[y], (size_t)a_lens[x]) == 0)
            {
                x++;
                y++;
            }
            v[max + k] = x;
            if (x >= n && y >= m)
            {
                d_found = d;
                break;
            }
        }
        if (d_found >= 0)
        {
            break;
        }
    }
    if (d_found < 0)
    {
        goto done;
    }

    /* Backtrack from (n, m) through the trace, collecting ops in reverse. */
    int x = n;
    int y = m;
    for (int d = d_found; d > 0; d--)
    {
        const int *vd = trace + (size_t)d * (size_t)width;
        int k = x - y;
        int prev_k;
        bool down;
        if (k == -d || (k != d && vd[max + k - 1] < vd[max + k + 1]))
        {
            prev_k = k + 1;
            down = true;
        }
        else
        {
            prev_k = k - 1;
            down = false;
        }
        int prev_x = vd[max + prev_k];
        int prev_y = prev_x - prev_k;
        while (x > prev_x && y > prev_y)
        {
            if (!LinesPush(&rev, PICO_DIFF_CTX, a_ptrs[x - 1], a_lens[x - 1]))
            {
                goto done;
            }
            x--;
            y--;
        }
        if (down)
        {
            if (!LinesPush(&rev, PICO_DIFF_ADD, b_ptrs[y - 1], b_lens[y - 1]))
            {
                goto done;
            }
            y--;
        }
        else
        {
            if (!LinesPush(&rev, PICO_DIFF_DEL, a_ptrs[x - 1], a_lens[x - 1]))
            {
                goto done;
            }
            x--;
        }
    }
    while (x > 0 && y > 0)
    {
        if (!LinesPush(&rev, PICO_DIFF_CTX, a_ptrs[x - 1], a_lens[x - 1]))
        {
            goto done;
        }
        x--;
        y--;
    }

    out->lines = NULL;
    out->count = 0;
    out->cap = 0;
    for (int i = rev.count - 1; i >= 0; i--)
    {
        if (!LinesPush(out, rev.lines[i].op, rev.lines[i].text, rev.lines[i].len))
        {
            PicoDiff_LinesFree(out);
            goto done;
        }
    }
    ok = true;

done:
    PicoDiff_LinesFree(&rev);
    free(a_ptrs);
    free(b_ptrs);
    free(a_lens);
    free(b_lens);
    free(trace);
    free(v);
    return ok;
}
