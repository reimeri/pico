#include "spell_engine.h"

#include <stdio.h>
#include <string.h>

/* Stub dictionary: check passes exactly for the words listed here, so a
 * test never depends on a host spell library being installed. */
static const char *kWords[] = {
    "the", "cat", "sat",  "use", "now",  "see", "open", "some",
    "var", "isn't", "ok", "le",  "caf\xc3\xa9", "ask", "or",   "mail",
};

static bool StubCheck(void *ctx, const char *word, int length)
{
    (void)ctx;
    for (size_t i = 0; i < sizeof(kWords) / sizeof(kWords[0]); i++)
    {
        if ((int)strlen(kWords[i]) == length && memcmp(kWords[i], word, (size_t)length) == 0)
        {
            return true;
        }
    }
    return false;
}

static PicoSpellBackend Backend(void)
{
    return (PicoSpellBackend){NULL, StubCheck};
}

static int g_failed;

static void Fail(const char *test, const char *detail)
{
    fprintf(stderr, "FAIL: %s: %s\n", test, detail);
    g_failed = 1;
}

static PicoSpellRanges CheckText(const char *test, const char *text, int want_count)
{
    PicoSpellRanges ranges = {0};
    PicoSpellBackend backend = Backend();
    pico_spell_check_text(&backend, text, (int)strlen(text), &ranges);
    if (ranges.count != want_count)
    {
        fprintf(stderr, "FAIL: %s: got %d ranges, want %d\n", test, ranges.count, want_count);
        g_failed = 1;
    }
    return ranges;
}

static void ExpectRange(const char *test, const PicoSpellRanges *ranges, int index, int start, int end)
{
    if (index >= ranges->count)
    {
        Fail(test, "range index out of bounds");
        return;
    }
    if (ranges->items[index].start != start || ranges->items[index].end != end)
    {
        fprintf(stderr, "FAIL: %s: range %d is [%d, %d), want [%d, %d)\n", test, index,
                ranges->items[index].start, ranges->items[index].end, start, end);
        g_failed = 1;
    }
}

static void TestCleanText(void)
{
    PicoSpellRanges r = CheckText("clean text", "the cat sat", 0);
    pico_spell_ranges_free(&r);
}

static void TestMisspellingOffsets(void)
{
    PicoSpellRanges r = CheckText("misspelling offsets", "the teh cat", 1);
    ExpectRange("misspelling offsets", &r, 0, 4, 7);
    pico_spell_ranges_free(&r);
}

static void TestUtf8Offsets(void)
{
    PicoSpellRanges r = CheckText("utf8 offsets", "le t\xc3\xa9h", 1);
    ExpectRange("utf8 offsets", &r, 0, 3, 7);
    pico_spell_ranges_free(&r);
    r = CheckText("utf8 known word", "le caf\xc3\xa9", 0);
    pico_spell_ranges_free(&r);
}

static void TestReuseDoesNotAccumulate(void)
{
    PicoSpellRanges r = {0};
    PicoSpellBackend backend = Backend();
    pico_spell_check_text(&backend, "teh mispelled", 13, &r);
    pico_spell_check_text(&backend, "teh mispelled", 13, &r);
    if (r.count != 2)
    {
        Fail("reuse", "second check did not replace previous ranges");
    }
    else
    {
        ExpectRange("reuse", &r, 0, 0, 3);
        ExpectRange("reuse", &r, 1, 4, 13);
    }
    pico_spell_ranges_free(&r);
}

static void TestRangeGrowth(void)
{
    char text[96] = {0};
    for (int i = 0; i < 20; i++)
    {
        strcat(text, "teh ");
    }
    PicoSpellRanges r = CheckText("range growth", text, 20);
    ExpectRange("range growth", &r, 19, 76, 79);
    pico_spell_ranges_free(&r);
}

static void TestNoBackendMeansNoRanges(void)
{
    PicoSpellRanges r = {0};
    pico_spell_check_text(NULL, "teh", 3, &r);
    if (r.count != 0)
    {
        Fail("null backend", "expected zero ranges");
    }
    PicoSpellBackend empty = {0};
    pico_spell_check_text(&empty, "teh", 3, &r);
    if (r.count != 0)
    {
        Fail("null check fn", "expected zero ranges");
    }
    PicoSpellBackend backend = Backend();
    pico_spell_check_text(&backend, NULL, 0, &r);
    if (r.count != 0)
    {
        Fail("null text", "expected zero ranges");
    }
    pico_spell_ranges_free(&r);
}

static void TestCodeSpanSkipped(void)
{
    PicoSpellRanges r = CheckText("code span", "use `teh` now", 0);
    pico_spell_ranges_free(&r);
}

static void TestUrlSkipped(void)
{
    PicoSpellRanges r = CheckText("url", "see https://teh.example.com/x", 0);
    pico_spell_ranges_free(&r);
}

static void TestPathSkipped(void)
{
    PicoSpellRanges r = CheckText("path", "open ~/src/teh.c", 0);
    pico_spell_ranges_free(&r);
}

static void TestMentionsSkipped(void)
{
    PicoSpellRanges r = CheckText("mentions", "ask @teh or #teh", 0);
    pico_spell_ranges_free(&r);
}

static void TestEmailSkipped(void)
{
    PicoSpellRanges r = CheckText("email", "mail teh@example.com", 0);
    pico_spell_ranges_free(&r);
}

static void TestDigitTokenSkipped(void)
{
    PicoSpellRanges r = CheckText("digit token", "the teh2 cat", 0);
    pico_spell_ranges_free(&r);
}

static void TestAcronymSkippedButTitleCaseChecked(void)
{
    PicoSpellRanges r = CheckText("acronym", "the NASA API", 0);
    pico_spell_ranges_free(&r);
    r = CheckText("title case", "Teh cat", 1);
    ExpectRange("title case", &r, 0, 0, 3);
    pico_spell_ranges_free(&r);
}

static void TestSingleLetterSkipped(void)
{
    PicoSpellRanges r = CheckText("single letter", "x teh", 1);
    ExpectRange("single letter", &r, 0, 2, 5);
    pico_spell_ranges_free(&r);
}

static void TestApostropheWord(void)
{
    PicoSpellRanges r = CheckText("apostrophe word", "isn't ok", 0);
    pico_spell_ranges_free(&r);
}

static void TestIdentifierSplit(void)
{
    PicoSpellRanges r = CheckText("identifier of words", "some_var", 0);
    pico_spell_ranges_free(&r);
    r = CheckText("identifier with typo", "teh_var", 1);
    ExpectRange("identifier with typo", &r, 0, 0, 3);
    pico_spell_ranges_free(&r);
}

static void TestSentenceFinalPeriodStillChecks(void)
{
    PicoSpellRanges r = CheckText("sentence period", "the teh.", 1);
    ExpectRange("sentence period", &r, 0, 4, 7);
    pico_spell_ranges_free(&r);
}

int main(void)
{
    TestCleanText();
    TestMisspellingOffsets();
    TestUtf8Offsets();
    TestReuseDoesNotAccumulate();
    TestRangeGrowth();
    TestNoBackendMeansNoRanges();
    TestCodeSpanSkipped();
    TestUrlSkipped();
    TestPathSkipped();
    TestMentionsSkipped();
    TestEmailSkipped();
    TestDigitTokenSkipped();
    TestAcronymSkippedButTitleCaseChecked();
    TestSingleLetterSkipped();
    TestApostropheWord();
    TestIdentifierSplit();
    TestSentenceFinalPeriodStillChecks();
    if (g_failed)
    {
        return 1;
    }
    printf("pico_spell_tests: all tests passed\n");
    return 0;
}
