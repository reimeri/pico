#ifndef PICO_JSON_H
#define PICO_JSON_H

#include <stdbool.h>
#include <stddef.h>

typedef struct JsonBuf {
    char *data;
    size_t len;
    size_t cap;
} JsonBuf;

void JsonBuf_Init(JsonBuf *b);
void JsonBuf_Free(JsonBuf *b);
void JsonBuf_Clear(JsonBuf *b);
void JsonBuf_Append(JsonBuf *b, const char *s, size_t n);
void JsonBuf_Puts(JsonBuf *b, const char *s);
void JsonBuf_Putc(JsonBuf *b, char c);
void JsonBuf_String(JsonBuf *b, const char *s);
void JsonBuf_Int(JsonBuf *b, int v);
void JsonBuf_Bool(JsonBuf *b, bool v);
char *JsonBuf_Steal(JsonBuf *b);

typedef struct JsonDoc {
    const char *src;
    size_t len;
    void *toks;
    int ntoks;
} JsonDoc;

int JsonParse(JsonDoc *doc, const char *src, size_t len);
void JsonFree(JsonDoc *doc);
/* Blank line and block comments to spaces, leaving strings untouched. */
void JsonStripComments(char *src, size_t len);
int JsonSkip(const JsonDoc *doc, int tok);
int JsonObjGet(const JsonDoc *doc, int obj, const char *key);
int JsonObjLen(const JsonDoc *doc, int obj);
bool JsonObjPair(const JsonDoc *doc, int obj, int index, int *key_tok, int *val_tok);
int JsonArrayLen(const JsonDoc *doc, int arr);
int JsonArrayAt(const JsonDoc *doc, int arr, int index);
bool JsonEq(const JsonDoc *doc, int tok, const char *s);
char *JsonStrDup(const JsonDoc *doc, int tok);
char *JsonRawDup(const JsonDoc *doc, int tok);
int JsonInt(const JsonDoc *doc, int tok, int fallback);
char *JsonObjStr(const JsonDoc *doc, int obj, const char *key);
char *JsonObjRaw(const JsonDoc *doc, int obj, const char *key);
int JsonObjInt(const JsonDoc *doc, int obj, const char *key, int fallback);
bool JsonIsObject(const JsonDoc *doc, int tok);
bool JsonIsArray(const JsonDoc *doc, int tok);

int JsonTokStart(const JsonDoc *doc, int tok);
int JsonTokEnd(const JsonDoc *doc, int tok);
char *JsonDup(const char *s);
char *Pico_ReadFile(const char *path, size_t *out_len);

#endif
