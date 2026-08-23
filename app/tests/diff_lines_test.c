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
static int RoundTrip(const char *test, const char *old_text, const char *new_text)
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
    for (int i = 0; i < d.count; i++)
    {
        PicoDiffLine *l = &d.lines[i];
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

static int CountOps(const char *test, const char *old_text, const char *new_text, int want_add,
                    int want_del)
{
    PicoDiffLines d = {0};
    if (!PicoDiff_Lines(old_text, new_text, &d))
    {
        PicoDiff_LinesFree(&d);
        return Fail(test, "PicoDiff_Lines failed");
    }
    int adds = 0;
    int dels = 0;
    for (int i = 0; i < d.count; i++)
    {
        if (d.lines[i].op == PICO_DIFF_ADD)
        {
            adds++;
        }
        else if (d.lines[i].op == PICO_DIFF_DEL)
        {
            dels++;
        }
    }
    PicoDiff_LinesFree(&d);
    if (adds != want_add || dels != want_del)
    {
        fprintf(stderr, "%s: got +%d -%d want +%d -%d\n", test, adds, dels, want_add, want_del);
        return 1;
    }
    return 0;
}

static int TestIdentical(void)
{
    return CountOps("identical", "a\nb\nc\n", "a\nb\nc\n", 0, 0);
}

static int TestAddition(void)
{
    const char *test = "addition";
    if (RoundTrip(test, "a\nc\n", "a\nb\nc\n"))
    {
        return 1;
    }
    return CountOps(test, "a\nc\n", "a\nb\nc\n", 1, 0);
}

static int TestDeletion(void)
{
    const char *test = "deletion";
    if (RoundTrip(test, "a\nb\nc\n", "a\nc\n"))
    {
        return 1;
    }
    return CountOps(test, "a\nb\nc\n", "a\nc\n", 0, 1);
}

static int TestReplacement(void)
{
    const char *test = "replacement";
    if (RoundTrip(test, "a\nb\nc\n", "a\nx\nc\n"))
    {
        return 1;
    }
    return CountOps(test, "a\nb\nc\n", "a\nx\nc\n", 1, 1);
}

static int TestEmptyToContent(void)
{
    const char *test = "empty_to_content";
    if (RoundTrip(test, "", "one\ntwo\n"))
    {
        return 1;
    }
    return CountOps(test, "", "one\ntwo\n", 2, 0);
}

static int TestContentToEmpty(void)
{
    const char *test = "content_to_empty";
    if (RoundTrip(test, "one\ntwo\n", ""))
    {
        return 1;
    }
    return CountOps(test, "one\ntwo\n", "", 0, 2);
}

static int TestMinimalEdits(void)
{
    /* A move should not be reported as a wholesale delete+add of the file. */
    const char *test = "minimal_edits";
    const char *old_text = "one\ntwo\nthree\nfour\nfive\n";
    const char *new_text = "two\nthree\nfour\nfive\none\n";
    if (RoundTrip(test, old_text, new_text))
    {
        return 1;
    }
    return CountOps(test, old_text, new_text, 1, 1);
}

static int TestNoTrailingNewline(void)
{
    const char *test = "no_trailing_newline";
    if (RoundTrip(test, "a\nb", "a\nc"))
    {
        return 1;
    }
    return CountOps(test, "a\nb", "a\nc", 1, 1);
}

static int TestDuplicateLines(void)
{
    const char *test = "duplicate_lines";
    if (RoundTrip(test, "x\nx\nx\n", "x\nx\nx\nx\n"))
    {
        return 1;
    }
    return CountOps(test, "x\nx\nx\n", "x\nx\nx\nx\n", 1, 0);
}

static int TestBothEmpty(void)
{
    const char *test = "both_empty";
    PicoDiffLines d = {0};
    if (!PicoDiff_Lines("", "", &d))
    {
        PicoDiff_LinesFree(&d);
        return Fail(test, "PicoDiff_Lines failed");
    }
    int count = d.count;
    PicoDiff_LinesFree(&d);
    if (count != 0)
    {
        fprintf(stderr, "%s: got %d lines, want 0\n", test, count);
        return 1;
    }
    return 0;
}

int main(void)
{
    int rc = 0;
    rc |= TestIdentical();
    rc |= TestAddition();
    rc |= TestDeletion();
    rc |= TestReplacement();
    rc |= TestEmptyToContent();
    rc |= TestContentToEmpty();
    rc |= TestMinimalEdits();
    rc |= TestNoTrailingNewline();
    rc |= TestDuplicateLines();
    rc |= TestBothEmpty();
    if (rc == 0)
    {
        printf("ok\n");
    }
    return rc;
}
