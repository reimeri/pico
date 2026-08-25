#define _POSIX_C_SOURCE 200809L

#include "canonical.h"

#include "path.h"

#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

static const char kB64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

static void MkdirP(const char *path)
{
    char buf[4096];
    snprintf(buf, sizeof(buf), "%s", path ? path : "");
    for (char *p = buf + 1; *p; p++)
    {
        if (*p == '/')
        {
            *p = '\0';
            if (mkdir(buf, 0755) != 0 && errno != EEXIST)
            {
            }
            *p = '/';
        }
    }
    if (buf[0] && mkdir(buf, 0755) != 0 && errno != EEXIST)
    {
    }
}

static const char *Ext(const char *path)
{
    if (!path)
    {
        return "";
    }
    const char *slash = strrchr(path, '/');
    const char *base = slash ? slash + 1 : path;
    const char *dot = strrchr(base, '.');
    return dot ? dot + 1 : "";
}

static int ExtEq(const char *path, const char *ext)
{
    const char *have = Ext(path);
    while (*ext && *have)
    {
        if (tolower((unsigned char)*ext) != tolower((unsigned char)*have))
        {
            return 0;
        }
        ext++;
        have++;
    }
    return *ext == 0 && *have == 0;
}

bool pico_canonical_is_image_path(const char *path)
{
    return ExtEq(path, "png") || ExtEq(path, "jpg") || ExtEq(path, "jpeg") || ExtEq(path, "gif") ||
           ExtEq(path, "webp") || ExtEq(path, "bmp");
}

bool pico_canonical_is_audio_path(const char *path)
{
    return ExtEq(path, "wav") || ExtEq(path, "mp3") || ExtEq(path, "mpeg") || ExtEq(path, "m4a") ||
           ExtEq(path, "ogg") || ExtEq(path, "flac") || ExtEq(path, "aac") || ExtEq(path, "webm");
}

const char *pico_canonical_mime_for_path(const char *path)
{
    if (ExtEq(path, "png"))
    {
        return "image/png";
    }
    if (ExtEq(path, "jpg") || ExtEq(path, "jpeg"))
    {
        return "image/jpeg";
    }
    if (ExtEq(path, "gif"))
    {
        return "image/gif";
    }
    if (ExtEq(path, "webp"))
    {
        return "image/webp";
    }
    if (ExtEq(path, "bmp"))
    {
        return "image/bmp";
    }
    if (ExtEq(path, "wav"))
    {
        return "audio/wav";
    }
    if (ExtEq(path, "mp3") || ExtEq(path, "mpeg"))
    {
        return "audio/mpeg";
    }
    if (ExtEq(path, "m4a") || ExtEq(path, "aac"))
    {
        return "audio/aac";
    }
    if (ExtEq(path, "ogg"))
    {
        return "audio/ogg";
    }
    if (ExtEq(path, "flac"))
    {
        return "audio/flac";
    }
    if (ExtEq(path, "webm"))
    {
        return "audio/webm";
    }
    return "application/octet-stream";
}

char *pico_canonical_audio_format(const char *path, const char *mime)
{
    if (ExtEq(path, "wav") || (mime && strstr(mime, "wav")))
    {
        return JsonDup("wav");
    }
    if (ExtEq(path, "mp3") || ExtEq(path, "mpeg") || (mime && strstr(mime, "mpeg")))
    {
        return JsonDup("mp3");
    }
    return JsonDup("wav");
}

static char *B64Encode(const unsigned char *data, size_t n)
{
    size_t out_n = ((n + 2) / 3) * 4;
    char *out = (char *)malloc(out_n + 1);
    if (!out)
    {
        return NULL;
    }
    size_t j = 0;
    for (size_t i = 0; i < n; i += 3)
    {
        unsigned a = data[i];
        unsigned b = i + 1 < n ? data[i + 1] : 0;
        unsigned c = i + 2 < n ? data[i + 2] : 0;
        unsigned triple = (a << 16) | (b << 8) | c;
        out[j++] = kB64[(triple >> 18) & 63];
        out[j++] = kB64[(triple >> 12) & 63];
        out[j++] = i + 1 < n ? kB64[(triple >> 6) & 63] : '=';
        out[j++] = i + 2 < n ? kB64[triple & 63] : '=';
    }
    out[j] = '\0';
    return out;
}

static int B64Val(int c)
{
    if (c >= 'A' && c <= 'Z')
    {
        return c - 'A';
    }
    if (c >= 'a' && c <= 'z')
    {
        return c - 'a' + 26;
    }
    if (c >= '0' && c <= '9')
    {
        return c - '0' + 52;
    }
    if (c == '+')
    {
        return 62;
    }
    if (c == '/')
    {
        return 63;
    }
    return -1;
}

static unsigned char *B64Decode(const char *s, size_t *out_n)
{
    if (out_n)
    {
        *out_n = 0;
    }
    if (!s)
    {
        return NULL;
    }
    size_t len = strlen(s);
    if (len == 0 || len % 4 != 0)
    {
        return NULL;
    }
    size_t padding = (s[len - 1] == '=') + (len > 1 && s[len - 2] == '=');
    size_t size = (len / 4) * 3 - padding;
    unsigned char *out = (unsigned char *)malloc(size + 1);
    if (!out)
    {
        return NULL;
    }
    size_t n = 0;
    for (size_t i = 0; i < len; i += 4)
    {
        int a = B64Val((unsigned char)s[i]);
        int b = B64Val((unsigned char)s[i + 1]);
        int c = s[i + 2] == '=' ? 0 : B64Val((unsigned char)s[i + 2]);
        int d = s[i + 3] == '=' ? 0 : B64Val((unsigned char)s[i + 3]);
        bool last = i + 4 == len;
        bool valid_padding = (!last && s[i + 2] != '=' && s[i + 3] != '=') ||
                             (last && (s[i + 2] != '=' || s[i + 3] == '='));
        if (a < 0 || b < 0 || c < 0 || d < 0 || !valid_padding)
        {
            free(out);
            return NULL;
        }
        unsigned value = ((unsigned)a << 18) | ((unsigned)b << 12) |
                         ((unsigned)c << 6) | (unsigned)d;
        if (n < size) out[n++] = (unsigned char)((value >> 16) & 0xFF);
        if (n < size) out[n++] = (unsigned char)((value >> 8) & 0xFF);
        if (n < size) out[n++] = (unsigned char)(value & 0xFF);
    }
    out[n] = 0;
    if (out_n)
    {
        *out_n = n;
    }
    return out;
}

char *pico_canonical_file_base64(const char *path, size_t *out_len)
{
    size_t n = 0;
    char *raw = Pico_ReadFile(path, &n);
    if (!raw)
    {
        return NULL;
    }
    char *b64 = B64Encode((const unsigned char *)raw, n);
    free(raw);
    if (out_len)
    {
        *out_len = n;
    }
    return b64;
}

char *pico_canonical_data_url(const char *path, const char *mime)
{
    char *b64 = pico_canonical_file_base64(path, NULL);
    if (!b64)
    {
        return NULL;
    }
    const char *use_mime = mime && mime[0] ? mime : pico_canonical_mime_for_path(path);
    JsonBuf b;
    JsonBuf_Init(&b);
    JsonBuf_Puts(&b, "data:");
    JsonBuf_Puts(&b, use_mime);
    JsonBuf_Puts(&b, ";base64,");
    JsonBuf_Puts(&b, b64);
    free(b64);
    return JsonBuf_Steal(&b);
}

char *pico_canonical_persist_bytes(const char *dir, const char *ext, const void *data, size_t n)
{
    if (!dir || !dir[0] || !data || n == 0)
    {
        return NULL;
    }
    MkdirP(dir);
    char path[4096];
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    const char *use_ext = ext && ext[0] ? ext : "bin";
    if (!PicoPath_Format(path, sizeof(path), "%s/part-%ld-%ld.%s", dir, (long)ts.tv_sec,
                         (long)ts.tv_nsec, use_ext))
    {
        return NULL;
    }
    FILE *f = fopen(path, "wb");
    if (!f)
    {
        return NULL;
    }
    size_t wrote = fwrite(data, 1, n, f);
    fclose(f);
    if (wrote != n)
    {
        return NULL;
    }
    return JsonDup(path);
}

char *pico_canonical_persist_data_url(const char *dir, const char *data_url)
{
    if (!data_url)
    {
        return NULL;
    }
    const char *comma = strchr(data_url, ',');
    const char *base64 = strstr(data_url, ";base64");
    if (!comma || strncmp(data_url, "data:", 5) != 0 || !base64 || base64 + 7 != comma)
    {
        return NULL;
    }
    const char *ext = "bin";
    if (strstr(data_url, "image/png"))
    {
        ext = "png";
    }
    else if (strstr(data_url, "image/jpeg"))
    {
        ext = "jpg";
    }
    else if (strstr(data_url, "image/gif"))
    {
        ext = "gif";
    }
    else if (strstr(data_url, "image/webp"))
    {
        ext = "webp";
    }
    else if (strstr(data_url, "audio/wav") || strstr(data_url, "audio/x-wav"))
    {
        ext = "wav";
    }
    else if (strstr(data_url, "audio/mpeg"))
    {
        ext = "mp3";
    }
    size_t n = 0;
    unsigned char *bytes = B64Decode(comma + 1, &n);
    if (!bytes)
    {
        return NULL;
    }
    char *path = pico_canonical_persist_bytes(dir, ext, bytes, n);
    free(bytes);
    return path;
}

void pico_canonical_free_parts(PicoLlmPart *parts, int n)
{
    for (int i = 0; i < n; i++)
    {
        free(parts[i].text);
        free(parts[i].path);
        free(parts[i].url);
        free(parts[i].mime);
    }
    free(parts);
}

static PicoLlmPartKind PartKindFromType(const char *type)
{
    if (!type || strcmp(type, "text") == 0)
    {
        return PICO_LLM_PART_TEXT;
    }
    if (strcmp(type, "refusal") == 0)
    {
        return PICO_LLM_PART_REFUSAL;
    }
    if (strcmp(type, "image") == 0)
    {
        return PICO_LLM_PART_IMAGE;
    }
    if (strcmp(type, "audio") == 0)
    {
        return PICO_LLM_PART_AUDIO;
    }
    return (PicoLlmPartKind)-1;
}

static const char *PartTypeName(PicoLlmPartKind kind)
{
    switch (kind)
    {
    case PICO_LLM_PART_REFUSAL:
        return "refusal";
    case PICO_LLM_PART_IMAGE:
        return "image";
    case PICO_LLM_PART_AUDIO:
        return "audio";
    case PICO_LLM_PART_TEXT:
    default:
        return "text";
    }
}

static void AppendPartJson(JsonBuf *b, const PicoLlmPart *part)
{
    JsonBuf_Puts(b, "{\"type\":");
    JsonBuf_String(b, PartTypeName(part->kind));
    if (part->kind == PICO_LLM_PART_TEXT || part->kind == PICO_LLM_PART_REFUSAL)
    {
        JsonBuf_Puts(b, ",\"text\":");
        JsonBuf_String(b, part->text ? part->text : "");
    }
    if (part->path && part->path[0])
    {
        JsonBuf_Puts(b, ",\"path\":");
        JsonBuf_String(b, part->path);
    }
    if (part->url && part->url[0])
    {
        JsonBuf_Puts(b, ",\"url\":");
        JsonBuf_String(b, part->url);
    }
    if (part->mime && part->mime[0])
    {
        JsonBuf_Puts(b, ",\"mime\":");
        JsonBuf_String(b, part->mime);
    }
    JsonBuf_Putc(b, '}');
}

char *pico_canonical_parts_json(const PicoLlmPart *parts, int n)
{
    JsonBuf b;
    JsonBuf_Init(&b);
    JsonBuf_Putc(&b, '[');
    for (int i = 0; i < n; i++)
    {
        if (i)
        {
            JsonBuf_Putc(&b, ',');
        }
        AppendPartJson(&b, &parts[i]);
    }
    JsonBuf_Putc(&b, ']');
    return JsonBuf_Steal(&b);
}

bool pico_canonical_parse_parts(const JsonDoc *doc, int obj, PicoLlmPart **out, int *n)
{
    if (out)
    {
        *out = NULL;
    }
    if (n)
    {
        *n = 0;
    }
    int arr = JsonObjGet(doc, obj, "parts");
    if (!JsonIsArray(doc, arr))
    {
        char *text = JsonObjStr(doc, obj, "text");
        if (!text)
        {
            return true;
        }
        PicoLlmPart *parts = (PicoLlmPart *)calloc(1, sizeof(PicoLlmPart));
        if (!parts)
        {
            free(text);
            return false;
        }
        parts[0].kind = PICO_LLM_PART_TEXT;
        parts[0].text = text;
        if (out)
        {
            *out = parts;
        }
        if (n)
        {
            *n = 1;
        }
        return true;
    }
    int count = JsonArrayLen(doc, arr);
    PicoLlmPart *parts = count > 0 ? (PicoLlmPart *)calloc((size_t)count, sizeof(PicoLlmPart)) : NULL;
    if (count > 0 && !parts)
    {
        return false;
    }
    for (int i = 0; i < count; i++)
    {
        int part = JsonArrayAt(doc, arr, i);
        char *type = JsonObjStr(doc, part, "type");
        PicoLlmPartKind kind = PartKindFromType(type);
        free(type);
        if ((int)kind < 0)
        {
            pico_canonical_free_parts(parts, i);
            return false;
        }
        parts[i].kind = kind;
        parts[i].text = JsonObjStr(doc, part, "text");
        parts[i].path = JsonObjStr(doc, part, "path");
        parts[i].url = JsonObjStr(doc, part, "url");
        parts[i].mime = JsonObjStr(doc, part, "mime");
    }
    if (out)
    {
        *out = parts;
    }
    else
    {
        pico_canonical_free_parts(parts, count);
    }
    if (n)
    {
        *n = count;
    }
    return true;
}

bool pico_canonical_normalize_user_parts(const char *json, char **canonical_out)
{
    if (canonical_out)
    {
        *canonical_out = NULL;
    }
    if (!json || !canonical_out)
    {
        return false;
    }
    JsonDoc doc;
    if (JsonParse(&doc, json, strlen(json)) != 0 || !JsonIsArray(&doc, 0))
    {
        if (doc.toks)
        {
            JsonFree(&doc);
        }
        return false;
    }
    int count = JsonArrayLen(&doc, 0);
    if (count <= 0)
    {
        JsonFree(&doc);
        return false;
    }
    PicoLlmPart *parts = (PicoLlmPart *)calloc((size_t)count, sizeof(PicoLlmPart));
    if (!parts)
    {
        JsonFree(&doc);
        return false;
    }
    bool valid = true;
    for (int i = 0; i < count && valid; i++)
    {
        int obj = JsonArrayAt(&doc, 0, i);
        if (!JsonIsObject(&doc, obj))
        {
            valid = false;
            break;
        }
        char *type = JsonObjStr(&doc, obj, "type");
        PicoLlmPartKind kind = PartKindFromType(type);
        if (!type || (kind != PICO_LLM_PART_TEXT && kind != PICO_LLM_PART_IMAGE &&
                      kind != PICO_LLM_PART_AUDIO))
        {
            free(type);
            valid = false;
            break;
        }
        free(type);
        parts[i].kind = kind;
        parts[i].text = JsonObjStr(&doc, obj, "text");
        parts[i].path = JsonObjStr(&doc, obj, "path");
        parts[i].url = JsonObjStr(&doc, obj, "url");
        parts[i].mime = JsonObjStr(&doc, obj, "mime");
        if (kind == PICO_LLM_PART_TEXT)
        {
            valid = parts[i].text != NULL;
        }
        else
        {
            bool has_source = (parts[i].path && parts[i].path[0]) ||
                              (parts[i].url && parts[i].url[0]);
            bool embeds_bytes = parts[i].url && strncmp(parts[i].url, "data:", 5) == 0;
            valid = has_source && !embeds_bytes;
        }
    }
    if (valid)
    {
        *canonical_out = pico_canonical_parts_json(parts, count);
        valid = *canonical_out != NULL;
    }
    pico_canonical_free_parts(parts, count);
    JsonFree(&doc);
    return valid;
}

char *pico_canonical_user_json(const PicoLlmPart *parts, int n)
{
    JsonBuf b;
    JsonBuf_Init(&b);
    JsonBuf_Puts(&b, "{\"type\":\"user\",\"parts\":");
    char *parts_json = pico_canonical_parts_json(parts, n);
    JsonBuf_Puts(&b, parts_json ? parts_json : "[]");
    free(parts_json);
    JsonBuf_Putc(&b, '}');
    return JsonBuf_Steal(&b);
}

char *pico_canonical_user_text(const char *text)
{
    PicoLlmPart part;
    memset(&part, 0, sizeof(part));
    part.kind = PICO_LLM_PART_TEXT;
    part.text = (char *)(text ? text : "");
    return pico_canonical_user_json(&part, 1);
}

char *pico_canonical_context_json(const PicoLlmPart *parts, int n)
{
    JsonBuf b;
    JsonBuf_Init(&b);
    JsonBuf_Puts(&b, "{\"type\":\"context\",\"parts\":");
    char *parts_json = pico_canonical_parts_json(parts, n);
    JsonBuf_Puts(&b, parts_json ? parts_json : "[]");
    free(parts_json);
    JsonBuf_Putc(&b, '}');
    return JsonBuf_Steal(&b);
}

char *pico_canonical_context_text(const char *text)
{
    PicoLlmPart part;
    memset(&part, 0, sizeof(part));
    part.kind = PICO_LLM_PART_TEXT;
    part.text = (char *)(text ? text : "");
    return pico_canonical_context_json(&part, 1);
}

char *pico_canonical_assistant_json(const PicoLlmPart *parts, int n, const char *thinking,
                                    const char *signature)
{
    JsonBuf b;
    JsonBuf_Init(&b);
    JsonBuf_Puts(&b, "{\"type\":\"assistant\",\"parts\":");
    char *parts_json = pico_canonical_parts_json(parts, n);
    JsonBuf_Puts(&b, parts_json ? parts_json : "[]");
    free(parts_json);
    if (thinking && thinking[0])
    {
        JsonBuf_Puts(&b, ",\"thinking\":");
        JsonBuf_String(&b, thinking);
    }
    if (signature && signature[0])
    {
        JsonBuf_Puts(&b, ",\"thinking_signature\":");
        JsonBuf_String(&b, signature);
    }
    JsonBuf_Putc(&b, '}');
    return JsonBuf_Steal(&b);
}

char *pico_canonical_tool_call_json(const char *call_id, const char *name, const char *arguments,
                                    const char *item_id)
{
    JsonBuf b;
    JsonBuf_Init(&b);
    JsonBuf_Puts(&b, "{\"type\":\"tool_call\",\"call_id\":");
    JsonBuf_String(&b, call_id ? call_id : "");
    JsonBuf_Puts(&b, ",\"name\":");
    JsonBuf_String(&b, name ? name : "");
    JsonBuf_Puts(&b, ",\"arguments\":");
    JsonBuf_String(&b, arguments ? arguments : "{}");
    if (item_id && item_id[0])
    {
        JsonBuf_Puts(&b, ",\"item_id\":");
        JsonBuf_String(&b, item_id);
    }
    JsonBuf_Putc(&b, '}');
    return JsonBuf_Steal(&b);
}

char *pico_canonical_tool_result_json(const char *call_id, const char *name, const char *output,
                                      bool is_error)
{
    JsonBuf b;
    JsonBuf_Init(&b);
    JsonBuf_Puts(&b, "{\"type\":\"tool_result\",\"call_id\":");
    JsonBuf_String(&b, call_id ? call_id : "");
    JsonBuf_Puts(&b, ",\"name\":");
    JsonBuf_String(&b, name ? name : "");
    JsonBuf_Puts(&b, ",\"output\":");
    JsonBuf_String(&b, output ? output : "");
    JsonBuf_Puts(&b, ",\"is_error\":");
    JsonBuf_Bool(&b, is_error);
    JsonBuf_Putc(&b, '}');
    return JsonBuf_Steal(&b);
}

char *pico_canonical_item_json(const PicoLlmItem *item)
{
    if (!item)
    {
        return JsonDup("{}");
    }
    if (item->kind == PICO_LLM_ITEM_TOOL_CALL)
    {
        return pico_canonical_tool_call_json(item->call_id, item->name, item->arguments, item->item_id);
    }
    return pico_canonical_assistant_json(item->parts, item->part_count, item->thinking,
                                         item->thinking_signature);
}

char *pico_canonical_display(const PicoLlmPart *parts, int n)
{
    JsonBuf b;
    JsonBuf_Init(&b);
    for (int i = 0; i < n; i++)
    {
        const PicoLlmPart *p = &parts[i];
        if (p->kind == PICO_LLM_PART_TEXT || p->kind == PICO_LLM_PART_REFUSAL)
        {
            if (p->text)
            {
                JsonBuf_Puts(&b, p->text);
            }
        }
        else if (p->kind == PICO_LLM_PART_IMAGE)
        {
            const char *src = p->path && p->path[0] ? p->path : (p->url ? p->url : "");
            JsonBuf_Puts(&b, "\n![image](");
            JsonBuf_Puts(&b, src);
            JsonBuf_Puts(&b, ")\n");
        }
        else if (p->kind == PICO_LLM_PART_AUDIO)
        {
            const char *src = p->path && p->path[0] ? p->path : (p->url ? p->url : "audio");
            JsonBuf_Puts(&b, "\n[audio: ");
            JsonBuf_Puts(&b, src);
            JsonBuf_Puts(&b, "]\n");
        }
    }
    return JsonBuf_Steal(&b);
}

char *pico_canonical_compact_text(const PicoLlmPart *parts, int n)
{
    JsonBuf b;
    JsonBuf_Init(&b);
    JsonBuf refusal;
    JsonBuf_Init(&refusal);
    for (int i = 0; i < n; i++)
    {
        if (parts[i].kind == PICO_LLM_PART_TEXT && parts[i].text && parts[i].text[0])
        {
            JsonBuf_Puts(&b, parts[i].text);
        }
        else if (parts[i].kind == PICO_LLM_PART_REFUSAL && parts[i].text)
        {
            JsonBuf_Puts(&refusal, parts[i].text);
        }
    }
    if (b.len)
    {
        JsonBuf_Free(&refusal);
        return JsonBuf_Steal(&b);
    }
    JsonBuf_Free(&b);
    return JsonBuf_Steal(&refusal);
}

char *pico_canonical_plain_text(const PicoLlmPart *parts, int n)
{
    JsonBuf b;
    JsonBuf_Init(&b);
    for (int i = 0; i < n; i++)
    {
        if ((parts[i].kind == PICO_LLM_PART_TEXT || parts[i].kind == PICO_LLM_PART_REFUSAL) &&
            parts[i].text)
        {
            JsonBuf_Puts(&b, parts[i].text);
        }
    }
    return JsonBuf_Steal(&b);
}

bool pico_canonical_parts_need_log(const PicoLlmPart *parts, int n)
{
    if (n <= 0)
    {
        return false;
    }
    if (n == 1 && parts[0].kind == PICO_LLM_PART_TEXT)
    {
        return false;
    }
    return true;
}

bool pico_canonical_parts_have_media(const PicoLlmPart *parts, int n)
{
    for (int i = 0; i < n; i++)
    {
        if (parts[i].kind == PICO_LLM_PART_IMAGE || parts[i].kind == PICO_LLM_PART_AUDIO)
        {
            return true;
        }
    }
    return false;
}

bool pico_canonical_json_has_media(const char *json)
{
    if (!json)
    {
        return false;
    }
    JsonDoc doc;
    if (JsonParse(&doc, json, strlen(json)) != 0)
    {
        return false;
    }
    PicoLlmPart *parts = NULL;
    int n = 0;
    bool ok = pico_canonical_parse_parts(&doc, 0, &parts, &n);
    bool media = ok && pico_canonical_parts_have_media(parts, n);
    pico_canonical_free_parts(parts, n);
    JsonFree(&doc);
    return media;
}

static void FreeLlmPart(PicoLlmPart *part)
{
    if (!part)
    {
        return;
    }
    free(part->text);
    free(part->path);
    free(part->url);
    free(part->mime);
    memset(part, 0, sizeof(*part));
}

static void FreeLlmItem(PicoLlmItem *item)
{
    if (!item)
    {
        return;
    }
    if (item->parts && item->part_count > 0)
    {
        for (int i = 0; i < item->part_count; i++)
        {
            FreeLlmPart(&item->parts[i]);
        }
    }
    free(item->parts);
    free(item->thinking);
    free(item->thinking_signature);
    free(item->call_id);
    free(item->name);
    free(item->arguments);
    free(item->item_id);
    memset(item, 0, sizeof(*item));
}

void pico_llm_result_free(PicoLlmResult *r)
{
    if (!r)
    {
        return;
    }
    free(r->error);
    if (r->items && r->item_count > 0)
    {
        for (int i = 0; i < r->item_count; i++)
        {
            FreeLlmItem(&r->items[i]);
        }
    }
    free(r->items);
    memset(r, 0, sizeof(*r));
}

PicoLlmItem *pico_llm_result_add_item(PicoLlmResult *r, PicoLlmItemKind kind)
{
    if (!r)
    {
        return NULL;
    }
    PicoLlmItem *next = (PicoLlmItem *)realloc(r->items, (size_t)(r->item_count + 1) * sizeof(PicoLlmItem));
    if (!next)
    {
        return NULL;
    }
    r->items = next;
    PicoLlmItem *item = &r->items[r->item_count];
    memset(item, 0, sizeof(*item));
    item->kind = kind;
    r->item_count++;
    return item;
}

bool pico_llm_item_add_part(PicoLlmItem *item, PicoLlmPartKind kind, const char *text, const char *path,
                            const char *url, const char *mime)
{
    if (!item)
    {
        return false;
    }
    PicoLlmPart *next =
        (PicoLlmPart *)realloc(item->parts, (size_t)(item->part_count + 1) * sizeof(PicoLlmPart));
    if (!next)
    {
        return false;
    }
    item->parts = next;
    PicoLlmPart *part = &item->parts[item->part_count];
    memset(part, 0, sizeof(*part));
    part->kind = kind;
    part->text = text ? JsonDup(text) : NULL;
    part->path = path ? JsonDup(path) : NULL;
    part->url = url ? JsonDup(url) : NULL;
    part->mime = mime ? JsonDup(mime) : NULL;
    item->part_count++;
    return true;
}

bool pico_llm_result_add_text(PicoLlmResult *r, const char *text)
{
    PicoLlmItem *item = pico_llm_result_add_item(r, PICO_LLM_ITEM_ASSISTANT);
    return item && pico_llm_item_add_part(item, PICO_LLM_PART_TEXT, text ? text : "", NULL, NULL, NULL);
}

bool pico_llm_result_add_refusal(PicoLlmResult *r, const char *text)
{
    PicoLlmItem *item = pico_llm_result_add_item(r, PICO_LLM_ITEM_ASSISTANT);
    return item && pico_llm_item_add_part(item, PICO_LLM_PART_REFUSAL, text ? text : "", NULL, NULL, NULL);
}

bool pico_llm_result_add_tool_call(PicoLlmResult *r, const char *call_id, const char *name,
                                   const char *arguments, const char *item_id)
{
    PicoLlmItem *item = pico_llm_result_add_item(r, PICO_LLM_ITEM_TOOL_CALL);
    if (!item)
    {
        return false;
    }
    item->call_id = JsonDup(call_id ? call_id : "");
    item->name = JsonDup(name ? name : "");
    item->arguments = JsonDup(arguments ? arguments : "{}");
    if (item_id && item_id[0])
    {
        item->item_id = JsonDup(item_id);
    }
    return true;
}

bool pico_llm_result_has_output(const PicoLlmResult *r)
{
    if (!r)
    {
        return false;
    }
    for (int i = 0; i < r->item_count; i++)
    {
        const PicoLlmItem *item = &r->items[i];
        if (item->kind == PICO_LLM_ITEM_TOOL_CALL)
        {
            return true;
        }
        if ((item->thinking && item->thinking[0]) || (item->thinking_signature && item->thinking_signature[0]))
        {
            return true;
        }
        for (int p = 0; p < item->part_count; p++)
        {
            const PicoLlmPart *part = &item->parts[p];
            if (part->kind == PICO_LLM_PART_TEXT || part->kind == PICO_LLM_PART_REFUSAL)
            {
                if (part->text && part->text[0])
                {
                    return true;
                }
            }
            else if (part->kind == PICO_LLM_PART_IMAGE || part->kind == PICO_LLM_PART_AUDIO)
            {
                if ((part->path && part->path[0]) || (part->url && part->url[0]))
                {
                    return true;
                }
            }
        }
    }
    return false;
}
