#include "builtins/hyper_auth.h"
#include "builtins/responses.h"
#include "canonical.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

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

static void FreeResult(PicoLlmResult *result)
{
    pico_llm_result_free(result);
}

static void TestRequestOptions(void)
{
    const char *input[] = {
        "{\"type\":\"user\",\"text\":\"hello\"}",
    };
    PicoTool tools[] = {{.name = "test",
                         .description = "test tool",
                         .params_json = "{\"type\":\"object\",\"properties\":{\"reasoning\":{\"type\":\"string\"}}}"}};
    PicoLlmTurn turn = {
        .model = "model",
        .instructions = "instructions",
        .cache_key = "cache",
        .effort = "high",
        .include_tools = true,
        .input_json = input,
        .input_count = 1,
        .tools = tools,
        .tool_count = 1,
    };
    PicoResponsesBuildOpts hyper = {
        .provider = "hyper",
        .store_false = true,
    };
    char *body = pico_responses_build_request(&turn, &hyper);
    Check(body && JsonValidSyntax(body, strlen(body)), "Hyper request is valid JSON");
    CheckContains(body, "\"store\":false", true, "Hyper disables storage");
    CheckContains(body, "reasoning.encrypted_content", false, "Hyper omits encrypted reasoning include");
    CheckContains(body, "\"summary\":\"auto\"", false, "Hyper omits reasoning summary");
    free(body);

    PicoResponsesBuildOpts openai = {
        .provider = "openai",
        .store_false = true,
        .include_encrypted_reasoning = true,
        .reasoning_summary_auto = true,
    };
    body = pico_responses_build_request(&turn, &openai);
    CheckContains(body, "reasoning.encrypted_content", true, "OpenAI includes encrypted reasoning");
    CheckContains(body, "\"summary\":\"auto\"", true, "OpenAI requests reasoning summaries");
    free(body);
}

static void TestContextItemConversion(void)
{
    PicoLlmPart assistant_part;
    memset(&assistant_part, 0, sizeof(assistant_part));
    assistant_part.kind = PICO_LLM_PART_TEXT;
    assistant_part.text = "hi";
    char *user = pico_canonical_user_text("hello");
    char *assistant = pico_canonical_assistant_json(&assistant_part, 1, NULL, NULL);
    char *context = pico_canonical_context_text("ephemeral-reminder");
    const char *input[] = {user, assistant, context};
    PicoLlmTurn turn = {
        .model = "model",
        .instructions = "instructions",
        .cache_key = "cache",
        .input_json = input,
        .input_count = 3,
    };
    PicoResponsesBuildOpts openai = {
        .provider = "openai",
        .store_false = true,
    };
    char *body = pico_responses_build_request(&turn, &openai);
    Check(body && JsonValidSyntax(body, strlen(body)), "Responses context request is valid JSON");
    JsonDoc doc;
    int parsed = body ? JsonParse(&doc, body, strlen(body)) : -1;
    Check(parsed == 0, "Responses context request parses");
    if (parsed == 0)
    {
        int arr = JsonObjGet(&doc, 0, "input");
        int n = JsonIsArray(&doc, arr) ? JsonArrayLen(&doc, arr) : 0;
        int last = n > 0 ? JsonArrayAt(&doc, arr, n - 1) : -1;
        int first = n > 0 ? JsonArrayAt(&doc, arr, 0) : -1;
        int content = last >= 0 ? JsonObjGet(&doc, last, "content") : -1;
        int text_part = JsonIsArray(&doc, content) && JsonArrayLen(&doc, content) > 0
                            ? JsonArrayAt(&doc, content, 0)
                            : -1;
        char *last_text = text_part >= 0 ? JsonObjStr(&doc, text_part, "text") : NULL;
        bool ok = n == 3 && JsonEq(&doc, JsonObjGet(&doc, first, "role"), "user") &&
                  JsonEq(&doc, JsonObjGet(&doc, last, "type"), "message") &&
                  JsonEq(&doc, JsonObjGet(&doc, last, "role"), "developer") && last_text &&
                  strcmp(last_text, "ephemeral-reminder") == 0 &&
                  !JsonEq(&doc, JsonObjGet(&doc, last, "role"), "user");
        Check(ok, "context item is a trailing developer message, not user");
        free(last_text);
        JsonFree(&doc);
    }
    free(body);
    free(user);
    free(assistant);
    free(context);
}

static void TestImageRequestConversion(void)
{
    char path[256];
    snprintf(path, sizeof(path), "/tmp/pico-responses-media-%ld.png", (long)getpid());
    FILE *f = fopen(path, "wb");
    Check(f != NULL, "Responses media fixture opens");
    if (!f)
    {
        return;
    }
    fwrite("PNG", 1, 3, f);
    fclose(f);

    char item[1024];
    snprintf(item, sizeof(item),
             "{\"type\":\"user\",\"parts\":[{\"type\":\"text\",\"text\":\"see\"},"
             "{\"type\":\"image\",\"path\":\"%s\",\"mime\":\"image/png\"}]}", path);
    const char *input[] = {item};
    PicoLlmTurn turn = {
        .model = "vision-model",
        .vision = true,
        .input_json = input,
        .input_count = 1,
    };
    PicoResponsesBuildOpts opts = {.provider = "openai"};
    char *body = pico_responses_build_request(&turn, &opts);
    Check(body && strstr(body, "\"type\":\"input_image\"") &&
              strstr(body, "data:image/png;base64,UE5H"),
          "Responses reads an image path into input_image at request time");
    free(body);
    unlink(path);
    body = pico_responses_build_request(&turn, &opts);
    Check(body == NULL, "Responses fails request projection when an image path is unreadable");
    free(body);
}

static void TestReasoningRemoval(void)
{
    const char *body =
        "{\"tools\":[{\"parameters\":{\"properties\":{\"reasoning\":{\"type\":\"string\"}}}}],"
        "\"input\":\"a } brace\",\"reasoning\":{\"effort\":\"x}y\"},\"tail\":true}";
    char *stripped = pico_responses_body_without_reasoning(body);
    Check(stripped && JsonValidSyntax(stripped, strlen(stripped)), "reasoning retry body is valid JSON");
    if (stripped)
    {
        JsonDoc doc;
        int rc = JsonParse(&doc, stripped, strlen(stripped));
        Check(rc == 0, "reasoning retry body parses");
        if (rc == 0)
        {
            Check(JsonObjGet(&doc, 0, "reasoning") < 0, "root reasoning member is removed");
            Check(JsonObjGet(&doc, 0, "tools") >= 0, "other root members remain");
            Check(JsonObjGet(&doc, 0, "tail") >= 0, "members after reasoning remain");
            JsonFree(&doc);
        }
        CheckContains(stripped, "\"reasoning\":{\"type\":\"string\"}", true,
                      "nested reasoning schema remains");
    }
    free(stripped);

    stripped = pico_responses_body_without_reasoning(
        "{\"tool\":{\"reasoning\":{\"type\":\"string\"}},\"tail\":true}");
    Check(stripped == NULL, "nested reasoning alone does not trigger removal");
    free(stripped);
    Check(pico_responses_body_without_reasoning("not json") == NULL, "invalid JSON is rejected");
}

static void TestRefreshDecision(void)
{
    time_t now = 1000;
    Check(pico_hyper_oauth_refresh_needed(NULL, 2000, now, false), "missing access token refreshes");
    Check(!pico_hyper_oauth_refresh_needed("token", 2000, now, false), "fresh access token is reused");
    Check(pico_hyper_oauth_refresh_needed("token", 1060, now, false), "nearly expired token refreshes");
    Check(pico_hyper_oauth_refresh_needed("token", 2000, now, true), "401 forces refresh");
    Check(pico_hyper_oauth_token_replaced("old", "new"),
          "a waiter reuses a token refreshed by another request");
    Check(!pico_hyper_oauth_token_replaced("same", "same"),
          "the request that received the 401 performs the refresh");
}

static void TestSignatureReplay(void)
{
    const char *openai_input[] = {
        "{\"type\":\"user\",\"text\":\"hello\"}",
        "{\"type\":\"assistant\",\"text\":\"ok\",\"thinking\":\"why\","
        "\"thinking_signature\":\"{\\\"type\\\":\\\"reasoning\\\",\\\"id\\\":\\\"rs_1\\\",\\\"encrypted_content\\\":\\\"blob\\\"}\"}",
        "{\"type\":\"tool_call\",\"call_id\":\"c1\",\"name\":\"sh\",\"arguments\":\"{}\",\"item_id\":\"fc_1\"}",
        "{\"type\":\"tool_result\",\"call_id\":\"c1\",\"output\":\"done\"}",
    };
    PicoLlmTurn turn = {
        .model = "gpt",
        .effort = "high",
        .input_json = openai_input,
        .input_count = 4,
    };
    PicoResponsesBuildOpts openai = {
        .provider = "openai",
        .store_false = true,
        .include_encrypted_reasoning = true,
        .reasoning_summary_auto = true,
    };
    char *body = pico_responses_build_request(&turn, &openai);
    Check(body && JsonValidSyntax(body, strlen(body)), "OpenAI request with signatures is valid JSON");
    CheckContains(body, "\"id\":\"rs_1\"", true, "OpenAI replays the stored reasoning item id");
    CheckContains(body, "\"encrypted_content\":\"blob\"", true, "OpenAI replays encrypted reasoning");
    CheckContains(body, "\"id\":\"fc_1\"", true, "OpenAI replays the stored function_call item id");
    CheckContains(body, "\"call_id\":\"c1\"", true, "OpenAI keeps the tool call_id");
    free(body);

    const char *hyper_input[] = {
        "{\"type\":\"user\",\"text\":\"hello\"}",
        "{\"type\":\"assistant\",\"text\":\"ok\",\"thinking\":\"why\",\"thinking_signature\":\"reasoning_content\"}",
        "{\"type\":\"tool_call\",\"call_id\":\"c1\",\"name\":\"sh\",\"arguments\":\"{}\"}",
        "{\"type\":\"tool_result\",\"call_id\":\"c1\",\"output\":\"done\"}",
    };
    turn.input_json = hyper_input;
    body = pico_responses_build_request(&turn, &openai);
    CheckContains(body, "\"type\":\"reasoning\"", false,
                  "Hyper thinking signatures are not sent as OpenAI reasoning items");
    CheckContains(body, "\"encrypted_content\":", false, "Hyper-to-OpenAI omits encrypted reasoning");
    CheckContains(body, "\"id\":\"fc_", false, "Hyper-to-OpenAI reconstructs function_call without an item id");
    CheckContains(body, "\"call_id\":\"c1\"", true, "Hyper-to-OpenAI still sends call_id");
    CheckContains(body, "\"type\":\"function_call\"", true, "Hyper-to-OpenAI reconstructs function_call items");
    free(body);

    const char *signature_only_input[] = {
        "{\"type\":\"user\",\"text\":\"hello\"}",
        "{\"type\":\"assistant\",\"text\":\"\",\"thinking_signature\":"
        "\"{\\\"type\\\":\\\"reasoning\\\",\\\"id\\\":\\\"rs_only\\\","
        "\\\"encrypted_content\\\":\\\"blob\\\"}\"}",
        "{\"type\":\"tool_call\",\"call_id\":\"c2\",\"name\":\"sh\",\"arguments\":\"{}\"}",
    };
    turn.input_json = signature_only_input;
    turn.input_count = 3;
    body = pico_responses_build_request(&turn, &openai);
    CheckContains(body, "\"id\":\"rs_only\"", true,
                  "signature-only assistant turns replay encrypted reasoning");
    CheckContains(body, "\"type\":\"output_text\"", false,
                  "signature-only turns do not invent an empty assistant message");
    free(body);
}

static void TestReasoningResultProjection(void)
{
    PicoResponsesCtx ctx;
    memset(&ctx, 0, sizeof(ctx));
    JsonBuf_Init(&ctx.items);
    JsonBuf_Init(&ctx.summary);
    JsonBuf_Puts(&ctx.items,
                 "{\"type\":\"reasoning\",\"id\":\"rs_projected\",\"summary\":[],"
                 "\"encrypted_content\":\"secret_blob\"},{\"type\":\"function_call\","
                 "\"id\":\"fc_projected\",\"call_id\":\"call_projected\",\"name\":\"sh\","
                 "\"arguments\":\"{\\\"cmd\\\":\\\"ls\\\"}\"}");
    ctx.item_count = 2;
    PicoLlmResult result;
    memset(&result, 0, sizeof(result));
    pico_responses_fill_result(&ctx, &result);
    Check(result.item_count >= 2 && result.items[0].kind == PICO_LLM_ITEM_ASSISTANT &&
              result.items[0].thinking_signature && strstr(result.items[0].thinking_signature, "rs_projected") &&
              strstr(result.items[0].thinking_signature, "secret_blob"),
          "reasoning items project their id and encrypted content to the canonical signature");
    Check(result.items[1].kind == PICO_LLM_ITEM_TOOL_CALL && result.items[1].item_id &&
              strcmp(result.items[1].item_id, "fc_projected") == 0 && result.items[1].call_id &&
              strcmp(result.items[1].call_id, "call_projected") == 0 && result.items[1].name &&
              strcmp(result.items[1].name, "sh") == 0 && result.items[1].arguments &&
              strcmp(result.items[1].arguments, "{\"cmd\":\"ls\"}") == 0,
          "function calls project both ids, name, and arguments");

    JsonBuf assistant;
    JsonBuf_Init(&assistant);
    JsonBuf_Puts(&assistant, "{\"type\":\"assistant\",\"text\":\"\",\"thinking_signature\":");
    JsonBuf_String(&assistant, result.items[0].thinking_signature ? result.items[0].thinking_signature : "");
    JsonBuf_Putc(&assistant, '}');
    JsonBuf call;
    JsonBuf_Init(&call);
    JsonBuf_Puts(&call, "{\"type\":\"tool_call\",\"call_id\":");
    JsonBuf_String(&call, result.items[1].call_id ? result.items[1].call_id : "");
    JsonBuf_Puts(&call, ",\"name\":");
    JsonBuf_String(&call, result.items[1].name ? result.items[1].name : "");
    JsonBuf_Puts(&call, ",\"arguments\":");
    JsonBuf_String(&call, result.items[1].arguments ? result.items[1].arguments : "{}");
    JsonBuf_Puts(&call, ",\"item_id\":");
    JsonBuf_String(&call, result.items[1].item_id ? result.items[1].item_id : "");
    JsonBuf_Putc(&call, '}');
    const char *input[] = {assistant.data, call.data};
    PicoLlmTurn turn = {
        .model = "gpt",
        .input_json = input,
        .input_count = 2,
    };
    PicoResponsesBuildOpts openai = {
        .provider = "openai",
        .store_false = true,
        .include_encrypted_reasoning = true,
    };
    char *body = pico_responses_build_request(&turn, &openai);
    CheckContains(body, "\"id\":\"rs_projected\"", true,
                  "projected reasoning id replays to Responses");
    CheckContains(body, "\"encrypted_content\":\"secret_blob\"", true,
                  "projected encrypted reasoning replays to Responses");
    CheckContains(body, "\"id\":\"fc_projected\"", true,
                  "projected function-call item id replays to Responses");
    CheckContains(body, "\"call_id\":\"call_projected\"", true,
                  "projected tool call id replays to Responses");
    JsonDoc request;
    int parse_rc = body ? JsonParse(&request, body, strlen(body)) : -1;
    Check(parse_rc == 0, "projected Responses request parses");
    if (parse_rc == 0)
    {
        int request_input = JsonObjGet(&request, 0, "input");
        int function_call = -1;
        int input_count = JsonIsArray(&request, request_input) ? JsonArrayLen(&request, request_input) : 0;
        for (int i = 0; i < input_count; i++)
        {
            int item = JsonArrayAt(&request, request_input, i);
            if (JsonEq(&request, JsonObjGet(&request, item, "type"), "function_call"))
            {
                function_call = item;
                break;
            }
        }
        char *name = function_call >= 0 ? JsonObjStr(&request, function_call, "name") : NULL;
        char *arguments = function_call >= 0 ? JsonObjStr(&request, function_call, "arguments") : NULL;
        Check(name && strcmp(name, "sh") == 0, "projected function-call name replays to Responses");
        Check(arguments && strcmp(arguments, "{\"cmd\":\"ls\"}") == 0,
              "projected function-call arguments replay to Responses");
        free(name);
        free(arguments);
        JsonFree(&request);
    }
    free(body);
    JsonBuf_Free(&assistant);
    JsonBuf_Free(&call);
    FreeResult(&result);
    pico_responses_ctx_free(&ctx);
}

static void FillFromItems(const char *items, PicoLlmResult *result)
{
    PicoResponsesCtx ctx;
    memset(&ctx, 0, sizeof(ctx));
    JsonBuf_Init(&ctx.items);
    JsonBuf_Init(&ctx.summary);
    JsonBuf_Puts(&ctx.items, items);
    ctx.item_count = 1;
    memset(result, 0, sizeof(*result));
    pico_responses_fill_result(&ctx, result);
    pico_responses_ctx_free(&ctx);
}

static void TestRefusalProjection(void)
{
    PicoLlmResult result;
    FillFromItems("{\"type\":\"message\",\"role\":\"assistant\",\"content\":["
                  "{\"type\":\"output_refusal\",\"text\":\"nope\"}]}",
                  &result);
    Check(result.error == NULL && pico_llm_result_has_output(&result) && result.item_count == 1 &&
              result.items[0].part_count == 1 && result.items[0].parts[0].kind == PICO_LLM_PART_REFUSAL &&
              result.items[0].parts[0].text && strcmp(result.items[0].parts[0].text, "nope") == 0,
          "output_refusal becomes a refusal part and is not an empty response");
    pico_llm_result_free(&result);
}

static void TestRefusalRequestReplay(void)
{
    const char *input[] = {
        "{\"type\":\"assistant\",\"parts\":[{\"type\":\"refusal\",\"text\":\"nope\"}]}"
    };
    PicoLlmTurn turn = {.model = "gpt", .input_json = input, .input_count = 1};
    PicoResponsesBuildOpts opts = {.provider = "openai"};
    char *body = pico_responses_build_request(&turn, &opts);
    Check(body && strstr(body, "\"type\":\"refusal\",\"refusal\":\"nope\""),
          "canonical refusal replays with the Responses refusal wire shape");
    free(body);
}

static void TestUnsupportedWebSearch(void)
{
    PicoLlmResult result;
    FillFromItems("{\"type\":\"web_search_call\",\"id\":\"ws1\"}", &result);
    Check(result.error && strstr(result.error, "unsupported output: web_search_call"),
          "web_search_call fails closed");
    pico_llm_result_free(&result);
}

static void TestAnnotations(void)
{
    PicoLlmResult empty_ann;
    FillFromItems("{\"type\":\"message\",\"role\":\"assistant\",\"content\":["
                  "{\"type\":\"output_text\",\"text\":\"hi\",\"annotations\":[]}]}",
                  &empty_ann);
    Check(empty_ann.error == NULL && empty_ann.item_count == 1 && empty_ann.items[0].part_count == 1 &&
              empty_ann.items[0].parts[0].kind == PICO_LLM_PART_TEXT,
          "empty annotations on output_text still project text");
    pico_llm_result_free(&empty_ann);

    PicoLlmResult nonempty;
    FillFromItems("{\"type\":\"message\",\"role\":\"assistant\",\"content\":["
                  "{\"type\":\"output_text\",\"text\":\"hi\",\"annotations\":[{\"type\":\"url_citation\"}]}]}",
                  &nonempty);
    Check(nonempty.error && strstr(nonempty.error, "unsupported output: annotations"),
          "non-empty annotations fail closed");
    pico_llm_result_free(&nonempty);
}

int main(void)
{
    TestRequestOptions();
    TestContextItemConversion();
    TestImageRequestConversion();
    TestReasoningRemoval();
    TestRefreshDecision();
    TestSignatureReplay();
    TestReasoningResultProjection();
    TestRefusalProjection();
    TestRefusalRequestReplay();
    TestUnsupportedWebSearch();
    TestAnnotations();
    return g_failed;
}
