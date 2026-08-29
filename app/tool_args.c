#include "agent.h"
#include "json.h"

#include <stdlib.h>
#include <string.h>

static void FlattenPut(JsonBuf *b, const char *s, size_t max)
{
    if (!s)
    {
        return;
    }
    for (; *s && b->len < max; s++)
    {
        char c = (*s == '\n' || *s == '\r' || *s == '\t') ? ' ' : *s;
        JsonBuf_Putc(b, c);
    }
}

char *PicoAgent_FormatToolArgs(const char *name, const char *args_json)
{
    JsonBuf b;
    JsonBuf_Init(&b);
    if (!args_json || !args_json[0])
    {
        return JsonBuf_Steal(&b);
    }
    JsonDoc doc;
    if (JsonParse(&doc, args_json, strlen(args_json)) != 0)
    {
        FlattenPut(&b, args_json, 240);
        return JsonBuf_Steal(&b);
    }
    if (name && strcmp(name, "sh") == 0)
    {
        char *description = JsonObjStr(&doc, 0, "description");
        if (description && description[0])
        {
            FlattenPut(&b, description, 240);
            free(description);
            JsonFree(&doc);
            return JsonBuf_Steal(&b);
        }
        free(description);
    }
    if (JsonIsObject(&doc, 0))
    {
        int n = JsonObjLen(&doc, 0);
        for (int i = 0; i < n; i++)
        {
            int key_tok = -1;
            int val_tok = -1;
            if (!JsonObjPair(&doc, 0, i, &key_tok, &val_tok))
            {
                continue;
            }
            if (b.len)
            {
                JsonBuf_Puts(&b, "  ");
            }
            char *key = JsonStrDup(&doc, key_tok);
            FlattenPut(&b, key, 240);
            free(key);
            JsonBuf_Puts(&b, ": ");
            if (JsonIsArray(&doc, val_tok))
            {
                int count = JsonArrayLen(&doc, val_tok);
                JsonBuf_Puts(&b, "[");
                JsonBuf_Int(&b, count);
                JsonBuf_Puts(&b, count == 1 ? " item]" : " items]");
            }
            else
            {
                char *val = NULL;
                if (JsonIsObject(&doc, val_tok))
                {
                    val = JsonRawDup(&doc, val_tok);
                }
                else
                {
                    val = JsonStrDup(&doc, val_tok);
                    if (!val)
                    {
                        val = JsonRawDup(&doc, val_tok);
                    }
                }
                FlattenPut(&b, val, 240);
                free(val);
            }
            if (b.len > 240)
            {
                JsonBuf_Puts(&b, "...");
                break;
            }
        }
    }
    else
    {
        char *raw = JsonRawDup(&doc, 0);
        FlattenPut(&b, raw, 240);
        free(raw);
    }
    JsonFree(&doc);
    return JsonBuf_Steal(&b);
}
