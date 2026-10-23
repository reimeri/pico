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

int main(void)
{
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
