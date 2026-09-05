#define _POSIX_C_SOURCE 200809L

#include "pico/http.h"
#include "http_capture.h"
#include "json.h"

#ifndef PICO_VERSION
#define PICO_VERSION "0.1.12"
#endif

#include <curl/curl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

typedef struct HttpCtx
{
    PicoHttpCancelFn cancel;
    PicoHttpJsonFn on_json;
    void *user;
    JsonBuf acc;
    bool saw_sse;
    bool framing_known;
    bool skip_lf;
    bool have_data;
    JsonBuf data;
    char event[64];
    bool json_abort;
    bool buffer_failed;
    PicoHttpCapture capture;
} HttpCtx;

static bool Cancelled(HttpCtx *c)
{
    return c->json_abort || (c->cancel && c->cancel(c->user));
}

static void EmitJson(HttpCtx *c, const char *event, const char *json, size_t len)
{
    if (len && c->on_json && !Cancelled(c) && !c->on_json(c->user, event, json, len))
    {
        c->json_abort = true;
    }
}

static void SseLine(HttpCtx *c)
{
    const char *line = c->acc.data ? c->acc.data : "";
    if (!c->acc.len)
    {
        if (c->have_data)
        {
            EmitJson(c, c->event[0] ? c->event : NULL,
                     c->data.data ? c->data.data : "", c->data.len);
        }
        JsonBuf_Clear(&c->data);
        c->have_data = false;
        c->event[0] = '\0';
    }
    else if (line[0] != ':')
    {
        const char *colon = strchr(line, ':');
        size_t field_len = colon ? (size_t)(colon - line) : strlen(line);
        const char *value = colon ? colon + 1 : "";
        if (*value == ' ')
            value++;
        if (field_len == 4 && memcmp(line, "data", 4) == 0)
        {
            if (c->have_data)
                JsonBuf_Putc(&c->data, '\n');
            JsonBuf_Puts(&c->data, value);
            c->have_data = true;
        }
        else if (field_len == 5 && memcmp(line, "event", 5) == 0)
        {
            snprintf(c->event, sizeof(c->event), "%s", value);
        }
    }
    JsonBuf_Clear(&c->acc);
}

static void DrainSse(HttpCtx *c, bool finish)
{
    if (!finish || c->buffer_failed || Cancelled(c))
        return;
    if (c->saw_sse)
    {
        if (c->acc.len)
            SseLine(c);
        SseLine(c);
    }
    else
    {
        EmitJson(c, NULL, c->acc.data, c->acc.len);
        JsonBuf_Clear(&c->acc);
    }
}

static size_t OnHeader(char *ptr, size_t size, size_t nmemb, void *userdata)
{
    HttpCtx *c = userdata;
    size_t n = size * nmemb;
    if (n >= 5 && memcmp(ptr, "HTTP/", 5) == 0)
    {
        /* A redirect or interim response must not determine the final framing. */
        c->saw_sse = false;
        c->framing_known = false;
        c->skip_lf = false;
        c->have_data = false;
        c->event[0] = '\0';
        JsonBuf_Clear(&c->acc);
        JsonBuf_Clear(&c->data);
    }
    if (n >= 13 && strncasecmp(ptr, "Content-Type:", 13) == 0)
    {
        c->framing_known = true;
        size_t i = 13;
        while (i < n && (ptr[i] == ' ' || ptr[i] == '\t'))
            i++;
        const char *type = "text/event-stream";
        size_t len = strlen(type);
        c->saw_sse = n - i >= len && strncasecmp(ptr + i, type, len) == 0 &&
                     (n - i == len || strchr("; \t\r\n", ptr[i + len]) != NULL);
    }
    return n;
}

static size_t OnWrite(char *ptr, size_t size, size_t nmemb, void *userdata)
{
    HttpCtx *c = userdata;
    size_t n = size * nmemb;
    PicoHttpCapture_Write(&c->capture, ptr, n);
    if (Cancelled(c))
        return 0;
    for (size_t i = 0; i < n && !c->json_abort; i++)
    {
        char ch = ptr[i];
        if (c->skip_lf && ch == '\n')
        {
            c->skip_lf = false;
            continue;
        }
        if (!c->framing_known)
        {
            /* Codex can omit Content-Type. Inspect only the first complete
             * line so transport chunk boundaries cannot affect detection.
             * An explicit Content-Type always takes precedence. */
            if (ch == '\r' || ch == '\n')
            {
                const char *line = c->acc.data ? c->acc.data : "";
                c->saw_sse = line[0] == ':' || strncmp(line, "event:", 6) == 0 ||
                             strncmp(line, "data:", 5) == 0 || strncmp(line, "id:", 3) == 0 ||
                             strncmp(line, "retry:", 6) == 0;
                c->framing_known = true;
            }
            else
            {
                JsonBuf_Putc(&c->acc, ch);
                if (c->acc.failed)
                {
                    c->buffer_failed = true;
                    break;
                }
                continue;
            }
        }
        if (!c->saw_sse)
        {
            JsonBuf_Append(&c->acc, ptr + i, n - i);
            break;
        }
        c->skip_lf = ch == '\r';
        if (ch == '\r' || ch == '\n')
            SseLine(c);
        else
            JsonBuf_Putc(&c->acc, ch);
        if (c->acc.failed || c->data.failed)
        {
            c->buffer_failed = true;
            break;
        }
    }
    c->buffer_failed |= c->acc.failed || c->data.failed;
    return Cancelled(c) || c->buffer_failed ? 0 : n;
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
    curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, OnHeader);
    curl_easy_setopt(curl, CURLOPT_HEADERDATA, &ctx);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, OnWrite);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &ctx);
    curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, OnXfer);
    curl_easy_setopt(curl, CURLOPT_XFERINFODATA, &ctx);
    curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 600L);
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
    JsonBuf_Free(&ctx.data);

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

typedef struct BodyCtx
{
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
