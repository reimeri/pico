#include "json.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int CheckEmpty(const char *label, const char *value)
{
    if (!value)
    {
        fprintf(stderr, "%s returned NULL\n", label);
        return 1;
    }
    if (value[0] != '\0')
    {
        fprintf(stderr, "%s returned a non-empty string\n", label);
        return 1;
    }
    return 0;
}

static int TestStringEncoding(void)
{
    const struct { const char *input, *expected; } cases[] = {
        {"quote\" slash\\ tab\t newline\n\x01", "quote\" slash\\ tab\t newline\n\x01"},
        {"\xC2\x80\xDF\xBF\xE0\xA0\x80\xED\x9F\xBF\xEE\x80\x80\xEF\xBF\xBF\xF0\x90\x80\x80\xF4\x8F\xBF\xBF",
         "\xC2\x80\xDF\xBF\xE0\xA0\x80\xED\x9F\xBF\xEE\x80\x80\xEF\xBF\xBF\xF0\x90\x80\x80\xF4\x8F\xBF\xBF"},
        {"before\xFF\xFF\xFF" "after ✓", "before���after ✓"},
        {"\x80", "�"},
        {"\xC0\xAF", "��"},
        {"\xE0\x80\x80", "���"},
        {"\xF0\x80\x80\x80", "����"},
        {"\xED\xA0\x80", "���"},
        {"\xF4\x90\x80\x80", "����"},
        {"\xE2\x82", "��"},
        {"\xE2(\xA1", "�(�"},
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++)
    {
        JsonBuf buf;
        JsonBuf_Init(&buf);
        JsonBuf_String(&buf, cases[i].input);
        char *json = JsonBuf_Steal(&buf);
        JsonDoc doc;
        bool valid = json && JsonValidSyntax(json, strlen(json)) &&
                     JsonParse(&doc, json, strlen(json)) == 0;
        char *decoded = valid ? JsonStrDup(&doc, 0) : NULL;
        bool matches = decoded && strcmp(decoded, cases[i].expected) == 0;
        if (valid) JsonFree(&doc);
        free(decoded);
        free(json);
        if (!matches)
        {
            fprintf(stderr, "JSON string case %zu must preserve valid text and replace each malformed UTF-8 byte\n", i);
            return 1;
        }
    }
    return 0;
}

/* Session tool payloads contain long ordinary runs separated by escapes.
 * Check encoding against independent JSON text, then decode that fixture so
 * matching bugs in encoder and decoder cannot hide each other. */
static int TestLargeStringSpans(void)
{
    const struct { const char *text, *json; } cases[] = {
        {"", ""},
        {"\"\\/\b\f\n\r\t\x01\x7f ✓🙂", "\\\"\\\\/\\b\\f\\n\\r\\t\\u0001\x7f ✓🙂"},
    };
    const size_t run = 16 * 1024;
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++)
    {
        size_t middle = strlen(cases[i].text), escaped = strlen(cases[i].json);
        size_t text_len = run * 2 + middle, json_len = run * 2 + escaped + 2;
        char *text = malloc(text_len + 1), *expected = malloc(json_len + 1);
        if (!text || !expected) { free(text); free(expected); return 1; }
        memset(text, 'a', run);
        memcpy(text + run, cases[i].text, middle);
        memset(text + run + middle, 'z', run);
        text[text_len] = '\0';
        expected[0] = '"';
        memset(expected + 1, 'a', run);
        memcpy(expected + 1 + run, cases[i].json, escaped);
        memset(expected + 1 + run + escaped, 'z', run);
        expected[json_len - 1] = '"';
        expected[json_len] = '\0';

        JsonBuf buf;
        JsonBuf_Init(&buf);
        JsonBuf_String(&buf, text);
        char *encoded = JsonBuf_Steal(&buf);
        bool encoded_ok = encoded && strcmp(encoded, expected) == 0;
        JsonDoc doc;
        bool parsed = JsonParse(&doc, expected, json_len) == 0;
        char *decoded = parsed ? JsonStrDup(&doc, 0) : NULL;
        bool decoded_ok = decoded && strcmp(decoded, text) == 0;
        if (parsed) JsonFree(&doc);
        free(encoded); free(decoded); free(text); free(expected);
        if (!encoded_ok || !decoded_ok)
        {
            fprintf(stderr, "Large JSON strings must preserve ordinary runs and adjacent escape/UTF-8 boundaries\n");
            return 1;
        }
    }
    return 0;
}

int main(void)
{
    if (TestStringEncoding() || TestLargeStringSpans()) return 1;
    JsonBuf buf;
    JsonBuf_Init(&buf);
    buf.data = (char *)malloc(64);
    if (!buf.data)
    {
        fprintf(stderr, "allocation failed\n");
        return 1;
    }
    memset(buf.data, 0xA5, 64);
    buf.cap = 64;

    char *stolen = JsonBuf_Steal(&buf);
    int failed = CheckEmpty("JsonBuf_Steal", stolen);
    if (buf.data || buf.len != 0 || buf.cap != 0)
    {
        fprintf(stderr, "JsonBuf_Steal did not reset the source buffer\n");
        failed = 1;
    }
    free(stolen);

    const char json[] = "{\"value\":\"\"}";
    JsonDoc doc;
    if (JsonParse(&doc, json, strlen(json)) != 0)
    {
        fprintf(stderr, "JsonParse failed\n");
        return 1;
    }
    char *value = JsonObjStr(&doc, 0, "value");
    failed |= CheckEmpty("JsonObjStr", value);
    free(value);
    JsonFree(&doc);

    const char *strings[] = {"\"\\ud83d\\ude42\"", "\"before\\u0000after\"", "\"\\ud83d\""};
    for (int i = 0; i < 3; ++i) {
        if (JsonParse(&doc, strings[i], strlen(strings[i]))) return 1;
        value = JsonStrDup(&doc, 0);
        if (i == 0 ? (!value || strcmp(value, "\xF0\x9F\x99\x82")) : value != NULL) {
            fprintf(stderr, "Unicode strings must decode complete scalar values without truncation\n"); failed = 1;
        }
        free(value); JsonFree(&doc);
    }
    return failed;
}
