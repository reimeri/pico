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

int main(void)
{
    int failed = 0;
    failed |= TestWordIdentifier();
    failed |= TestPunctuationRun();
    failed |= TestWhitespaceRun();
    failed |= TestParagraph();
    failed |= TestClickSeq();
    return failed;
}
