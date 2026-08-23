#include "builtins/completions.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_failed;

static void Check(bool ok, const char *message)
{
    if (!ok)
    {
        fprintf(stderr, "FAIL: %s\n", message);
        g_failed = 1;
    }
}

static void CheckContains(const char *text, const char *needle, bool expected, const char *message)
{
    Check(text && ((strstr(text, needle) != NULL) == expected), message);
}

static void FreeResult(PicoLlmResult *r)
{
    if (!r)
    {
        return;
    }
    free(r->error);
    free(r->assistant_text);
    free(r->think_text);
    free(r->think_signature);
    for (int i = 0; i < r->call_count; i++)
    {
        free(r->calls[i].call_id);
        free(r->calls[i].name);
        free(r->calls[i].arguments);
        free(r->calls[i].item_id);
    }
    free(r->calls);
    memset(r, 0, sizeof(*r));
}

static PicoCompletionsBuildOpts HyperOpts(bool reasoning)
{
    PicoCompletionsBuildOpts opts = {
        .provider = "hyper",
        .store_false = true,
        .thinking = PICO_COMPLETIONS_THINKING_DEEPSEEK,
        .requires_reasoning_content = reasoning,
        .max_tokens_field = "max_tokens",
        .max_tokens = 0,
    };
    return opts;
}

static void TestUrls(void)
{
    char out[128];
    pico_completions_resolve_url(NULL, "https://hyper.charm.land/v1", out, sizeof(out));
    Check(strcmp(out, "https://hyper.charm.land/v1/chat/completions") == 0,
          "fallback base appends /chat/completions");
    pico_completions_resolve_url("https://hyper.charm.land/v1/", NULL, out, sizeof(out));
    Check(strcmp(out, "https://hyper.charm.land/v1/chat/completions") == 0,
          "trailing slash is stripped before the suffix");
    pico_completions_resolve_url("https://hyper.charm.land/v1/chat/completions", NULL, out, sizeof(out));
    Check(strcmp(out, "https://hyper.charm.land/v1/chat/completions") == 0,
          "existing Completions suffix is kept");

    const char *canonical = "https://hyper.charm.land/v1";
    Check(pico_completions_resolve_canonical_url(NULL, canonical, out, sizeof(out)) &&
              strcmp(out, "https://hyper.charm.land/v1/chat/completions") == 0,
          "empty override uses canonical Hyper Completions endpoint");
    Check(pico_completions_resolve_canonical_url("https://hyper.charm.land/v1/", canonical, out,
                                                 sizeof(out)),
          "canonical base with trailing slash is accepted");
    Check(pico_completions_resolve_canonical_url("https://hyper.charm.land/v1/chat/completions",
                                                 canonical, out, sizeof(out)),
          "canonical Completions endpoint is accepted");
    Check(!pico_completions_resolve_canonical_url("https://hyper.charm.land/v1/responses", canonical,
                                                  out, sizeof(out)),
          "legacy Responses path is rejected");
    Check(!pico_completions_resolve_canonical_url("http://hyper.charm.land/v1", canonical, out,
                                                  sizeof(out)),
          "plaintext Hyper endpoint is rejected");
    Check(!pico_completions_resolve_canonical_url("https://attacker.example/v1", canonical, out,
                                                  sizeof(out)),
          "another origin is rejected");
}

static void TestRequestConversion(void)
{
    const char *input[] = {
        "{\"type\":\"user\",\"text\":\"hello\"}",
        "{\"type\":\"assistant\",\"text\":\"hi\",\"thinking\":\"why\",\"thinking_signature\":\"reasoning_content\"}",
        "{\"type\":\"tool_call\",\"call_id\":\"c1\",\"name\":\"sh\",\"arguments\":\"{\\\"cmd\\\":\\\"ls\\\"}\",\"item_id\":\"fc_x\"}",
        "{\"type\":\"tool_result\",\"call_id\":\"c1\",\"output\":\"ok\"}",
    };
    PicoTool tools[] = {{.name = "sh",
                         .description = "shell",
                         .params_json = "{\"type\":\"object\",\"properties\":{}}"}};
    PicoLlmTurn turn = {
        .model = "kimi",
        .instructions = "be brief",
        .effort = "high",
        .include_tools = true,
        .input_json = input,
        .input_count = 4,
        .tools = tools,
        .tool_count = 1,
    };
    PicoCompletionsBuildOpts opts = HyperOpts(true);
    char *body = pico_completions_build_request(&turn, &opts);
    Check(body && JsonValidSyntax(body, strlen(body)), "Completions request is valid JSON");
    CheckContains(body, "\"store\":false", true, "Hyper disables storage");
    CheckContains(body, "\"stream_options\":{\"include_usage\":true}", true,
                  "streamed requests ask for usage");
    CheckContains(body, "\"thinking\":{\"type\":\"enabled\"}", true, "DeepSeek thinking is enabled");
    CheckContains(body, "\"reasoning_effort\":\"high\"", true, "effort is sent as reasoning_effort");
    CheckContains(body, "\"role\":\"system\"", true, "instructions become a system message");
    CheckContains(body, "\"role\":\"user\"", true, "user turns become chat messages");
    CheckContains(body, "\"reasoning_content\":\"why\"", true, "thinking text is replayed");
    CheckContains(body, "\"tool_calls\"", true, "following tool_call items coalesce onto the assistant");
    CheckContains(body, "\"id\":\"c1\"", true, "Completions uses call_id as the tool id");
    CheckContains(body, "fc_x", false, "Responses function-call item ids are dropped");
    CheckContains(body, "\"role\":\"tool\"", true, "tool results become tool messages");
    CheckContains(body, "\"tool_call_id\":\"c1\"", true, "tool results keep the Completions call id");
    free(body);

    turn.effort = "none";
    opts.requires_reasoning_content = false;
    body = pico_completions_build_request(&turn, &opts);
    CheckContains(body, "\"thinking\":{\"type\":\"disabled\"}", true, "effort none disables DeepSeek thinking");
    CheckContains(body, "reasoning_effort", false, "disabled thinking omits reasoning_effort");
    free(body);
}

static void TestEncryptedSignatureDropped(void)
{
    const char *input[] = {
        "{\"type\":\"user\",\"text\":\"hello\"}",
        "{\"type\":\"assistant\",\"text\":\"hi\",\"thinking\":\"plain\","
        "\"thinking_signature\":\"{\\\"type\\\":\\\"reasoning\\\",\\\"encrypted_content\\\":\\\"SECRET\\\"}\"}",
        "{\"type\":\"tool_call\",\"call_id\":\"c1\",\"name\":\"sh\",\"arguments\":\"{}\"}",
    };
    PicoLlmTurn turn = {
        .model = "kimi",
        .effort = "high",
        .input_json = input,
        .input_count = 3,
    };
    PicoCompletionsBuildOpts opts = HyperOpts(true);
    char *body = pico_completions_build_request(&turn, &opts);
    CheckContains(body, "\"reasoning_content\":\"plain\"", true,
                  "Completions keeps thinking text when the signature is an OpenAI blob");
    CheckContains(body, "SECRET", false, "encrypted thinking signatures are not sent to Completions");
    CheckContains(body, "encrypted_content", false, "reasoning item JSON is not replayed on Completions");
    free(body);
}

static void TestToolOnlyReasoningContent(void)
{
    const char *input[] = {
        "{\"type\":\"user\",\"text\":\"do it\"}",
        "{\"type\":\"tool_call\",\"call_id\":\"c1\",\"name\":\"sh\",\"arguments\":\"{}\"}",
    };
    PicoLlmTurn turn = {
        .model = "kimi",
        .effort = "high",
        .input_json = input,
        .input_count = 2,
    };
    PicoCompletionsBuildOpts opts = HyperOpts(true);
    char *body = pico_completions_build_request(&turn, &opts);
    CheckContains(body, "\"reasoning_content\":\"\"", true,
                  "DeepSeek tool-only turns still send empty reasoning_content");
    CheckContains(body, "\"tool_calls\"", true, "orphan tool_call items still become assistant tool_calls");
    free(body);
}

static void TestWithoutThinking(void)
{
    const char *body =
        "{\"model\":\"kimi\",\"thinking\":{\"type\":\"enabled\"},\"reasoning_effort\":\"high\","
        "\"messages\":[{\"role\":\"assistant\",\"content\":\"a } brace\","
        "\"reasoning_content\":\"private\"},{\"role\":\"tool\",\"content\":\"ok\"}],"
        "\"tools\":[{\"function\":{\"parameters\":{\"properties\":{"
        "\"reasoning_content\":{\"type\":\"string\"}}}}}],\"tail\":true}";
    char *stripped = pico_completions_body_without_thinking(body);
    Check(stripped && JsonValidSyntax(stripped, strlen(stripped)), "thinking retry body is valid JSON");
    if (stripped)
    {
        JsonDoc doc;
        int rc = JsonParse(&doc, stripped, strlen(stripped));
        Check(rc == 0, "thinking retry body parses");
        if (rc == 0)
        {
            Check(JsonObjGet(&doc, 0, "thinking") < 0, "root thinking member is removed");
            Check(JsonObjGet(&doc, 0, "reasoning_effort") < 0, "root reasoning_effort is removed");
            Check(JsonObjGet(&doc, 0, "messages") >= 0, "other root members remain");
            Check(JsonObjGet(&doc, 0, "tools") >= 0, "tool definitions remain");
            Check(JsonObjGet(&doc, 0, "tail") >= 0, "members after thinking remain");
            int messages = JsonObjGet(&doc, 0, "messages");
            int assistant = JsonArrayAt(&doc, messages, 0);
            Check(JsonObjGet(&doc, assistant, "reasoning_content") < 0,
                  "assistant reasoning_content is removed");
            JsonFree(&doc);
        }
        CheckContains(stripped, "\"reasoning_content\":{\"type\":\"string\"}", true,
                      "nested tool-schema properties remain");
        CheckContains(stripped, "private", false, "historical reasoning text is removed");
    }
    free(stripped);
    Check(pico_completions_body_without_thinking("not json") == NULL, "invalid JSON is rejected");
}

static void TestFeedChunks(void)
{
    PicoCompletionsCtx ctx;
    pico_completions_ctx_init(&ctx);
    const char *content = "{\"choices\":[{\"delta\":{\"content\":\"Hi\"}}]}";
    const char *think = "{\"choices\":[{\"delta\":{\"reasoning_content\":\"why\"}}]}";
    const char *tool1 =
        "{\"choices\":[{\"delta\":{\"tool_calls\":[{\"index\":0,\"id\":\"c1\","
        "\"function\":{\"name\":\"sh\",\"arguments\":\"{\"}}]}}]}";
    const char *tool2 =
        "{\"choices\":[{\"delta\":{\"tool_calls\":[{\"index\":0,\"function\":{\"arguments\":\"}\"}}]}}]}";
    const char *usage =
        "{\"usage\":{\"prompt_tokens\":9,\"prompt_tokens_details\":{\"cached_tokens\":3}}}";
    Check(pico_completions_feed(&ctx, content, strlen(content)), "content chunk is accepted");
    Check(pico_completions_feed(&ctx, think, strlen(think)), "reasoning_content chunk is accepted");
    Check(pico_completions_feed(&ctx, tool1, strlen(tool1)), "first tool-call delta is accepted");
    Check(pico_completions_feed(&ctx, tool2, strlen(tool2)), "split tool-call arguments are accepted");
    Check(pico_completions_feed(&ctx, usage, strlen(usage)), "usage chunk is accepted");
    Check(pico_completions_feed(&ctx, "[DONE]", 6), "DONE sentinel is ignored");

    PicoLlmResult out;
    memset(&out, 0, sizeof(out));
    pico_completions_fill_result(&ctx, &out);
    Check(out.assistant_text && strcmp(out.assistant_text, "Hi") == 0, "content deltas assemble");
    Check(out.think_text && strcmp(out.think_text, "why") == 0, "reasoning deltas assemble");
    Check(out.think_signature && strcmp(out.think_signature, "reasoning_content") == 0,
          "Completions thinking signature is the field name");
    Check(out.call_count == 1 && out.calls && out.calls[0].call_id &&
              strcmp(out.calls[0].call_id, "c1") == 0,
          "tool-call id is captured");
    Check(out.calls[0].name && strcmp(out.calls[0].name, "sh") == 0, "tool-call name is captured");
    Check(out.calls[0].arguments && strcmp(out.calls[0].arguments, "{}") == 0,
          "split tool-call arguments concatenate");
    Check(out.calls[0].item_id == NULL, "Completions does not invent Responses item ids");
    Check(out.input_tokens == 9, "prompt_tokens become input tokens");
    Check(out.cached_tokens == 3, "cached prompt tokens are recorded");
    FreeResult(&out);
    pico_completions_ctx_free(&ctx);
}

int main(void)
{
    TestUrls();
    TestRequestConversion();
    TestEncryptedSignatureDropped();
    TestToolOnlyReasoningContent();
    TestWithoutThinking();
    TestFeedChunks();
    return g_failed;
}
