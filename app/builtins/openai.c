#define _POSIX_C_SOURCE 200809L

#include "pico/plugin.h"
#include "pico/http.h"
#include "pico/auth.h"
#include "json.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static const char kDefaultBase[] = "https://api.openai.com/v1";
static const char kCodexResponses[] = "https://chatgpt.com/backend-api/codex/responses";
static const char kClientId[] = "app_EMoamEEZ73f0CkXaXp7hrann";
static const char kIssuer[] = "https://auth.openai.com";
static const char kBriefPrompt[] =
    "Handoff brief: goals, constraints, progress, decisions, next steps, exact values; omit noise. "
    "Return text, not a tool call.";

static void ResolveUrl(const char *base, char *out, size_t cap)
{
    const char *src = base && base[0] ? base : kDefaultBase;
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

static char *BuildResponsesMessage(const char *role, const char *text, bool assistant)
{
    JsonBuf b;
    JsonBuf_Init(&b);
    JsonBuf_Puts(&b, "{\"type\":\"message\",\"role\":");
    JsonBuf_String(&b, role ? role : "user");
    JsonBuf_Puts(&b, ",\"content\":[{\"type\":");
    JsonBuf_String(&b, assistant ? "output_text" : "input_text");
    JsonBuf_Puts(&b, ",\"text\":");
    JsonBuf_String(&b, text ? text : "");
    JsonBuf_Puts(&b, "}]}");
    return JsonBuf_Steal(&b);
}

static char *MapPicoItem(const char *json)
{
    if (!json || !json[0])
    {
        return NULL;
    }
    JsonDoc doc;
    if (JsonParse(&doc, json, strlen(json)) != 0)
    {
        return NULL;
    }
    char *type = JsonObjStr(&doc, 0, "type");
    char *out = NULL;
    if (type && strcmp(type, "user") == 0)
    {
        char *text = JsonObjStr(&doc, 0, "text");
        out = BuildResponsesMessage("user", text, false);
        free(text);
    }
    else if (type && strcmp(type, "assistant") == 0)
    {
        char *text = JsonObjStr(&doc, 0, "text");
        out = BuildResponsesMessage("assistant", text, true);
        free(text);
    }
    else if (type && strcmp(type, "tool_call") == 0)
    {
        JsonBuf b;
        JsonBuf_Init(&b);
        JsonBuf_Puts(&b, "{\"type\":\"function_call\",\"name\":");
        char *name = JsonObjStr(&doc, 0, "name");
        char *args = JsonObjStr(&doc, 0, "arguments");
        char *id = JsonObjStr(&doc, 0, "call_id");
        JsonBuf_String(&b, name ? name : "");
        JsonBuf_Puts(&b, ",\"arguments\":");
        JsonBuf_String(&b, args ? args : "{}");
        JsonBuf_Puts(&b, ",\"call_id\":");
        JsonBuf_String(&b, id ? id : "");
        JsonBuf_Putc(&b, '}');
        out = JsonBuf_Steal(&b);
        free(name);
        free(args);
        free(id);
    }
    else if (type && strcmp(type, "tool_result") == 0)
    {
        JsonBuf b;
        JsonBuf_Init(&b);
        JsonBuf_Puts(&b, "{\"type\":\"function_call_output\",\"call_id\":");
        char *id = JsonObjStr(&doc, 0, "call_id");
        char *output = JsonObjStr(&doc, 0, "output");
        JsonBuf_String(&b, id ? id : "");
        JsonBuf_Puts(&b, ",\"output\":");
        JsonBuf_String(&b, output ? output : "");
        JsonBuf_Putc(&b, '}');
        out = JsonBuf_Steal(&b);
        free(id);
        free(output);
    }
    else if (type && strcmp(type, "raw") == 0)
    {
        char *prov = JsonObjStr(&doc, 0, "provider");
        if (prov && strcmp(prov, "openai") == 0)
        {
            out = JsonObjRaw(&doc, 0, "json");
        }
        free(prov);
    }
    free(type);
    JsonFree(&doc);
    return out;
}

static char *BuildRequest(const PicoLlmTurn *turn, bool codex)
{
    JsonBuf b;
    JsonBuf_Init(&b);
    JsonBuf_Puts(&b, "{\"model\":");
    JsonBuf_String(&b, turn->model ? turn->model : "");
    JsonBuf_Puts(&b, ",\"prompt_cache_key\":");
    JsonBuf_String(&b, turn->cache_key ? turn->cache_key : "");
    if (codex)
    {
        /* The ChatGPT Codex backend rejects stored responses, and with store off
         * the server only replays reasoning items that carry their encrypted
         * payload. Plain Responses endpoints (including OpenAI-compatible
         * gateways behind models[].base_url) may reject either field. */
        JsonBuf_Puts(&b, ",\"store\":false,\"include\":[\"reasoning.encrypted_content\"]");
    }
    JsonBuf_Puts(&b, ",\"stream\":true,\"instructions\":");
    JsonBuf_String(&b, turn->instructions ? turn->instructions : "");
    JsonBuf_Puts(&b, ",\"input\":[");
    int wrote = 0;
    for (int i = 0; i < turn->input_count; i++)
    {
        char *mapped = MapPicoItem(turn->input_json[i]);
        if (!mapped)
        {
            continue;
        }
        if (wrote)
        {
            JsonBuf_Putc(&b, ',');
        }
        JsonBuf_Puts(&b, mapped);
        free(mapped);
        wrote++;
    }
    if (turn->compact)
    {
        if (wrote)
        {
            JsonBuf_Putc(&b, ',');
        }
        char *brief = BuildResponsesMessage("user", kBriefPrompt, false);
        JsonBuf_Puts(&b, brief);
        free(brief);
    }
    JsonBuf_Puts(&b, "]");
    if (turn->include_tools && turn->tools && turn->tool_count > 0)
    {
        JsonBuf_Puts(&b, ",\"tools\":[");
        for (int i = 0; i < turn->tool_count; i++)
        {
            if (i)
            {
                JsonBuf_Putc(&b, ',');
            }
            JsonBuf_Puts(&b, "{\"type\":\"function\",\"name\":");
            JsonBuf_String(&b, turn->tools[i].name ? turn->tools[i].name : "");
            JsonBuf_Puts(&b, ",\"description\":");
            JsonBuf_String(&b, turn->tools[i].description ? turn->tools[i].description : "");
            JsonBuf_Puts(&b, ",\"parameters\":");
            JsonBuf_Puts(&b, turn->tools[i].params_json && turn->tools[i].params_json[0]
                                 ? turn->tools[i].params_json
                                 : "{\"type\":\"object\",\"properties\":{}}");
            JsonBuf_Putc(&b, '}');
        }
        JsonBuf_Putc(&b, ']');
    }
    if (turn->compact)
    {
        JsonBuf_Puts(&b, ",\"tool_choice\":\"none\"");
    }
    const char *effort = turn->effort;
    if (effort && effort[0] && strcmp(effort, "none") != 0 && strcmp(effort, "off") != 0)
    {
        JsonBuf_Puts(&b, ",\"reasoning\":{\"effort\":");
        JsonBuf_String(&b, effort);
        JsonBuf_Puts(&b, ",\"summary\":\"auto\"}");
    }
    JsonBuf_Putc(&b, '}');
    return JsonBuf_Steal(&b);
}

static char *BodyWithoutReasoning(const char *body)
{
    if (!body)
    {
        return NULL;
    }
    const char *found = strstr(body, "\"reasoning\":");
    if (!found)
    {
        return NULL;
    }
    const char *start = found;
    if (start > body && start[-1] == ',')
    {
        start--;
    }
    const char *brace = strchr(found, '{');
    if (!brace)
    {
        return NULL;
    }
    int depth = 0;
    const char *end = brace;
    for (; *end; end++)
    {
        if (*end == '{')
        {
            depth++;
        }
        else if (*end == '}')
        {
            depth--;
            if (depth == 0)
            {
                end++;
                break;
            }
        }
    }
    JsonBuf b;
    JsonBuf_Init(&b);
    JsonBuf_Append(&b, body, (size_t)(start - body));
    JsonBuf_Puts(&b, end);
    return JsonBuf_Steal(&b);
}

/* Device-code polling runs on its own thread: every request is a blocking curl
 * call, and the render loop cannot afford to stall on the network. The thread
 * only touches the auth store (mutex-guarded) and this struct; user-visible text
 * is queued here and emitted by the render thread in OpenAiFrame. */
#define PICO_DEVICE_MAX_NOTES 8
#define PICO_DEVICE_TIMEOUT_SEC 900
#define PICO_DEVICE_MAX_TRANSPORT_FAILS 5

typedef struct DeviceLogin {
    pthread_mutex_t mu;
    pthread_t thread;
    bool joinable;
    bool running;
    bool cancel;
    char *notes[PICO_DEVICE_MAX_NOTES];
    int note_count;
} DeviceLogin;

static DeviceLogin g_login = {.mu = PTHREAD_MUTEX_INITIALIZER};

static void LoginNote(const char *text)
{
    if (!text || !text[0])
    {
        return;
    }
    pthread_mutex_lock(&g_login.mu);
    if (g_login.note_count < PICO_DEVICE_MAX_NOTES)
    {
        g_login.notes[g_login.note_count] = JsonDup(text);
        if (g_login.notes[g_login.note_count])
        {
            g_login.note_count++;
        }
    }
    pthread_mutex_unlock(&g_login.mu);
}

static bool LoginCancelled(void *user)
{
    (void)user;
    pthread_mutex_lock(&g_login.mu);
    bool c = g_login.cancel;
    pthread_mutex_unlock(&g_login.mu);
    return c;
}

static bool LoginActive(void)
{
    pthread_mutex_lock(&g_login.mu);
    bool r = g_login.running;
    pthread_mutex_unlock(&g_login.mu);
    return r;
}

/* Sleeps in short slices so `/login cancel` and shutdown are not held up by the
 * poll interval. */
static bool LoginSleep(int seconds)
{
    for (int i = 0; i < seconds * 5; i++)
    {
        if (LoginCancelled(NULL))
        {
            return false;
        }
        struct timespec ts = {.tv_sec = 0, .tv_nsec = 200 * 1000 * 1000};
        nanosleep(&ts, NULL);
    }
    return !LoginCancelled(NULL);
}

static void StopDeviceLogin(void)
{
    pthread_mutex_lock(&g_login.mu);
    g_login.cancel = true;
    bool joinable = g_login.joinable;
    pthread_t t = g_login.thread;
    g_login.joinable = false;
    pthread_mutex_unlock(&g_login.mu);
    if (joinable)
    {
        pthread_join(t, NULL);
    }
}

typedef struct LlmCtx {
    PicoLlmCancelFn cancel;
    PicoLlmDeltaFn on_delta;
    void *user;
    JsonBuf items;
    int item_count;
    int input_tokens;
    int cached_tokens;
    char *error;
    bool failed;
    bool saw_text;
    long http;
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

/* Hosted OpenAI encrypts raw CoT, so reasoning_text can be a few control
 * bytes. Compatible /responses endpoints often send the same field as
 * readable thinking; keep those, drop the opaque blobs. */
static bool ThinkUsable(const char *s, size_t n)
{
    if (!s || n == 0)
    {
        return false;
    }
    for (size_t i = 0; i < n; i++)
    {
        unsigned char c = (unsigned char)s[i];
        if (c < 0x20 && c != '\t' && c != '\n' && c != '\r')
        {
            return false;
        }
        if (c == 0x7F)
        {
            return false;
        }
    }
    return true;
}

static void AppendThink(JsonBuf *think, const char *text)
{
    if (!ThinkUsable(text, text ? strlen(text) : 0))
    {
        return;
    }
    if (think->len)
    {
        JsonBuf_Puts(think, "\n\n");
    }
    JsonBuf_Puts(think, text);
}

static void EmitDelta(LlmCtx *c, PicoLlmDeltaKind kind, const char *s, size_t n)
{
    if (kind == PICO_LLM_DELTA_THINKING && !ThinkUsable(s, n))
    {
        return;
    }
    if (kind == PICO_LLM_DELTA_TEXT && s && n)
    {
        c->saw_text = true;
    }
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

/* The HTTP layer hands the same user pointer to every callback, so route the
 * agent's cancel check through the LlmCtx instead of passing it `user` raw. */
static bool HandleCancel(void *user)
{
    LlmCtx *c = (LlmCtx *)user;
    return c->cancel && c->cancel(c->user);
}

static bool HandleJson(void *user, const char *event, const char *json, size_t len)
{
    LlmCtx *c = (LlmCtx *)user;
    JsonDoc doc;
    if (JsonParse(&doc, json, len) != 0)
    {
        return true;
    }
    int err = JsonObjGet(&doc, 0, "error");
    if (err >= 0 && JsonIsObject(&doc, err))
    {
        char *msg = JsonObjStr(&doc, err, "message");
        if (msg && msg[0])
        {
            SetError(c, msg);
        }
        else
        {
            SetError(c, "LLM request failed");
        }
        free(msg);
        JsonFree(&doc);
        return false;
    }
    /* The Codex backend reports failures as {"detail": ...} rather than an error object. */
    int detail = JsonObjGet(&doc, 0, "detail");
    if (detail >= 0)
    {
        char *msg = JsonStrDup(&doc, detail);
        if (!msg || !msg[0])
        {
            free(msg);
            msg = JsonRawDup(&doc, detail);
        }
        SetError(c, msg && msg[0] ? msg : "LLM request failed");
        free(msg);
        JsonFree(&doc);
        return false;
    }
    char *type = JsonObjStr(&doc, 0, "type");
    if ((!type || !type[0]) && event && event[0])
    {
        free(type);
        type = JsonDup(event);
    }
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
    else if (type && (strcmp(type, "response.failed") == 0 || strcmp(type, "error") == 0 ||
                      strstr(type, "invalid_request") || strstr(type, "authentication") ||
                      strstr(type, "permission") || strstr(type, "rate_limit")))
    {
        char *msg = JsonObjStr(&doc, 0, "message");
        if (!msg)
        {
            int nested = JsonObjGet(&doc, 0, "error");
            msg = JsonIsObject(&doc, nested) ? JsonObjStr(&doc, nested, "message") : JsonStrDup(&doc, nested);
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
    return !c->failed;
}

static bool GrowCalls(PicoLlmResult *out)
{
    PicoLlmToolCall *next =
        (PicoLlmToolCall *)realloc(out->calls, (size_t)(out->call_count + 1) * sizeof(PicoLlmToolCall));
    if (!next)
    {
        return false;
    }
    out->calls = next;
    memset(&out->calls[out->call_count], 0, sizeof(PicoLlmToolCall));
    return true;
}

static void AddRaw(PicoLlmResult *out, const char *json)
{
    if (!json || !json[0])
    {
        return;
    }
    char **next = (char **)realloc(out->raw_items, (size_t)(out->raw_count + 1) * sizeof(char *));
    if (!next)
    {
        return;
    }
    out->raw_items = next;
    out->raw_items[out->raw_count] = JsonDup(json);
    out->raw_count++;
}

static void FillResult(LlmCtx *c, PicoLlmResult *out)
{
    out->input_tokens = c->input_tokens;
    out->cached_tokens = c->cached_tokens;
    if (c->error)
    {
        out->error = c->error;
        c->error = NULL;
        return;
    }
    if (!c->items.len)
    {
        return;
    }
    JsonBuf wrapped;
    JsonBuf_Init(&wrapped);
    JsonBuf_Putc(&wrapped, '[');
    JsonBuf_Append(&wrapped, c->items.data, c->items.len);
    JsonBuf_Putc(&wrapped, ']');
    JsonDoc doc;
    if (JsonParse(&doc, wrapped.data, wrapped.len) != 0)
    {
        JsonBuf_Free(&wrapped);
        return;
    }
    int n = JsonArrayLen(&doc, 0);
    JsonBuf think;
    JsonBuf_Init(&think);
    JsonBuf assistant;
    JsonBuf_Init(&assistant);
    for (int i = 0; i < n; i++)
    {
        int item = JsonArrayAt(&doc, 0, i);
        if (JsonEq(&doc, JsonObjGet(&doc, item, "type"), "function_call"))
        {
            if (!GrowCalls(out))
            {
                continue;
            }
            PicoLlmToolCall *call = &out->calls[out->call_count++];
            call->call_id = JsonObjStr(&doc, item, "call_id");
            call->name = JsonObjStr(&doc, item, "name");
            call->arguments = JsonObjStr(&doc, item, "arguments");
            continue;
        }
        if (JsonEq(&doc, JsonObjGet(&doc, item, "type"), "message"))
        {
            char *text = ContentText(&doc, JsonObjGet(&doc, item, "content"));
            if (text && text[0])
            {
                JsonBuf_Puts(&assistant, text);
            }
            free(text);
            continue;
        }
        char *raw = JsonRawDup(&doc, item);
        AddRaw(out, raw);
        if (JsonEq(&doc, JsonObjGet(&doc, item, "type"), "reasoning"))
        {
            char *summary = ContentText(&doc, JsonObjGet(&doc, item, "summary"));
            char *content = ContentText(&doc, JsonObjGet(&doc, item, "content"));
            AppendThink(&think, summary);
            if (content && (!summary || strcmp(content, summary) != 0))
            {
                AppendThink(&think, content);
            }
            free(summary);
            free(content);
        }
        free(raw);
    }
    JsonFree(&doc);
    JsonBuf_Free(&wrapped);
    if (assistant.len)
    {
        out->assistant_text = JsonBuf_Steal(&assistant);
    }
    else
    {
        JsonBuf_Free(&assistant);
    }
    if (think.len)
    {
        out->think_text = JsonBuf_Steal(&think);
    }
    else
    {
        JsonBuf_Free(&think);
    }
}

static void Note(PicoApp *app, const char *text)
{
    PicoApp_AddMessage(app, PICO_ROLE_ASSISTANT, text);
}

static int B64UrlVal(char c)
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
    if (c == '-')
    {
        return 62;
    }
    if (c == '_')
    {
        return 63;
    }
    return -1;
}

static char *B64UrlDecode(const char *s, size_t n)
{
    unsigned char *out = (unsigned char *)malloc(n + 1);
    size_t o = 0;
    int val = 0;
    int bits = 0;
    if (!out)
    {
        return NULL;
    }
    for (size_t i = 0; i < n; i++)
    {
        int d = B64UrlVal(s[i]);
        if (d < 0)
        {
            continue;
        }
        val = (val << 6) | d;
        bits += 6;
        if (bits >= 8)
        {
            bits -= 8;
            out[o++] = (unsigned char)((val >> bits) & 0xff);
        }
    }
    out[o] = 0;
    return (char *)out;
}

static char *AccountFromJwt(const char *jwt)
{
    if (!jwt || !jwt[0])
    {
        return NULL;
    }
    const char *dot1 = strchr(jwt, '.');
    if (!dot1)
    {
        return NULL;
    }
    const char *dot2 = strchr(dot1 + 1, '.');
    if (!dot2)
    {
        return NULL;
    }
    char *json = B64UrlDecode(dot1 + 1, (size_t)(dot2 - (dot1 + 1)));
    if (!json)
    {
        return NULL;
    }
    JsonDoc doc;
    char *id = NULL;
    if (JsonParse(&doc, json, strlen(json)) == 0)
    {
        id = JsonObjStr(&doc, 0, "chatgpt_account_id");
        if (!id || !id[0])
        {
            free(id);
            int auth = JsonObjGet(&doc, 0, "https://api.openai.com/auth");
            id = JsonIsObject(&doc, auth) ? JsonObjStr(&doc, auth, "chatgpt_account_id") : NULL;
        }
        if (!id || !id[0])
        {
            free(id);
            id = JsonObjStr(&doc, 0, "account_id");
        }
        JsonFree(&doc);
    }
    free(json);
    if (id && !id[0])
    {
        free(id);
        id = NULL;
    }
    return id;
}

static char *PickAccountId(const char *access, const char *id_token, const char *fallback)
{
    char *id = AccountFromJwt(id_token);
    if (id)
    {
        return id;
    }
    id = AccountFromJwt(access);
    if (id)
    {
        return id;
    }
    return (fallback && fallback[0]) ? JsonDup(fallback) : NULL;
}

static int PostRaw(const char *url, const char *body, const char *content_type,
                   PicoHttpCancelFn cancel, void *cancel_user, long *http, char **out, char **err)
{
    PicoHttpReq req;
    memset(&req, 0, sizeof(req));
    req.url = url;
    req.body = body ? body : "";
    req.headers[0] = content_type;
    req.header_count = content_type ? 1 : 0;
    req.cancel = cancel;
    req.user = cancel_user;
    return pico_http_post(&req, http, out, err);
}

/* Lets a token refresh honour the same cancel the streaming request does. */
typedef struct TurnCancel {
    PicoLlmCancelFn fn;
    void *user;
} TurnCancel;

static bool TurnCancelled(void *user)
{
    TurnCancel *t = (TurnCancel *)user;
    return t && t->fn && t->fn(t->user);
}

/* Prefers the server's own explanation, which pico_http_post captures even for a
 * 4xx; curl's `err` is only set when the request never completed. */
static char *HttpDetail(const char *body, const char *err, long http)
{
    static const char *kFields[] = {"error_description", "detail", "message", "error"};
    if (body && body[0])
    {
        JsonDoc doc;
        if (JsonParse(&doc, body, strlen(body)) == 0)
        {
            for (size_t i = 0; i < sizeof(kFields) / sizeof(kFields[0]); i++)
            {
                int tok = JsonObjGet(&doc, 0, kFields[i]);
                char *s = NULL;
                if (JsonIsObject(&doc, tok))
                {
                    s = JsonObjStr(&doc, tok, "message");
                }
                else if (tok >= 0)
                {
                    s = JsonStrDup(&doc, tok);
                }
                if (s && s[0])
                {
                    JsonFree(&doc);
                    return s;
                }
                free(s);
            }
            JsonFree(&doc);
        }
    }
    if (err && err[0])
    {
        return JsonDup(err);
    }
    char buf[64];
    snprintf(buf, sizeof(buf), "HTTP %ld", http);
    return JsonDup(buf);
}

static bool ApplyTokenBody(PicoApp *app, PicoAuthEntry *auth, const char *body)
{
    JsonDoc doc;
    if (!body || JsonParse(&doc, body, strlen(body)) != 0)
    {
        return false;
    }
    char *access = JsonObjStr(&doc, 0, "access_token");
    char *refresh = JsonObjStr(&doc, 0, "refresh_token");
    char *id_token = JsonObjStr(&doc, 0, "id_token");
    int expires_in = JsonObjInt(&doc, 0, "expires_in", 3600);
    if (expires_in < 1)
    {
        expires_in = 3600;
    }
    bool ok = access && access[0];
    if (ok)
    {
        const char *use_refresh = (refresh && refresh[0]) ? refresh : (auth ? auth->refresh_token : NULL);
        char *account = PickAccountId(access, id_token, auth ? auth->account_id : NULL);
        long expires_at = (long)time(NULL) + expires_in;
        if (!pico_auth_set_oauth(app, "openai", access, use_refresh, account, expires_at))
        {
            LoginNote("Warning: could not write `~/.config/pico/auth.json`. This session stays "
                      "signed in, but the login will not survive a restart.");
        }
        if (auth)
        {
            pico_auth_entry_free(auth);
            pico_auth_copy(app, "openai", auth);
        }
        free(account);
    }
    free(access);
    free(refresh);
    free(id_token);
    JsonFree(&doc);
    return ok;
}

static bool RefreshOauth(PicoApp *app, PicoAuthEntry *auth, TurnCancel *tc)
{
    if (!auth || !auth->refresh_token || !auth->refresh_token[0])
    {
        return false;
    }
    const char *keys[] = {"grant_type", "refresh_token", "client_id"};
    const char *vals[] = {"refresh_token", auth->refresh_token, kClientId};
    char *form = pico_http_form_encode(keys, vals, 3);
    char url[256];
    snprintf(url, sizeof(url), "%s/oauth/token", kIssuer);
    long http = 0;
    char *body = NULL;
    char *err = NULL;
    int rc = PostRaw(url, form, "Content-Type: application/x-www-form-urlencoded",
                     tc ? TurnCancelled : NULL, tc, &http, &body, &err);
    free(form);
    bool ok = rc == PICO_HTTP_OK && http < 400 && ApplyTokenBody(app, auth, body);
    free(body);
    free(err);
    return ok;
}

static bool OauthDue(const PicoAuthEntry *auth)
{
    if (!auth || auth->expires_at <= 0)
    {
        return false;
    }
    return time(NULL) + 60 >= auth->expires_at;
}

static int IntervalOf(const JsonDoc *doc, int obj)
{
    char *s = JsonObjStr(doc, obj, "interval");
    int v = 5;
    if (s && s[0])
    {
        v = atoi(s);
    }
    else
    {
        v = JsonObjInt(doc, obj, "interval", 5);
    }
    free(s);
    return v < 1 ? 5 : v;
}

static bool ExchangeDeviceCode(PicoApp *app, const char *code, const char *verifier)
{
    const char *keys[] = {"grant_type", "code", "redirect_uri", "client_id", "code_verifier"};
    char redirect[256];
    snprintf(redirect, sizeof(redirect), "%s/deviceauth/callback", kIssuer);
    const char *vals[] = {"authorization_code", code, redirect, kClientId, verifier};
    char *form = pico_http_form_encode(keys, vals, 5);
    char url[256];
    snprintf(url, sizeof(url), "%s/oauth/token", kIssuer);
    long http = 0;
    char *body = NULL;
    char *err = NULL;
    int rc = PostRaw(url, form, "Content-Type: application/x-www-form-urlencoded", LoginCancelled,
                     NULL, &http, &body, &err);
    free(form);
    bool ok = rc == PICO_HTTP_OK && http < 400 && ApplyTokenBody(app, NULL, body);
    if (ok)
    {
        LoginNote("Signed in with ChatGPT. Pico will use your Codex subscription.");
    }
    else if (rc != PICO_HTTP_CANCEL)
    {
        char *detail = HttpDetail(body, err, http);
        char buf[512];
        snprintf(buf, sizeof(buf), "Token exchange failed: %s", detail ? detail : "unknown error");
        free(detail);
        LoginNote(buf);
    }
    free(body);
    free(err);
    return ok;
}

typedef enum DevicePoll {
    DEVICE_PENDING = 0,
    DEVICE_READY,
    /* The request never completed, so retrying may still succeed. */
    DEVICE_UNREACHABLE,
    /* The server answered with a verdict; retrying will not change it. */
    DEVICE_FAILED,
    DEVICE_CANCELLED,
} DevicePoll;

static bool IsPendingError(const char *s)
{
    return s && (strstr(s, "authorization_pending") || strstr(s, "slow_down") || strstr(s, "pending"));
}

/* Anything short of an explicit failure counts as "not approved yet". Guessing
 * wrong that way only costs another poll, whereas treating an unfamiliar pending
 * response as fatal would drop the user out of the flow entirely. */
static DevicePoll PollDeviceOnce(const char *device_auth_id, const char *user_code, char **out_code,
                                 char **out_verifier, char **out_error)
{
    char url[256];
    snprintf(url, sizeof(url), "%s/api/accounts/deviceauth/token", kIssuer);
    JsonBuf b;
    JsonBuf_Init(&b);
    JsonBuf_Puts(&b, "{\"device_auth_id\":");
    JsonBuf_String(&b, device_auth_id);
    JsonBuf_Puts(&b, ",\"user_code\":");
    JsonBuf_String(&b, user_code);
    JsonBuf_Putc(&b, '}');
    char *req = JsonBuf_Steal(&b);
    long http = 0;
    char *body = NULL;
    char *err = NULL;
    int rc = PostRaw(url, req, "Content-Type: application/json", LoginCancelled, NULL, &http, &body,
                     &err);
    free(req);

    DevicePoll state = DEVICE_PENDING;
    if (rc == PICO_HTTP_CANCEL)
    {
        state = DEVICE_CANCELLED;
    }
    else if (rc != PICO_HTTP_OK)
    {
        *out_error = HttpDetail(NULL, err, http);
        state = DEVICE_UNREACHABLE;
    }
    else if (http >= 400 && http != 403 && http != 404)
    {
        char *detail = HttpDetail(body, err, http);
        if (IsPendingError(body) || IsPendingError(detail))
        {
            free(detail);
        }
        else
        {
            *out_error = detail;
            state = DEVICE_FAILED;
        }
    }
    else if (body && body[0])
    {
        JsonDoc doc;
        if (JsonParse(&doc, body, strlen(body)) == 0)
        {
            char *code = JsonObjStr(&doc, 0, "authorization_code");
            char *verifier = JsonObjStr(&doc, 0, "code_verifier");
            if (code && code[0] && verifier && verifier[0])
            {
                *out_code = code;
                *out_verifier = verifier;
                state = DEVICE_READY;
            }
            else
            {
                free(code);
                free(verifier);
            }
            JsonFree(&doc);
        }
    }
    free(body);
    free(err);
    return state;
}

static bool RequestUserCode(char *id, size_t id_cap, char *code, size_t code_cap, int *interval)
{
    char url[256];
    snprintf(url, sizeof(url), "%s/api/accounts/deviceauth/usercode", kIssuer);
    JsonBuf b;
    JsonBuf_Init(&b);
    JsonBuf_Puts(&b, "{\"client_id\":");
    JsonBuf_String(&b, kClientId);
    JsonBuf_Putc(&b, '}');
    char *req = JsonBuf_Steal(&b);
    long http = 0;
    char *body = NULL;
    char *err = NULL;
    int rc = PostRaw(url, req, "Content-Type: application/json", LoginCancelled, NULL, &http, &body,
                     &err);
    free(req);
    if (rc == PICO_HTTP_CANCEL)
    {
        free(body);
        free(err);
        return false;
    }
    if (rc != PICO_HTTP_OK || http >= 400)
    {
        if (http == 404)
        {
            LoginNote("Device-code login is not enabled for this ChatGPT account. Enable it in your "
                      "ChatGPT security settings, or ask a workspace admin.");
        }
        else
        {
            char *detail = HttpDetail(body, err, http);
            char buf[512];
            snprintf(buf, sizeof(buf), "Could not start device login: %s",
                     detail ? detail : "unknown error");
            free(detail);
            LoginNote(buf);
        }
        free(body);
        free(err);
        return false;
    }
    JsonDoc doc;
    if (!body || JsonParse(&doc, body, strlen(body)) != 0)
    {
        LoginNote("Could not start device login: bad response.");
        free(body);
        free(err);
        return false;
    }
    char *got_id = JsonObjStr(&doc, 0, "device_auth_id");
    char *got_code = JsonObjStr(&doc, 0, "user_code");
    if (!got_code || !got_code[0])
    {
        free(got_code);
        got_code = JsonObjStr(&doc, 0, "usercode");
    }
    bool ok = got_id && got_id[0] && got_code && got_code[0];
    if (ok)
    {
        snprintf(id, id_cap, "%s", got_id);
        snprintf(code, code_cap, "%s", got_code);
        *interval = IntervalOf(&doc, 0);
    }
    else
    {
        LoginNote("Could not start device login: missing user code.");
    }
    free(got_id);
    free(got_code);
    JsonFree(&doc);
    free(body);
    free(err);
    return ok;
}

static void *DeviceLoginMain(void *arg)
{
    PicoApp *app = (PicoApp *)arg;
    char device_auth_id[128] = {0};
    char user_code[64] = {0};
    int interval = 5;
    if (RequestUserCode(device_auth_id, sizeof(device_auth_id), user_code, sizeof(user_code), &interval))
    {
        char msg[512];
        snprintf(msg, sizeof(msg),
                 "Sign in at %s/codex/device\nEnter code: `%s`\n\nThe code expires in %d minutes. "
                 "`/login cancel` to stop.",
                 kIssuer, user_code, PICO_DEVICE_TIMEOUT_SEC / 60);
        LoginNote(msg);

        time_t deadline = time(NULL) + PICO_DEVICE_TIMEOUT_SEC;
        int fails = 0;
        while (LoginSleep(interval))
        {
            if (time(NULL) >= deadline)
            {
                LoginNote("Device login timed out. Run `/login` to try again.");
                break;
            }
            char *code = NULL;
            char *verifier = NULL;
            char *error = NULL;
            DevicePoll state = PollDeviceOnce(device_auth_id, user_code, &code, &verifier, &error);
            bool keep_polling = state == DEVICE_PENDING;
            if (state == DEVICE_READY)
            {
                ExchangeDeviceCode(app, code, verifier);
            }
            else if (state == DEVICE_PENDING)
            {
                fails = 0;
            }
            else if (state == DEVICE_UNREACHABLE && ++fails < PICO_DEVICE_MAX_TRANSPORT_FAILS)
            {
                /* The user may already have entered the code, so ride out a few
                 * dropped requests rather than abandoning the login. */
                keep_polling = true;
            }
            else if (state == DEVICE_UNREACHABLE || state == DEVICE_FAILED)
            {
                char buf[512];
                snprintf(buf, sizeof(buf), "Device login failed: %s", error ? error : "unknown error");
                LoginNote(buf);
            }
            free(code);
            free(verifier);
            free(error);
            if (!keep_polling)
            {
                break;
            }
        }
    }
    pthread_mutex_lock(&g_login.mu);
    g_login.running = false;
    pthread_mutex_unlock(&g_login.mu);
    return NULL;
}

static void StartDeviceLogin(PicoApp *app)
{
    StopDeviceLogin();
    pthread_mutex_lock(&g_login.mu);
    /* Drop anything the previous attempt queued so it cannot mix into this flow. */
    for (int i = 0; i < g_login.note_count; i++)
    {
        free(g_login.notes[i]);
        g_login.notes[i] = NULL;
    }
    g_login.note_count = 0;
    g_login.cancel = false;
    g_login.running = true;
    bool spawned = pthread_create(&g_login.thread, NULL, DeviceLoginMain, app) == 0;
    g_login.joinable = spawned;
    if (!spawned)
    {
        g_login.running = false;
    }
    pthread_mutex_unlock(&g_login.mu);
    if (!spawned)
    {
        Note(app, "Could not start device login: thread creation failed.");
    }
}

/* The render thread owns the message list, so queued login text surfaces here. */
static void DrainLoginNotes(PicoApp *app)
{
    for (;;)
    {
        pthread_mutex_lock(&g_login.mu);
        char *text = NULL;
        if (g_login.note_count > 0)
        {
            text = g_login.notes[0];
            for (int i = 1; i < g_login.note_count; i++)
            {
                g_login.notes[i - 1] = g_login.notes[i];
            }
            g_login.note_count--;
        }
        bool reap = !text && !g_login.running && g_login.joinable;
        pthread_mutex_unlock(&g_login.mu);
        if (text)
        {
            Note(app, text);
            free(text);
            continue;
        }
        if (reap)
        {
            StopDeviceLogin();
        }
        return;
    }
}

static int Fold(int c)
{
    return (c >= 'A' && c <= 'Z') ? c - 'A' + 'a' : c;
}

static bool FoldEq(const char *a, const char *b)
{
    if (!a || !b)
    {
        return false;
    }
    while (*a && *b)
    {
        if (Fold((unsigned char)*a) != Fold((unsigned char)*b))
        {
            return false;
        }
        a++;
        b++;
    }
    return *a == *b;
}

static bool IsKeyArg(const char *s)
{
    return FoldEq(s, "key") || FoldEq(s, "api-key") || FoldEq(s, "apikey");
}

static bool IsCancelArg(const char *s)
{
    return FoldEq(s, "cancel");
}

/* `/login` forwards whatever followed the provider name, so pull out the single
 * verb here and reject anything trailing it. */
static void OpenAiLogin(PicoApp *app, const char *args)
{
    const char *p = args ? args : "";
    while (*p == ' ' || *p == '\t')
    {
        p++;
    }
    char verb[32];
    size_t n = 0;
    while (p[n] && p[n] != ' ' && p[n] != '\t' && n + 1 < sizeof(verb))
    {
        verb[n] = p[n];
        n++;
    }
    verb[n] = '\0';
    const char *tail = p + n;
    while (*tail == ' ' || *tail == '\t')
    {
        tail++;
    }
    if (tail[0] || (verb[0] && !IsCancelArg(verb) && !IsKeyArg(verb)))
    {
        Note(app, "Usage: `/login`, `/login key`, or `/login cancel`.");
        return;
    }
    if (IsCancelArg(verb))
    {
        if (LoginActive())
        {
            StopDeviceLogin();
            Note(app, "Login cancelled.");
        }
        else
        {
            Note(app, "No login in progress.");
        }
        return;
    }
    if (IsKeyArg(verb))
    {
        StopDeviceLogin();
        PicoAuthEntry e;
        pico_auth_copy(app, "openai", &e);
        if (!e.api_key || !e.api_key[0])
        {
            Note(app, "No API key. Set `PICO_API_KEY` or `OPENAI_API_KEY`.");
            pico_auth_entry_free(&e);
            return;
        }
        if (pico_auth_set_active(app, "openai", PICO_AUTH_API_KEY))
        {
            Note(app, "Using OpenAI API key.");
        }
        else
        {
            Note(app, "Using OpenAI API key, but `~/.config/pico/auth.json` could not be written, so "
                      "this choice will not survive a restart.");
        }
        pico_auth_entry_free(&e);
        return;
    }
    StartDeviceLogin(app);
}

static void OpenAiLogout(PicoApp *app)
{
    StopDeviceLogin();
    bool saved = pico_auth_clear_oauth(app, "openai");
    PicoAuthEntry e;
    pico_auth_copy(app, "openai", &e);
    if (!saved)
    {
        Note(app, "Logged out of ChatGPT, but `~/.config/pico/auth.json` could not be written, so the "
                  "stored tokens may still be on disk.");
    }
    else if (e.api_key && e.api_key[0])
    {
        Note(app, "Logged out of ChatGPT. Using API key.");
    }
    else
    {
        Note(app, "Logged out of ChatGPT.");
    }
    pico_auth_entry_free(&e);
}

static void OpenAiFrame(PicoApp *app, float dt)
{
    (void)dt;
    DrainLoginNotes(app);
}

static int PostOnce(const char *url, const char *bearer, const char *account_id, bool oauth,
                    const char *body, const char *session_id, LlmCtx *ctx, PicoLlmCancelFn cancel,
                    PicoLlmDeltaFn on_delta, void *user)
{
    memset(ctx, 0, sizeof(*ctx));
    ctx->cancel = cancel;
    ctx->on_delta = on_delta;
    ctx->user = user;
    JsonBuf_Init(&ctx->items);

    char *auth = NULL;
    char *acct = NULL;
    char session_hdr[80];
    PicoHttpPost req;
    memset(&req, 0, sizeof(req));
    req.url = url;
    req.body = body;
    req.cancel = HandleCancel;
    req.on_json = HandleJson;
    req.user = ctx;
    req.headers[req.header_count++] = "Content-Type: application/json";
    req.headers[req.header_count++] = "Accept: text/event-stream";
    if (bearer && bearer[0])
    {
        size_t n = strlen(bearer) + 32;
        auth = (char *)malloc(n);
        if (auth)
        {
            snprintf(auth, n, "Authorization: Bearer %s", bearer);
            req.headers[req.header_count++] = auth;
        }
    }
    if (oauth)
    {
        req.headers[req.header_count++] = "originator: pico";
        req.headers[req.header_count++] = "OpenAI-Beta: responses=experimental";
        if (account_id && account_id[0])
        {
            size_t n = strlen(account_id) + 32;
            acct = (char *)malloc(n);
            if (acct)
            {
                snprintf(acct, n, "chatgpt-account-id: %s", account_id);
                req.headers[req.header_count++] = acct;
            }
        }
    }
    if (session_id && session_id[0])
    {
        snprintf(session_hdr, sizeof(session_hdr), "session_id: %s", session_id);
        req.headers[req.header_count++] = session_hdr;
    }

    long http = 0;
    char *err = NULL;
    int rc = pico_http_post_sse(&req, &http, &err);
    ctx->http = http;
    free(auth);
    free(acct);
    if (rc == PICO_HTTP_CANCEL)
    {
        free(err);
        JsonBuf_Free(&ctx->items);
        free(ctx->error);
        memset(ctx, 0, sizeof(*ctx));
        return PICO_LLM_CANCEL;
    }
    if (rc != PICO_HTTP_OK && !ctx->error)
    {
        SetError(ctx, err ? err : "LLM request failed");
    }
    free(err);
    if (!ctx->error && http >= 400)
    {
        char buf[256];
        snprintf(buf, sizeof(buf), "HTTP %ld from %s", http, url);
        SetError(ctx, buf);
    }
    if (ctx->failed || ctx->error)
    {
        return PICO_LLM_FAIL;
    }
    return PICO_LLM_OK;
}

static const char *BearerOf(const PicoAuthEntry *auth, bool oauth)
{
    if (oauth)
    {
        return auth->access_token;
    }
    return auth->api_key;
}

static int OpenAiStream(PicoApp *app, const PicoLlmTurn *turn, PicoLlmCancelFn cancel,
                        PicoLlmDeltaFn on_delta, void *user, PicoLlmResult *out)
{
    if (out)
    {
        memset(out, 0, sizeof(*out));
    }
    if (!app || !turn || !out)
    {
        return PICO_LLM_FAIL;
    }

    TurnCancel tc = {.fn = cancel, .user = user};
    PicoAuthEntry auth;
    pico_auth_copy(app, "openai", &auth);
    bool oauth = strcmp(auth.active, PICO_AUTH_OAUTH) == 0;
    if (oauth)
    {
        if (OauthDue(&auth) || !auth.access_token || !auth.access_token[0])
        {
            if (!RefreshOauth(app, &auth, &tc))
            {
                pico_auth_entry_free(&auth);
                if (TurnCancelled(&tc))
                {
                    return PICO_LLM_CANCEL;
                }
                out->error = JsonDup("Codex login expired. Run `/login`.");
                return PICO_LLM_FAIL;
            }
        }
    }
    else if (!auth.api_key || !auth.api_key[0])
    {
        pico_auth_entry_free(&auth);
        out->error = JsonDup(
            "No OpenAI credentials. Run `/login` for a ChatGPT subscription, or set "
            "`PICO_API_KEY` / `OPENAI_API_KEY`.");
        return PICO_LLM_FAIL;
    }

    char url[1024];
    if (oauth)
    {
        snprintf(url, sizeof(url), "%s", kCodexResponses);
    }
    else
    {
        ResolveUrl(turn->base_url, url, sizeof(url));
    }
    char *body = BuildRequest(turn, oauth);
    if (!body)
    {
        pico_auth_entry_free(&auth);
        out->error = JsonDup("failed to build request");
        return PICO_LLM_FAIL;
    }

    const char *bearer = BearerOf(&auth, oauth);
    LlmCtx ctx;
    int rc = PostOnce(url, bearer, auth.account_id, oauth, body, turn->cache_key, &ctx, cancel, on_delta,
                      user);
    if (rc == PICO_LLM_FAIL && oauth && ctx.http == 401)
    {
        JsonBuf_Free(&ctx.items);
        free(ctx.error);
        ctx.error = NULL;
        if (!RefreshOauth(app, &auth, &tc))
        {
            free(body);
            pico_auth_entry_free(&auth);
            if (TurnCancelled(&tc))
            {
                return PICO_LLM_CANCEL;
            }
            out->error = JsonDup("Codex login expired. Run `/login`.");
            return PICO_LLM_FAIL;
        }
        bearer = BearerOf(&auth, true);
        rc = PostOnce(url, bearer, auth.account_id, true, body, turn->cache_key, &ctx, cancel, on_delta,
                      user);
    }
    if (rc == PICO_LLM_FAIL && ctx.error && strstr(ctx.error, "easoning"))
    {
        char *stripped = BodyWithoutReasoning(body);
        if (stripped)
        {
            JsonBuf_Free(&ctx.items);
            free(ctx.error);
            rc = PostOnce(url, BearerOf(&auth, oauth), auth.account_id, oauth, stripped, turn->cache_key,
                          &ctx, cancel, on_delta, user);
            free(stripped);
        }
    }
    free(body);
    pico_auth_entry_free(&auth);
    if (rc == PICO_LLM_CANCEL)
    {
        return PICO_LLM_CANCEL;
    }
    FillResult(&ctx, out);
    JsonBuf_Free(&ctx.items);
    if (rc != PICO_LLM_OK)
    {
        if (!out->error)
        {
            out->error = ctx.error ? ctx.error : JsonDup("LLM request failed");
            ctx.error = NULL;
        }
        free(ctx.error);
        return PICO_LLM_FAIL;
    }
    if (!out->error && !ctx.saw_text && !out->assistant_text && out->call_count == 0 && !out->think_text)
    {
        char buf[300];
        snprintf(buf, sizeof(buf), "empty response from %s", url);
        out->error = JsonDup(buf);
        free(ctx.error);
        return PICO_LLM_FAIL;
    }
    free(ctx.error);
    return PICO_LLM_OK;
}

static const char *FirstEnv(const char *a, const char *b)
{
    const char *v = getenv(a);
    if (v && v[0])
    {
        return v;
    }
    v = getenv(b);
    return (v && v[0]) ? v : NULL;
}

static void OpenAiInit(PicoApp *app)
{
    pico_add_provider(app, &(PicoProvider){.name = "openai", .stream = OpenAiStream});
    pico_add_auth(app, &(PicoAuth){.provider = "openai",
                                   .help = "ChatGPT device-code or API key",
                                   .verbs = "key cancel",
                                   .login = OpenAiLogin,
                                   .logout = OpenAiLogout});
    pico_auth_set_env_key(app, "openai", FirstEnv("PICO_API_KEY", "OPENAI_API_KEY"));
}

static void OpenAiShutdown(PicoApp *app)
{
    (void)app;
    StopDeviceLogin();
    pthread_mutex_lock(&g_login.mu);
    for (int i = 0; i < g_login.note_count; i++)
    {
        free(g_login.notes[i]);
        g_login.notes[i] = NULL;
    }
    g_login.note_count = 0;
    pthread_mutex_unlock(&g_login.mu);
}

PicoExt pico_ext_openai(void)
{
    return (PicoExt){
        .abi = PICO_EXT_ABI,
        .name = "openai",
        .description = "OpenAI-compatible provider",
        .init = OpenAiInit,
        .shutdown = OpenAiShutdown,
        .on_frame = OpenAiFrame,
    };
}
