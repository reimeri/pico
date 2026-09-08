#include "text_range.h"

#include <stdio.h>
#include <string.h>

static int Fail(const char *test, const char *message)
{
    fprintf(stderr, "%s: %s\n", test, message);
    return 1;
}

static int ExpectRange(const char *test, int from, int to, int want_from, int want_to)
{
    if (from != want_from || to != want_to)
    {
        fprintf(stderr, "%s: got [%d, %d) want [%d, %d)\n", test, from, to, want_from, want_to);
        return 1;
    }
    return 0;
}

static int TestWordIdentifier(void)
{
    const char *test = "word_under_identifier";
    const char s[] = "hello world";
    int from = -1;
    int to = -1;
    PicoText_WordRange(s, (int)strlen(s), 1, &from, &to);
    return ExpectRange(test, from, to, 0, 5);
}

static int TestPunctuationRun(void)
{
    const char *test = "punctuation_vs_word";
    const char s[] = "foo...bar";
    int from = -1;
    int to = -1;
    PicoText_WordRange(s, (int)strlen(s), 4, &from, &to);
    if (ExpectRange(test, from, to, 3, 6))
    {
        return 1;
    }
    PicoText_WordRange(s, (int)strlen(s), 0, &from, &to);
    return ExpectRange(test, from, to, 0, 3);
}

static int TestWhitespaceRun(void)
{
    const char *test = "whitespace_run";
    const char s[] = "a  \tb";
    int from = -1;
    int to = -1;
    PicoText_WordRange(s, (int)strlen(s), 2, &from, &to);
    return ExpectRange(test, from, to, 1, 4);
}

static int TestParagraph(void)
{
    const char *test = "paragraph_range";
    const char s[] = "one line\ntwo line\n";
    int from = -1;
    int to = -1;
    PicoText_ParaRange(s, (int)strlen(s), 10, &from, &to);
    if (ExpectRange(test, from, to, 9, 17))
    {
        return 1;
    }
    const char whole[] = "no newlines here";
    PicoText_ParaRange(whole, (int)strlen(whole), 4, &from, &to);
    return ExpectRange(test, from, to, 0, (int)strlen(whole));
}

static int TestClickSeq(void)
{
    const char *test = "click_seq";
    PicoClickSeq seq;
    PicoClickSeq_Reset(&seq);
    int n = PicoClickSeq_Press(&seq, 1.0, 10, 10);
    if (n != 1)
    {
        return Fail(test, "first press was not 1");
    }
    n = PicoClickSeq_Press(&seq, 1.2, 11, 10);
    if (n != 2)
    {
        return Fail(test, "close second press was not 2");
    }
    PicoClickSeq_Reset(&seq);
    PicoClickSeq_Press(&seq, 1.0, 10, 10);
    n = PicoClickSeq_Press(&seq, 2.0, 10, 10);
    if (n != 1)
    {
        return Fail(test, "far-apart press was not 1");
    }
    PicoClickSeq_Reset(&seq);
    PicoClickSeq_Press(&seq, 1.0, 0, 0);
    PicoClickSeq_Press(&seq, 1.1, 0, 0);
    PicoClickSeq_Press(&seq, 1.2, 0, 0);
    n = PicoClickSeq_Press(&seq, 1.3, 0, 0);
    if (n != 1)
    {
        return Fail(test, "fourth press was not 1");
    }
    return 0;
}

static int ExpectPos(const char *test, int got, int want)
{
    if (got != want)
    {
        fprintf(stderr, "%s: got %d want %d\n", test, got, want);
        return 1;
    }
    return 0;
}

/* A word step consumes the word plus any adjacent punctuation run, matching
 * the composer's Ctrl+Arrow / Ctrl+W behavior. */
static int TestPrevWord(void)
{
    const char *test = "prev_word";
    const char s[] = "foo...bar baz";
    int len = (int)strlen(s);
    if (ExpectPos(test, PicoText_PrevWord(s, len), 10))
    {
        return 1;
    }
    if (ExpectPos(test, PicoText_PrevWord(s, 9), 3))
    {
        return 1;
    }
    if (ExpectPos(test, PicoText_PrevWord(s, 6), 3))
    {
        return 1;
    }
    if (ExpectPos(test, PicoText_PrevWord(s, 3), 0))
    {
        return 1;
    }
    return ExpectPos(test, PicoText_PrevWord(s, 0), 0);
}

static int TestNextWord(void)
{
    const char *test = "next_word";
    const char s[] = "foo...bar baz";
    int len = (int)strlen(s);
    if (ExpectPos(test, PicoText_NextWord(s, len, 0), 6))
    {
        return 1;
    }
    if (ExpectPos(test, PicoText_NextWord(s, len, 3), 6))
    {
        return 1;
    }
    if (ExpectPos(test, PicoText_NextWord(s, len, 6), 9))
    {
        return 1;
    }
    if (ExpectPos(test, PicoText_NextWord(s, len, 9), len))
    {
        return 1;
    }
    return ExpectPos(test, PicoText_NextWord(s, len, len), len);
}

static int TestWordStepEmpty(void)
{
    const char *test = "word_step_empty";
    if (ExpectPos(test, PicoText_PrevWord("", 0), 0))
    {
        return 1;
    }
    return ExpectPos(test, PicoText_NextWord("", 0, 0), 0);
}

static int TestUtf8Step(void)
{
    const char *test = "utf8_step";
    const char s[] = "h\xC3\xA9llo";
    int len = (int)strlen(s);
    if (ExpectPos(test, PicoText_Utf8Next(s, len, 0), 1))
    {
        return 1;
    }
    if (ExpectPos(test, PicoText_Utf8Next(s, len, 1), 3))
    {
        return 1;
    }
    if (ExpectPos(test, PicoText_Utf8Next(s, len, len), len))
    {
        return 1;
    }
    if (ExpectPos(test, PicoText_Utf8Prev(s, 3), 1))
    {
        return 1;
    }
    return ExpectPos(test, PicoText_Utf8Prev(s, 0), 0);
}

static int TestUtf8EncodeRoundTrip(void)
{
    const char *test = "utf8_encode_round_trip";
    char encoded[4] = {0};
    int n = PicoText_Utf8Encode(0x20AC, encoded);
    if (n != 3 || memcmp(encoded, "\xE2\x82\xAC", 3) != 0)
    {
        return Fail(test, "euro sign did not encode to 3 expected bytes");
    }
    /* Encoding a code point reproduces the bytes that stepping walks over. */
    const char s[] = "a\xE2\x82\xAC";
    return ExpectPos(test, PicoText_Utf8Next(s, (int)strlen(s), 1), 4);
}

int main(void)
{
    int failed = 0;
    failed |= TestWordIdentifier();
    failed |= TestPunctuationRun();
    failed |= TestWhitespaceRun();
    failed |= TestParagraph();
    failed |= TestClickSeq();
    failed |= TestPrevWord();
    failed |= TestNextWord();
    failed |= TestWordStepEmpty();
    failed |= TestUtf8Step();
    failed |= TestUtf8EncodeRoundTrip();
    return failed;
}
