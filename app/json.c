#define JSMN_STATIC
#define JSMN_STRICT
#include "jsmn.h"

#include "json.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

char *JsonDup(const char *s)
{
    if (!s)
    {
        s = "";
    }
    size_t n = strlen(s);
    char *d = (char *)malloc(n + 1);
    if (d)
    {
        memcpy(d, s, n + 1);
    }
    return d;
}

char *Pico_ReadFile(const char *path, size_t *out_len)
{
    if (out_len)
    {
        *out_len = 0;
    }
    if (!path || !path[0])
    {
        return NULL;
    }
    FILE *f = fopen(path, "rb");
    if (!f)
    {
        return NULL;
    }
    /* fopen() succeeds on directories; ftell() then reports LONG_MAX and the
       allocation below requests petabytes. Only read regular files. */
    struct stat st;
    if (fstat(fileno(f), &st) != 0 || !S_ISREG(st.st_mode))
    {
        fclose(f);
        return NULL;
    }
    if (fseek(f, 0, SEEK_END) != 0)
    {
        fclose(f);
        return NULL;
    }
    long sz = ftell(f);
    if (sz < 0)
    {
        fclose(f);
        return NULL;
    }
    rewind(f);
    char *buf = (char *)malloc((size_t)sz + 1);
    if (!buf)
    {
        fclose(f);
        return NULL;
    }
    size_t n = fread(buf, 1, (size_t)sz, f);
    fclose(f);
    buf[n] = '\0';
    if (out_len)
    {
        *out_len = n;
    }
    return buf;
}

void JsonBuf_Init(JsonBuf *b)
{
    memset(b, 0, sizeof(*b));
}

void JsonBuf_Free(JsonBuf *b)
{
    free(b->data);
    memset(b, 0, sizeof(*b));
}

void JsonBuf_Clear(JsonBuf *b)
{
    b->len = 0;
    b->failed = false;
    if (b->data)
    {
        b->data[0] = '\0';
    }
}

static void JsonBuf_Need(JsonBuf *b, size_t extra)
{
    size_t needed;
    size_t cap;
    char *next;
    if (b->failed)
    {
        return;
    }
    if (extra > SIZE_MAX - b->len - 1)
    {
        b->failed = true;
        return;
    }
    needed = b->len + extra + 1;
    if (needed <= b->cap)
    {
        return;
    }
    cap = b->cap ? b->cap : 64;
    while (cap < needed)
    {
        if (cap > SIZE_MAX / 2)
        {
            cap = needed;
            break;
        }
        cap *= 2;
    }
    next = (char *)realloc(b->data, cap);
    if (!next)
    {
        b->failed = true;
        return;
    }
    b->data = next;
    b->cap = cap;
}

void JsonBuf_Append(JsonBuf *b, const char *s, size_t n)
{
    if (!s || n == 0)
    {
        return;
    }
    JsonBuf_Need(b, n);
    if (b->failed || !b->data || b->len + n + 1 > b->cap)
    {
        b->failed = true;
        return;
    }
    memcpy(b->data + b->len, s, n);
    b->len += n;
    b->data[b->len] = '\0';
}

void JsonBuf_Puts(JsonBuf *b, const char *s)
{
    if (s)
    {
        JsonBuf_Append(b, s, strlen(s));
    }
}

void JsonBuf_Putc(JsonBuf *b, char c)
{
    JsonBuf_Append(b, &c, 1);
}

void JsonBuf_String(JsonBuf *b, const char *s)
{
    JsonBuf_Putc(b, '"');
    if (!s)
    {
        JsonBuf_Putc(b, '"');
        return;
    }
    for (const unsigned char *p = (const unsigned char *)s; *p; p++)
    {
        unsigned char c = *p;
        switch (c)
        {
        case '"':
            JsonBuf_Puts(b, "\\\"");
            break;
        case '\\':
            JsonBuf_Puts(b, "\\\\");
            break;
        case '\b':
            JsonBuf_Puts(b, "\\b");
            break;
        case '\f':
            JsonBuf_Puts(b, "\\f");
            break;
        case '\n':
            JsonBuf_Puts(b, "\\n");
            break;
        case '\r':
            JsonBuf_Puts(b, "\\r");
            break;
        case '\t':
            JsonBuf_Puts(b, "\\t");
            break;
        default:
            if (c < 0x20)
            {
                char hex[8];
                snprintf(hex, sizeof(hex), "\\u%04x", c);
                JsonBuf_Puts(b, hex);
            }
            else
            {
                JsonBuf_Putc(b, (char)c);
            }
            break;
        }
    }
    JsonBuf_Putc(b, '"');
}

void JsonBuf_Int(JsonBuf *b, int v)
{
    char buf[32];
    snprintf(buf, sizeof(buf), "%d", v);
    JsonBuf_Puts(b, buf);
}

void JsonBuf_Bool(JsonBuf *b, bool v)
{
    JsonBuf_Puts(b, v ? "true" : "false");
}

char *JsonBuf_Steal(JsonBuf *b)
{
    char *data;
    JsonBuf_Need(b, 0);
    if (b->failed)
    {
        free(b->data);
        memset(b, 0, sizeof(*b));
        return NULL;
    }
    data = b->data;
    if (data)
    {
        data[b->len] = '\0';
    }
    else
    {
        data = JsonDup("");
    }
    memset(b, 0, sizeof(*b));
    return data;
}

void JsonStripComments(char *src, size_t len)
{
    if (!src || len == 0)
    {
        return;
    }
    bool in_string = false;
    bool escaped = false;
    for (size_t i = 0; i < len; i++)
    {
        char c = src[i];
        if (in_string)
        {
            if (escaped)
            {
                escaped = false;
                continue;
            }
            if (c == '\\')
            {
                escaped = true;
                continue;
            }
            if (c == '"')
            {
                in_string = false;
            }
            continue;
        }
        if (c == '"')
        {
            in_string = true;
            continue;
        }
        if (c == '/' && i + 1 < len && src[i + 1] == '/')
        {
            while (i < len && src[i] != '\n' && src[i] != '\r')
            {
                src[i] = ' ';
                i++;
            }
            if (i < len)
            {
                i--;
            }
            continue;
        }
        if (c == '/' && i + 1 < len && src[i + 1] == '*')
        {
            src[i] = ' ';
            src[i + 1] = ' ';
            i += 2;
            while (i < len)
            {
                if (i + 1 < len && src[i] == '*' && src[i + 1] == '/')
                {
                    src[i] = ' ';
                    src[i + 1] = ' ';
                    i++;
                    break;
                }
                if (src[i] != '\n' && src[i] != '\r')
                {
                    src[i] = ' ';
                }
                i++;
            }
            continue;
        }
    }
}

static jsmntok_t *Toks(const JsonDoc *doc)
{
    return (jsmntok_t *)doc->toks;
}

int JsonParse(JsonDoc *doc, const char *src, size_t len)
{
    memset(doc, 0, sizeof(*doc));
    if (!src)
    {
        return -1;
    }
    doc->src = src;
    doc->len = len;
    int cap = 256;
    jsmntok_t *toks = NULL;
    for (;;)
    {
        jsmntok_t *next = (jsmntok_t *)realloc(toks, (size_t)cap * sizeof(jsmntok_t));
        if (!next)
        {
            free(toks);
            return -1;
        }
        toks = next;
        jsmn_parser parser;
        jsmn_init(&parser);
        int n = jsmn_parse(&parser, src, len, toks, (unsigned int)cap);
        if (n == JSMN_ERROR_NOMEM)
        {
            cap *= 2;
            continue;
        }
        if (n < 0)
        {
            free(toks);
            return -1;
        }
        doc->toks = toks;
        doc->ntoks = n;
        return 0;
    }
}

void JsonFree(JsonDoc *doc)
{
    free(doc->toks);
    memset(doc, 0, sizeof(*doc));
}

bool JsonValidUtf8(const char *src, size_t len)
{
    if (!src && len)
    {
        return false;
    }
    size_t i = 0;
    while (i < len)
    {
        unsigned char first = (unsigned char)src[i];
        unsigned cp;
        int bytes;
        if (first < 0x80)
        {
            i++;
            continue;
        }
        if (first >= 0xC2 && first <= 0xDF)
        {
            cp = first & 0x1F;
            bytes = 2;
        }
        else if (first >= 0xE0 && first <= 0xEF)
        {
            cp = first & 0x0F;
            bytes = 3;
        }
        else if (first >= 0xF0 && first <= 0xF4)
        {
            cp = first & 0x07;
            bytes = 4;
        }
        else
        {
            return false;
        }
        if (i + (size_t)bytes > len)
        {
            return false;
        }
        for (int j = 1; j < bytes; j++)
        {
            unsigned char next = (unsigned char)src[i + (size_t)j];
            if ((next & 0xC0) != 0x80)
            {
                return false;
            }
            cp = (cp << 6) | (next & 0x3F);
        }
        if ((bytes == 3 && cp < 0x800) || (bytes == 4 && cp < 0x10000) ||
            (cp >= 0xD800 && cp <= 0xDFFF) || cp > 0x10FFFF)
        {
            return false;
        }
        i += (size_t)bytes;
    }
    return true;
}

static void StrictSkipWs(const char *src, size_t len, size_t *at)
{
    while (*at < len && (src[*at] == ' ' || src[*at] == '\t' || src[*at] == '\r' || src[*at] == '\n'))
    {
        (*at)++;
    }
}

static bool StrictString(const char *src, size_t len, size_t *at)
{
    if (*at >= len || src[(*at)++] != '"') return false;
    while (*at < len)
    {
        unsigned char c = (unsigned char)src[(*at)++];
        if (c == '"') return true;
        if (c < 0x20) return false;
        if (c != '\\') continue;
        if (*at >= len) return false;
        char esc = src[(*at)++];
        if (esc == '"' || esc == '\\' || esc == '/' || esc == 'b' || esc == 'f' ||
            esc == 'n' || esc == 'r' || esc == 't') continue;
        if (esc != 'u' || *at + 4 > len) return false;
        for (int i = 0; i < 4; i++)
        {
            char h = src[(*at)++];
            if (!((h >= '0' && h <= '9') || (h >= 'a' && h <= 'f') || (h >= 'A' && h <= 'F')))
                return false;
        }
    }
    return false;
}

static bool StrictNumber(const char *src, size_t len, size_t *at)
{
    size_t i = *at;
    if (i < len && src[i] == '-') i++;
    if (i >= len) return false;
    if (src[i] == '0') i++;
    else
    {
        if (src[i] < '1' || src[i] > '9') return false;
        while (i < len && src[i] >= '0' && src[i] <= '9') i++;
    }
    if (i < len && src[i] == '.')
    {
        i++;
        if (i >= len || src[i] < '0' || src[i] > '9') return false;
        while (i < len && src[i] >= '0' && src[i] <= '9') i++;
    }
    if (i < len && (src[i] == 'e' || src[i] == 'E'))
    {
        i++;
        if (i < len && (src[i] == '+' || src[i] == '-')) i++;
        if (i >= len || src[i] < '0' || src[i] > '9') return false;
        while (i < len && src[i] >= '0' && src[i] <= '9') i++;
    }
    *at = i;
    return true;
}

static bool StrictValue(const char *src, size_t len, size_t *at, int depth)
{
    if (depth > 256) return false;
    StrictSkipWs(src, len, at);
    if (*at >= len) return false;
    if (src[*at] == '"') return StrictString(src, len, at);
    if (src[*at] == '{')
    {
        (*at)++; StrictSkipWs(src, len, at);
        if (*at < len && src[*at] == '}') { (*at)++; return true; }
        for (;;)
        {
            if (!StrictString(src, len, at)) return false;
            StrictSkipWs(src, len, at);
            if (*at >= len || src[(*at)++] != ':') return false;
            if (!StrictValue(src, len, at, depth + 1)) return false;
            StrictSkipWs(src, len, at);
            if (*at < len && src[*at] == '}') { (*at)++; return true; }
            if (*at >= len || src[(*at)++] != ',') return false;
            StrictSkipWs(src, len, at);
        }
    }
    if (src[*at] == '[')
    {
        (*at)++; StrictSkipWs(src, len, at);
        if (*at < len && src[*at] == ']') { (*at)++; return true; }
        for (;;)
        {
            if (!StrictValue(src, len, at, depth + 1)) return false;
            StrictSkipWs(src, len, at);
            if (*at < len && src[*at] == ']') { (*at)++; return true; }
            if (*at >= len || src[(*at)++] != ',') return false;
        }
    }
    static const char *literals[] = {"true", "false", "null"};
    for (int i = 0; i < 3; i++)
    {
        size_t n = strlen(literals[i]);
        if (*at + n <= len && memcmp(src + *at, literals[i], n) == 0)
        {
            *at += n;
            return true;
        }
    }
    return StrictNumber(src, len, at);
}

bool JsonValidSyntax(const char *src, size_t len)
{
    if (!src || !JsonValidUtf8(src, len)) return false;
    size_t at = 0;
    if (!StrictValue(src, len, &at, 0)) return false;
    StrictSkipWs(src, len, &at);
    return at == len;
}

int JsonTokStart(const JsonDoc *doc, int tok)
{
    if (!doc || tok < 0 || tok >= doc->ntoks)
    {
        return -1;
    }
    return Toks(doc)[tok].start;
}

int JsonTokEnd(const JsonDoc *doc, int tok)
{
    if (!doc || tok < 0 || tok >= doc->ntoks)
    {
        return -1;
    }
    return Toks(doc)[tok].end;
}

int JsonSkip(const JsonDoc *doc, int tok)
{
    if (!doc || tok < 0 || tok >= doc->ntoks)
    {
        return tok + 1;
    }
    jsmntok_t *t = &Toks(doc)[tok];
    if (t->type == JSMN_OBJECT)
    {
        int i = tok + 1;
        for (int k = 0; k < t->size; k++)
        {
            i = JsonSkip(doc, i);
            i = JsonSkip(doc, i);
        }
        return i;
    }
    if (t->type == JSMN_ARRAY)
    {
        int i = tok + 1;
        for (int k = 0; k < t->size; k++)
        {
            i = JsonSkip(doc, i);
        }
        return i;
    }
    return tok + 1;
}

static bool TokEq(const JsonDoc *doc, const jsmntok_t *t, const char *s)
{
    size_t n = (size_t)(t->end - t->start);
    return strlen(s) == n && memcmp(doc->src + t->start, s, n) == 0;
}

int JsonObjGet(const JsonDoc *doc, int obj, const char *key)
{
    if (!doc || obj < 0 || obj >= doc->ntoks || Toks(doc)[obj].type != JSMN_OBJECT)
    {
        return -1;
    }
    int i = obj + 1;
    int n = Toks(doc)[obj].size;
    for (int k = 0; k < n; k++)
    {
        int key_tok = i;
        i = JsonSkip(doc, i);
        int val_tok = i;
        i = JsonSkip(doc, i);
        if (key_tok >= 0 && key_tok < doc->ntoks && Toks(doc)[key_tok].type == JSMN_STRING &&
            TokEq(doc, &Toks(doc)[key_tok], key))
        {
            return val_tok;
        }
    }
    return -1;
}

int JsonObjLen(const JsonDoc *doc, int obj)
{
    if (!doc || obj < 0 || obj >= doc->ntoks || Toks(doc)[obj].type != JSMN_OBJECT)
    {
        return 0;
    }
    return Toks(doc)[obj].size;
}

bool JsonObjPair(const JsonDoc *doc, int obj, int index, int *key_tok, int *val_tok)
{
    if (index < 0 || index >= JsonObjLen(doc, obj))
    {
        return false;
    }
    int i = obj + 1;
    for (int k = 0; k < index; k++)
    {
        i = JsonSkip(doc, i);
        i = JsonSkip(doc, i);
    }
    if (key_tok)
    {
        *key_tok = i;
    }
    i = JsonSkip(doc, i);
    if (val_tok)
    {
        *val_tok = i;
    }
    return true;
}

int JsonArrayLen(const JsonDoc *doc, int arr)
{
    if (!doc || arr < 0 || arr >= doc->ntoks || Toks(doc)[arr].type != JSMN_ARRAY)
    {
        return 0;
    }
    return Toks(doc)[arr].size;
}

int JsonArrayAt(const JsonDoc *doc, int arr, int index)
{
    if (index < 0 || index >= JsonArrayLen(doc, arr))
    {
        return -1;
    }
    int i = arr + 1;
    for (int k = 0; k < index; k++)
    {
        i = JsonSkip(doc, i);
    }
    return i;
}

bool JsonEq(const JsonDoc *doc, int tok, const char *s)
{
    if (!doc || tok < 0 || tok >= doc->ntoks || !s)
    {
        return false;
    }
    return TokEq(doc, &Toks(doc)[tok], s);
}

static int HexVal(char c)
{
    if (c >= '0' && c <= '9')
    {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f')
    {
        return c - 'a' + 10;
    }
    if (c >= 'A' && c <= 'F')
    {
        return c - 'A' + 10;
    }
    return -1;
}

char *JsonStrDup(const JsonDoc *doc, int tok)
{
    if (!doc || tok < 0 || tok >= doc->ntoks)
    {
        return NULL;
    }
    jsmntok_t *t = &Toks(doc)[tok];
    if (t->type != JSMN_STRING)
    {
        if (t->type == JSMN_PRIMITIVE)
        {
            /* A JSON null is an absent value, not the four characters "null". */
            return TokEq(doc, t, "null") ? NULL : JsonRawDup(doc, tok);
        }
        return NULL;
    }
    const char *s = doc->src + t->start;
    int n = t->end - t->start;
    JsonBuf b;
    JsonBuf_Init(&b);
    for (int i = 0; i < n; i++)
    {
        if (s[i] == '\\' && i + 1 < n)
        {
            i++;
            switch (s[i])
            {
            case '"':
            case '\\':
            case '/':
                JsonBuf_Putc(&b, s[i]);
                break;
            case 'b':
                JsonBuf_Putc(&b, '\b');
                break;
            case 'f':
                JsonBuf_Putc(&b, '\f');
                break;
            case 'n':
                JsonBuf_Putc(&b, '\n');
                break;
            case 'r':
                JsonBuf_Putc(&b, '\r');
                break;
            case 't':
                JsonBuf_Putc(&b, '\t');
                break;
            case 'u':
                if (i + 4 < n)
                {
                    int cp = 0;
                    int ok = 1;
                    for (int h = 0; h < 4; h++)
                    {
                        int v = HexVal(s[i + 1 + h]);
                        if (v < 0)
                        {
                            ok = 0;
                            break;
                        }
                        cp = (cp << 4) | v;
                    }
                    if (ok)
                    {
                        i += 4;
                        if (cp < 0x80)
                        {
                            JsonBuf_Putc(&b, (char)cp);
                        }
                        else if (cp < 0x800)
                        {
                            JsonBuf_Putc(&b, (char)(0xC0 | (cp >> 6)));
                            JsonBuf_Putc(&b, (char)(0x80 | (cp & 0x3F)));
                        }
                        else
                        {
                            JsonBuf_Putc(&b, (char)(0xE0 | (cp >> 12)));
                            JsonBuf_Putc(&b, (char)(0x80 | ((cp >> 6) & 0x3F)));
                            JsonBuf_Putc(&b, (char)(0x80 | (cp & 0x3F)));
                        }
                        break;
                    }
                }
                JsonBuf_Putc(&b, 'u');
                break;
            default:
                JsonBuf_Putc(&b, s[i]);
                break;
            }
        }
        else
        {
            JsonBuf_Putc(&b, s[i]);
        }
    }
    return JsonBuf_Steal(&b);
}

char *JsonRawDup(const JsonDoc *doc, int tok)
{
    if (!doc || tok < 0 || tok >= doc->ntoks)
    {
        return NULL;
    }
    jsmntok_t *t = &Toks(doc)[tok];
    int n = t->end - t->start;
    if (n < 0)
    {
        return NULL;
    }
    char *d = (char *)malloc((size_t)n + 1);
    if (!d)
    {
        return NULL;
    }
    memcpy(d, doc->src + t->start, (size_t)n);
    d[n] = '\0';
    return d;
}

int JsonInt(const JsonDoc *doc, int tok, int fallback)
{
    char *s = JsonRawDup(doc, tok);
    if (!s)
    {
        return fallback;
    }
    int v = atoi(s);
    free(s);
    return v;
}

char *JsonObjStr(const JsonDoc *doc, int obj, const char *key)
{
    return JsonStrDup(doc, JsonObjGet(doc, obj, key));
}

char *JsonObjRaw(const JsonDoc *doc, int obj, const char *key)
{
    return JsonRawDup(doc, JsonObjGet(doc, obj, key));
}

int JsonObjInt(const JsonDoc *doc, int obj, const char *key, int fallback)
{
    return JsonInt(doc, JsonObjGet(doc, obj, key), fallback);
}

bool JsonIsObject(const JsonDoc *doc, int tok)
{
    return doc && tok >= 0 && tok < doc->ntoks && Toks(doc)[tok].type == JSMN_OBJECT;
}

bool JsonIsArray(const JsonDoc *doc, int tok)
{
    return doc && tok >= 0 && tok < doc->ntoks && Toks(doc)[tok].type == JSMN_ARRAY;
}

bool JsonIsNull(const JsonDoc *doc, int tok)
{
    return doc && tok >= 0 && tok < doc->ntoks && Toks(doc)[tok].type == JSMN_PRIMITIVE &&
           TokEq(doc, &Toks(doc)[tok], "null");
}
