#define _POSIX_C_SOURCE 200809L

#include "pico/http.h"
#include "http_capture.h"
#include "json.h"

#ifndef PICO_VERSION
#define PICO_VERSION "0.1.0"
#endif

#include <curl/curl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct HttpCtx {
    PicoHttpCancelFn cancel;
    PicoHttpJsonFn on_json;
    void *user;
    JsonBuf acc;
    bool saw_sse;
    bool json_abort;
    PicoHttpCapture capture;
} HttpCtx;

static bool Cancelled(HttpCtx *c)
{
    return c->json_abort || (c->cancel && c->cancel(c->user));
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

static void EmitJson(HttpCtx *c, const char *event, const char *json, size_t len)
{
    if (len == 0 || !c->on_json)
    {
        return;
    }
    if (!c->on_json(c->user, event, json, len))
    {
        c->json_abort = true;
    }
}

static void HandleSseBlock(HttpCtx *c, char *block)
{
    StripCR(block);
    JsonBuf data;
    JsonBuf_Init(&data);
    char event[64] = {0};
    char *save = NULL;
    for (char *line = strtok_r(block, "\n", &save); line; line = strtok_r(NULL, "\n", &save))
    {
        if (!line[0] || line[0] == ':')
        {
            continue;
        }
        if (strncmp(line, "event:", 6) == 0)
        {
            const char *p = line + 6;
            if (*p == ' ')
            {
                p++;
            }
            snprintf(event, sizeof(event), "%s", p);
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
        EmitJson(c, event[0] ? event : NULL, data.data, data.len);
    }
    JsonBuf_Free(&data);
}

static void DrainSse(HttpCtx *c, bool finish)
{
    for (;;)
    {
        if (!c->acc.data || c->acc.len == 0)
        {
            break;
        }
        char *start = c->acc.data;
        char *split = strstr(start, "\n\n");
        size_t sep = 2;
        if (!split)
        {
            split = strstr(start, "\r\n\r\n");
            if (!split)
            {
                break;
            }
            sep = 4;
        }
        *split = '\0';
        HandleSseBlock(c, start);
        size_t used = (size_t)(split - start) + sep;
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
            EmitJson(c, NULL, c->acc.data, c->acc.len);
        }
        JsonBuf_Clear(&c->acc);
    }
}

static size_t OnWrite(char *ptr, size_t size, size_t nmemb, void *userdata)
{
    HttpCtx *c = (HttpCtx *)userdata;
    size_t n = size * nmemb;
    if (n == 0)
    {
        return 0;
    }
    PicoHttpCapture_Write(&c->capture, ptr, n);
    if (Cancelled(c))
    {
        return 0;
    }
    JsonBuf_Append(&c->acc, ptr, n);
    if (!c->saw_sse && c->acc.data && (strstr(c->acc.data, "data:") || strstr(c->acc.data, "event:")))
    {
        c->saw_sse = true;
    }
    if (c->saw_sse)
    {
        DrainSse(c, false);
    }
    return Cancelled(c) ? 0 : n;
}

static int OnXfer(void *clientp, curl_off_t dltotal, curl_off_t dlnow, curl_off_t ultotal, curl_off_t ulnow)
{
    (void)dltotal;
    (void)dlnow;
    (void)ultotal;
    (void)ulnow;
    return Cancelled((HttpCtx *)clientp) ? 1 : 0;
}

int pico_http_post_sse(const PicoHttpPost *req, long *out_http, char **out_error)
{
    if (out_http)
    {
        *out_http = 0;
    }
    if (out_error)
    {
        *out_error = NULL;
    }
    if (!req || !req->url || !req->body)
    {
        if (out_error)
        {
            *out_error = JsonDup("missing HTTP url or body");
        }
        return PICO_HTTP_FAIL;
    }

    HttpCtx ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.cancel = req->cancel;
    ctx.on_json = req->on_json;
    ctx.user = req->user;
    JsonBuf_Init(&ctx.acc);
    PicoHttpCapture_Begin(&ctx.capture);

    CURL *curl = curl_easy_init();
    if (!curl)
    {
        PicoHttpCapture_Finish(&ctx.capture, req->url, 0, "transport_error",
                               (int)CURLE_FAILED_INIT, "curl_easy_init failed");
        JsonBuf_Free(&ctx.acc);
        if (out_error)
        {
            *out_error = JsonDup("curl_easy_init failed");
        }
        return PICO_HTTP_FAIL;
    }

    struct curl_slist *headers = NULL;
    for (int i = 0; i < req->header_count && i < PICO_HTTP_MAX_HEADERS; i++)
    {
        if (req->headers[i] && req->headers[i][0])
        {
            headers = curl_slist_append(headers, req->headers[i]);
        }
    }

    curl_easy_setopt(curl, CURLOPT_URL, req->url);
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_POST, 1L);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, req->body);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, (long)strlen(req->body));
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

    if (out_http)
    {
        *out_http = http;
    }

    bool cancelled = req->cancel && req->cancel(req->user);
    if (!cancelled && !ctx.json_abort)
    {
        DrainSse(&ctx, true);
    }
    const char *outcome = "completed";
    if (cancelled || rc == CURLE_ABORTED_BY_CALLBACK)
    {
        outcome = "cancelled";
    }
    else if (ctx.json_abort)
    {
        outcome = "callback_aborted";
    }
    else if (rc != CURLE_OK)
    {
        outcome = "transport_error";
    }
    else if (http >= 400)
    {
        outcome = "http_error";
    }
    PicoHttpCapture_Finish(&ctx.capture, req->url, http, outcome, (int)rc,
                           rc == CURLE_OK ? "" : curl_easy_strerror(rc));
    JsonBuf_Free(&ctx.acc);

    /* An aborting callback already recorded why it stopped, so report success
     * and let the caller surface its own error. */
    if (ctx.json_abort)
    {
        return PICO_HTTP_OK;
    }
    if (cancelled || rc == CURLE_ABORTED_BY_CALLBACK)
    {
        return PICO_HTTP_CANCEL;
    }
    if (rc != CURLE_OK)
    {
        if (out_error)
        {
            *out_error = JsonDup(curl_easy_strerror(rc));
        }
        return PICO_HTTP_FAIL;
    }
    return PICO_HTTP_OK;
}

typedef struct BodyCtx {
    PicoHttpCancelFn cancel;
    void *user;
    JsonBuf acc;
} BodyCtx;

static bool BodyCancelled(BodyCtx *c)
{
    return c->cancel && c->cancel(c->user);
}

static size_t OnBody(char *ptr, size_t size, size_t nmemb, void *userdata)
{
    BodyCtx *c = (BodyCtx *)userdata;
    size_t n = size * nmemb;
    if (n == 0 || BodyCancelled(c))
    {
        return 0;
    }
    JsonBuf_Append(&c->acc, ptr, n);
    return BodyCancelled(c) ? 0 : n;
}

static int OnBodyXfer(void *clientp, curl_off_t dltotal, curl_off_t dlnow, curl_off_t ultotal,
                      curl_off_t ulnow)
{
    (void)dltotal;
    (void)dlnow;
    (void)ultotal;
    (void)ulnow;
    return BodyCancelled((BodyCtx *)clientp) ? 1 : 0;
}

static int BufferedRequest(const PicoHttpReq *req, bool get, long *out_http, char **out_body,
                           char **out_error)
{
    if (out_http)
    {
        *out_http = 0;
    }
    if (out_body)
    {
        *out_body = NULL;
    }
    if (out_error)
    {
        *out_error = NULL;
    }
    if (!req || !req->url || !req->url[0])
    {
        if (out_error)
        {
            *out_error = JsonDup("missing HTTP url");
        }
        return PICO_HTTP_FAIL;
    }

    BodyCtx ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.cancel = req->cancel;
    ctx.user = req->user;
    JsonBuf_Init(&ctx.acc);

    CURL *curl = curl_easy_init();
    if (!curl)
    {
        JsonBuf_Free(&ctx.acc);
        if (out_error)
        {
            *out_error = JsonDup("curl_easy_init failed");
        }
        return PICO_HTTP_FAIL;
    }

    struct curl_slist *headers = NULL;
    for (int i = 0; i < req->header_count && i < PICO_HTTP_MAX_HEADERS; i++)
    {
        if (req->headers[i] && req->headers[i][0])
        {
            headers = curl_slist_append(headers, req->headers[i]);
        }
    }

    curl_easy_setopt(curl, CURLOPT_URL, req->url);
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    if (get)
    {
        curl_easy_setopt(curl, CURLOPT_HTTPGET, 1L);
    }
    else
    {
        const char *body = req->body ? req->body : "";
        curl_easy_setopt(curl, CURLOPT_POST, 1L);
        curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body);
        curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, (long)strlen(body));
    }
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, OnBody);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &ctx);
    curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, OnBodyXfer);
    curl_easy_setopt(curl, CURLOPT_XFERINFODATA, &ctx);
    curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 30L);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "Pico/" PICO_VERSION);

    CURLcode rc = curl_easy_perform(curl);
    long http = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http);
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);

    if (out_http)
    {
        *out_http = http;
    }

    bool cancelled = BodyCancelled(&ctx) || rc == CURLE_ABORTED_BY_CALLBACK;
    if (cancelled)
    {
        JsonBuf_Free(&ctx.acc);
        return PICO_HTTP_CANCEL;
    }
    if (rc != CURLE_OK)
    {
        JsonBuf_Free(&ctx.acc);
        if (out_error)
        {
            *out_error = JsonDup(curl_easy_strerror(rc));
        }
        return PICO_HTTP_FAIL;
    }
    if (out_body)
    {
        *out_body = ctx.acc.data ? JsonBuf_Steal(&ctx.acc) : JsonDup("");
    }
    else
    {
        JsonBuf_Free(&ctx.acc);
    }
    return PICO_HTTP_OK;
}

int pico_http_post(const PicoHttpReq *req, long *out_http, char **out_body, char **out_error)
{
    return BufferedRequest(req, false, out_http, out_body, out_error);
}

int pico_http_get(const PicoHttpReq *req, long *out_http, char **out_body, char **out_error)
{
    return BufferedRequest(req, true, out_http, out_body, out_error);
}

static void FormPct(JsonBuf *b, const char *s)
{
    static const char kHex[] = "0123456789ABCDEF";
    if (!s)
    {
        return;
    }
    for (; *s; s++)
    {
        unsigned char c = (unsigned char)*s;
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' ||
            c == '_' || c == '.' || c == '~')
        {
            JsonBuf_Putc(b, (char)c);
        }
        else
        {
            JsonBuf_Putc(b, '%');
            JsonBuf_Putc(b, kHex[c >> 4]);
            JsonBuf_Putc(b, kHex[c & 15]);
        }
    }
}

char *pico_http_form_encode(const char *const *keys, const char *const *vals, int n)
{
    JsonBuf b;
    JsonBuf_Init(&b);
    for (int i = 0; i < n; i++)
    {
        if (!keys || !keys[i])
        {
            continue;
        }
        if (b.len)
        {
            JsonBuf_Putc(&b, '&');
        }
        FormPct(&b, keys[i]);
        JsonBuf_Putc(&b, '=');
        FormPct(&b, vals ? vals[i] : NULL);
    }
    return b.data ? JsonBuf_Steal(&b) : JsonDup("");
}
