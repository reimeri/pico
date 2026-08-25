#define _POSIX_C_SOURCE 200809L

#include "builtins/completions.h"
#include "canonical.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char kBriefPrompt[] =
    "Handoff brief: goals, constraints, progress, decisions, next steps, exact values; omit noise. "
    "Return text, not a tool call.";

void pico_completions_resolve_url(const char *base, const char *fallback, char *out, size_t cap)
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
    size_t suffix = strlen("/chat/completions");
    if (n >= suffix && strcmp(out + n - suffix, "/chat/completions") == 0)
    {
        return;
    }
    snprintf(out + n, cap - n, "/chat/completions");
}

bool pico_completions_resolve_canonical_url(const char *base, const char *canonical_base, char *out,
                                            size_t cap)
{
    if (!canonical_base || !canonical_base[0] || !out || cap == 0)
    {
        return false;
    }
    char canonical[1024];
    char requested[1024];
    pico_completions_resolve_url(NULL, canonical_base, canonical, sizeof(canonical));
    pico_completions_resolve_url(base, canonical_base, requested, sizeof(requested));
    if (strcmp(requested, canonical) != 0 || strlen(canonical) >= cap)
    {
        out[0] = '\0';
        return false;
    }
    snprintf(out, cap, "%s", canonical);
    return true;
}

static bool EffortOn(const char *effort)
{
    return effort && effort[0] && strcmp(effort, "none") != 0 && strcmp(effort, "off") != 0;
}

static char *ItemType(const char *json)
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
    JsonFree(&doc);
    return type;
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

static char *CompletionsPartText(const PicoLlmPart *parts, int n, PicoLlmPartKind kind)
{
    JsonBuf b;
    JsonBuf_Init(&b);
    for (int i = 0; i < n; i++)
    {
        if (parts[i].kind == kind && parts[i].text)
        {
            JsonBuf_Puts(&b, parts[i].text);
        }
    }
    return JsonBuf_Steal(&b);
}

static char *CompletionsContentArray(const PicoLlmPart *parts, int n, bool assistant)
{
    JsonBuf b;
    JsonBuf_Init(&b);
    JsonBuf_Putc(&b, '[');
    int wrote = 0;
    for (int i = 0; i < n; i++)
    {
        const PicoLlmPart *p = &parts[i];
        if (p->kind == PICO_LLM_PART_TEXT ||
            (!assistant && p->kind == PICO_LLM_PART_REFUSAL))
        {
            if (wrote)
            {
                JsonBuf_Putc(&b, ',');
            }
            JsonBuf_Puts(&b, "{\"type\":\"text\",\"text\":");
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
            JsonBuf_Puts(&b, "{\"type\":\"image_url\",\"image_url\":{\"url\":");
            JsonBuf_String(&b, url);
            JsonBuf_Puts(&b, "}}");
            free(url);
            wrote++;
        }
        else if (p->kind == PICO_LLM_PART_AUDIO)
        {
            char *data = (p->path && p->path[0]) ? pico_canonical_file_base64(p->path, NULL) : NULL;
            char *fmt = pico_canonical_audio_format(p->path, p->mime);
            if (!data)
            {
                free(fmt);
                continue;
            }
            if (wrote)
            {
                JsonBuf_Putc(&b, ',');
            }
            JsonBuf_Puts(&b, "{\"type\":\"input_audio\",\"input_audio\":{\"data\":");
            JsonBuf_String(&b, data);
            JsonBuf_Puts(&b, ",\"format\":");
            JsonBuf_String(&b, fmt && fmt[0] ? fmt : "wav");
            JsonBuf_Puts(&b, "}}");
            free(data);
            free(fmt);
            wrote++;
        }
    }
    JsonBuf_Putc(&b, ']');
    return JsonBuf_Steal(&b);
}

static void FlushAssistant(JsonBuf *b, int *wrote, const PicoLlmPart *parts, int part_n,
                           const char *thinking, const char *signature, const char *const *call_json,
                           int call_count, bool requires_reasoning)
{
    char *text = CompletionsPartText(parts, part_n, PICO_LLM_PART_TEXT);
    char *refusal = CompletionsPartText(parts, part_n, PICO_LLM_PART_REFUSAL);
    bool media = pico_canonical_parts_have_media(parts, part_n);
    bool has_text = (text && text[0]) || (refusal && refusal[0]) || media;
    bool has_calls = call_count > 0;
    bool has_thinking = thinking && thinking[0];
    if (!has_text && !has_calls && !has_thinking && !requires_reasoning)
    {
        free(text);
        free(refusal);
        return;
    }
    if (*wrote)
    {
        JsonBuf_Putc(b, ',');
    }
    JsonBuf_Puts(b, "{\"role\":\"assistant\",\"content\":");
    if (media)
    {
        char *content = CompletionsContentArray(parts, part_n, true);
        JsonBuf_Puts(b, content ? content : "[]");
        free(content);
    }
    else if (has_text)
    {
        JsonBuf_String(b, text ? text : "");
    }
    else
    {
        JsonBuf_Puts(b, "null");
    }
    if (refusal && refusal[0])
    {
        JsonBuf_Puts(b, ",\"refusal\":");
        JsonBuf_String(b, refusal);
    }
    if (requires_reasoning || has_thinking)
    {
        JsonBuf_Puts(b, ",\"reasoning_content\":");
        JsonBuf_String(b, thinking ? thinking : "");
    }
    (void)signature;
    if (has_calls)
    {
        JsonBuf_Puts(b, ",\"tool_calls\":[");
        int call_wrote = 0;
        for (int i = 0; i < call_count; i++)
        {
            JsonDoc doc;
            if (JsonParse(&doc, call_json[i], strlen(call_json[i])) != 0)
            {
                continue;
            }
            if (call_wrote)
            {
                JsonBuf_Putc(b, ',');
            }
            char *id = JsonObjStr(&doc, 0, "call_id");
            char *name = JsonObjStr(&doc, 0, "name");
            char *args = JsonObjStr(&doc, 0, "arguments");
            JsonBuf_Puts(b, "{\"id\":");
            JsonBuf_String(b, id ? id : "");
            JsonBuf_Puts(b, ",\"type\":\"function\",\"function\":{\"name\":");
            JsonBuf_String(b, name ? name : "");
            JsonBuf_Puts(b, ",\"arguments\":");
            JsonBuf_String(b, args ? args : "{}");
            JsonBuf_Puts(b, "}}");
            free(id);
            free(name);
            free(args);
            JsonFree(&doc);
            call_wrote++;
        }
        JsonBuf_Putc(b, ']');
    }
    JsonBuf_Putc(b, '}');
    (*wrote)++;
    free(text);
    free(refusal);
}

static bool CompletionsInputMediaValid(const char *json)
{
    JsonDoc doc;
    if (!json || JsonParse(&doc, json, strlen(json)) != 0)
    {
        return false;
    }
    char *type = JsonObjStr(&doc, 0, "type");
    bool content = type && (strcmp(type, "user") == 0 || strcmp(type, "assistant") == 0);
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
            valid = part->kind == PICO_LLM_PART_IMAGE;
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

char *pico_completions_build_request(const PicoLlmTurn *turn, const PicoCompletionsBuildOpts *opts)
{
    if (!turn || !opts)
    {
        return NULL;
    }
    for (int i = 0; i < turn->input_count; i++)
    {
        if (!CompletionsInputMediaValid(turn->input_json[i]))
        {
            return NULL;
        }
    }
    JsonBuf b;
    JsonBuf_Init(&b);
    JsonBuf_Puts(&b, "{\"model\":");
    JsonBuf_String(&b, turn->model ? turn->model : "");
    JsonBuf_Puts(&b, ",\"stream\":true,\"stream_options\":{\"include_usage\":true}");
    if (opts->store_false)
    {
        JsonBuf_Puts(&b, ",\"store\":false");
    }
    if (opts->max_tokens > 0 && opts->max_tokens_field && opts->max_tokens_field[0])
    {
        JsonBuf_Putc(&b, ',');
        JsonBuf_Puts(&b, opts->max_tokens_field);
        JsonBuf_Putc(&b, ':');
        JsonBuf_Int(&b, opts->max_tokens);
    }
    bool reasoning_on = EffortOn(turn->effort);
    if (opts->thinking == PICO_COMPLETIONS_THINKING_DEEPSEEK)
    {
        JsonBuf_Puts(&b, ",\"thinking\":{\"type\":");
        JsonBuf_String(&b, reasoning_on ? "enabled" : "disabled");
        JsonBuf_Putc(&b, '}');
        if (reasoning_on)
        {
            JsonBuf_Puts(&b, ",\"reasoning_effort\":");
            JsonBuf_String(&b, turn->effort);
        }
    }
    JsonBuf_Puts(&b, ",\"messages\":[");
    int wrote = 0;
    if (turn->instructions && turn->instructions[0])
    {
        JsonBuf_Puts(&b, "{\"role\":\"system\",\"content\":");
        JsonBuf_String(&b, turn->instructions);
        JsonBuf_Putc(&b, '}');
        wrote++;
    }
    bool requires_reasoning =
        opts->requires_reasoning_content && opts->thinking == PICO_COMPLETIONS_THINKING_DEEPSEEK &&
        reasoning_on;
    for (int i = 0; i < turn->input_count; i++)
    {
        char *type = ItemType(turn->input_json[i]);
        if (!type)
        {
            continue;
        }
        JsonDoc doc;
        if (JsonParse(&doc, turn->input_json[i], strlen(turn->input_json[i])) != 0)
        {
            free(type);
            continue;
        }
        if (strcmp(type, "user") == 0)
        {
            PicoLlmPart *parts = NULL;
            int n = 0;
            pico_canonical_parse_parts(&doc, 0, &parts, &n);
            if (wrote)
            {
                JsonBuf_Putc(&b, ',');
            }
            JsonBuf_Puts(&b, "{\"role\":\"user\",\"content\":");
            if (pico_canonical_parts_have_media(parts, n))
            {
                char *content = CompletionsContentArray(parts, n, false);
                JsonBuf_Puts(&b, content ? content : "[]");
                free(content);
            }
            else
            {
                char *plain = pico_canonical_plain_text(parts, n);
                JsonBuf_String(&b, plain ? plain : "");
                free(plain);
            }
            JsonBuf_Putc(&b, '}');
            wrote++;
            pico_canonical_free_parts(parts, n);
        }
        else if (strcmp(type, "context") == 0)
        {
            PicoLlmPart *parts = NULL;
            int n = 0;
            pico_canonical_parse_parts(&doc, 0, &parts, &n);
            char *plain = pico_canonical_plain_text(parts, n);
            if (wrote)
            {
                JsonBuf_Putc(&b, ',');
            }
            JsonBuf_Puts(&b, "{\"role\":\"system\",\"content\":");
            JsonBuf_String(&b, plain ? plain : "");
            JsonBuf_Putc(&b, '}');
            wrote++;
            free(plain);
            pico_canonical_free_parts(parts, n);
        }
        else if (strcmp(type, "assistant") == 0)
        {
            PicoLlmPart *parts = NULL;
            int n = 0;
            pico_canonical_parse_parts(&doc, 0, &parts, &n);
            char *thinking = JsonObjStr(&doc, 0, "thinking");
            char *signature = JsonObjStr(&doc, 0, "thinking_signature");
            const char *calls[32];
            int call_count = 0;
            int j = i + 1;
            for (; j < turn->input_count && call_count < 32; j++)
            {
                char *next = ItemType(turn->input_json[j]);
                bool is_call = next && strcmp(next, "tool_call") == 0;
                free(next);
                if (!is_call)
                {
                    break;
                }
                calls[call_count++] = turn->input_json[j];
            }
            FlushAssistant(&b, &wrote, parts, n, thinking, signature, calls, call_count, requires_reasoning);
            i = j - 1;
            pico_canonical_free_parts(parts, n);
            free(thinking);
            free(signature);
        }
        else if (strcmp(type, "tool_call") == 0)
        {
            const char *calls[1] = {turn->input_json[i]};
            FlushAssistant(&b, &wrote, NULL, 0, NULL, NULL, calls, 1, requires_reasoning);
        }
        else if (strcmp(type, "tool_result") == 0)
        {
            char *id = JsonObjStr(&doc, 0, "call_id");
            char *output = JsonObjStr(&doc, 0, "output");
            if (wrote)
            {
                JsonBuf_Putc(&b, ',');
            }
            JsonBuf_Puts(&b, "{\"role\":\"tool\",\"tool_call_id\":");
            JsonBuf_String(&b, id ? id : "");
            JsonBuf_Puts(&b, ",\"content\":");
            JsonBuf_String(&b, output ? output : "");
            JsonBuf_Putc(&b, '}');
            wrote++;
            free(id);
            free(output);
        }
        JsonFree(&doc);
        free(type);
    }
    if (turn->compact)
    {
        if (wrote)
        {
            JsonBuf_Putc(&b, ',');
        }
        JsonBuf_Puts(&b, "{\"role\":\"user\",\"content\":");
        JsonBuf_String(&b, kBriefPrompt);
        JsonBuf_Putc(&b, '}');
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
            JsonBuf_Puts(&b, "{\"type\":\"function\",\"function\":{\"name\":");
            JsonBuf_String(&b, turn->tools[i].name ? turn->tools[i].name : "");
            JsonBuf_Puts(&b, ",\"description\":");
            JsonBuf_String(&b, turn->tools[i].description ? turn->tools[i].description : "");
            JsonBuf_Puts(&b, ",\"parameters\":");
            JsonBuf_Puts(&b, turn->tools[i].params_json && turn->tools[i].params_json[0]
                                 ? turn->tools[i].params_json
                                 : "{\"type\":\"object\",\"properties\":{}}");
            JsonBuf_Puts(&b, "}}");
        }
        JsonBuf_Putc(&b, ']');
    }
    if (turn->compact)
    {
        JsonBuf_Puts(&b, ",\"tool_choice\":\"none\"");
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

static int FullTokenStart(const JsonDoc *doc, int tok)
{
    int start = JsonTokStart(doc, tok);
    if (start > 0 && (size_t)start <= doc->len && doc->src[start - 1] == '"')
    {
        start--;
    }
    return start;
}

static bool AppendRawToken(JsonBuf *b, const JsonDoc *doc, int tok)
{
    int start = FullTokenStart(doc, tok);
    int end = FullTokenEnd(doc, tok);
    if (start < 0 || end <= start || (size_t)end > doc->len)
    {
        return false;
    }
    JsonBuf_Append(b, doc->src + start, (size_t)(end - start));
    return true;
}

static bool AppendMessageWithoutReasoning(JsonBuf *b, const JsonDoc *doc, int obj)
{
    if (!JsonIsObject(doc, obj))
    {
        return AppendRawToken(b, doc, obj);
    }
    JsonBuf_Putc(b, '{');
    bool wrote = false;
    int pairs = JsonObjLen(doc, obj);
    for (int i = 0; i < pairs; i++)
    {
        int key = -1;
        int val = -1;
        if (!JsonObjPair(doc, obj, i, &key, &val))
        {
            return false;
        }
        if (JsonEq(doc, key, "reasoning_content"))
        {
            continue;
        }
        if (wrote)
        {
            JsonBuf_Putc(b, ',');
        }
        int start = FullTokenStart(doc, key);
        int end = FullTokenEnd(doc, val);
        if (start < 0 || end <= start || (size_t)end > doc->len)
        {
            return false;
        }
        JsonBuf_Append(b, doc->src + start, (size_t)(end - start));
        wrote = true;
    }
    JsonBuf_Putc(b, '}');
    return true;
}

static bool AppendMessagesWithoutReasoning(JsonBuf *b, const JsonDoc *doc, int messages)
{
    if (!JsonIsArray(doc, messages))
    {
        return AppendRawToken(b, doc, messages);
    }
    JsonBuf_Putc(b, '[');
    int n = JsonArrayLen(doc, messages);
    for (int i = 0; i < n; i++)
    {
        if (i)
        {
            JsonBuf_Putc(b, ',');
        }
        if (!AppendMessageWithoutReasoning(b, doc, JsonArrayAt(doc, messages, i)))
        {
            return false;
        }
    }
    JsonBuf_Putc(b, ']');
    return true;
}

static bool SkipThinkingKey(const JsonDoc *doc, int key)
{
    return JsonEq(doc, key, "thinking") || JsonEq(doc, key, "reasoning_effort");
}

char *pico_completions_body_without_thinking(const char *body)
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
    if (!JsonIsObject(&doc, 0) ||
        (JsonObjGet(&doc, 0, "thinking") < 0 && JsonObjGet(&doc, 0, "reasoning_effort") < 0))
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
        if (!JsonObjPair(&doc, 0, i, &key, &val) || SkipThinkingKey(&doc, key))
        {
            continue;
        }
        if (wrote)
        {
            JsonBuf_Putc(&b, ',');
        }
        if (JsonEq(&doc, key, "messages"))
        {
            if (!AppendRawToken(&b, &doc, key))
            {
                JsonBuf_Free(&b);
                JsonFree(&doc);
                return NULL;
            }
            JsonBuf_Putc(&b, ':');
            if (!AppendMessagesWithoutReasoning(&b, &doc, val))
            {
                JsonBuf_Free(&b);
                JsonFree(&doc);
                return NULL;
            }
        }
        else
        {
            int start = FullTokenStart(&doc, key);
            int end = FullTokenEnd(&doc, val);
            if (start < 0 || end <= start || (size_t)end > len)
            {
                JsonBuf_Free(&b);
                JsonFree(&doc);
                return NULL;
            }
            JsonBuf_Append(&b, body + start, (size_t)(end - start));
        }
        wrote = true;
    }
    JsonBuf_Putc(&b, '}');
    JsonFree(&doc);
    return JsonBuf_Steal(&b);
}

static void EmitDelta(PicoCompletionsCtx *c, PicoLlmDeltaKind kind, const char *s, size_t n);

static void SetError(PicoCompletionsCtx *c, const char *msg)
{
    if (!msg || c->error)
    {
        return;
    }
    c->error = JsonDup(msg);
    c->failed = true;
}

static void FailUnsupported(PicoCompletionsCtx *c, const char *type)
{
    char buf[192];
    snprintf(buf, sizeof(buf), "unsupported output: %s", type && type[0] ? type : "unknown");
    SetError(c, buf);
}

static bool CtxAddPart(PicoCompletionsCtx *c, PicoLlmPartKind kind, const char *text, const char *path,
                       const char *url, const char *mime)
{
    if ((kind == PICO_LLM_PART_TEXT || kind == PICO_LLM_PART_REFUSAL) && text && c->output_count > 0)
    {
        PicoCompletionsOutput *last_output = &c->output[c->output_count - 1];
        PicoLlmPart *last = &last_output->part;
        if (!last_output->tool_call && last->kind == kind)
        {
            size_t a = last->text ? strlen(last->text) : 0;
            size_t b = strlen(text);
            char *next = (char *)malloc(a + b + 1);
            if (!next)
            {
                return false;
            }
            if (a)
            {
                memcpy(next, last->text, a);
            }
            memcpy(next + a, text, b + 1);
            free(last->text);
            last->text = next;
            return true;
        }
    }
    PicoCompletionsOutput *next = (PicoCompletionsOutput *)realloc(
        c->output, (size_t)(c->output_count + 1) * sizeof(PicoCompletionsOutput));
    if (!next)
    {
        return false;
    }
    c->output = next;
    PicoCompletionsOutput *entry = &c->output[c->output_count];
    memset(entry, 0, sizeof(*entry));
    entry->part.kind = kind;
    entry->part.text = text ? JsonDup(text) : NULL;
    entry->part.path = path ? JsonDup(path) : NULL;
    entry->part.url = url ? JsonDup(url) : NULL;
    entry->part.mime = mime ? JsonDup(mime) : NULL;
    c->output_count++;
    return true;
}

static bool CtxAddToolOrder(PicoCompletionsCtx *c, int call_index)
{
    PicoCompletionsOutput *next = (PicoCompletionsOutput *)realloc(
        c->output, (size_t)(c->output_count + 1) * sizeof(PicoCompletionsOutput));
    if (!next)
    {
        return false;
    }
    c->output = next;
    PicoCompletionsOutput *entry = &c->output[c->output_count++];
    memset(entry, 0, sizeof(*entry));
    entry->tool_call = true;
    entry->call_index = call_index;
    return true;
}

static char *CompletionsPartUrl(const JsonDoc *doc, int part)
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
    return NULL;
}

static bool CompletionsHasAnnotations(const JsonDoc *doc, int obj)
{
    int annotations = JsonObjGet(doc, obj, "annotations");
    if (annotations < 0)
    {
        return false;
    }
    return !JsonIsArray(doc, annotations) || JsonArrayLen(doc, annotations) > 0;
}

static bool ProjectCompletionsPart(PicoCompletionsCtx *c, const JsonDoc *doc, int part)
{
    if (CompletionsHasAnnotations(doc, part))
    {
        FailUnsupported(c, "annotations");
        return false;
    }
    char *type = JsonObjStr(doc, part, "type");
    const char *t = type ? type : "text";
    bool ok = true;
    if (strcmp(t, "text") == 0 || strcmp(t, "output_text") == 0)
    {
        char *text = JsonObjStr(doc, part, "text");
        if (text && text[0])
        {
            JsonBuf_Puts(&c->text, text);
            EmitDelta(c, PICO_LLM_DELTA_TEXT, text, strlen(text));
            ok = CtxAddPart(c, PICO_LLM_PART_TEXT, text, NULL, NULL, NULL);
        }
        free(text);
    }
    else if (strcmp(t, "refusal") == 0 || strcmp(t, "output_refusal") == 0)
    {
        char *text = JsonObjStr(doc, part, "text");
        if (!text)
        {
            text = JsonObjStr(doc, part, "refusal");
        }
        if (text && text[0])
        {
            EmitDelta(c, PICO_LLM_DELTA_TEXT, text, strlen(text));
            ok = CtxAddPart(c, PICO_LLM_PART_REFUSAL, text, NULL, NULL, NULL);
        }
        free(text);
    }
    else if (strcmp(t, "image_url") == 0 || strcmp(t, "image") == 0 || strcmp(t, "input_image") == 0)
    {
        char *url = CompletionsPartUrl(doc, part);
        ok = CtxAddPart(c, PICO_LLM_PART_IMAGE, NULL, NULL, url, NULL);
        free(url);
    }
    else if (strcmp(t, "input_audio") == 0 || strcmp(t, "audio") == 0)
    {
        char *url = CompletionsPartUrl(doc, part);
        ok = CtxAddPart(c, PICO_LLM_PART_AUDIO, NULL, NULL, url, NULL);
        free(url);
    }
    else
    {
        FailUnsupported(c, t);
        ok = false;
    }
    free(type);
    return ok && !c->failed;
}

static bool ApplyContent(PicoCompletionsCtx *c, const JsonDoc *doc, int content)
{
    if (content < 0)
    {
        return true;
    }
    if (JsonIsArray(doc, content))
    {
        int n = JsonArrayLen(doc, content);
        for (int i = 0; i < n; i++)
        {
            if (!ProjectCompletionsPart(c, doc, JsonArrayAt(doc, content, i)))
            {
                return false;
            }
        }
        return true;
    }
    if (JsonIsObject(doc, content))
    {
        return ProjectCompletionsPart(c, doc, content);
    }
    char *text = JsonStrDup(doc, content);
    if (text && text[0])
    {
        JsonBuf_Puts(&c->text, text);
        EmitDelta(c, PICO_LLM_DELTA_TEXT, text, strlen(text));
        bool ok = CtxAddPart(c, PICO_LLM_PART_TEXT, text, NULL, NULL, NULL);
        free(text);
        return ok;
    }
    free(text);
    return true;
}

static void ApplyRefusal(PicoCompletionsCtx *c, const JsonDoc *doc, int delta)
{
    char *refusal = JsonObjStr(doc, delta, "refusal");
    if (refusal && refusal[0])
    {
        EmitDelta(c, PICO_LLM_DELTA_TEXT, refusal, strlen(refusal));
        if (!CtxAddPart(c, PICO_LLM_PART_REFUSAL, refusal, NULL, NULL, NULL))
        {
            SetError(c, "out of memory");
        }
    }
    free(refusal);
}

static void EmitDelta(PicoCompletionsCtx *c, PicoLlmDeltaKind kind, const char *s, size_t n)
{
    if (kind == PICO_LLM_DELTA_TEXT && s && n)
    {
        c->saw_text = true;
    }
    if (!c->on_delta || !s || n == 0)
    {
        return;
    }
    c->on_delta(c->user, kind, s, n);
}

static PicoCompletionsCall *EnsureCall(PicoCompletionsCtx *c, int index)
{
    if (index < 0)
    {
        index = c->call_count;
    }
    while (c->call_count <= index)
    {
        PicoCompletionsCall *next = (PicoCompletionsCall *)realloc(
            c->calls, (size_t)(c->call_count + 1) * sizeof(PicoCompletionsCall));
        if (!next)
        {
            return NULL;
        }
        c->calls = next;
        memset(&c->calls[c->call_count], 0, sizeof(PicoCompletionsCall));
        JsonBuf_Init(&c->calls[c->call_count].arguments);
        c->calls[c->call_count].index = c->call_count;
        c->call_count++;
    }
    return &c->calls[index];
}

static void ApplyUsage(PicoCompletionsCtx *c, const JsonDoc *doc, int usage)
{
    if (!JsonIsObject(doc, usage))
    {
        return;
    }
    int input = JsonObjInt(doc, usage, "prompt_tokens", 0);
    if (input <= 0)
    {
        input = JsonObjInt(doc, usage, "input_tokens", 0);
    }
    if (input > 0)
    {
        c->input_tokens = input;
    }
    int details = JsonObjGet(doc, usage, "prompt_tokens_details");
    if (!JsonIsObject(doc, details))
    {
        details = JsonObjGet(doc, usage, "input_tokens_details");
    }
    int cached = 0;
    if (JsonIsObject(doc, details))
    {
        cached = JsonObjInt(doc, details, "cached_tokens", 0);
    }
    if (cached <= 0)
    {
        cached = JsonObjInt(doc, usage, "cached_tokens", 0);
    }
    if (cached > 0)
    {
        c->cached_tokens = cached;
    }
}

static void ApplyToolCallDelta(PicoCompletionsCtx *c, const JsonDoc *doc, int tc)
{
    int index = JsonObjInt(doc, tc, "index", -1);
    PicoCompletionsCall *call = EnsureCall(c, index);
    if (!call)
    {
        SetError(c, "out of memory");
        return;
    }
    if (!call->ordered)
    {
        if (!CtxAddToolOrder(c, call->index))
        {
            SetError(c, "out of memory");
            return;
        }
        call->ordered = true;
    }
    char *id = JsonObjStr(doc, tc, "id");
    if (id && id[0])
    {
        free(call->id);
        call->id = id;
        id = NULL;
        EmitDelta(c, PICO_LLM_DELTA_STATUS, call->id, strlen(call->id));
    }
    free(id);
    int fn = JsonObjGet(doc, tc, "function");
    if (JsonIsObject(doc, fn))
    {
        char *name = JsonObjStr(doc, fn, "name");
        if (name && name[0])
        {
            free(call->name);
            call->name = name;
            name = NULL;
            EmitDelta(c, PICO_LLM_DELTA_STATUS, call->name, strlen(call->name));
        }
        free(name);
        char *args = JsonObjStr(doc, fn, "arguments");
        if (args)
        {
            JsonBuf_Puts(&call->arguments, args);
            free(args);
        }
    }
}

static bool ValidateCompletionsDelta(PicoCompletionsCtx *c, const JsonDoc *doc, int delta)
{
    int pairs = JsonObjLen(doc, delta);
    for (int i = 0; i < pairs; i++)
    {
        int key = -1;
        int value = -1;
        if (!JsonObjPair(doc, delta, i, &key, &value))
        {
            continue;
        }
        if (JsonEq(doc, key, "role") || JsonEq(doc, key, "content") ||
            JsonEq(doc, key, "refusal") || JsonEq(doc, key, "reasoning_content") ||
            JsonEq(doc, key, "reasoning") || JsonEq(doc, key, "reasoning_text") ||
            JsonEq(doc, key, "tool_calls"))
        {
            continue;
        }
        if (JsonEq(doc, key, "annotations"))
        {
            if (JsonIsArray(doc, value) && JsonArrayLen(doc, value) == 0)
            {
                continue;
            }
            FailUnsupported(c, "annotations");
            return false;
        }
        char *name = JsonStrDup(doc, key);
        FailUnsupported(c, name);
        free(name);
        return false;
    }
    return true;
}

static bool HandleJson(void *user, const char *event, const char *json, size_t len)
{
    PicoCompletionsCtx *c = (PicoCompletionsCtx *)user;
    (void)event;
    if (!json || len == 0)
    {
        return true;
    }
    if (len >= 6 && strncmp(json, "[DONE]", 6) == 0)
    {
        return true;
    }
    JsonDoc doc;
    if (JsonParse(&doc, json, len) != 0)
    {
        return true;
    }
    int err = JsonObjGet(&doc, 0, "error");
    if (err >= 0)
    {
        char *msg = NULL;
        if (JsonIsObject(&doc, err))
        {
            msg = JsonObjStr(&doc, err, "message");
        }
        else
        {
            msg = JsonStrDup(&doc, err);
        }
        SetError(c, msg && msg[0] ? msg : "LLM request failed");
        free(msg);
        JsonFree(&doc);
        return false;
    }
    ApplyUsage(c, &doc, JsonObjGet(&doc, 0, "usage"));
    int choices = JsonObjGet(&doc, 0, "choices");
    if (JsonIsArray(&doc, choices) && JsonArrayLen(&doc, choices) > 0)
    {
        int choice = JsonArrayAt(&doc, choices, 0);
        int delta = JsonObjGet(&doc, choice, "delta");
        if (!JsonIsObject(&doc, delta))
        {
            delta = JsonObjGet(&doc, choice, "message");
        }
        if (JsonIsObject(&doc, delta))
        {
            if (!ValidateCompletionsDelta(c, &doc, delta))
            {
                JsonFree(&doc);
                return false;
            }
            if (!ApplyContent(c, &doc, JsonObjGet(&doc, delta, "content")))
            {
                JsonFree(&doc);
                return false;
            }
            ApplyRefusal(c, &doc, delta);
            if (c->failed)
            {
                JsonFree(&doc);
                return false;
            }
            char *think = JsonObjStr(&doc, delta, "reasoning_content");
            if (!think)
            {
                think = JsonObjStr(&doc, delta, "reasoning");
            }
            if (!think)
            {
                think = JsonObjStr(&doc, delta, "reasoning_text");
            }
            if (think && think[0])
            {
                JsonBuf_Puts(&c->think, think);
                EmitDelta(c, PICO_LLM_DELTA_THINKING, think, strlen(think));
            }
            free(think);
            int tool_calls = JsonObjGet(&doc, delta, "tool_calls");
            if (JsonIsArray(&doc, tool_calls))
            {
                int n = JsonArrayLen(&doc, tool_calls);
                for (int i = 0; i < n; i++)
                {
                    ApplyToolCallDelta(c, &doc, JsonArrayAt(&doc, tool_calls, i));
                }
            }
        }
    }
    JsonFree(&doc);
    return !c->failed;
}

bool pico_completions_feed(PicoCompletionsCtx *c, const char *json, size_t len)
{
    if (!c)
    {
        return false;
    }
    return HandleJson(c, NULL, json, len);
}

static bool HandleCancel(void *user)
{
    PicoCompletionsCtx *c = (PicoCompletionsCtx *)user;
    return c->cancel && c->cancel(c->user);
}

void pico_completions_ctx_init(PicoCompletionsCtx *c)
{
    if (!c)
    {
        return;
    }
    memset(c, 0, sizeof(*c));
    JsonBuf_Init(&c->text);
    JsonBuf_Init(&c->think);
}

static void SetResultProjectionError(PicoLlmResult *out)
{
    int input_tokens = out->input_tokens;
    int cached_tokens = out->cached_tokens;
    pico_llm_result_free(out);
    out->input_tokens = input_tokens;
    out->cached_tokens = cached_tokens;
    out->error = JsonDup("out of memory");
}

void pico_completions_fill_result(PicoCompletionsCtx *c, PicoLlmResult *out)
{
    if (!c || !out)
    {
        return;
    }
    out->input_tokens = c->input_tokens;
    out->cached_tokens = c->cached_tokens;
    if (c->error)
    {
        out->error = c->error;
        c->error = NULL;
        return;
    }
    /* A Chat Completions choice is one assistant message. Content, reasoning, and
     * tool_calls are sibling fields on that message, not independently ordered
     * output items. Keep one canonical assistant item before all of its calls so
     * continuation replay can reconstruct the required assistant/tool sequence. */
    PicoLlmItem *assistant = NULL;
    if (c->output_count > 0 || c->think.len)
    {
        assistant = pico_llm_result_add_item(out, PICO_LLM_ITEM_ASSISTANT);
        if (!assistant)
        {
            SetResultProjectionError(out);
            return;
        }
    }
    for (int i = 0; assistant && i < c->output_count; i++)
    {
        PicoCompletionsOutput *entry = &c->output[i];
        if (!entry->tool_call)
        {
            int before = assistant->part_count;
            bool added = pico_llm_item_add_part(assistant, entry->part.kind, entry->part.text,
                                                entry->part.path, entry->part.url, entry->part.mime);
            PicoLlmPart *part = added && assistant->part_count == before + 1
                                    ? &assistant->parts[before]
                                    : NULL;
            bool copied = part && (!entry->part.text || part->text) &&
                          (!entry->part.path || part->path) && (!entry->part.url || part->url) &&
                          (!entry->part.mime || part->mime);
            if (!copied)
            {
                SetResultProjectionError(out);
                return;
            }
        }
    }
    if (assistant && c->think.len)
    {
        assistant->thinking = JsonBuf_Steal(&c->think);
        assistant->thinking_signature = JsonDup("reasoning_content");
        if (!assistant->thinking || !assistant->thinking_signature)
        {
            SetResultProjectionError(out);
            return;
        }
    }
    for (int i = 0; i < c->output_count; i++)
    {
        PicoCompletionsOutput *entry = &c->output[i];
        if (!entry->tool_call || entry->call_index < 0 || entry->call_index >= c->call_count)
        {
            continue;
        }
        PicoCompletionsCall *call = &c->calls[entry->call_index];
        const char *args = call->arguments.len ? call->arguments.data : "{}";
        int before = out->item_count;
        bool added = pico_llm_result_add_tool_call(out, call->id, call->name, args, NULL);
        PicoLlmItem *projected = added && out->item_count == before + 1 ? &out->items[before] : NULL;
        if (!projected || !projected->call_id || !projected->name || !projected->arguments)
        {
            SetResultProjectionError(out);
            return;
        }
    }
}

void pico_completions_ctx_free(PicoCompletionsCtx *c)
{
    if (!c)
    {
        return;
    }
    JsonBuf_Free(&c->text);
    JsonBuf_Free(&c->think);
    for (int i = 0; i < c->output_count; i++)
    {
        if (!c->output[i].tool_call)
        {
            free(c->output[i].part.text);
            free(c->output[i].part.path);
            free(c->output[i].part.url);
            free(c->output[i].part.mime);
        }
    }
    free(c->output);
    c->output = NULL;
    c->output_count = 0;
    for (int i = 0; i < c->call_count; i++)
    {
        free(c->calls[i].id);
        free(c->calls[i].name);
        JsonBuf_Free(&c->calls[i].arguments);
    }
    free(c->calls);
    free(c->error);
    c->error = NULL;
    c->calls = NULL;
    c->call_count = 0;
}

int pico_completions_post(const char *url, const char *body, const char *bearer,
                          const char *const extra_headers[], int extra_count, PicoLlmCancelFn cancel,
                          PicoLlmDeltaFn on_delta, void *user, PicoCompletionsCtx *ctx)
{
    pico_completions_ctx_init(ctx);
    ctx->cancel = cancel;
    ctx->on_delta = on_delta;
    ctx->user = user;

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
        pico_completions_ctx_free(ctx);
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
