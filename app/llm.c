#define _POSIX_C_SOURCE 200809L

#include "llm.h"
#include "json.h"

#ifndef PICO_VERSION
#define PICO_VERSION "0.1.0"
#endif

#include <ctype.h>
#include <curl/curl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void PicoLlm_ResolveUrl(const char *base, char *out, size_t cap)
{
    const char *src = base && base[0] ? base : "https://api.openai.com/v1";
    snprintf(out, cap, "%s", src);
    size_t n = strlen(out);
    while (n > 0 && out[n - 1] == '/')
    {
        out[--n] = '\0';
    }
    size_t suffix = strlen("/responses");
    if (n >= suffix && strcmp(out + n - suffix, "/responses") == 0)
    {
        return;
    }
    snprintf(out + n, cap - n, "/responses");
}

static void ResolveRoot(const char *base, char *out, size_t cap)
{
    const char *src = base && base[0] ? base : "https://api.openai.com/v1";
    snprintf(out, cap, "%s", src);
    size_t n = strlen(out);
    while (n > 0 && out[n - 1] == '/')
    {
        out[--n] = '\0';
    }
    size_t suffix = strlen("/responses");
    if (n >= suffix && strcmp(out + n - suffix, "/responses") == 0)
    {
        out[n - suffix] = '\0';
    }
}

static int KnownContextLimit(const char *id)
{
    if (!id || !id[0])
    {
        return 0;
    }
    if (strstr(id, "gpt-4.1"))
    {
        return 1047576;
    }
    if (strstr(id, "gpt-4o") || strstr(id, "gpt-4-turbo"))
    {
        return 128000;
    }
    if (strstr(id, "gpt-5"))
    {
        return 256000;
    }
    if (strstr(id, "o1") || strstr(id, "o3") || strstr(id, "o4"))
    {
        return 200000;
    }
    return 0;
}

static bool ModelHasVision(const char *id)
{
    return id && (strstr(id, "gpt-4o") || strstr(id, "gpt-4.1") || strstr(id, "vision") || strstr(id, "gpt-5"));
}

static size_t OnGetWrite(char *ptr, size_t size, size_t nmemb, void *userdata)
{
    JsonBuf *b = (JsonBuf *)userdata;
    size_t n = size * nmemb;
    JsonBuf_Append(b, ptr, n);
    return n;
}

int PicoLlm_ListModels(const char *base, const char *api_key, PicoModel **out, int *out_n, char **out_error)
{
    if (out)
    {
        *out = NULL;
    }
    if (out_n)
    {
        *out_n = 0;
    }
    if (out_error)
    {
        *out_error = NULL;
    }
    char root[1024];
    char url[1100];
    ResolveRoot(base, root, sizeof(root));
    snprintf(url, sizeof(url), "%s/models", root);

    JsonBuf acc;
    JsonBuf_Init(&acc);
    CURL *curl = curl_easy_init();
    if (!curl)
    {
        JsonBuf_Free(&acc);
        if (out_error)
        {
            *out_error = JsonDup("curl_easy_init failed");
        }
        return PICO_LLM_FAIL;
    }
    struct curl_slist *headers = NULL;
    headers = curl_slist_append(headers, "Accept: application/json");
    char auth[600];
    if (api_key && api_key[0])
    {
        snprintf(auth, sizeof(auth), "Authorization: Bearer %s", api_key);
        headers = curl_slist_append(headers, auth);
    }
    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_HTTPGET, 1L);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, OnGetWrite);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &acc);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 8L);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "Pico/" PICO_VERSION);
    CURLcode rc = curl_easy_perform(curl);
    long http = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http);
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);
    if (rc != CURLE_OK || http >= 400 || !acc.data)
    {
        if (out_error)
        {
            char buf[160];
            snprintf(buf, sizeof(buf), "model list failed (%s HTTP %ld)",
                     rc != CURLE_OK ? curl_easy_strerror(rc) : "ok", http);
            *out_error = JsonDup(buf);
        }
        JsonBuf_Free(&acc);
        return PICO_LLM_FAIL;
    }

    JsonDoc doc;
    if (JsonParse(&doc, acc.data, acc.len) != 0)
    {
        JsonBuf_Free(&acc);
        if (out_error)
        {
            *out_error = JsonDup("model list was not JSON");
        }
        return PICO_LLM_FAIL;
    }
    int arr = JsonObjGet(&doc, 0, "data");
    if (!JsonIsArray(&doc, arr))
    {
        arr = JsonIsArray(&doc, 0) ? 0 : -1;
    }
    int n = JsonIsArray(&doc, arr) ? JsonArrayLen(&doc, arr) : 0;
    PicoModel *list = NULL;
    int count = 0;
    if (n > 0)
    {
        list = (PicoModel *)calloc((size_t)n, sizeof(PicoModel));
    }
    for (int i = 0; list && i < n; i++)
    {
        int item = JsonArrayAt(&doc, arr, i);
        char *id = JsonObjStr(&doc, item, "id");
        if (!id || !id[0])
        {
            free(id);
            continue;
        }
        snprintf(list[count].id, sizeof(list[count].id), "%s", id);
        snprintf(list[count].name, sizeof(list[count].name), "%s", id);
        list[count].context_limit = KnownContextLimit(id);
        list[count].vision = ModelHasVision(id);
        count++;
        free(id);
    }
    JsonFree(&doc);
    JsonBuf_Free(&acc);
    if (out)
    {
        *out = list;
    }
    else
    {
        free(list);
    }
    if (out_n)
    {
        *out_n = count;
    }
    return PICO_LLM_OK;
}

typedef struct LlmCtx {
    PicoLlmCancelFn cancel;
    PicoLlmDeltaFn on_delta;
    void *user;
    JsonBuf acc;
    JsonBuf items;
    int item_count;
    int input_tokens;
    int cached_tokens;
    char *error;
    bool saw_sse;
    bool failed;
} LlmCtx;

static void SetError(LlmCtx *c, const char *msg)
{
    if (!msg || c->error)
    {
        return;
    }
    c->error = JsonDup(msg);
    c->failed = true;
}

static void AppendItem(LlmCtx *c, const char *raw)
{
    if (!raw || !raw[0])
    {
        return;
    }
    if (c->item_count)
    {
        JsonBuf_Putc(&c->items, ',');
    }
    JsonBuf_Puts(&c->items, raw);
    c->item_count++;
}

static void CopyOutputArray(LlmCtx *c, const JsonDoc *doc, int arr)
{
    if (!JsonIsArray(doc, arr) || c->item_count > 0)
    {
        return;
    }
    int n = JsonArrayLen(doc, arr);
    for (int i = 0; i < n; i++)
    {
        char *raw = JsonRawDup(doc, JsonArrayAt(doc, arr, i));
        AppendItem(c, raw);
        free(raw);
    }
}

static void HandleResponseObject(LlmCtx *c, const JsonDoc *doc, int obj)
{
    if (!JsonIsObject(doc, obj))
    {
        return;
    }
    int usage = JsonObjGet(doc, obj, "usage");
    if (JsonIsObject(doc, usage))
    {
        int input = JsonObjInt(doc, usage, "input_tokens", 0);
        int total = JsonObjInt(doc, usage, "total_tokens", 0);
        int details = JsonObjGet(doc, usage, "input_tokens_details");
        c->input_tokens = input > 0 ? input : total;
        if (JsonIsObject(doc, details))
        {
            c->cached_tokens = JsonObjInt(doc, details, "cached_tokens", 0);
        }
        else
        {
            c->cached_tokens = JsonObjInt(doc, usage, "cached_tokens", 0);
        }
    }
    CopyOutputArray(c, doc, JsonObjGet(doc, obj, "output"));
    int err = JsonObjGet(doc, obj, "error");
    if (err >= 0)
    {
        char *msg = NULL;
        if (JsonIsObject(doc, err))
        {
            msg = JsonObjStr(doc, err, "message");
        }
        else
        {
            msg = JsonStrDup(doc, err);
        }
        if (msg && msg[0])
        {
            SetError(c, msg);
        }
        free(msg);
    }
}

static char *ContentText(const JsonDoc *doc, int content)
{
    if (JsonIsArray(doc, content))
    {
        JsonBuf b;
        JsonBuf_Init(&b);
        int n = JsonArrayLen(doc, content);
        for (int i = 0; i < n; i++)
        {
            int part = JsonArrayAt(doc, content, i);
            char *text = JsonObjStr(doc, part, "text");
            if (text)
            {
                JsonBuf_Puts(&b, text);
                free(text);
            }
        }
        return JsonBuf_Steal(&b);
    }
    return JsonStrDup(doc, content);
}

static void EmitDelta(LlmCtx *c, PicoLlmDeltaKind kind, const char *s, size_t n)
{
    if (c->on_delta && s && n)
    {
        c->on_delta(c->user, kind, s, n);
    }
}

static void EmitMessageText(LlmCtx *c, const JsonDoc *doc, int items)
{
    if (!c->on_delta || !JsonIsArray(doc, items))
    {
        return;
    }
    int n = JsonArrayLen(doc, items);
    for (int i = 0; i < n; i++)
    {
        int item = JsonArrayAt(doc, items, i);
        if (!JsonEq(doc, JsonObjGet(doc, item, "type"), "message"))
        {
            continue;
        }
        char *text = ContentText(doc, JsonObjGet(doc, item, "content"));
        if (text && text[0])
        {
            EmitDelta(c, PICO_LLM_DELTA_TEXT, text, strlen(text));
        }
        free(text);
    }
}

static void HandleJson(LlmCtx *c, const char *json, size_t len)
{
    while (len && isspace((unsigned char)json[0]))
    {
        json++;
        len--;
    }
    if (len == 0 || json[0] == '[')
    {
        return;
    }
    if (len >= 6 && strncmp(json, "[DONE]", 6) == 0)
    {
        return;
    }
    JsonDoc doc;
    if (JsonParse(&doc, json, len) != 0)
    {
        return;
    }
    char *type = JsonObjStr(&doc, 0, "type");
    if (type && strcmp(type, "response.output_text.delta") == 0)
    {
        char *delta = JsonObjStr(&doc, 0, "delta");
        if (delta)
        {
            EmitDelta(c, PICO_LLM_DELTA_TEXT, delta, strlen(delta));
            free(delta);
        }
    }
    else if (type && (strcmp(type, "response.reasoning_summary_text.delta") == 0 ||
                      strcmp(type, "response.reasoning_text.delta") == 0))
    {
        char *delta = JsonObjStr(&doc, 0, "delta");
        if (delta)
        {
            EmitDelta(c, PICO_LLM_DELTA_THINKING, delta, strlen(delta));
            free(delta);
        }
    }
    else if (type && strcmp(type, "response.output_item.added") == 0)
    {
        int item = JsonObjGet(&doc, 0, "item");
        if (JsonEq(&doc, JsonObjGet(&doc, item, "type"), "function_call"))
        {
            char *name = JsonObjStr(&doc, item, "name");
            if (name)
            {
                EmitDelta(c, PICO_LLM_DELTA_STATUS, name, strlen(name));
                free(name);
            }
        }
    }
    else if (type && strcmp(type, "response.output_item.done") == 0)
    {
        char *item = JsonObjRaw(&doc, 0, "item");
        AppendItem(c, item);
        free(item);
    }
    else if (type && strcmp(type, "response.completed") == 0)
    {
        HandleResponseObject(c, &doc, JsonObjGet(&doc, 0, "response"));
    }
    else if (type && (strcmp(type, "response.failed") == 0 || strcmp(type, "error") == 0))
    {
        char *msg = JsonObjStr(&doc, 0, "message");
        if (!msg)
        {
            int err = JsonObjGet(&doc, 0, "error");
            msg = JsonIsObject(&doc, err) ? JsonObjStr(&doc, err, "message") : JsonStrDup(&doc, err);
        }
        SetError(c, msg ? msg : "LLM request failed");
        free(msg);
    }
    else if (!type || strcmp(type, "response") == 0)
    {
        HandleResponseObject(c, &doc, 0);
        if (c->item_count > 0 && c->on_delta)
        {
            int output = JsonObjGet(&doc, 0, "output");
            if (output < 0)
            {
                int resp = JsonObjGet(&doc, 0, "response");
                output = JsonObjGet(&doc, resp, "output");
            }
            EmitMessageText(c, &doc, output);
        }
    }
    free(type);
    JsonFree(&doc);
}

static void StripCR(char *s)
{
    char *w = s;
    for (char *r = s; *r; r++)
    {
        if (*r != '\r')
        {
            *w++ = *r;
        }
    }
    *w = '\0';
}

static void HandleSseBlock(LlmCtx *c, char *block)
{
    StripCR(block);
    JsonBuf data;
    JsonBuf_Init(&data);
    char *save = NULL;
    for (char *line = strtok_r(block, "\n", &save); line; line = strtok_r(NULL, "\n", &save))
    {
        if (!line[0] || line[0] == ':')
        {
            continue;
        }
        if (strncmp(line, "data:", 5) == 0)
        {
            const char *p = line + 5;
            if (*p == ' ')
            {
                p++;
            }
            if (data.len)
            {
                JsonBuf_Putc(&data, '\n');
            }
            JsonBuf_Puts(&data, p);
        }
    }
    if (data.len)
    {
        HandleJson(c, data.data, data.len);
    }
    JsonBuf_Free(&data);
}

static void DrainSse(LlmCtx *c, bool finish)
{
    for (;;)
    {
        if (!c->acc.data || c->acc.len == 0)
        {
            break;
        }
        char *start = c->acc.data;
        char *split = strstr(start, "\n\n");
        if (!split)
        {
            split = strstr(start, "\r\n\r\n");
            if (!split)
            {
                break;
            }
            *split = '\0';
            HandleSseBlock(c, start);
            size_t used = (size_t)(split - start) + 4;
            size_t remain = c->acc.len > used ? c->acc.len - used : 0;
            memmove(c->acc.data, c->acc.data + used, remain);
            c->acc.len = remain;
            if (c->acc.data)
            {
                c->acc.data[c->acc.len] = '\0';
            }
            continue;
        }
        *split = '\0';
        HandleSseBlock(c, start);
        size_t used = (size_t)(split - start) + 2;
        size_t remain = c->acc.len > used ? c->acc.len - used : 0;
        memmove(c->acc.data, c->acc.data + used, remain);
        c->acc.len = remain;
        if (c->acc.data)
        {
            c->acc.data[c->acc.len] = '\0';
        }
    }
    if (finish && c->acc.len)
    {
        if (c->saw_sse)
        {
            HandleSseBlock(c, c->acc.data);
        }
        else
        {
            HandleJson(c, c->acc.data, c->acc.len);
        }
        JsonBuf_Clear(&c->acc);
    }
}

static size_t OnWrite(char *ptr, size_t size, size_t nmemb, void *userdata)
{
    LlmCtx *c = (LlmCtx *)userdata;
    size_t n = size * nmemb;
    if (c->cancel && c->cancel(c->user))
    {
        return 0;
    }
    if (n == 0)
    {
        return 0;
    }
    JsonBuf_Append(&c->acc, ptr, n);
    if (!c->saw_sse && c->acc.data &&
        (strstr(c->acc.data, "data:") || strstr(c->acc.data, "event:")))
    {
        c->saw_sse = true;
    }
    if (c->saw_sse)
    {
        DrainSse(c, false);
    }
    return n;
}

static int OnXfer(void *clientp, curl_off_t dltotal, curl_off_t dlnow, curl_off_t ultotal, curl_off_t ulnow)
{
    (void)dltotal;
    (void)dlnow;
    (void)ultotal;
    (void)ulnow;
    LlmCtx *c = (LlmCtx *)clientp;
    if (c->cancel && c->cancel(c->user))
    {
        return 1;
    }
    return 0;
}

int PicoLlm_Stream(const char *url, const char *api_key, const char *body, const char *session_id,
                   PicoLlmCancelFn cancel, PicoLlmDeltaFn on_delta, void *user, char **out_items_json,
                   PicoLlmUsage *out_usage, char **out_error)
{
    if (out_items_json)
    {
        *out_items_json = NULL;
    }
    if (out_usage)
    {
        memset(out_usage, 0, sizeof(*out_usage));
    }
    if (out_error)
    {
        *out_error = NULL;
    }
    if (!url || !body)
    {
        if (out_error)
        {
            *out_error = JsonDup("missing LLM url or body");
        }
        return PICO_LLM_FAIL;
    }

    LlmCtx ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.cancel = cancel;
    ctx.on_delta = on_delta;
    ctx.user = user;
    JsonBuf_Init(&ctx.acc);
    JsonBuf_Init(&ctx.items);

    CURL *curl = curl_easy_init();
    if (!curl)
    {
        JsonBuf_Free(&ctx.acc);
        JsonBuf_Free(&ctx.items);
        if (out_error)
        {
            *out_error = JsonDup("curl_easy_init failed");
        }
        return PICO_LLM_FAIL;
    }

    struct curl_slist *headers = NULL;
    headers = curl_slist_append(headers, "Content-Type: application/json");
    headers = curl_slist_append(headers, "Accept: text/event-stream");
    char auth[600];
    if (api_key && api_key[0])
    {
        snprintf(auth, sizeof(auth), "Authorization: Bearer %s", api_key);
        headers = curl_slist_append(headers, auth);
    }
    char session_hdr[80];
    if (session_id && session_id[0])
    {
        snprintf(session_hdr, sizeof(session_hdr), "session_id: %s", session_id);
        headers = curl_slist_append(headers, session_hdr);
    }

    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_POST, 1L);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, (long)strlen(body));
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, OnWrite);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &ctx);
    curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, OnXfer);
    curl_easy_setopt(curl, CURLOPT_XFERINFODATA, &ctx);
    curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 300L);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "Pico/" PICO_VERSION);

    CURLcode rc = curl_easy_perform(curl);
    long http = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http);
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);

    bool cancelled = (rc == CURLE_ABORTED_BY_CALLBACK) || (cancel && cancel(user));
    if (!cancelled)
    {
        DrainSse(&ctx, true);
    }

    if (!cancelled && http >= 400 && !ctx.error)
    {
        JsonDoc doc;
        if (ctx.acc.data && JsonParse(&doc, ctx.acc.data, ctx.acc.len) == 0)
        {
            HandleResponseObject(&ctx, &doc, 0);
            int err = JsonObjGet(&doc, 0, "error");
            if (!ctx.error && JsonIsObject(&doc, err))
            {
                char *msg = JsonObjStr(&doc, err, "message");
                SetError(&ctx, msg ? msg : "LLM request failed");
                free(msg);
            }
            JsonFree(&doc);
        }
        if (!ctx.error)
        {
            char buf[128];
            snprintf(buf, sizeof(buf), "HTTP %ld from Responses API", http);
            SetError(&ctx, buf);
        }
    }

    if (cancelled)
    {
        JsonBuf_Free(&ctx.acc);
        JsonBuf_Free(&ctx.items);
        free(ctx.error);
        return PICO_LLM_CANCEL;
    }

    if (rc != CURLE_OK && !ctx.error)
    {
        SetError(&ctx, curl_easy_strerror(rc));
    }

    if (ctx.failed || ctx.error)
    {
        if (out_error)
        {
            *out_error = ctx.error ? ctx.error : JsonDup("LLM request failed");
        }
        else
        {
            free(ctx.error);
        }
        JsonBuf_Free(&ctx.acc);
        JsonBuf_Free(&ctx.items);
        return PICO_LLM_FAIL;
    }

    JsonBuf wrapped;
    JsonBuf_Init(&wrapped);
    JsonBuf_Putc(&wrapped, '[');
    if (ctx.items.len)
    {
        JsonBuf_Append(&wrapped, ctx.items.data, ctx.items.len);
    }
    JsonBuf_Putc(&wrapped, ']');
    if (out_items_json)
    {
        *out_items_json = JsonBuf_Steal(&wrapped);
    }
    else
    {
        JsonBuf_Free(&wrapped);
    }
    if (out_usage)
    {
        out_usage->input_tokens = ctx.input_tokens;
        out_usage->cached_tokens = ctx.cached_tokens;
    }
    JsonBuf_Free(&ctx.acc);
    JsonBuf_Free(&ctx.items);
    return PICO_LLM_OK;
}
