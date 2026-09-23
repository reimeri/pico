#include "text_field.h"

#include <stdio.h>
#include <string.h>

static int Fail(const char *test, const char *message)
{
    fprintf(stderr, "%s: %s\n", test, message);
    return 1;
}

static int ExpectText(const char *test, const PicoTextField *f, const char *want)
{
    if (!f->text || strcmp(f->text, want) != 0)
    {
        fprintf(stderr, "%s: got \"%s\" want \"%s\"\n", test, f->text ? f->text : "(null)", want);
        return 1;
    }
    return 0;
}

static int ExpectCursor(const char *test, const PicoTextField *f, int cursor, int anchor)
{
    if (f->cursor != cursor || f->anchor != anchor)
    {
        fprintf(stderr, "%s: got cursor=%d anchor=%d want cursor=%d anchor=%d\n", test, f->cursor,
                f->anchor, cursor, anchor);
        return 1;
    }
    return 0;
}

static int TestInsertAndMove(void)
{
    const char *test = "insert_and_move";
    char buf[64] = {0};
    PicoTextField f = {0};
    PicoTextField_Bind(&f, buf, sizeof(buf));
    if (!PicoTextField_Insert(&f, "hello", 5))
    {
        return Fail(test, "insert rejected");
    }
    if (ExpectText(test, &f, "hello") || ExpectCursor(test, &f, 5, 5))
    {
        return 1;
    }
    PicoTextField_Move(&f, 0, false);
    PicoTextField_Insert(&f, "say ", 4);
    if (ExpectText(test, &f, "say hello") || ExpectCursor(test, &f, 4, 4))
    {
        return 1;
    }
    /* Movement clamps to the text bounds. */
    PicoTextField_Move(&f, 1000, false);
    return ExpectCursor(test, &f, 9, 9);
}

static int TestUtf8Editing(void)
{
    const char *test = "utf8_editing";
    char buf[64] = {0};
    PicoTextField f = {0};
    PicoTextField_Bind(&f, buf, sizeof(buf));
    PicoTextField_SetText(&f, "a\xE2\x82\xAC" "b"); /* a € b */
    /* Backspace removes the whole code point, not one byte. */
    PicoTextField_DeleteBackward(&f, false);
    if (ExpectText(test, &f, "a\xE2\x82\xAC"))
    {
        return 1;
    }
    PicoTextField_DeleteBackward(&f, false);
    return ExpectText(test, &f, "a");
}

static int TestWordDelete(void)
{
    const char *test = "word_delete";
    char buf[64] = {0};
    PicoTextField f = {0};
    PicoTextField_Bind(&f, buf, sizeof(buf));
    PicoTextField_SetText(&f, "foo bar baz");
    PicoTextField_DeleteBackward(&f, true);
    if (ExpectText(test, &f, "foo bar ") || ExpectCursor(test, &f, 8, 8))
    {
        return 1;
    }
    PicoTextField_Move(&f, 0, false);
    PicoTextField_DeleteForward(&f, true);
    /* Ctrl+Delete removes the word up to its end, leaving the space. */
    return ExpectText(test, &f, " bar ");
}

static int TestSelectionReplace(void)
{
    const char *test = "selection_replace";
    char buf[64] = {0};
    PicoTextField f = {0};
    PicoTextField_Bind(&f, buf, sizeof(buf));
    PicoTextField_SetText(&f, "hello world");
    PicoTextField_SelectAll(&f);
    if (ExpectCursor(test, &f, 11, 0))
    {
        return 1;
    }
    PicoTextField_Insert(&f, "x", 1);
    if (ExpectText(test, &f, "x") || ExpectCursor(test, &f, 1, 1))
    {
        return 1;
    }
    /* Extending the selection then deleting leaves the rest intact. */
    PicoTextField_SetText(&f, "abcdef");
    PicoTextField_Move(&f, 2, false);
    PicoTextField_Move(&f, 4, true);
    PicoTextField_DeleteSelection(&f);
    return ExpectText(test, &f, "abef");
}

static int TestPasteSanitizes(void)
{
    const char *test = "paste_sanitizes";
    char buf[64] = {0};
    PicoTextField f = {0};
    PicoTextField_Bind(&f, buf, sizeof(buf));
    PicoTextField_PasteText(&f, "one\ntwo\tthree");
    return ExpectText(test, &f, "one two three");
}

static int TestFixedCapacity(void)
{
    const char *test = "fixed_capacity";
    char buf[8] = {0};
    PicoTextField f = {0};
    PicoTextField_Bind(&f, buf, sizeof(buf));
    /* More text than fits is truncated to a whole code point prefix. */
    PicoTextField_SetText(&f, "abcdefgh");
    if (ExpectText(test, &f, "abcdefg"))
    {
        return 1;
    }
    /* Insert at the cap keeps the buffer NUL-terminated and whole. */
    PicoTextField_SetText(&f, "abcd");
    PicoTextField_Move(&f, 4, false);
    PicoTextField_Insert(&f, "e\xE2\x82\xAC" "fg", 6);
    return ExpectText(test, &f, "abcde");
}

static int TestGrowable(void)
{
    const char *test = "growable";
    PicoTextField f = {0};
    PicoTextField_BindGrowable(&f);
    char long_text[600];
    memset(long_text, 'x', sizeof(long_text) - 1);
    long_text[sizeof(long_text) - 1] = '\0';
    if (!PicoTextField_Insert(&f, long_text, (int)strlen(long_text)))
    {
        PicoTextField_Free(&f);
        return Fail(test, "growable insert rejected");
    }
    if (f.capacity <= (int)sizeof(long_text) || f.length != (int)sizeof(long_text) - 1)
    {
        PicoTextField_Free(&f);
        return Fail(test, "buffer did not grow to fit");
    }
    int ok = ExpectText(test, &f, long_text);
    PicoTextField_Free(&f);
    if (ok)
    {
        return 1;
    }
    return f.text != NULL || f.bound ? Fail(test, "free did not release storage") : 0;
}

static int TestRevision(void)
{
    const char *test = "revision";
    char buf[64] = {0};
    PicoTextField f = {0};
    PicoTextField_Bind(&f, buf, sizeof(buf));
    unsigned base = f.revision;
    PicoTextField_Move(&f, 0, false);
    PicoTextField_SelectAll(&f);
    if (f.revision != base)
    {
        return Fail(test, "cursor movement bumped the revision");
    }
    PicoTextField_Insert(&f, "a", 1);
    PicoTextField_DeleteBackward(&f, false);
    PicoTextField_SetText(&f, "");
    return f.revision == base + 3 ? 0 : Fail(test, "edits did not bump the revision");
}

static int TestSelectUnits(void)
{
    const char *test = "select_units";
    char buf[64] = {0};
    PicoTextField f = {0};
    PicoTextField_Bind(&f, buf, sizeof(buf));
    PicoTextField_SetText(&f, "foo bar baz");
    /* Single click places the caret. */
    PicoTextField_SelectUnit(&f, 5, 1, false);
    if (ExpectCursor(test, &f, 5, 5))
    {
        return 1;
    }
    /* Double click selects the word. */
    PicoTextField_SelectUnit(&f, 5, 2, false);
    if (ExpectCursor(test, &f, 7, 4))
    {
        return 1;
    }
    /* Dragging extends by whole words from the clicked word. */
    PicoTextField_ExtendUnit(&f, 9);
    if (ExpectCursor(test, &f, 11, 4))
    {
        return 1;
    }
    /* Triple click selects the whole line. */
    PicoTextField_SelectUnit(&f, 5, 3, false);
    return ExpectCursor(test, &f, 11, 0);
}

static int TestCopyText(void)
{
    const char *test = "copy_text";
    char buf[64] = {0};
    char out[64];
    PicoTextField f = {0};
    PicoTextField_Bind(&f, buf, sizeof(buf));
    PicoTextField_SetText(&f, "hello world");
    if (PicoTextField_CopyText(&f, out, sizeof(out)) != 0 || out[0] != '\0')
    {
        return Fail(test, "copy without selection produced text");
    }
    PicoTextField_Move(&f, 6, false);
    PicoTextField_Move(&f, 11, true);
    if (PicoTextField_CopyText(&f, out, sizeof(out)) != 5 || strcmp(out, "world") != 0)
    {
        return Fail(test, "copy did not return the selection");
    }
    return 0;
}

static int TestDeleteBackwardWordWithSelection(void)
{
    const char *test = "word_delete_with_selection";
    char buf[64] = {0};
    PicoTextField f = {0};
    PicoTextField_Bind(&f, buf, sizeof(buf));
    PicoTextField_SetText(&f, "foo bar");
    PicoTextField_SelectAll(&f);
    /* With a selection, word delete removes the selection, not a word. */
    PicoTextField_DeleteBackward(&f, true);
    return ExpectText(test, &f, "");
}

static int TestRefresh(void)
{
    const char *test = "refresh";
    char buf[64] = "hello world";
    PicoTextField f = {0};
    PicoTextField_Bind(&f, buf, sizeof(buf));
    PicoTextField_SelectAll(&f);
    /* The caller rewrote the buffer directly (shorter text). */
    buf[0] = 'h';
    buf[1] = 'i';
    buf[2] = '\0';
    PicoTextField_Refresh(&f);
    if (f.length != 2)
    {
        return Fail(test, "length did not follow the buffer");
    }
    /* The cursor clamps into the new bounds; the anchor was already valid. */
    if (ExpectCursor(test, &f, 2, 0))
    {
        return 1;
    }
    /* Editing continues from the refreshed state. */
    PicoTextField_Move(&f, 2, false);
    PicoTextField_Insert(&f, "!", 1);
    return ExpectText(test, &f, "hi!");
}

int main(void)
{
    int failed = 0;
    failed |= TestInsertAndMove();
    failed |= TestUtf8Editing();
    failed |= TestWordDelete();
    failed |= TestSelectionReplace();
    failed |= TestPasteSanitizes();
    failed |= TestFixedCapacity();
    failed |= TestGrowable();
    failed |= TestRevision();
    failed |= TestSelectUnits();
    failed |= TestCopyText();
    failed |= TestDeleteBackwardWordWithSelection();
    failed |= TestRefresh();
    return failed;
}
