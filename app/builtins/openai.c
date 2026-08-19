#define _POSIX_C_SOURCE 200809L

#include "pico/plugin.h"
#include "pico/http.h"
#include "json.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char kDefaultBase[] = "https://api.openai.com/v1";
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

static char *BuildRequest(const PicoLlmTurn *turn)
{
    JsonBuf b;
    JsonBuf_Init(&b);
    JsonBuf_Puts(&b, "{\"model\":");
    JsonBuf_String(&b, turn->model ? turn->model : "");
    JsonBuf_Puts(&b, ",\"prompt_cache_key\":");
    JsonBuf_String(&b, turn->cache_key ? turn->cache_key : "");
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
            char *text = ContentText(&doc, JsonObjGet(&doc, item, "summary"));
            if (!text || !text[0])
            {
                free(text);
                text = ContentText(&doc, JsonObjGet(&doc, item, "content"));
            }
            if (text && text[0])
            {
                if (think.len)
                {
                    JsonBuf_Puts(&think, "\n\n");
                }
                JsonBuf_Puts(&think, text);
            }
            free(text);
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

static int PostOnce(const char *url, const char *api_key, const char *body, const char *session_id,
                    LlmCtx *ctx, PicoLlmCancelFn cancel, PicoLlmDeltaFn on_delta, void *user)
{
    memset(ctx, 0, sizeof(*ctx));
    ctx->cancel = cancel;
    ctx->on_delta = on_delta;
    ctx->user = user;
    JsonBuf_Init(&ctx->items);

    char auth[600];
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
    if (api_key && api_key[0])
    {
        snprintf(auth, sizeof(auth), "Authorization: Bearer %s", api_key);
        req.headers[req.header_count++] = auth;
    }
    if (session_id && session_id[0])
    {
        snprintf(session_hdr, sizeof(session_hdr), "session_id: %s", session_id);
        req.headers[req.header_count++] = session_hdr;
    }

    long http = 0;
    char *err = NULL;
    int rc = pico_http_post_sse(&req, &http, &err);
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
    if (!turn->api_key || !turn->api_key[0])
    {
        out->error = JsonDup(
            "No API key. Set `PICO_API_KEY` or `OPENAI_API_KEY`, or put `api_key` in "
            "`~/.config/pico/settings.json`.");
        return PICO_LLM_FAIL;
    }

    char url[1024];
    ResolveUrl(turn->base_url, url, sizeof(url));
    char *body = BuildRequest(turn);
    if (!body)
    {
        out->error = JsonDup("failed to build request");
        return PICO_LLM_FAIL;
    }

    LlmCtx ctx;
    int rc = PostOnce(url, turn->api_key, body, turn->cache_key, &ctx, cancel, on_delta, user);
    if (rc == PICO_LLM_FAIL && ctx.error && strstr(ctx.error, "easoning"))
    {
        char *stripped = BodyWithoutReasoning(body);
        if (stripped)
        {
            JsonBuf_Free(&ctx.items);
            free(ctx.error);
            rc = PostOnce(url, turn->api_key, stripped, turn->cache_key, &ctx, cancel, on_delta, user);
            free(stripped);
        }
    }
    free(body);
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

static void OpenAiInit(PicoApp *app)
{
    pico_add_provider(app, &(PicoProvider){.name = "openai", .stream = OpenAiStream});
}

PicoExt pico_ext_openai(void)
{
    return (PicoExt){
        .abi = PICO_EXT_ABI,
        .name = "openai",
        .init = OpenAiInit,
    };
}
