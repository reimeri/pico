#define _POSIX_C_SOURCE 200809L

#include "builtins/responses.h"
#include "canonical.h"
#include "builtins/provider_retry.h"

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

static char *PartMediaUrl(const PicoLlmPart *part)
{
    if (part->url && part->url[0])
    {
        return JsonDup(part->url);
    }
    if (part->path && part->path[0])
    {
        return pico_canonical_data_url(part->path, part->mime);
    }
    return NULL;
}

static char *BuildResponsesParts(const char *role, const PicoLlmPart *parts, int n, bool assistant)
{
    JsonBuf b;
    JsonBuf_Init(&b);
    JsonBuf_Puts(&b, "{\"type\":\"message\",\"role\":");
    JsonBuf_String(&b, role ? role : "user");
    JsonBuf_Puts(&b, ",\"content\":[");
    int wrote = 0;
    for (int i = 0; i < n; i++)
    {
        const PicoLlmPart *p = &parts[i];
        if (p->kind == PICO_LLM_PART_TEXT)
        {
            if (wrote)
            {
                JsonBuf_Putc(&b, ',');
            }
            JsonBuf_Puts(&b, "{\"type\":");
            JsonBuf_String(&b, assistant ? "output_text" : "input_text");
            JsonBuf_Puts(&b, ",\"text\":");
            JsonBuf_String(&b, p->text ? p->text : "");
            JsonBuf_Putc(&b, '}');
            wrote++;
        }
        else if (p->kind == PICO_LLM_PART_REFUSAL)
        {
            if (wrote)
            {
                JsonBuf_Putc(&b, ',');
            }
            JsonBuf_Puts(&b, "{\"type\":\"refusal\",\"refusal\":");
            JsonBuf_String(&b, p->text ? p->text : "");
            JsonBuf_Putc(&b, '}');
            wrote++;
        }
        else if (p->kind == PICO_LLM_PART_IMAGE)
        {
            char *url = PartMediaUrl(p);
            if (!url)
            {
                continue;
            }
            if (wrote)
            {
                JsonBuf_Putc(&b, ',');
            }
            JsonBuf_Puts(&b, "{\"type\":\"input_image\",\"image_url\":");
            JsonBuf_String(&b, url);
            JsonBuf_Putc(&b, '}');
            free(url);
            wrote++;
        }
        else if (p->kind == PICO_LLM_PART_AUDIO)
        {
            char *data = (p->path && p->path[0]) ? pico_canonical_file_base64(p->path, NULL) : NULL;
            char *fmt = pico_canonical_audio_format(p->path, p->mime);
            if (wrote)
            {
                JsonBuf_Putc(&b, ',');
            }
            if (data)
            {
                JsonBuf_Puts(&b, "{\"type\":\"input_audio\",\"input_audio\":{\"data\":");
                JsonBuf_String(&b, data);
                JsonBuf_Puts(&b, ",\"format\":");
                JsonBuf_String(&b, fmt && fmt[0] ? fmt : "wav");
                JsonBuf_Puts(&b, "}}");
            }
            else
            {
                char *url = PartMediaUrl(p);
                if (!url)
                {
                    if (wrote)
                    {
                        /* comma already written; emit empty text to keep JSON valid */
                        JsonBuf_Puts(&b, "{\"type\":\"input_text\",\"text\":\"\"}");
                    }
                    free(fmt);
                    continue;
                }
                JsonBuf_Puts(&b, "{\"type\":\"input_audio\",\"audio_url\":");
                JsonBuf_String(&b, url);
                JsonBuf_Putc(&b, '}');
                free(url);
            }
            free(data);
            free(fmt);
            wrote++;
        }
    }
    JsonBuf_Puts(&b, "]}");
    return JsonBuf_Steal(&b);
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
        PicoLlmPart *parts = NULL;
        int n = 0;
        pico_canonical_parse_parts(&doc, 0, &parts, &n);
        if (pico_canonical_parts_have_media(parts, n))
        {
            out = BuildResponsesParts("user", parts, n, false);
        }
        else
        {
            char *plain = pico_canonical_plain_text(parts, n);
            out = BuildResponsesMessage("user", plain, false);
            free(plain);
        }
        pico_canonical_free_parts(parts, n);
    }
    else if (type && strcmp(type, "provider_state") == 0)
    {
        if (pico_canonical_provider_state_valid(&doc, 0) &&
            JsonEq(&doc, JsonObjGet(&doc, 0, "provider"), provider) &&
            JsonEq(&doc, JsonObjGet(&doc, 0, "format"), "openai.compaction.v1"))
            out = JsonObjRaw(&doc, 0, "data");
    }
    else if (type && strcmp(type, "context") == 0)
    {
        PicoLlmPart *parts = NULL;
        int n = 0;
        pico_canonical_parse_parts(&doc, 0, &parts, &n);
        char *plain = pico_canonical_plain_text(parts, n);
        out = BuildResponsesMessage("developer", plain, false);
        free(plain);
        pico_canonical_free_parts(parts, n);
    }
    else if (type && strcmp(type, "assistant") == 0)
    {
        PicoLlmPart *parts = NULL;
        int n = 0;
        pico_canonical_parse_parts(&doc, 0, &parts, &n);
        char *plain = pico_canonical_plain_text(parts, n);
        char *sig = JsonObjStr(&doc, 0, "thinking_signature");
        bool structured = pico_canonical_parts_need_log(parts, n);
        bool has_visible = (plain && plain[0]) || pico_canonical_parts_have_media(parts, n);
        char *msg = NULL;
        if (has_visible)
        {
            msg = structured ? BuildResponsesParts("assistant", parts, n, true)
                             : BuildResponsesMessage("assistant", plain, true);
        }
        if (provider && strcmp(provider, "openai") == 0 && IsReasoningSignature(sig))
        {
            if (msg)
            {
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
        else
        {
            out = msg;
        }
        pico_canonical_free_parts(parts, n);
        free(plain);
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
    free(type);
    JsonFree(&doc);
    return out;
}

static bool ResponsesInputMediaValid(const char *json)
{
    JsonDoc doc;
    if (!json || JsonParse(&doc, json, strlen(json)) != 0)
    {
        return false;
    }
    if (!pico_canonical_tool_images_valid(&doc, true)) { JsonFree(&doc); return false; }
    char *type = JsonObjStr(&doc, 0, "type");
    bool content = type && (strcmp(type, "user") == 0 || strcmp(type, "assistant") == 0 || strcmp(type, "tool_result") == 0);
    PicoLlmPart *parts = NULL;
    int n = 0;
    bool valid = !content || pico_canonical_parse_parts(&doc, 0, &parts, &n);
    for (int i = 0; valid && i < n; i++)
    {
        PicoLlmPart *part = &parts[i];
        if (part->kind != PICO_LLM_PART_IMAGE && part->kind != PICO_LLM_PART_AUDIO)
        {
            continue;
        }
        if (part->url && part->url[0])
        {
            continue;
        }
        char *data = (part->path && part->path[0])
                         ? pico_canonical_file_base64(part->path, NULL)
                         : NULL;
        valid = data != NULL;
        free(data);
    }
    pico_canonical_free_parts(parts, n);
    free(type);
    JsonFree(&doc);
    return valid;
}

bool pico_responses_openai_fast_route(const char *base, bool codex)
{
    if (codex)
        return true;
    char url[1024];
    pico_responses_resolve_url(base, "https://api.openai.com/v1", url, sizeof(url));
    return strcmp(url, "https://api.openai.com/v1/responses") == 0;
}

const char *pico_responses_openai_service_tier(const PicoLlmTurn *turn, bool codex)
{
    if (!turn || !pico_responses_openai_fast_route(turn->base_url, codex))
        return NULL;
    return turn->fast ? "priority" : (codex ? NULL : "default");
}

char *pico_responses_build_request(const PicoLlmTurn *turn, const PicoResponsesBuildOpts *opts)
{
    if (!turn || !opts)
    {
        return NULL;
    }
    for (int i = 0; i < turn->input_count; i++)
    {
        JsonDoc state;
        if (JsonParse(&state, turn->input_json[i], strlen(turn->input_json[i]))) return NULL;
        bool opaque = pico_canonical_is_provider_state(&state, 0);
        bool valid = !opaque || (pico_canonical_provider_state_valid(&state, 0) &&
            JsonEq(&state, JsonObjGet(&state, 0, "provider"), opts->provider) &&
            JsonEq(&state, JsonObjGet(&state, 0, "format"), "openai.compaction.v1"));
        JsonFree(&state);
        if (!valid || !ResponsesInputMediaValid(turn->input_json[i]))
        {
            return NULL;
        }
    }
    JsonBuf b;
    JsonBuf_Init(&b);
    JsonBuf_Puts(&b, "{\"model\":");
    JsonBuf_String(&b, turn->model ? turn->model : "");
    if (opts->service_tier)
    {
        JsonBuf_Puts(&b, ",\"service_tier\":");
        JsonBuf_String(&b, opts->service_tier);
    }
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
        JsonDoc current, next;
        bool result = !JsonParse(&current, turn->input_json[i], strlen(turn->input_json[i]));
        bool next_result = false;
        if (result) { result = JsonEq(&current, JsonObjGet(&current, 0, "type"), "tool_result"); JsonFree(&current); }
        if (i + 1 < turn->input_count && !JsonParse(&next, turn->input_json[i+1], strlen(turn->input_json[i+1]))) {
            next_result = JsonEq(&next, JsonObjGet(&next, 0, "type"), "tool_result"); JsonFree(&next);
        }
        if (result && !next_result) {
            int begin = i;
            while (begin > 0) {
                JsonDoc prev;
                if (JsonParse(&prev, turn->input_json[begin-1], strlen(turn->input_json[begin-1]))) break;
                bool is_result = JsonEq(&prev, JsonObjGet(&prev, 0, "type"), "tool_result"); JsonFree(&prev);
                if (!is_result) break;
                --begin;
            }
            for (int j = begin; j <= i; ++j) {
                char *observation = pico_canonical_tool_observation(turn->input_json[j]);
                char *image = observation ? MapPicoItem(observation, opts->provider) : NULL;
                if (image) { JsonBuf_Putc(&b, ','); JsonBuf_Puts(&b, image); wrote++; }
                free(image); free(observation);
            }
        }
    }
    if (turn->compact)
    {
        if (wrote)
        {
            JsonBuf_Putc(&b, ',');
        }
        if (turn->native_compact) JsonBuf_Puts(&b, "{\"type\":\"compaction_trigger\"}");
        else {
            char *brief = BuildResponsesMessage("user", kBriefPrompt, false);
            JsonBuf_Puts(&b, brief);
            free(brief);
        }
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
            JsonBuf_Puts(&b, ",\"strict\":false,\"parameters\":");
            JsonBuf_Puts(&b, turn->tools[i].params_json && turn->tools[i].params_json[0]
                                 ? turn->tools[i].params_json
                                 : "{\"type\":\"object\",\"properties\":{}}");
            JsonBuf_Putc(&b, '}');
        }
        JsonBuf_Putc(&b, ']');
    }
    if (turn->compact)
    {
        JsonBuf_Puts(&b, turn->native_compact ?
            ",\"tool_choice\":\"auto\",\"parallel_tool_calls\":true" : ",\"tool_choice\":\"none\"");
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

static bool GuardDelta(PicoResponsesCtx *c, PicoStreamKind kind, int index,
                       const char *text, size_t len)
{
    const char *error = pico_stream_guard_delta(&c->guard, kind, index, text, len);
    if (error) SetError(c, error);
    return !c->failed;
}

static bool GuardToolSize(PicoResponsesCtx *c, const char *arguments)
{
    const char *error = pico_stream_guard_tool_size(&c->guard, arguments ? strlen(arguments) : 0);
    if (error) SetError(c, error);
    return !c->failed;
}

/* Terminal snapshots may arrive without any preceding argument deltas. */
static bool GuardOutputTools(PicoResponsesCtx *c, const JsonDoc *doc, int output)
{
    for (int i = 0; i < JsonArrayLen(doc, output); i++)
    {
        int item = JsonArrayAt(doc, output, i);
        if (JsonEq(doc, JsonObjGet(doc, item, "type"), "function_call"))
        {
            char *arguments = JsonObjStr(doc, item, "arguments");
            bool ok = GuardToolSize(c, arguments);
            free(arguments);
            if (!ok) return false;
        }
    }
    return !c->failed;
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
    if (!GuardOutputTools(c, doc, JsonObjGet(doc, obj, "output")))
        return;
    if (!JsonIsObject(doc, obj))
    {
        return;
    }
    if (JsonEq(doc, JsonObjGet(doc, obj, "status"), "completed")) c->completed = true;
    if (c->completed) {
        if (c->completed_response) SetError(c, "Duplicate terminal Responses object");
        else {
            c->completed_response = JsonRawDup(doc, obj);
            if (!c->completed_response) SetError(c, "Could not copy terminal Responses object");
        }
    }
    if (JsonEq(doc, JsonObjGet(doc, obj, "status"), "incomplete") ||
        JsonEq(doc, JsonObjGet(doc, obj, "status"), "failed") ||
        JsonEq(doc, JsonObjGet(doc, obj, "status"), "cancelled"))
        SetError(c, "Responses request did not complete successfully");
    char *tier = JsonObjStr(doc, obj, "service_tier");
    if (tier && tier[0])
        snprintf(c->service_tier, sizeof(c->service_tier), "%s", tier);
    free(tier);
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
    if (err >= 0 && !JsonIsNull(doc, err))
    {
        c->capacity_error |= pico_provider_capacity_error(doc, err);
        char *msg = NULL;
        if (JsonIsObject(doc, err))
        {
            msg = JsonObjStr(doc, err, "message");
        }
        else
        {
            msg = JsonStrDup(doc, err);
        }
        SetError(c, msg && msg[0] ? msg : "LLM request failed");
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
    if (c->failed) return;
    if ((kind == PICO_LLM_DELTA_TEXT || kind == PICO_LLM_DELTA_THINKING) &&
        !GuardDelta(c, kind == PICO_LLM_DELTA_TEXT ? PICO_STREAM_TEXT : PICO_STREAM_THINKING,
                    -1, s, n))
        return;
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
            PicoLlmDelta d = {.kind = kind, .text = "", .len = 0, .call_index = -1};
            c->on_delta(c->user, &d);
        }
        return;
    }
    if (s)
    {
        PicoLlmDelta d = {.kind = kind, .text = s, .len = n, .call_index = -1};
        c->on_delta(c->user, &d);
    }
}

static void EmitToolBegin(PicoResponsesCtx *c, int output_index, const char *call_id,
                          const char *name)
{
    if (c->failed || !c->on_delta || !name || !name[0])
    {
        return;
    }
    PicoLlmDelta d = {.kind = PICO_LLM_DELTA_TOOL_CALL_BEGIN,
                      .call_index = output_index,
                      .call_id = call_id,
                      .name = name};
    c->on_delta(c->user, &d);
}

static void EmitToolArgs(PicoResponsesCtx *c, int output_index, const char *s, size_t n)
{
    if (c->failed || !GuardDelta(c, PICO_STREAM_ARGUMENTS, output_index, s, n))
        return;
    if (!c->on_delta || !s || n == 0)
    {
        return;
    }
    PicoLlmDelta d = {
        .kind = PICO_LLM_DELTA_TOOL_CALL_ARGS, .text = s, .len = n, .call_index = output_index};
    c->on_delta(c->user, &d);
}

static void EmitToolDone(PicoResponsesCtx *c, int output_index, const char *call_id,
                         const char *name, const char *arguments)
{
    if (!GuardToolSize(c, arguments) || !c->on_delta)
    {
        return;
    }
    const char *args = arguments ? arguments : "";
    PicoLlmDelta d = {.kind = PICO_LLM_DELTA_TOOL_CALL_DONE,
                      .text = args,
                      .len = strlen(args),
                      .call_index = output_index,
                      .call_id = call_id,
                      .name = name};
    c->on_delta(c->user, &d);
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
    if (!GuardDelta(c, PICO_STREAM_SUMMARY, -1, delta, strlen(delta)))
        return;
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
    if (c->failed) return false;
    JsonDoc doc;
    if (!(len == 6 && !memcmp(json, "[DONE]", 6)) && !JsonValidSyntax(json, len)) c->malformed = true;
    if (JsonParse(&doc, json, len) != 0)
    {
        if (len != 6 || memcmp(json, "[DONE]", 6)) c->malformed = true;
        return true;
    }
    int err = JsonObjGet(&doc, 0, "error");
    bool capacity_event = pico_provider_capacity_message(event);
    c->capacity_error |= capacity_event || pico_provider_capacity_error(&doc, err);
    if (capacity_event || (err >= 0 && !JsonIsNull(&doc, err)))
    {
        char *msg = err < 0 ? JsonObjStr(&doc, 0, "message") :
                    JsonIsObject(&doc, err) ? JsonObjStr(&doc, err, "message") : JsonStrDup(&doc, err);
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
        c->capacity_error |= pico_provider_capacity_error(&doc, detail);
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
                char *call_id = JsonObjStr(&doc, item, "call_id");
                int output_index = JsonObjInt(&doc, 0, "output_index", -1);
                EmitToolBegin(c, output_index, call_id, name);
                free(call_id);
                free(name);
            }
        }
    }
    else if (type && strcmp(type, "response.function_call_arguments.delta") == 0)
    {
        char *delta = JsonObjStr(&doc, 0, "delta");
        if (delta)
        {
            int output_index = JsonObjInt(&doc, 0, "output_index", -1);
            EmitToolArgs(c, output_index, delta, strlen(delta));
            free(delta);
        }
    }
    else if (type && strcmp(type, "response.function_call_arguments.done") == 0)
    {
        char *arguments = JsonObjStr(&doc, 0, "arguments");
        /* Missing arguments must not announce an empty completion. */
        if (arguments)
        {
            int output_index = JsonObjInt(&doc, 0, "output_index", -1);
            EmitToolDone(c, output_index, NULL, NULL, arguments);
        }
        free(arguments);
    }
    else if (type && strcmp(type, "response.output_item.done") == 0)
    {
        int item = JsonObjGet(&doc, 0, "item");
        if (JsonEq(&doc, JsonObjGet(&doc, item, "type"), "function_call"))
        {
            char *arguments = JsonObjStr(&doc, item, "arguments");
            /* Missing arguments must not clobber an earlier arguments.done. */
            if (arguments)
            {
                char *call_id = JsonObjStr(&doc, item, "call_id");
                char *name = JsonObjStr(&doc, item, "name");
                int output_index = JsonObjInt(&doc, 0, "output_index", -1);
                EmitToolDone(c, output_index, call_id, name, arguments);
                free(call_id);
                free(name);
            }
            free(arguments);
        }
        char *raw = JsonObjRaw(&doc, 0, "item");
        if (!c->failed) AppendItem(c, raw);
        free(raw);
    }
    else if (type && strcmp(type, "response.completed") == 0)
    {
        int response = JsonObjGet(&doc, 0, "response");
        if (JsonIsObject(&doc, response)) c->completed = true;
        HandleResponseObject(c, &doc, response);
    }
    else if (type && strcmp(type, "response.incomplete") == 0)
    {
        HandleResponseObject(c, &doc, JsonObjGet(&doc, 0, "response"));
        SetError(c, "Responses request did not complete successfully");
    }
    else if ((type && (strcmp(type, "response.failed") == 0 || strcmp(type, "error") == 0 ||
                      strstr(type, "invalid_request") || strstr(type, "authentication") ||
                      strstr(type, "permission") || strstr(type, "rate_limit") ||
                      pico_provider_capacity_message(type))) || pico_provider_capacity_error(&doc, 0))
    {
        c->capacity_error |= pico_provider_capacity_error(&doc, 0);
        HandleResponseObject(c, &doc, JsonObjGet(&doc, 0, "response"));
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
        if (!c->saw_text && c->item_count > 0 && c->on_delta)
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

static void FailUnsupported(PicoLlmResult *out, const char *type)
{
    char buf[192];
    snprintf(buf, sizeof(buf), "unsupported output: %s", type && type[0] ? type : "unknown");
    if (out && !out->error)
    {
        out->error = JsonDup(buf);
    }
}

static bool HasAnnotations(const JsonDoc *doc, int obj)
{
    int ann = JsonObjGet(doc, obj, "annotations");
    if (ann < 0)
    {
        return false;
    }
    if (JsonIsArray(doc, ann))
    {
        return JsonArrayLen(doc, ann) > 0;
    }
    return true;
}

static char *ContentPartUrl(const JsonDoc *doc, int part)
{
    char *url = JsonObjStr(doc, part, "url");
    if (url && url[0])
    {
        return url;
    }
    free(url);
    int image_url = JsonObjGet(doc, part, "image_url");
    if (JsonIsObject(doc, image_url))
    {
        return JsonObjStr(doc, image_url, "url");
    }
    if (image_url >= 0)
    {
        return JsonStrDup(doc, image_url);
    }
    int audio = JsonObjGet(doc, part, "input_audio");
    if (audio < 0)
    {
        audio = JsonObjGet(doc, part, "audio");
    }
    if (JsonIsObject(doc, audio))
    {
        char *data = JsonObjStr(doc, audio, "data");
        char *format = JsonObjStr(doc, audio, "format");
        if (data && data[0])
        {
            JsonBuf b;
            JsonBuf_Init(&b);
            JsonBuf_Puts(&b, "data:audio/");
            JsonBuf_Puts(&b, format && format[0] ? format : "wav");
            JsonBuf_Puts(&b, ";base64,");
            JsonBuf_Puts(&b, data);
            free(data);
            free(format);
            return JsonBuf_Steal(&b);
        }
        free(data);
        free(format);
        return JsonObjStr(doc, audio, "url");
    }
    return NULL;
}

static bool ProjectMessageParts(PicoLlmItem *item, PicoLlmResult *out, const JsonDoc *doc, int message)
{
    if (HasAnnotations(doc, message))
    {
        FailUnsupported(out, "annotations");
        return false;
    }
    int content = JsonObjGet(doc, message, "content");
    if (JsonIsArray(doc, content))
    {
        int n = JsonArrayLen(doc, content);
        for (int i = 0; i < n; i++)
        {
            int part = JsonArrayAt(doc, content, i);
            if (HasAnnotations(doc, part))
            {
                FailUnsupported(out, "annotations");
                return false;
            }
            char *type = JsonObjStr(doc, part, "type");
            const char *t = type ? type : "";
            bool ok = true;
            if (strcmp(t, "output_text") == 0 || strcmp(t, "text") == 0 || strcmp(t, "input_text") == 0)
            {
                char *text = JsonObjStr(doc, part, "text");
                ok = pico_llm_item_add_part(item, PICO_LLM_PART_TEXT, text ? text : "", NULL, NULL, NULL);
                free(text);
            }
            else if (strcmp(t, "output_refusal") == 0 || strcmp(t, "refusal") == 0)
            {
                char *text = JsonObjStr(doc, part, "text");
                if (!text)
                {
                    text = JsonObjStr(doc, part, "refusal");
                }
                ok = pico_llm_item_add_part(item, PICO_LLM_PART_REFUSAL, text ? text : "", NULL, NULL, NULL);
                free(text);
            }
            else if (strcmp(t, "input_image") == 0 || strcmp(t, "output_image") == 0 ||
                     strcmp(t, "image") == 0 || strcmp(t, "image_url") == 0)
            {
                char *url = ContentPartUrl(doc, part);
                char *mime = JsonObjStr(doc, part, "mime");
                ok = pico_llm_item_add_part(item, PICO_LLM_PART_IMAGE, NULL, NULL, url, mime);
                free(url);
                free(mime);
            }
            else if (strcmp(t, "input_audio") == 0 || strcmp(t, "output_audio") == 0 ||
                     strcmp(t, "audio") == 0)
            {
                char *url = ContentPartUrl(doc, part);
                char *mime = JsonObjStr(doc, part, "mime");
                ok = pico_llm_item_add_part(item, PICO_LLM_PART_AUDIO, NULL, NULL, url, mime);
                free(url);
                free(mime);
            }
            else
            {
                FailUnsupported(out, t[0] ? t : "unknown");
                free(type);
                return false;
            }
            free(type);
            if (!ok)
            {
                return false;
            }
        }
        return true;
    }
    char *text = JsonStrDup(doc, content);
    bool ok = true;
    if (text && text[0])
    {
        ok = pico_llm_item_add_part(item, PICO_LLM_PART_TEXT, text, NULL, NULL, NULL);
    }
    free(text);
    return ok;
}

static void AttachThink(PicoLlmItem *item, JsonBuf *think, char **sig)
{
    if (!item)
    {
        return;
    }
    if (think && think->len)
    {
        free(item->thinking);
        item->thinking = JsonBuf_Steal(think);
        JsonBuf_Init(think);
    }
    if (sig && *sig)
    {
        free(item->thinking_signature);
        item->thinking_signature = *sig;
        *sig = NULL;
    }
}

static PicoLlmItem *FlushPendingAssistant(PicoLlmResult *out, JsonBuf *think, char **sig)
{
    if ((!think || !think->len) && (!sig || !*sig))
    {
        return NULL;
    }
    PicoLlmItem *item = pico_llm_result_add_item(out, PICO_LLM_ITEM_ASSISTANT);
    AttachThink(item, think, sig);
    return item;
}

void pico_responses_fill_result(PicoResponsesCtx *c, PicoLlmResult *out)
{
    out->input_tokens = c->input_tokens;
    out->cached_tokens = c->cached_tokens;
    snprintf(out->service_tier, sizeof(out->service_tier), "%s", c->service_tier);
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
    char *pending_sig = NULL;
    PicoLlmItem *last_asst = NULL;
    for (int i = 0; i < n && !out->error; i++)
    {
        int item = JsonArrayAt(&doc, 0, i);
        char *type = JsonObjStr(&doc, item, "type");
        if (!type || !type[0])
        {
            FailUnsupported(out, "unknown");
            free(type);
            break;
        }
        if (strcmp(type, "function_call") == 0)
        {
            FlushPendingAssistant(out, &think, &pending_sig);
            last_asst = NULL;
            char *call_id = JsonObjStr(&doc, item, "call_id");
            char *name = JsonObjStr(&doc, item, "name");
            char *arguments = JsonObjStr(&doc, item, "arguments");
            char *item_id = JsonObjStr(&doc, item, "id");
            pico_llm_result_add_tool_call(out, call_id, name, arguments, item_id);
            free(call_id);
            free(name);
            free(arguments);
            free(item_id);
        }
        else if (strcmp(type, "message") == 0)
        {
            PicoLlmItem *asst = pico_llm_result_add_item(out, PICO_LLM_ITEM_ASSISTANT);
            if (!asst || !ProjectMessageParts(asst, out, &doc, item))
            {
                free(type);
                break;
            }
            AttachThink(asst, &think, &pending_sig);
            last_asst = asst;
        }
        else if (strcmp(type, "reasoning") == 0)
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
            free(pending_sig);
            pending_sig = JsonRawDup(&doc, item);
            free(summary);
            free(content);
            if (last_asst)
            {
                AttachThink(last_asst, &think, &pending_sig);
            }
        }
        else
        {
            FailUnsupported(out, type);
        }
        free(type);
    }
    if (!out->error)
    {
        FlushPendingAssistant(out, &think, &pending_sig);
    }
    JsonFree(&doc);
    JsonBuf_Free(&wrapped);
    JsonBuf_Free(&think);
    free(pending_sig);
}

void pico_responses_ctx_free(PicoResponsesCtx *c)
{
    if (!c)
    {
        return;
    }
    pico_stream_guard_free(&c->guard);
    JsonBuf_Free(&c->items);
    JsonBuf_Free(&c->summary);
    free(c->error);
    c->error = NULL;
    free(c->completed_response); c->completed_response = NULL;
}

bool pico_responses_feed(PicoResponsesCtx *c, const char *json, size_t len)
{
    if (!c)
    {
        return false;
    }
    return HandleJson(c, NULL, json, len);
}

static void HandleRetry(void *user, int retry, int max_retries, int delay, const char *error)
{
    (void)error;
    PicoResponsesCtx *c = user;
    if (!c->on_delta) return;
    char status[128];
    if (delay)
        snprintf(status, sizeof(status), "Connection failed — retry %d/%d in %ds", retry, max_retries, delay);
    else
        snprintf(status, sizeof(status), "Connecting — retry %d/%d…", retry, max_retries);
    PicoLlmDelta delta = {.kind = PICO_LLM_DELTA_STATUS, .text = status,
                          .len = strlen(status), .call_index = -1};
    c->on_delta(c->user, &delta);
}

int pico_responses_post(const char *url, const char *body, const char *bearer,
                        const char *const extra_headers[], int extra_count, PicoLlmCancelFn cancel,
                        PicoLlmDeltaFn on_delta, void *user, PicoResponsesCtx *ctx)
{
    char *auth = NULL;
    PicoHttpPost req;
    memset(&req, 0, sizeof(req));
    req.url = url;
    req.body = body;
    req.cancel = HandleCancel;
    req.on_json = HandleJson;
    req.on_retry = HandleRetry;
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

    for (int retry = 0;; retry++)
    {
        memset(ctx, 0, sizeof(*ctx));
        ctx->cancel = cancel;
        ctx->on_delta = on_delta;
        ctx->user = user;
        ctx->summary_output_index = -1;
        ctx->summary_index = -1;
        JsonBuf_Init(&ctx->items);
        JsonBuf_Init(&ctx->summary);
        long http = 0;
        char *err = NULL;
        int rc = pico_http_post_sse(&req, &http, &err);
        ctx->http = http;
        ctx->capacity_error |= http == 429 || http == 503;
        if (rc != PICO_HTTP_CANCEL)
        {
            if (rc != PICO_HTTP_OK && !ctx->error)
                SetError(ctx, err ? err : "LLM request failed");
            if (!ctx->error && http >= 400)
            {
                char buf[256];
                snprintf(buf, sizeof(buf), "HTTP %ld from %s", http, url ? url : "");
                SetError(ctx, buf);
            }
        }
        free(err);
        if (rc == PICO_HTTP_CANCEL)
        {
            free(auth);
            pico_responses_ctx_free(ctx);
            memset(ctx, 0, sizeof(*ctx));
            return PICO_LLM_CANCEL;
        }
        if (!ctx->failed && !ctx->error)
        {
            free(auth);
            return PICO_LLM_OK;
        }
        if (retry >= PICO_PROVIDER_MAX_RETRIES || !ctx->capacity_error || ctx->guard.error)
        {
            free(auth);
            return PICO_LLM_FAIL;
        }
        pico_responses_ctx_free(ctx);
        memset(ctx, 0, sizeof(*ctx));
        if (!pico_provider_retry_wait(retry + 1, cancel, on_delta, user))
        {
            free(auth);
            return PICO_LLM_CANCEL;
        }
    }
}


static bool ValidCompactionItem(const JsonDoc *doc, int item)
{
    int encrypted = JsonObjGet(doc, item, "encrypted_content");
    int id = JsonObjGet(doc, item, "id");
    return JsonIsObject(doc, item) && JsonEq(doc, JsonObjGet(doc, item, "type"), "compaction") &&
        JsonIsString(doc, encrypted) && JsonTokEnd(doc, encrypted) > JsonTokStart(doc, encrypted) &&
        (id < 0 || JsonIsNull(doc, id) ||
         (JsonIsString(doc, id) && JsonTokEnd(doc, id) > JsonTokStart(doc, id)));
}

char *pico_responses_check_state(const PicoLlmTurn *turn, const char *backend, const char *scope)
{
    for (int i = 0; i < turn->input_count; ++i) {
        JsonDoc doc;
        if (JsonParse(&doc, turn->input_json[i], strlen(turn->input_json[i])))
            return JsonDup("Invalid conversation history");
        bool state = pico_canonical_is_provider_state(&doc, 0);
        bool valid = !state || (pico_canonical_provider_state_valid(&doc, 0) &&
            JsonEq(&doc, JsonObjGet(&doc, 0, "provider"), "openai") &&
            JsonEq(&doc, JsonObjGet(&doc, 0, "format"), "openai.compaction.v1") &&
            backend && scope && JsonEq(&doc, JsonObjGet(&doc, 0, "backend"), backend) &&
            JsonEq(&doc, JsonObjGet(&doc, 0, "scope"), scope));
        if (valid && state) {
            int data = JsonObjGet(&doc, 0, "data");
            valid = ValidCompactionItem(&doc, data);
        }
        JsonFree(&doc);
        if (!valid) return JsonDup("Native compaction state cannot be used with this backend/account/key. "
            "Restore the original OpenAI backend and account/key, or start a new session.");
    }
    return NULL;
}

/* Byte-level BPE cannot use more text tokens than UTF-8 bytes. Include
 * canonical envelope overhead and a separate media allowance; never tokenize
 * ciphertext. Cross-model native-state acceptance remains the backend's call. */
static size_t RetainedTokens(const JsonDoc *doc, int item)
{
    PicoLlmPart *parts = NULL;
    int n = 0;
    size_t tokens = 32;
    if (!pico_canonical_parse_parts(doc, item, &parts, &n)) return SIZE_MAX;
    for (int i = 0; i < n; ++i) {
        if (parts[i].text) tokens += strlen(parts[i].text) + 16;
        if (parts[i].kind == PICO_LLM_PART_IMAGE || parts[i].kind == PICO_LLM_PART_AUDIO) tokens += 32768;
    }
    pico_canonical_free_parts(parts, n);
    return tokens;
}

void pico_responses_compact_result(PicoResponsesCtx *c, const PicoLlmTurn *turn,
                                  const char *backend, const char *scope, PicoLlmResult *out)
{
    out->input_tokens = c->input_tokens;
    out->cached_tokens = c->cached_tokens;
    snprintf(out->service_tier, sizeof(out->service_tier), "%s", c->service_tier);
    if (c->error) { out->error = JsonDup(c->error); return; }
    if (!c->completed || c->malformed || !c->completed_response) {
        out->error = JsonDup("Native compaction stream ended without a valid response.completed"); return;
    }
    JsonDoc terminal;
    if (JsonParse(&terminal, c->completed_response, strlen(c->completed_response))) {
        out->error = JsonDup("Invalid native compaction terminal response"); return;
    }
    int terminal_output = JsonObjGet(&terminal, 0, "output");
    int terminal_artifact = -1, terminal_count = 0;
    bool terminal_valid = JsonEq(&terminal, JsonObjGet(&terminal, 0, "status"), "completed") &&
                          JsonIsArray(&terminal, terminal_output);
    for (int i = 0; terminal_valid && i < JsonArrayLen(&terminal, terminal_output); ++i) {
        int item = JsonArrayAt(&terminal, terminal_output, i);
        if (JsonEq(&terminal, JsonObjGet(&terminal, item, "type"), "compaction")) {
            terminal_artifact = item; terminal_count++;
        } else if (!JsonEq(&terminal, JsonObjGet(&terminal, item, "type"), "reasoning")) terminal_valid = false;
    }
    char *terminal_encrypted = terminal_artifact >= 0 ? JsonObjStr(&terminal, terminal_artifact, "encrypted_content") : NULL;
    terminal_valid = terminal_valid && terminal_count == 1 && terminal_encrypted && terminal_encrypted[0] &&
        ValidCompactionItem(&terminal, terminal_artifact);
    if (!terminal_valid) {
        free(terminal_encrypted); JsonFree(&terminal);
        out->error = JsonDup("Invalid native compaction terminal status/output"); return;
    }
    JsonBuf array; JsonBuf_Init(&array);
    JsonBuf_Putc(&array, '['); JsonBuf_Append(&array, c->items.data, c->items.len); JsonBuf_Putc(&array, ']');
    JsonDoc doc;
    if (!array.data || array.failed || JsonParse(&doc, array.data, array.len)) {
        JsonBuf_Free(&array); free(terminal_encrypted); JsonFree(&terminal);
        out->error = JsonDup("Invalid native compaction output"); return;
    }
    int n = JsonArrayLen(&doc, 0), artifact = -1, count = 0;
    bool valid = true;
    for (int i = 0; i < n; ++i) {
        int item = JsonArrayAt(&doc, 0, i);
        if (JsonEq(&doc, JsonObjGet(&doc, item, "type"), "compaction")) { artifact = item; count++; }
        else if (!JsonEq(&doc, JsonObjGet(&doc, item, "type"), "reasoning")) valid = false;
    }
    char *encrypted = artifact >= 0 ? JsonObjStr(&doc, artifact, "encrypted_content") : NULL;
    valid = valid && count == 1 && encrypted && !strcmp(encrypted, terminal_encrypted) && backend && scope &&
        ValidCompactionItem(&doc, artifact);
    char *stream_id = artifact >= 0 ? JsonObjStr(&doc, artifact, "id") : NULL;
    char *terminal_id = JsonObjStr(&terminal, terminal_artifact, "id");
    valid = valid && ((!stream_id && !terminal_id) ||
                     (stream_id && terminal_id && !strcmp(stream_id, terminal_id)));
    free(stream_id); free(terminal_id); free(terminal_encrypted); free(encrypted);
    if (!valid) {
        out->error = JsonDup("Native compaction requires exactly one valid compaction item and no tool or message output");
        JsonFree(&doc); JsonFree(&terminal); JsonBuf_Free(&array); return;
    }
    /* Keep whole recent user messages; stop at the first that does not fit,
     * rather than retaining disconnected older turns or truncating attachments. */
    size_t budget = 64000;
    if (turn->context_limit > 0 && (size_t)turn->context_limit / 4 < budget)
        budget = (size_t)turn->context_limit / 4;
    size_t fixed = turn->instructions ? strlen(turn->instructions) : 0;
    for (int i = 0; i < turn->tool_count; ++i) {
        fixed += (turn->tools[i].params_json ? strlen(turn->tools[i].params_json) : 0);
        fixed += (turn->tools[i].description ? strlen(turn->tools[i].description) : 0);
    }
    budget = fixed < budget ? budget - fixed : 0;
    char *seed = turn->compact_seed ? pico_canonical_user_text(turn->compact_seed) : NULL;
    if (turn->compact_seed && !seed) { valid = false; }
    if (seed) {
        size_t cost = strlen(turn->compact_seed) + 48;
        if (cost > budget) valid = false;
        else budget -= cost;
    }
    bool *keep = calloc((size_t)turn->compaction_history_count + 1, sizeof(*keep));
    if (!keep) valid = false;
    for (int i = turn->compaction_history_count - 1; valid && i >= 0; --i) {
        JsonDoc user;
        if (JsonParse(&user, turn->compaction_history_json[i], strlen(turn->compaction_history_json[i]))) { valid = false; break; }
        if (JsonEq(&user, JsonObjGet(&user, 0, "type"), "user") &&
            !JsonEq(&user, JsonObjGet(&user, 0, "synthetic"), "true") &&
            (!seed || strcmp(seed, turn->compaction_history_json[i]))) {
            size_t cost = RetainedTokens(&user, 0);
            if (cost > budget) { JsonFree(&user); break; }
            budget -= cost; keep[i] = true;
        }
        JsonFree(&user);
    }
    JsonBuf result; JsonBuf_Init(&result); JsonBuf_Putc(&result, '[');
    bool wrote = false;
    if (seed) { JsonBuf_Puts(&result, seed); wrote = true; }
    for (int i = 0; valid && i < turn->compaction_history_count; ++i) if (keep[i]) {
        if (wrote) JsonBuf_Putc(&result, ',');
        JsonBuf_Puts(&result, turn->compaction_history_json[i]); wrote = true;
    }
    if (wrote) JsonBuf_Putc(&result, ',');
    JsonBuf_Puts(&result, "{\"type\":\"provider_state\",\"provider\":\"openai\",\"format\":\"openai.compaction.v1\",\"backend\":");
    JsonBuf_String(&result, backend); JsonBuf_Puts(&result, ",\"scope\":"); JsonBuf_String(&result, scope);
    JsonBuf_Puts(&result, ",\"model\":"); JsonBuf_String(&result, turn->model);
    JsonBuf_Puts(&result, ",\"data\":");
    char *raw = JsonRawDup(&terminal, terminal_artifact);
    if (!raw) valid = false;
    JsonBuf_Puts(&result, raw); JsonBuf_Puts(&result, "}]");
    if (valid) out->compaction_json = JsonBuf_Steal(&result);
    if (!out->compaction_json) out->error = JsonDup("Could not prepare native compaction history");
    free(raw); free(seed); free(keep); JsonBuf_Free(&result); JsonFree(&doc); JsonFree(&terminal); JsonBuf_Free(&array);
}
