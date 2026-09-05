#include "diff_lines.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int Fail(const char *test, const char *message)
{
    fprintf(stderr, "%s: %s\n", test, message);
    return 1;
}

/* The line model has no trailing-newline flag: rebuilds end with exactly one
 * newline per line. Normalize the same way before comparing. */
static char *Normalized(const char *text)
{
    size_t len = strlen(text);
    bool trailing = len > 0 && text[len - 1] == '\n';
    char *out = malloc(len + (trailing ? 1 : 2));
    if (!out)
    {
        return NULL;
    }
    memcpy(out, text, len);
    if (!trailing && len > 0)
    {
        out[len++] = '\n';
    }
    out[len] = '\0';
    return out;
}

/* Applying the ops to the old text must reproduce the new text, with context
 * lines appearing in both. */
static int CheckDiff(const char *test, const char *old_text, const char *new_text,
                     int want_add, int want_del)
{
    PicoDiffLines d = {0};
    if (!PicoDiff_Lines(old_text, new_text, &d))
    {
        PicoDiff_LinesFree(&d);
        return Fail(test, "PicoDiff_Lines failed");
    }

    size_t cap = strlen(old_text) + strlen(new_text) + 2;
    char *rebuilt_new = malloc(cap);
    char *rebuilt_old = malloc(cap);
    char *want_new = Normalized(new_text);
    char *want_old = Normalized(old_text);
    if (!rebuilt_new || !rebuilt_old || !want_new || !want_old)
    {
        free(rebuilt_new);
        free(rebuilt_old);
        free(want_new);
        free(want_old);
        PicoDiff_LinesFree(&d);
        return Fail(test, "oom");
    }
    size_t wn = 0;
    size_t wo = 0;
    int adds = 0;
    int dels = 0;
    for (int i = 0; i < d.count; i++)
    {
        PicoDiffLine *l = &d.lines[i];
        adds += l->op == PICO_DIFF_ADD;
        dels += l->op == PICO_DIFF_DEL;
        if (l->op == PICO_DIFF_CTX || l->op == PICO_DIFF_ADD)
        {
            memcpy(rebuilt_new + wn, l->text, (size_t)l->len);
            wn += (size_t)l->len;
            rebuilt_new[wn++] = '\n';
        }
        if (l->op == PICO_DIFF_CTX || l->op == PICO_DIFF_DEL)
        {
            memcpy(rebuilt_old + wo, l->text, (size_t)l->len);
            wo += (size_t)l->len;
            rebuilt_old[wo++] = '\n';
        }
    }
    rebuilt_new[wn] = '\0';
    rebuilt_old[wo] = '\0';

    int rc = 0;
    if (adds != want_add || dels != want_del)
    {
        fprintf(stderr, "%s: got +%d -%d want +%d -%d\n", test, adds, dels, want_add, want_del);
        rc = 1;
    }
    if (!old_text[0] && !new_text[0] && d.count != 0)
    {
        rc = Fail(test, "empty inputs produced diff lines");
    }
    if (strcmp(rebuilt_old, want_old) != 0)
    {
        fprintf(stderr, "%s: old round-trip mismatch:\n--- got ---\n%s\n--- want ---\n%s\n", test,
                rebuilt_old, want_old);
        rc = 1;
    }
    if (strcmp(rebuilt_new, want_new) != 0)
    {
        fprintf(stderr, "%s: new round-trip mismatch:\n--- got ---\n%s\n--- want ---\n%s\n", test,
                rebuilt_new, want_new);
        rc = 1;
    }

    free(rebuilt_new);
    free(rebuilt_old);
    free(want_new);
    free(want_old);
    PicoDiff_LinesFree(&d);
    return rc;
}

int main(void)
{
    const struct {
        const char *name;
        const char *old_text;
        const char *new_text;
        int adds;
        int dels;
    } cases[] = {
        {"identical", "a\nb\nc\n", "a\nb\nc\n", 0, 0},
        {"addition", "a\nc\n", "a\nb\nc\n", 1, 0},
        {"deletion", "a\nb\nc\n", "a\nc\n", 0, 1},
        {"replacement", "a\nb\nc\n", "a\nx\nc\n", 1, 1},
        {"empty_to_content", "", "one\ntwo\n", 2, 0},
        {"content_to_empty", "one\ntwo\n", "", 0, 2},
        /* A move should not be reported as a wholesale delete+add of the file. */
        {"minimal_edits", "one\ntwo\nthree\nfour\nfive\n", "two\nthree\nfour\nfive\none\n", 1, 1},
        {"no_trailing_newline", "a\nb", "a\nc", 1, 1},
        {"duplicate_lines", "x\nx\nx\n", "x\nx\nx\nx\n", 1, 0},
        {"both_empty", "", "", 0, 0},
    };
    int rc = 0;
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++)
    {
        rc |= CheckDiff(cases[i].name, cases[i].old_text, cases[i].new_text,
                        cases[i].adds, cases[i].dels);
    }
    if (rc == 0)
    {
        printf("ok\n");
    }
    return rc;
}
