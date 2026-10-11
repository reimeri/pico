#include "session_ref.h"

#include <ctype.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define REF_SCHEME "pico-session:1:"

static bool Safe(unsigned char c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
           (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == '~';
}

static char *Encode(char *p, const char *s)
{
    static const char hex[] = "0123456789ABCDEF";
    while (*s)
    {
        unsigned char c = (unsigned char)*s++;
        if (Safe(c)) *p++ = (char)c;
        else { *p++ = '%'; *p++ = hex[c >> 4]; *p++ = hex[c & 15]; }
    }
    return p;
}

static int Hex(unsigned char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    return -1;
}

static bool Decode(const char *s, int n, char *out, size_t cap)
{
    size_t used = 0;
    for (int i = 0; i < n; i++)
    {
        unsigned char c = (unsigned char)s[i];
        if (c == '%')
        {
            if (i + 2 >= n || Hex((unsigned char)s[i+1]) < 0 || Hex((unsigned char)s[i+2]) < 0) return false;
            c = (unsigned char)(Hex((unsigned char)s[i+1]) * 16 + Hex((unsigned char)s[i+2]));
            i += 2;
        }
        else if (!Safe(c)) return false;
        if (!c || used + 1 >= cap) return false;
        out[used++] = (char)c;
    }
    out[used] = '\0';
    return used > 0;
}

char *PicoSessionRef_Serialize(const PicoSessionRef *ref)
{
    if (!ref || !ref->title[0] || !ref->id[0] || ref->checkout[0] != '/' || ref->path[0] != '/') return NULL;
    size_t cap = strlen(ref->title) * 2 +
                 (strlen(ref->id) + strlen(ref->checkout) + strlen(ref->path)) * 3 + 40;
    char *text = malloc(cap);
    if (!text) return NULL;
    char *p = text;
    *p++ = '['; *p++ = '#';
    for (const char *s = ref->title; *s; s++)
    {
        if (*s == '\n' || *s == '\r') { *p++ = ' '; continue; }
        if (*s == '\\' || *s == '[' || *s == ']' || *s == '*' || *s == '_' || *s == '`' || *s == '&' || *s == '~' || *s == '<' || *s == '>') *p++ = '\\';
        *p++ = *s;
    }
    memcpy(p, "](" REF_SCHEME, strlen("](" REF_SCHEME)); p += strlen("](" REF_SCHEME);
    p = Encode(p, ref->id); *p++ = ':';
    p = Encode(p, ref->checkout); *p++ = ':';
    p = Encode(p, ref->path); *p++ = ')'; *p = '\0';
    return text;
}

bool PicoSessionRef_FromLink(const char *label, int length, const char *url, PicoSessionRef *out)
{
    if (!out || !label || length < 2 || label[0] != '#' || length > (int)sizeof(out->title) ||
        !url || strncmp(url, REF_SCHEME, strlen(REF_SCHEME))) return false;
    const char *a = url + strlen(REF_SCHEME), *b = strchr(a, ':'), *c = b ? strchr(b + 1, ':') : NULL;
    if (!b || !c || strchr(c + 1, ':')) return false;
    if (!Decode(a, (int)(b-a), out->id, sizeof(out->id)) ||
        !Decode(b+1, (int)(c-b-1), out->checkout, sizeof(out->checkout)) ||
        !Decode(c+1, (int)strlen(c+1), out->path, sizeof(out->path)) ||
        out->checkout[0] != '/' || out->path[0] != '/') return false;
    memcpy(out->title, label+1, (size_t)length-1); out->title[length-1] = '\0';
    return true;
}

bool PicoSessionRef_Parse(const char *text, int length, int at, PicoSessionRef *out)
{
    if (!text || !out || at < 0 || length < 3 || at > length-3 || text[at] != '[' || text[at+1] != '#') return false;
    char label[290]; int used = 0, i = at + 1;
    while (i < length && text[i] != ']')
    {
        char c = text[i++];
        if (c == '\\') { if (i >= length) return false; c = text[i++]; }
        if (c == '\n' || c == '\r' || used + 1 >= (int)sizeof(label)) return false;
        label[used++] = c;
    }
    label[used] = '\0';
    if (i > length-3 || text[i] != ']' || text[i+1] != '(') return false;
    int url_start = i + 2;
    const char *close = memchr(text + url_start, ')', (size_t)(length-url_start));
    if (!close) return false;
    int url_len = (int)(close - text - url_start);
    if (url_len < (int)strlen(REF_SCHEME) || strncmp(text+url_start, REF_SCHEME, strlen(REF_SCHEME))) return false;
    char *url = malloc((size_t)url_len+1);
    if (!url) return false;
    memcpy(url, text+url_start, (size_t)url_len); url[url_len] = '\0';
    bool ok = PicoSessionRef_FromLink(label, used, url, out);
    free(url);
    if (ok) { out->start = at; out->end = (int)(close-text)+1; }
    return ok;
}

bool PicoSessionRef_Next(const char *text, int length, int from, PicoSessionRef *out)
{
    if (!text) return false;
    for (int i = from < 0 ? 0 : from; i < length; i++)
        if (text[i] == '[' && PicoSessionRef_Parse(text, length, i, out)) return true;
    return false;
}

int PicoSessionRef_Snap(const char *text, int length, int offset, bool forward)
{
    PicoSessionRef ref;
    int from = 0;
    while (PicoSessionRef_Next(text, length, from, &ref))
    {
        if (offset <= ref.start) break;
        if (offset < ref.end) return forward ? ref.end : ref.start;
        from = ref.end;
    }
    return offset;
}

char *PicoSessionRef_Project(const char *text)
{
    if (!text) text = "";
    size_t len = strlen(text);
    if (len > INT_MAX) return NULL;
    size_t capacity = len+1;
    PicoSessionRef sizing;
    int scan = 0;
    while (PicoSessionRef_Next(text,(int)len,scan,&sizing))
    {
        size_t extra = 24;
        const char *fields[] = {sizing.title,sizing.path};
        for (int k=0; k<2; k++)
            for (const unsigned char *s=(const unsigned char *)fields[k]; *s; s++)
                extra += *s < 32 ? 6 : (*s == '\\' || *s == '"') ? 2 : 1;
        if (capacity > SIZE_MAX-extra) return NULL;
        capacity += extra;
        scan = sizing.end;
    }
    char *result = malloc(capacity);
    if (!result) return NULL;
    char *p = result;
    int from = 0;
    PicoSessionRef ref;
    while (PicoSessionRef_Next(text, (int)len, from, &ref))
    {
        memcpy(p, text+from, (size_t)(ref.start-from)); p += ref.start-from;
        memcpy(p, "Session \"", 9); p += 9;
        const char *fields[] = {ref.title, ref.path};
        for (int k = 0; k < 2; k++)
        {
            for (const char *s = fields[k]; *s; s++)
            {
                if (*s == '\\' || *s == '"') *p++ = '\\';
                if ((unsigned char)*s < 32) { p += sprintf(p, "\\u%04x", (unsigned char)*s); }
                else *p++ = *s;
            }
            const char *tail = k == 0 ? "\" (JSONL): \"" : "\"";
            size_t n = strlen(tail); memcpy(p, tail, n); p += n;
        }
        from = ref.end;
    }
    memcpy(p, text+from, len-(size_t)from+1);
    return result;
}
