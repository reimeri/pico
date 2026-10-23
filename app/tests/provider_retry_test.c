#define _POSIX_C_SOURCE 200809L

#include "builtins/responses.h"
#include "builtins/completions.h"
#include "canonical.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static int failed;
static void Check(bool ok, const char *message)
{
    if (!ok) { fprintf(stderr, "FAIL: %s\n", message); failed = 1; }
}

typedef struct Attempt {
    long http;
    const char *chunks[5];
    const char *event;
} Attempt;

static Attempt attempts[8];
static int attempt_count, requests, resets, announced, max_retries, delay;
static double slept;
static bool cancel_wait, cancelled;
static JsonBuf visible;

/* Replace only the wait boundary, keeping the production retry/cancellation loop. */
int nanosleep(const struct timespec *duration, struct timespec *remaining)
{
    (void)remaining;
    slept += duration->tv_sec + duration->tv_nsec / 1000000000.0;
    if (cancel_wait) cancelled = true;
    return 0;
}

static bool Cancel(void *user) { (void)user; return cancelled; }

static void Delta(void *user, const PicoLlmDelta *delta)
{
    (void)user;
    if (delta->kind == PICO_LLM_DELTA_RESET)
    {
        resets++;
        JsonBuf_Clear(&visible);
    }
    else if (delta->kind == PICO_LLM_DELTA_TEXT)
        JsonBuf_Append(&visible, delta->text, delta->len);
    else if (delta->kind == PICO_LLM_DELTA_STATUS)
    {
        int retry, maximum, seconds;
        const char *pending = strstr(delta->text, "retry ");
        if (pending && sscanf(pending, "retry %d/%d in %ds", &retry, &maximum, &seconds) == 3)
        {
            Check(retry == ++announced && seconds > 0, "retry status gives the pending attempt and nonzero delay");
            Check(seconds >= delay, "capacity retry delays do not decrease");
            max_retries = maximum;
            delay = seconds;
            slept = 0;
        }
    }
}

int pico_http_post_sse(const PicoHttpPost *req, long *http, char **error)
{
    bool auth = false, session = false;
    for (int i = 0; i < req->header_count; i++)
    {
        auth |= strcmp(req->headers[i], "Authorization: Bearer test-token") == 0;
        session |= strcmp(req->headers[i], "session_id: stable") == 0;
    }
    Check(strcmp(req->body, "{\"request\":true}") == 0 && auth && session,
          "capacity retries replay the original body and headers");
    if (requests) Check(slept + 0.001 >= delay && resets == requests,
                        "each replay follows a reset and the full advertised wait");
    if (requests > 32) { Check(false, "retry loop is bounded"); *http = 400; return PICO_HTTP_OK; }
    const Attempt *a = &attempts[requests < attempt_count ? requests : attempt_count - 1];
    requests++;
    *http = a->http;
    *error = NULL;
    if (req->cancel && req->cancel(req->user)) return PICO_HTTP_CANCEL;
    for (int i = 0; a->chunks[i]; i++)
        if (!req->on_json(req->user, a->event, a->chunks[i], strlen(a->chunks[i]))) break;
    return PICO_HTTP_OK;
}

static void Reset(void)
{
    memset(attempts, 0, sizeof(attempts));
    attempt_count = requests = resets = announced = max_retries = delay = 0;
    slept = 0;
    cancel_wait = cancelled = false;
    JsonBuf_Free(&visible);
    JsonBuf_Init(&visible);
}

static int Post(bool responses, PicoLlmResult *out)
{
    const char *extras[] = {"session_id: stable"};
    if (responses)
    {
        PicoResponsesCtx ctx;
        int rc = pico_responses_post("https://provider.invalid", "{\"request\":true}", "test-token",
                                     extras, 1, Cancel, Delta, NULL, &ctx);
        if (rc != PICO_LLM_CANCEL) pico_responses_fill_result(&ctx, out);
        pico_responses_ctx_free(&ctx);
        return rc;
    }
    PicoCompletionsCtx ctx;
    int rc = pico_completions_post("https://provider.invalid", "{\"request\":true}", "test-token",
                                   extras, 1, Cancel, Delta, NULL, &ctx);
    if (rc != PICO_LLM_CANCEL) pico_completions_fill_result(&ctx, out);
    pico_completions_ctx_free(&ctx);
    return rc;
}

static Attempt Success(bool responses)
{
    return responses ? (Attempt){200, {
        "{\"type\":\"response.output_text.delta\",\"delta\":\"replacement\"}",
        "{\"type\":\"response.completed\",\"response\":{\"usage\":{\"input_tokens\":7},\"output\":["
        "{\"type\":\"message\",\"role\":\"assistant\",\"content\":[{\"type\":\"output_text\",\"text\":\"replacement\"}]}]}}"
    }, NULL} : (Attempt){200, {
        "{\"choices\":[{\"delta\":{\"content\":\"replacement\"},\"finish_reason\":\"stop\"}],\"usage\":{\"prompt_tokens\":7}}"
    }, NULL};
}

static void CheckRecovered(int rc, const PicoLlmResult *out)
{
    Check(rc == PICO_LLM_OK && requests == 2 && resets == 1 &&
          visible.data && strcmp(visible.data, "replacement") == 0 &&
          out->item_count == 1 && !out->error && out->input_tokens == 7 && !out->service_tier[0],
          "a capacity retry returns only replacement output and usage");
}

static void TestHttpRecovery(bool responses, long status)
{
    Reset();
    attempts[0].http = status; /* No JSON body: an HTML or empty capacity response. */
    attempts[1] = Success(responses);
    attempt_count = 2;
    PicoLlmResult out = {0};
    int rc = Post(responses, &out);
    CheckRecovered(rc, &out);
    pico_llm_result_free(&out);
}

static void TestStreamRecovery(bool responses)
{
    Reset();
    attempts[0] = responses ? (Attempt){200, {
        "{\"type\":\"response.output_text.delta\",\"delta\":\"discarded\"}",
        "{\"type\":\"response.output_item.done\",\"item\":{\"type\":\"function_call\",\"call_id\":\"stale\",\"name\":\"sh\",\"arguments\":\"{}\"}}",
        "{\"type\":\"response.failed\",\"response\":{\"service_tier\":\"priority\",\"usage\":{\"input_tokens\":999},"
        "\"error\":{\"code\":\"server_error\",\"message\":\"Our servers are currently overloaded. Please try again later.\"}}}"
    }, NULL} : (Attempt){200, {
        "{\"service_tier\":\"priority\",\"usage\":{\"prompt_tokens\":999},\"choices\":[{\"delta\":{\"content\":\"discarded\","
        "\"reasoning_content\":\"stale thinking\",\"tool_calls\":[{\"index\":0,\"id\":\"stale\",\"function\":{\"name\":\"sh\",\"arguments\":\"{}\"}}]}}]}",
        "{\"type\":\"overloaded_error\",\"message\":\"Please retry later\"}"
    }, NULL};
    attempts[1] = Success(responses);
    attempt_count = 2;
    PicoLlmResult out = {0};
    int rc = Post(responses, &out);
    CheckRecovered(rc, &out);
    pico_llm_result_free(&out);
}

static void TestCodeOnlyRecovery(bool responses)
{
    Reset();
    attempts[0] = (Attempt){200, {"{\"error\":{\"code\":\"capacity_exceeded\",\"message\":\"Try later\"}}"}, NULL};
    attempts[1] = Success(responses);
    attempt_count = 2;
    PicoLlmResult out = {0};
    int rc = Post(responses, &out);
    CheckRecovered(rc, &out);
    pico_llm_result_free(&out);
}

static void TestCapacityEvent(bool responses)
{
    Reset();
    attempts[0] = (Attempt){200, {"{\"message\":\"Try again later\"}"}, "overloaded_error"};
    attempts[1] = Success(responses);
    attempt_count = 2;
    PicoLlmResult out = {0};
    int rc = Post(responses, &out);
    CheckRecovered(rc, &out);
    pico_llm_result_free(&out);
}

static void TestNestedCodeOnlyError(void)
{
    Reset();
    attempts[0] = (Attempt){200, {
        "{\"type\":\"response.completed\",\"response\":{\"error\":{\"code\":\"capacity_exceeded\"},"
        "\"output\":[{\"type\":\"function_call\",\"call_id\":\"stale\",\"name\":\"sh\",\"arguments\":\"{}\"}]}}"
    }, NULL};
    attempts[1] = Success(true);
    attempt_count = 2;
    PicoLlmResult out = {0};
    int rc = Post(true, &out);
    CheckRecovered(rc, &out);
    pico_llm_result_free(&out);
}

static void TestExhaustion(bool responses)
{
    Reset();
    attempts[0] = (Attempt){503, {"{\"error\":{\"message\":\"Server at capacity\"}}"}, NULL};
    attempt_count = 1; /* Repeat until the provider stops retrying. */
    PicoLlmResult out = {0};
    int rc = Post(responses, &out);
    Check(rc == PICO_LLM_FAIL && max_retries > 0 && requests == max_retries + 1 &&
          announced == max_retries && resets == max_retries && out.error &&
          strcmp(out.error, "Server at capacity") == 0,
          "capacity exhaustion stops at the advertised retry limit and preserves the last error");
    pico_llm_result_free(&out);
}

static void TestCancellation(bool responses)
{
    Reset();
    attempts[0].http = 429;
    attempt_count = 1;
    cancel_wait = true;
    PicoLlmResult out = {0};
    int rc = Post(responses, &out);
    Check(rc == PICO_LLM_CANCEL && requests == 1 && slept < delay && resets == 1 && !out.error,
          "cancelling a capacity wait returns promptly without another request or failure");
    pico_llm_result_free(&out);
}

static void TestCapacityTextIsNotAnError(bool responses)
{
    Reset();
    attempts[0] = responses ? (Attempt){200, {
        "{\"type\":\"response\",\"error\":null,\"output\":[{\"type\":\"message\",\"role\":\"assistant\","
        "\"content\":[{\"type\":\"output_text\",\"text\":\"The servers are overloaded\"}]}]}"
    }, NULL} : (Attempt){200, {
        "{\"error\":null,\"choices\":[{\"message\":{\"content\":\"The servers are overloaded\"},\"finish_reason\":\"stop\"}]}"
    }, NULL};
    attempt_count = 1;
    PicoLlmResult out = {0};
    int rc = Post(responses, &out);
    Check(rc == PICO_LLM_OK && requests == 1 && !resets && !out.error &&
          visible.data && strcmp(visible.data, "The servers are overloaded") == 0,
          "assistant text mentioning overload is not mistaken for a capacity error");
    pico_llm_result_free(&out);
}

static void TestOtherErrors(bool responses)
{
    Reset();
    attempts[0] = (Attempt){400, {"{\"error\":{\"code\":\"invalid_request\",\"message\":\"Invalid model\"}}"}, NULL};
    attempt_count = 1;
    PicoLlmResult out = {0};
    int rc = Post(responses, &out);
    Check(rc == PICO_LLM_FAIL && requests == 1 && !resets && out.error &&
          strcmp(out.error, "Invalid model") == 0, "ordinary provider failures are not replayed");
    pico_llm_result_free(&out);
}

int main(void)
{
    for (int responses = 0; responses <= 1; responses++)
    {
        TestHttpRecovery(responses, 429);
        TestHttpRecovery(responses, 503);
        TestStreamRecovery(responses);
        TestCodeOnlyRecovery(responses);
        TestCapacityEvent(responses);
        TestExhaustion(responses);
        TestCancellation(responses);
        TestOtherErrors(responses);
        TestCapacityTextIsNotAnError(responses);
    }
    TestNestedCodeOnlyError();
    JsonBuf_Free(&visible);
    return failed;
}
