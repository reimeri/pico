#define _POSIX_C_SOURCE 200809L

#include "builtins/responses.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char kBriefPrompt[] =
    "Handoff brief: goals, constraints, progress, decisions, next steps, exact values; omit noise. "
    "Return text, not a tool call.";

void pico_responses_resolve_url(const char *base, const char *fallback, char *out, size_t cap)
{
    if (!out || cap == 0)
    {
        return;
    }
    const char *src = base && base[0] ? base : fallback;
    snprintf(out, cap, "%s", src ? src : "");
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

static bool IsReasoningSignature(const char *sig)
{
    if (!sig)
    {
        return false;
    }
    while (*sig == ' ' || *sig == '\n' || *sig == '\r' || *sig == '\t')
    {
        sig++;
    }
    if (*sig != '{')
    {
        return false;
    }
    JsonDoc doc;
    if (JsonParse(&doc, sig, strlen(sig)) != 0)
    {
        return false;
    }
    bool ok = JsonIsObject(&doc, 0);
    char *type = JsonObjStr(&doc, 0, "type");
    if (ok && type && strcmp(type, "reasoning") != 0)
    {
        ok = false;
    }
    if (ok && !type && JsonObjGet(&doc, 0, "encrypted_content") < 0)
    {
        ok = false;
    }
    free(type);
    JsonFree(&doc);
    return ok;
}

static char *MapPicoItem(const char *json, const char *provider)
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
        char *sig = JsonObjStr(&doc, 0, "thinking_signature");
        bool has_text = text && text[0];
        if (provider && strcmp(provider, "openai") == 0 && IsReasoningSignature(sig))
        {
            if (has_text)
            {
                char *msg = BuildResponsesMessage("assistant", text, true);
                JsonBuf b;
                JsonBuf_Init(&b);
                JsonBuf_Puts(&b, sig);
                JsonBuf_Putc(&b, ',');
                JsonBuf_Puts(&b, msg);
                free(msg);
                out = JsonBuf_Steal(&b);
            }
            else
            {
                out = JsonDup(sig);
            }
        }
        else if (has_text)
        {
            out = BuildResponsesMessage("assistant", text, true);
        }
        free(text);
        free(sig);
    }
    else if (type && strcmp(type, "tool_call") == 0)
    {
        JsonBuf b;
        JsonBuf_Init(&b);
        JsonBuf_Puts(&b, "{\"type\":\"function_call\",\"name\":");
        char *name = JsonObjStr(&doc, 0, "name");
        char *args = JsonObjStr(&doc, 0, "arguments");
        char *id = JsonObjStr(&doc, 0, "call_id");
        char *item_id = JsonObjStr(&doc, 0, "item_id");
        JsonBuf_String(&b, name ? name : "");
        JsonBuf_Puts(&b, ",\"arguments\":");
        JsonBuf_String(&b, args ? args : "{}");
        JsonBuf_Puts(&b, ",\"call_id\":");
        JsonBuf_String(&b, id ? id : "");
        if (item_id && item_id[0])
        {
            JsonBuf_Puts(&b, ",\"id\":");
            JsonBuf_String(&b, item_id);
        }
        JsonBuf_Putc(&b, '}');
        out = JsonBuf_Steal(&b);
        free(name);
        free(args);
        free(id);
        free(item_id);
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
        if (prov && provider && strcmp(prov, provider) == 0)
        {
            out = JsonObjRaw(&doc, 0, "json");
        }
        free(prov);
    }
    free(type);
    JsonFree(&doc);
    return out;
}

char *pico_responses_build_request(const PicoLlmTurn *turn, const PicoResponsesBuildOpts *opts)
{
    if (!turn || !opts)
    {
        return NULL;
    }
    JsonBuf b;
    JsonBuf_Init(&b);
    JsonBuf_Puts(&b, "{\"model\":");
    JsonBuf_String(&b, turn->model ? turn->model : "");
    JsonBuf_Puts(&b, ",\"prompt_cache_key\":");
    JsonBuf_String(&b, turn->cache_key ? turn->cache_key : "");
    if (opts->store_false)
    {
        JsonBuf_Puts(&b, ",\"store\":false");
    }
    if (opts->include_encrypted_reasoning)
    {
        JsonBuf_Puts(&b, ",\"include\":[\"reasoning.encrypted_content\"]");
    }
    JsonBuf_Puts(&b, ",\"stream\":true,\"instructions\":");
    JsonBuf_String(&b, turn->instructions ? turn->instructions : "");
    JsonBuf_Puts(&b, ",\"input\":[");
    int wrote = 0;
    for (int i = 0; i < turn->input_count; i++)
    {
        char *mapped = MapPicoItem(turn->input_json[i], opts->provider);
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
        if (opts->reasoning_summary_auto)
        {
            JsonBuf_Puts(&b, ",\"summary\":\"auto\"");
        }
        JsonBuf_Putc(&b, '}');
    }
    JsonBuf_Putc(&b, '}');
    return JsonBuf_Steal(&b);
}

static int FullTokenEnd(const JsonDoc *doc, int tok)
{
    int end = JsonTokEnd(doc, tok);
    if (end >= 0 && (size_t)end < doc->len && doc->src[end] == '"')
    {
        end++;
    }
    return end;
}

char *pico_responses_body_without_reasoning(const char *body)
{
    if (!body)
    {
        return NULL;
    }
    JsonDoc doc;
    size_t len = strlen(body);
    if (JsonParse(&doc, body, len) != 0)
    {
        return NULL;
    }
    if (!JsonIsObject(&doc, 0) || JsonObjGet(&doc, 0, "reasoning") < 0)
    {
        JsonFree(&doc);
        return NULL;
    }

    JsonBuf b;
    JsonBuf_Init(&b);
    JsonBuf_Putc(&b, '{');
    bool wrote = false;
    int pairs = JsonObjLen(&doc, 0);
    for (int i = 0; i < pairs; i++)
    {
        int key = -1;
        int val = -1;
        if (!JsonObjPair(&doc, 0, i, &key, &val) || JsonEq(&doc, key, "reasoning"))
        {
            continue;
        }
        int start = JsonTokStart(&doc, key) - 1;
        int end = FullTokenEnd(&doc, val);
        if (start < 0 || end <= start || (size_t)end > len)
        {
            JsonBuf_Free(&b);
            JsonFree(&doc);
            return NULL;
        }
        if (wrote)
        {
            JsonBuf_Putc(&b, ',');
        }
        JsonBuf_Append(&b, body + start, (size_t)(end - start));
        wrote = true;
    }
    JsonBuf_Putc(&b, '}');
    JsonFree(&doc);
    return JsonBuf_Steal(&b);
}

static void SetError(PicoResponsesCtx *c, const char *msg)
{
    if (!msg || c->error)
    {
        return;
    }
    c->error = JsonDup(msg);
    c->failed = true;
}

static void AppendItem(PicoResponsesCtx *c, const char *raw)
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

static void CopyOutputArray(PicoResponsesCtx *c, const JsonDoc *doc, int arr)
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

static void HandleResponseObject(PicoResponsesCtx *c, const JsonDoc *doc, int obj)
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

static void EmitDelta(PicoResponsesCtx *c, PicoLlmDeltaKind kind, const char *s, size_t n)
{
    if ((kind == PICO_LLM_DELTA_THINKING || kind == PICO_LLM_DELTA_THINKING_SUMMARY) && n > 0 &&
        !ThinkUsable(s, n))
    {
        return;
    }
    if (kind == PICO_LLM_DELTA_TEXT && s && n)
    {
        c->saw_text = true;
    }
    if (kind == PICO_LLM_DELTA_THINKING_SUMMARY && n > 0)
    {
        c->saw_summary = true;
    }
    if (!c->on_delta)
    {
        return;
    }
    if (n == 0)
    {
        if (kind == PICO_LLM_DELTA_THINKING_SUMMARY)
        {
            c->on_delta(c->user, kind, "", 0);
        }
        return;
    }
    if (s)
    {
        c->on_delta(c->user, kind, s, n);
    }
}

static void BeginSummaryStep(PicoResponsesCtx *c, int output_index, int summary_index)
{
    if (c->summary_output_index == output_index && c->summary_index == summary_index)
    {
        return;
    }
    c->summary_output_index = output_index;
    c->summary_index = summary_index;
    JsonBuf_Clear(&c->summary);
    EmitDelta(c, PICO_LLM_DELTA_THINKING_SUMMARY, "", 0);
}

static void SetSummarySnapshot(PicoResponsesCtx *c, const char *text)
{
    if (!ThinkUsable(text, text ? strlen(text) : 0))
    {
        return;
    }
    JsonBuf_Clear(&c->summary);
    JsonBuf_Puts(&c->summary, text);
    EmitDelta(c, PICO_LLM_DELTA_THINKING_SUMMARY, c->summary.data, c->summary.len);
}

static void HandleSummaryDelta(PicoResponsesCtx *c, const JsonDoc *doc, const char *delta)
{
    int output_index = JsonObjInt(doc, 0, "output_index", 0);
    int summary_index = JsonObjInt(doc, 0, "summary_index", 0);
    BeginSummaryStep(c, output_index, summary_index);
    if (!delta || !delta[0] || !ThinkUsable(delta, strlen(delta)))
    {
        return;
    }
    JsonBuf_Puts(&c->summary, delta);
    EmitDelta(c, PICO_LLM_DELTA_THINKING_SUMMARY, c->summary.data, c->summary.len);
}

static void HandleSummaryDone(PicoResponsesCtx *c, const JsonDoc *doc)
{
    int output_index = JsonObjInt(doc, 0, "output_index", 0);
    int summary_index = JsonObjInt(doc, 0, "summary_index", 0);
    BeginSummaryStep(c, output_index, summary_index);
    char *text = JsonObjStr(doc, 0, "text");
    if (text)
    {
        SetSummarySnapshot(c, text);
        free(text);
    }
}

static void EmitSummaryParts(PicoResponsesCtx *c, const JsonDoc *doc, int summary, int item_index)
{
    if (c->saw_summary)
    {
        return;
    }
    if (JsonIsArray(doc, summary))
    {
        int n = JsonArrayLen(doc, summary);
        for (int i = 0; i < n; i++)
        {
            int part = JsonArrayAt(doc, summary, i);
            char *text = JsonObjStr(doc, part, "text");
            if (!text)
            {
                continue;
            }
            BeginSummaryStep(c, item_index, i);
            SetSummarySnapshot(c, text);
            free(text);
        }
        return;
    }
    char *text = JsonStrDup(doc, summary);
    if (text)
    {
        BeginSummaryStep(c, item_index, 0);
        SetSummarySnapshot(c, text);
        free(text);
    }
}

static void EmitMessageText(PicoResponsesCtx *c, const JsonDoc *doc, int items)
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

static bool HandleCancel(void *user)
{
    PicoResponsesCtx *c = (PicoResponsesCtx *)user;
    return c->cancel && c->cancel(c->user);
}

static bool HandleJson(void *user, const char *event, const char *json, size_t len)
{
    PicoResponsesCtx *c = (PicoResponsesCtx *)user;
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
    else if (type && strcmp(type, "response.reasoning_text.delta") == 0)
    {
        char *delta = JsonObjStr(&doc, 0, "delta");
        if (delta)
        {
            EmitDelta(c, PICO_LLM_DELTA_THINKING, delta, strlen(delta));
            free(delta);
        }
    }
    else if (type && strcmp(type, "response.reasoning_summary_text.delta") == 0)
    {
        char *delta = JsonObjStr(&doc, 0, "delta");
        HandleSummaryDelta(c, &doc, delta);
        free(delta);
    }
    else if (type && strcmp(type, "response.reasoning_summary_text.done") == 0)
    {
        HandleSummaryDone(c, &doc);
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

void pico_responses_fill_result(PicoResponsesCtx *c, PicoLlmResult *out)
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
            call->item_id = JsonObjStr(&doc, item, "id");
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
        if (JsonEq(&doc, JsonObjGet(&doc, item, "type"), "reasoning"))
        {
            int summary_tok = JsonObjGet(&doc, item, "summary");
            EmitSummaryParts(c, &doc, summary_tok, i);
            char *summary = ContentText(&doc, summary_tok);
            char *content = ContentText(&doc, JsonObjGet(&doc, item, "content"));
            AppendThink(&think, summary);
            if (content && (!summary || strcmp(content, summary) != 0))
            {
                AppendThink(&think, content);
            }
            free(out->think_signature);
            out->think_signature = JsonDup(raw);
            free(summary);
            free(content);
        }
        else
        {
            AddRaw(out, raw);
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

void pico_responses_ctx_free(PicoResponsesCtx *c)
{
    if (!c)
    {
        return;
    }
    JsonBuf_Free(&c->items);
    JsonBuf_Free(&c->summary);
    free(c->error);
    c->error = NULL;
}

int pico_responses_post(const char *url, const char *body, const char *bearer,
                        const char *const extra_headers[], int extra_count, PicoLlmCancelFn cancel,
                        PicoLlmDeltaFn on_delta, void *user, PicoResponsesCtx *ctx)
{
    memset(ctx, 0, sizeof(*ctx));
    ctx->cancel = cancel;
    ctx->on_delta = on_delta;
    ctx->user = user;
    ctx->summary_output_index = -1;
    ctx->summary_index = -1;
    JsonBuf_Init(&ctx->items);
    JsonBuf_Init(&ctx->summary);

    char *auth = NULL;
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
    for (int i = 0; i < extra_count && extra_headers; i++)
    {
        if (extra_headers[i] && extra_headers[i][0] && req.header_count < PICO_HTTP_MAX_HEADERS)
        {
            req.headers[req.header_count++] = extra_headers[i];
        }
    }

    long http = 0;
    char *err = NULL;
    int rc = pico_http_post_sse(&req, &http, &err);
    ctx->http = http;
    free(auth);
    if (rc == PICO_HTTP_CANCEL)
    {
        free(err);
        pico_responses_ctx_free(ctx);
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
        snprintf(buf, sizeof(buf), "HTTP %ld from %s", http, url ? url : "");
        SetError(ctx, buf);
    }
    if (ctx->failed || ctx->error)
    {
        return PICO_LLM_FAIL;
    }
    return PICO_LLM_OK;
}
