#include "builtins/hyper_auth.h"
#include "builtins/responses.h"

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

static void TestRequestOptions(void)
{
    const char *input[] = {
        "{\"type\":\"user\",\"text\":\"hello\"}",
        "{\"type\":\"raw\",\"provider\":\"hyper\",\"json\":{\"type\":\"reasoning\",\"id\":\"keep\"}}",
        "{\"type\":\"raw\",\"provider\":\"openai\",\"json\":{\"type\":\"reasoning\",\"id\":\"drop\"}}",
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
        .input_count = 3,
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
    CheckContains(body, "\"id\":\"keep\"", true, "Hyper replays Hyper raw items");
    CheckContains(body, "\"id\":\"drop\"", false, "Hyper excludes another provider's raw items");
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
    CheckContains(body, "\"id\":\"keep\"", false, "OpenAI excludes Hyper raw items");
    CheckContains(body, "\"id\":\"drop\"", true, "OpenAI replays OpenAI raw items");
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

static void TestCanonicalUrl(void)
{
    const char *canonical = "https://hyper.charm.land/v1";
    char out[128];
    Check(pico_responses_resolve_canonical_url(NULL, canonical, out, sizeof(out)) &&
              strcmp(out, "https://hyper.charm.land/v1/responses") == 0,
          "empty override uses canonical Hyper endpoint");
    Check(pico_responses_resolve_canonical_url("https://hyper.charm.land/v1/", canonical, out,
                                                sizeof(out)),
          "canonical base with trailing slash is accepted");
    Check(pico_responses_resolve_canonical_url("https://hyper.charm.land/v1/responses", canonical, out,
                                                sizeof(out)),
          "canonical responses endpoint is accepted");
    Check(!pico_responses_resolve_canonical_url("http://hyper.charm.land/v1", canonical, out, sizeof(out)),
          "plaintext Hyper endpoint is rejected");
    Check(!pico_responses_resolve_canonical_url("https://attacker.example/v1", canonical, out, sizeof(out)),
          "another origin is rejected");
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

int main(void)
{
    TestRequestOptions();
    TestReasoningRemoval();
    TestCanonicalUrl();
    TestRefreshDecision();
    return g_failed;
}
