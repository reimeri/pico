#include "file_mention.h"
#include "json.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>

static bool Bare(unsigned char c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
           (c >= '0' && c <= '9') || c == '_' || c == '-' || c == '.' || c == '/';
}

bool PicoFileMention_Encode(const char *path, char *out, size_t cap)
{
    if (!path || !path[0] || !out || !cap || !JsonValidUtf8(path, strlen(path))) return false;
    bool bare = true;
    for (const unsigned char *p = (const unsigned char *)path; *p; p++)
        if (!Bare(*p)) bare = false;
    JsonBuf b; JsonBuf_Init(&b);
    JsonBuf_Putc(&b, '@');
    if (bare) JsonBuf_Puts(&b, path);
    else JsonBuf_String(&b, path);
    bool ok = !b.failed && b.len < cap;
    if (ok) memcpy(out, b.data, b.len + 1);
    else out[0] = '\0';
    JsonBuf_Free(&b);
    return ok;
}

int PicoFileMention_QuotedEnd(const char *text, int len, int start, bool *closed)
{
    *closed = false;
    for (int i = start + 2; i < len; i++)
    {
        if (text[i] == '\\' && i + 1 < len) { i++; continue; }
        if (text[i] == '"') { *closed = true; return i + 1; }
    }
    return len;
}

static bool Decode(const char *text, size_t len, char *out, size_t cap)
{
    /* NUL is not a filename byte. Reject it before JSON decoding can hide a
     * suffix behind a C-string terminator and resolve a different prefix file. */
    for (size_t i = 1; i + 1 < len; i++)
    {
        if (text[i] != '\\') continue;
        if (i + 5 < len && !memcmp(text + i + 1, "u0000", 5)) return false;
        i++;
    }
    JsonDoc doc = {0};
    bool ok = len >= 2 && text[0] == '"' && JsonValidSyntax(text, len) &&
              JsonParse(&doc, text, len) == 0;
    char *decoded = ok ? JsonStrDup(&doc, 0) : NULL;
    ok = decoded && strlen(decoded) < cap && JsonValidUtf8(decoded, strlen(decoded));
    if (ok) memcpy(out, decoded, strlen(decoded) + 1);
    JsonFree(&doc);
    free(decoded);
    return ok;
}

bool PicoFileMention_Query(const char *prefix, char *out, size_t cap)
{
    if (!prefix) prefix = "";
    if (prefix[0] != '"')
    {
        size_t len = strlen(prefix);
        if (len >= cap) return false;
        memcpy(out, prefix, len + 1);
        return true;
    }
    /* Prefixes contain the opening quote but may not yet contain its close. */
    size_t len = strlen(prefix);
    bool closed = false;
    for (size_t i = 1; i < len; i++)
    {
        if (prefix[i] == '\\' && i + 1 < len) { i++; continue; }
        if (prefix[i] == '"') { closed = i + 1 == len; break; }
    }
    if (closed) return Decode(prefix, len, out, cap);
    JsonBuf b; JsonBuf_Init(&b);
    JsonBuf_Puts(&b, prefix); JsonBuf_Putc(&b, '"');
    bool ok = !b.failed && Decode(b.data, b.len, out, cap);
    JsonBuf_Free(&b);
    return ok;
}

bool PicoFileMention_Parse(const char *text, int len, int start, int *end,
                            char *path, size_t cap)
{
    *end = start + 1;
    if (start < 0 || start >= len || text[start] != '@') return false;
    if (start + 1 < len && text[start + 1] == '"')
    {
        bool closed;
        *end = PicoFileMention_QuotedEnd(text, len, start, &closed);
        if (!closed) return false;
        /* No silently ignored suffix after the closing quote. */
        if (*end < len && !isspace((unsigned char)text[*end]))
        {
            while (*end < len && !isspace((unsigned char)text[*end])) (*end)++;
            return false;
        }
        return Decode(text + start + 1, (size_t)(*end - start - 1), path, cap) && path[0];
    }
    bool valid = true;
    while (*end < len && !isspace((unsigned char)text[*end]))
    {
        if (!Bare((unsigned char)text[*end])) valid = false;
        (*end)++;
    }
    size_t size = (size_t)(*end - start - 1);
    if (!valid || !size || size >= cap) return false;
    memcpy(path, text + start + 1, size); path[size] = '\0';
    return true;
}
