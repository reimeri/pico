#include "highlight.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Each check highlights a snippet and expects an exact span list. Expected
 * spans are located by searching for their text with a moving cursor, so
 * duplicate needles (two `let`s) resolve in order. */

typedef struct Want {
    const char *needle;
    PicoHlClass class;
} Want;

static int Check(const char *test, const char *tag, const char *text, const Want *wants,
                 int want_count)
{
    const PicoHlLang *lang = PicoHl_Lookup(tag);
    if (!lang)
    {
        fprintf(stderr, "%s: tag '%s' did not resolve\n", test, tag);
        return 1;
    }
    int count = PicoHl_Count(lang, text);
    PicoHlSpan *spans = malloc(sizeof(PicoHlSpan) * (size_t)(count > 0 ? count : 1));
    if (!spans)
    {
        fprintf(stderr, "%s: out of memory\n", test);
        return 1;
    }
    int got = PicoHl_Fill(lang, text, spans, count);
    int fails = 0;
    if (got != count)
    {
        fprintf(stderr, "%s: Fill wrote %d spans, Count said %d\n", test, got, count);
        fails++;
    }
    for (int i = 0; i < got; i++)
    {
        if (spans[i].start < 0 || spans[i].end > (int)strlen(text) || spans[i].start >= spans[i].end)
        {
            fprintf(stderr, "%s: span %d out of bounds [%d,%d)\n", test, i, spans[i].start,
                    spans[i].end);
            fails++;
        }
        if (i > 0 && spans[i].start < spans[i - 1].end)
        {
            fprintf(stderr, "%s: span %d overlaps previous span\n", test, i);
            fails++;
        }
    }
    if (got != want_count)
    {
        fprintf(stderr, "%s: got %d spans, want %d\n", test, got, want_count);
        for (int i = 0; i < got; i++)
        {
            fprintf(stderr, "  span [%d,%d) class %d: %.*s\n", spans[i].start, spans[i].end,
                    spans[i].class, spans[i].end - spans[i].start, text + spans[i].start);
        }
        free(spans);
        return fails + 1;
    }
    const char *cursor = text;
    for (int i = 0; i < want_count; i++)
    {
        const char *found = strstr(cursor, wants[i].needle);
        if (!found)
        {
            fprintf(stderr, "%s: expected span text '%s' not found after offset %ld\n", test,
                    wants[i].needle, (long)(cursor - text));
            fails++;
            continue;
        }
        int start = (int)(found - text);
        int end = start + (int)strlen(wants[i].needle);
        if (spans[i].start != start || spans[i].end != end || spans[i].class != wants[i].class)
        {
            fprintf(stderr,
                    "%s: span %d is [%d,%d) class %d, want [%d,%d) class %d ('%s')\n",
                    test, i, spans[i].start, spans[i].end, spans[i].class, start, end,
                    wants[i].class, wants[i].needle);
            fails++;
        }
        cursor = text + end;
    }
    free(spans);
    return fails;
}

static int TestCBasics(void)
{
    const char *t = "#include <stdio.h>\n"
                    "int main(void) {\n"
                    "    // greet\n"
                    "    printf(\"hi\\n\"); /* done */ return 0;\n"
                    "}\n";
    const Want wants[] = {
        { "#include <stdio.h>", PICO_HL_PREPROC },
        { "int", PICO_HL_KEYWORD },
        { "main", PICO_HL_FUNC },
        { "void", PICO_HL_KEYWORD },
        { "// greet", PICO_HL_COMMENT },
        { "printf", PICO_HL_FUNC },
        { "\"hi\\n\"", PICO_HL_STRING },
        { "/* done */", PICO_HL_COMMENT },
        { "return", PICO_HL_KEYWORD },
        { "0", PICO_HL_NUMBER },
    };
    return Check("c basics", "c", t, wants, (int)(sizeof(wants) / sizeof(wants[0])));
}

static int TestCBlockCommentAcrossLines(void)
{
    const char *t = "int a; /* one\ntwo */ int b;";
    const Want wants[] = {
        { "int", PICO_HL_KEYWORD },
        { "/* one\ntwo */", PICO_HL_COMMENT },
        { "int", PICO_HL_KEYWORD },
    };
    return Check("c block comment", "c", t, wants, 3);
}

static int TestCLineCommentDoesNotLeak(void)
{
    const char *t = "// c\nint x;";
    const Want wants[] = {
        { "// c", PICO_HL_COMMENT },
        { "int", PICO_HL_KEYWORD },
    };
    return Check("c line comment scope", "c", t, wants, 2);
}

static int TestCppRawString(void)
{
    const char *t = "auto s = R\"(a \"quoted\" thing)\";";
    const Want wants[] = {
        { "auto", PICO_HL_KEYWORD },
        { "R\"(a \"quoted\" thing)\"", PICO_HL_STRING },
    };
    return Check("cpp raw string", "cpp", t, wants, 2);
}

static int TestPythonTripleString(void)
{
    const char *t = "def f():\n"
                    "    s = \"\"\"line1\nline2\"\"\"  # done\n"
                    "    return 1\n";
    const Want wants[] = {
        { "def", PICO_HL_KEYWORD },
        { "f", PICO_HL_FUNC },
        { "\"\"\"line1\nline2\"\"\"", PICO_HL_STRING },
        { "# done", PICO_HL_COMMENT },
        { "return", PICO_HL_KEYWORD },
        { "1", PICO_HL_NUMBER },
    };
    return Check("python triple string", "python", t, wants, 6);
}

static int TestJsonKeysAndValues(void)
{
    const char *t = "{\"name\": \"pico\", \"n\": 42, \"ok\": true}";
    const Want wants[] = {
        { "\"name\"", PICO_HL_FIELD },
        { "\"pico\"", PICO_HL_STRING },
        { "\"n\"", PICO_HL_FIELD },
        { "42", PICO_HL_NUMBER },
        { "\"ok\"", PICO_HL_FIELD },
        { "true", PICO_HL_TYPE },
    };
    return Check("json keys", "json", t, wants, 6);
}

static int TestJsonUnterminatedString(void)
{
    const char *t = "{\"a\": \"x";
    const Want wants[] = {
        { "\"a\"", PICO_HL_FIELD },
        { "\"x", PICO_HL_STRING },
    };
    return Check("json unterminated", "json", t, wants, 2);
}

static int TestRustNestingAndLifetime(void)
{
    const char *t = "/* a /* b */ c */ let x = 'q'; let r: &'a str;";
    const Want wants[] = {
        { "/* a /* b */ c */", PICO_HL_COMMENT },
        { "let", PICO_HL_KEYWORD },
        { "'q'", PICO_HL_STRING },
        { "let", PICO_HL_KEYWORD },
        { "str", PICO_HL_TYPE },
    };
    return Check("rust nesting and lifetime", "rust", t, wants, 5);
}

static int TestJsTemplateLiteral(void)
{
    const char *t = "const s = `a\nb ${x}`; // end";
    const Want wants[] = {
        { "const", PICO_HL_KEYWORD },
        { "`a\nb ${x}`", PICO_HL_STRING },
        { "// end", PICO_HL_COMMENT },
    };
    return Check("js template literal", "js", t, wants, 3);
}

static int TestGoRawStringQuotes(void)
{
    const char *t = "s := `say \"hi\"`\nfmt.Println(\"a\\\"b\")";
    const Want wants[] = {
        { "`say \"hi\"`", PICO_HL_STRING },
        { "Println", PICO_HL_FUNC },
        { "\"a\\\"b\"", PICO_HL_STRING },
    };
    return Check("go raw string", "go", t, wants, 3);
}

static int TestBash(void)
{
    const char *t = "if [ -n \"$HOME\" ]; then\n  echo $USER && ${HOME} # done\nfi\n";
    const Want wants[] = {
        { "if", PICO_HL_KEYWORD },
        { "\"$HOME\"", PICO_HL_STRING },
        { "then", PICO_HL_KEYWORD },
        { "echo", PICO_HL_TYPE },
        { "$USER", PICO_HL_VAR },
        { "${HOME}", PICO_HL_VAR },
        { "# done", PICO_HL_COMMENT },
        { "fi", PICO_HL_KEYWORD },
    };
    return Check("bash", "bash", t, wants, 8);
}

static int TestLua(void)
{
    const char *t = "--[[ block\ncomment ]]\nlocal x = [[long\nstr]] -- end\n";
    const Want wants[] = {
        { "--[[ block\ncomment ]]", PICO_HL_COMMENT },
        { "local", PICO_HL_KEYWORD },
        { "[[long\nstr]]", PICO_HL_STRING },
        { "-- end", PICO_HL_COMMENT },
    };
    return Check("lua", "lua", t, wants, 4);
}

static int TestTypeScriptKeywords(void)
{
    const char *t = "interface P { readonly x: number }\nconst p: P = { x: 1 };";
    const Want wants[] = {
        { "interface", PICO_HL_KEYWORD },
        { "readonly", PICO_HL_KEYWORD },
        { "const", PICO_HL_KEYWORD },
        { "1", PICO_HL_NUMBER },
    };
    return Check("typescript keywords", "ts", t, wants, 4);
}

static int TestMarkdown(void)
{
    const char *t = "# Title\n"
                    "\n"
                    "text with `code` and **bold** and [link](http://x).\n"
                    "\n"
                    "- item\n"
                    "> quote\n"
                    "\n"
                    "```c\n"
                    "int x; // not highlighted\n"
                    "```\n";
    const Want wants[] = {
        { "# Title", PICO_HL_KEYWORD },
        { "`code`", PICO_HL_STRING },
        { "**bold**", PICO_HL_TYPE },
        { "[link]", PICO_HL_FIELD },
        { "(http://x)", PICO_HL_STRING },
        { "-", PICO_HL_PREPROC },
        { "> quote", PICO_HL_COMMENT },
        { "```c", PICO_HL_PREPROC },
        { "```", PICO_HL_PREPROC },
    };
    return Check("markdown", "markdown", t, wants, 9);
}

static int TestDiff(void)
{
    const char *t = "--- a/f.c\n"
                    "+++ b/f.c\n"
                    "@@ -1,2 +1,2 @@\n"
                    "-old\n"
                    "+new\n"
                    " same\n";
    const Want wants[] = {
        { "--- a/f.c", PICO_HL_HUNK },
        { "+++ b/f.c", PICO_HL_HUNK },
        { "@@ -1,2 +1,2 @@", PICO_HL_HUNK },
        { "-old", PICO_HL_DEL },
        { "+new", PICO_HL_ADD },
    };
    return Check("diff", "diff", t, wants, 5);
}

static int TestLookup(void)
{
    int fails = 0;
    if (PicoHl_Lookup("C") != PicoHl_Lookup("c") || !PicoHl_Lookup("c"))
    {
        fprintf(stderr, "lookup: case-insensitive c failed\n");
        fails++;
    }
    if (PicoHl_Lookup("c++") != PicoHl_Lookup("cpp"))
    {
        fprintf(stderr, "lookup: c++ alias failed\n");
        fails++;
    }
    if (PicoHl_Lookup("txt") || PicoHl_Lookup("") || PicoHl_Lookup(NULL))
    {
        fprintf(stderr, "lookup: unknown tags must return NULL\n");
        fails++;
    }
    if (PicoHl_Lookup("python extras") != PicoHl_Lookup("py"))
    {
        fprintf(stderr, "lookup: trailing info string words must be ignored\n");
        fails++;
    }
    if (PicoHl_LangForPath("src/main.c") != PicoHl_Lookup("c") ||
        PicoHl_LangForPath("a/b/lib.rs") != PicoHl_Lookup("rust") ||
        PicoHl_LangForPath("Makefile") != NULL || PicoHl_LangForPath("x.unknownext") != NULL)
    {
        fprintf(stderr, "lookup: path extension mapping failed\n");
        fails++;
    }
    return fails;
}

static int TestEmptyAndNull(void)
{
    int fails = 0;
    const PicoHlLang *c = PicoHl_Lookup("c");
    if (PicoHl_Count(c, "") != 0 || PicoHl_Count(NULL, "int x;") != 0)
    {
        fprintf(stderr, "empty/null: expected zero spans\n");
        fails++;
    }
    PicoHlSpan one;
    if (PicoHl_Fill(NULL, "int x;", &one, 1) != 0)
    {
        fprintf(stderr, "empty/null: Fill with NULL lang must write nothing\n");
        fails++;
    }
    return fails;
}

int main(void)
{
    int fails = 0;
    fails += TestCBasics();
    fails += TestCBlockCommentAcrossLines();
    fails += TestCLineCommentDoesNotLeak();
    fails += TestCppRawString();
    fails += TestPythonTripleString();
    fails += TestJsonKeysAndValues();
    fails += TestJsonUnterminatedString();
    fails += TestRustNestingAndLifetime();
    fails += TestJsTemplateLiteral();
    fails += TestGoRawStringQuotes();
    fails += TestBash();
    fails += TestLua();
    fails += TestTypeScriptKeywords();
    fails += TestMarkdown();
    fails += TestDiff();
    fails += TestLookup();
    fails += TestEmptyAndNull();
    if (fails == 0)
    {
        printf("highlight tests: all passed\n");
    }
    return fails > 0 ? 1 : 0;
}
