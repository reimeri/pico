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
    const char *headers;
    PicoHttpFailure failure;
    bool stall;
    int elapsed;
} Attempt;

static Attempt attempts[8];
static int attempt_count, requests, resets, announced, delay;
static long long fake_ns;
static double first_failure;
static double slept;
static bool cancel_wait, cancelled;
static JsonBuf visible;
static bool routing_test;
static char *routing_state;

int clock_gettime(clockid_t clock, struct timespec *ts)
{
    (void)clock;
    ts->tv_sec = fake_ns / 1000000000;
    ts->tv_nsec = fake_ns % 1000000000;
    return 0;
}

/* Replace only the wait boundary, keeping the production retry/cancellation loop. */
int nanosleep(const struct timespec *duration, struct timespec *remaining)
{
    (void)remaining;
    slept += duration->tv_sec + duration->tv_nsec / 1000000000.0;
    fake_ns += duration->tv_sec * 1000000000LL + duration->tv_nsec;
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
        unsigned retry;
        int seconds;
        const char *pending = strstr(delta->text, "retry ");
        if (pending && sscanf(pending, "retry %u in %ds", &retry, &seconds) == 2)
        {
            Check(retry == (unsigned)++announced && seconds > 0, "retry status gives pending attempt and delay");
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
        session |= strcmp(req->headers[i], "session-id: stable") == 0;
    }
    Check(strcmp(req->body, "{\"request\":true}") == 0 && auth && session,
          "capacity retries replay the original body and headers");
    if (requests) Check(slept + 1.01 >= delay,
                        "each replay follows the advertised wait");
    if (requests > 100) { Check(false, "retry loop is bounded"); *http = 400; return PICO_HTTP_OK; }
    const Attempt *a = &attempts[requests < attempt_count ? requests : attempt_count - 1];
    if (routing_test) {
        bool found = false;
        for (int i = 0; i < req->header_count; ++i) {
            if (!strncmp(req->headers[i], "x-codex-turn-state:", 19)) {
                found = true;
                Check(!strcmp(req->headers[i], "x-codex-turn-state: original-route"),
                      "retry sends the first server routing token unchanged");
            }
        }
        Check(found == (requests > 0), "routing token is absent initially and present on retry");
        Check(req->on_headers != NULL, "Codex receives response headers");
        if (req->on_headers) {
            const char *ignored = "HTTP/1.1 103 Early Hints\r\nx-codex-turn-state: interim\r\n\r\n";
            req->on_headers(req->user, 103, ignored, strlen(ignored));
            req->on_headers(req->user, 302, ignored, strlen(ignored));
            if (a->headers) req->on_headers(req->user, a->http, a->headers, strlen(a->headers));
        }
    }
    Check(req->provider_managed_retry, "provider owns a single retry budget, without nested HTTP retries");
    if (requests) Check(req->fresh_connection, "recovery avoids the suspect connection");
    if (!requests) first_failure = fake_ns / 1e9;
    if (req->on_headers && a->headers && !routing_test)
        req->on_headers(req->user, a->http, a->headers, strlen(a->headers));
    requests++;
    fake_ns += (long long)a->elapsed * 1000000000LL;
    *http = a->http;
    *error = NULL;
    if (req->cancel && req->cancel(req->user)) return PICO_HTTP_CANCEL;
    for (int i = 0; a->chunks[i]; i++)
        if (!req->on_json(req->user, a->event, a->chunks[i], strlen(a->chunks[i]))) break;
    if (a->stall) {
        /* A healthy initial request can be slow; only retry requests have a recovery deadline. */
        for (int tick = 0; tick < 10000; ++tick) {
            fake_ns += 1000000000LL;
            if (req->cancel && req->cancel(req->user)) return PICO_HTTP_CANCEL;
        }
        Check(false, "in-flight recovery request must be bounded");
    }
    if (req->failure) *req->failure = a->failure;
    if (a->failure != PICO_HTTP_FAILURE_NONE) {
        *error = JsonDup("simulated transport failure");
        return PICO_HTTP_FAIL;
    }
    return PICO_HTTP_OK;
}

static void Reset(void)
{
    free(routing_state); routing_state = NULL; routing_test = false;
    memset(attempts, 0, sizeof(attempts));
    attempt_count = requests = resets = announced = delay = 0;
    fake_ns = 1000000000LL;
    slept = 0;
    cancel_wait = cancelled = false;
    JsonBuf_Free(&visible);
    JsonBuf_Init(&visible);
}

static int PostWithRecovery(bool responses, PicoLlmResult *out, PicoProviderRetry *recovery)
{
    const char *extras[] = {"session-id: stable"};
    if (responses)
    {
        PicoResponsesCtx ctx;
        int rc = pico_responses_post("https://provider.invalid", "{\"request\":true}", "test-token",
                                     extras, 1, Cancel, Delta, NULL, routing_test ? &routing_state : NULL, &ctx, recovery);
        if (rc != PICO_LLM_CANCEL) pico_responses_fill_result(&ctx, out);
        pico_responses_ctx_free(&ctx);
        return rc;
    }
    PicoCompletionsCtx ctx;
    int rc = pico_completions_post("https://provider.invalid", "{\"request\":true}", "test-token",
                                   extras, 1, Cancel, Delta, NULL, &ctx, recovery);
    if (rc != PICO_LLM_CANCEL) pico_completions_fill_result(&ctx, out);
    pico_completions_ctx_free(&ctx);
    return rc;
}

static int Post(bool responses, PicoLlmResult *out)
{
    return PostWithRecovery(responses, out, NULL);
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
    attempts[0].http = status;
    attempts[0].chunks[0] = "<html>Gateway temporarily unavailable</html>";
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
    Check(rc == PICO_LLM_FAIL && requests > 2 && out.error && strstr(out.error, "recovery timed out") &&
          fake_ns / 1e9 - first_failure > 0 && fake_ns / 1e9 - first_failure < 601,
          "repeated transient failures exhaust a bounded elapsed-time budget");
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
        "{\"type\":\"response\",\"status\":\"completed\",\"error\":null,\"output\":[{\"type\":\"message\",\"role\":\"assistant\","
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

static char *ArgumentChunk(bool responses, size_t length)
{
    char *args = malloc(length + 1);
    if (!args) return NULL;
    memset(args, 'a', length);
    memcpy(args, "{\"x\":\"", 6);
    memcpy(args + length - 2, "\"}", 2);
    args[length] = 0;
    JsonBuf b = {0};
    JsonBuf_Puts(&b, responses ?
        "{\"type\":\"response.function_call_arguments.delta\",\"output_index\":0,\"delta\":" :
        "{\"choices\":[{\"delta\":{\"tool_calls\":[{\"index\":0,\"id\":\"call\",\"function\":{\"name\":\"sh\",\"arguments\":");
    JsonBuf_String(&b, args);
    JsonBuf_Puts(&b, responses ? "}" : "}}]}}]}");
    free(args);
    return JsonBuf_Steal(&b);
}

static void TestGuardRetryPolicy(bool responses)
{
    Reset();
    /* Each attempt is individually below the cap; together they exceed it. */
    char *chunk = ArgumentChunk(responses, PICO_STREAM_TOOL_ARGUMENT_LIMIT * 3 / 4);
    Check(chunk != NULL, "allocate retry guard fixture");
    if (!chunk) return;
    attempts[0] = (Attempt){503, {chunk}, NULL};
    attempts[1] = Success(responses);
    attempts[1].chunks[2] = attempts[1].chunks[1];
    attempts[1].chunks[1] = attempts[1].chunks[0];
    attempts[1].chunks[0] = chunk;
    attempt_count = 2;
    PicoLlmResult out = {0};
    int rc = Post(responses, &out);
    Check(rc == PICO_LLM_OK && !out.error && requests == 2 && resets == 1,
          "capacity replay starts with a fresh argument budget");
    pico_llm_result_free(&out);
    free(chunk);

    Reset();
    chunk = ArgumentChunk(responses, PICO_STREAM_TOOL_ARGUMENT_LIMIT + 1u);
    Check(chunk != NULL, "allocate oversized guard fixture");
    if (!chunk) return;
    attempts[0] = (Attempt){503, {chunk}, NULL};
    attempt_count = 1;
    rc = Post(responses, &out);
    Check(rc == PICO_LLM_FAIL && out.error && strstr(out.error, "1 MiB") &&
          !out.item_count && requests == 1 && !resets,
          "a generation guard failure is never replayed, even with a capacity HTTP status");
    pico_llm_result_free(&out);
    free(chunk);
}

static void TestRoutingAcrossCapacityRetry(void)
{
    Reset(); routing_test = true;
    /* A provider must never turn a stored value into injected request headers. */
    routing_state = JsonDup("bad\r\ninjected: value");
    attempts[0].http = 503;
    attempts[0].headers = "HTTP/1.1 503 Unavailable\r\nX-CoDeX-Turn-State: original-route \t\r\n\r\n";
    attempts[1] = Success(true);
    attempts[1].headers = "HTTP/1.1 200 OK\r\nx-codex-turn-state: replacement-must-be-ignored\r\n\r\n";
    attempt_count = 2;
    PicoLlmResult out = {0};
    CheckRecovered(Post(true, &out), &out);
    Check(routing_state && !strcmp(routing_state, "original-route"),
          "interim/redirect headers cannot set routing; the first final token survives retries");
    pico_llm_result_free(&out);
    Reset();
}

static void TestInterruptedStream(bool responses, bool transport)
{
    Reset();
    attempts[0] = responses ? (Attempt){200, {
        "{\"type\":\"response.output_text.delta\",\"delta\":\"discarded\"}",
        "{\"type\":\"response.output_item.done\",\"item\":{\"type\":\"function_call\",\"call_id\":\"stale\",\"name\":\"sh\",\"arguments\":\"{}\"}}"
    }, NULL} : (Attempt){200, {
        "{\"choices\":[{\"delta\":{\"content\":\"discarded\",\"tool_calls\":[{\"index\":0,\"id\":\"stale\",\"function\":{\"name\":\"sh\",\"arguments\":\"{\"}}]}}]}"
    }, NULL};
    attempts[0].failure = transport ? PICO_HTTP_FAILURE_TRANSIENT : PICO_HTTP_FAILURE_NONE;
    attempts[1] = Success(responses); attempt_count = 2;
    PicoLlmResult out = {0};
    CheckRecovered(Post(responses, &out), &out);
    pico_llm_result_free(&out);
}

static void TestPermanentTransport(bool responses)
{
    Reset(); attempts[0].failure = PICO_HTTP_FAILURE_PERMANENT; attempt_count = 1;
    PicoLlmResult out = {0};
    Check(Post(responses, &out) == PICO_LLM_FAIL && requests == 1 && !resets,
          "certificate/configuration transport failures are never replayed");
    pico_llm_result_free(&out);
}

static void TestQuota(bool responses)
{
    Reset(); attempts[0] = (Attempt){429, {"{\"error\":{\"code\":\"insufficient_quota\",\"message\":\"Quota exhausted\"}}"}, NULL};
    attempt_count = 1; PicoLlmResult out = {0};
    Check(Post(responses, &out) == PICO_LLM_FAIL && requests == 1 && !resets,
          "permanent quota error overrides a retryable HTTP status");
    pico_llm_result_free(&out);
}

static void TestRetryAfter(bool responses, bool beyond_budget)
{
    Reset(); attempts[0].http = 503;
    attempts[0].headers = beyond_budget ? "HTTP/1.1 503 Busy\r\nRetry-After: 3600\r\n\r\n" :
                                         "HTTP/1.1 503 Busy\r\nRetry-After: 20\r\n\r\n";
    attempts[1] = Success(responses); attempt_count = 2;
    PicoLlmResult out = {0}; int rc = Post(responses, &out);
    if (beyond_budget) Check(rc == PICO_LLM_FAIL && requests == 1 && out.error,
                             "Retry-After beyond recovery budget stops without an early replay");
    else Check(rc == PICO_LLM_OK && requests == 2 && slept >= 20,
               "server Retry-After is a minimum delay");
    pico_llm_result_free(&out);
}

static void TestInFlightDeadline(bool responses, bool first_attempt)
{
    Reset(); attempts[0].http = 503; attempts[1].stall = true; attempts[1].http = 200;
    if (first_attempt) {
        attempts[0].stall = true;
        attempts[0].headers = "HTTP/1.1 503 Busy\r\n\r\n";
    }
    attempt_count = 2; PicoLlmResult out = {0};
    Check(Post(responses, &out) == PICO_LLM_FAIL && requests == (first_attempt ? 1 : 2) && out.error &&
          strstr(out.error, "recovery timed out"), "deadline aborts an in-flight retry as failure, not user cancellation");
    pico_llm_result_free(&out);
}

static void TestHealthyInitialRequest(bool responses)
{
    Reset(); attempts[0] = Success(responses); attempts[0].elapsed = 1200; attempt_count = 1;
    PicoLlmResult out = {0};
    Check(Post(responses, &out) == PICO_LLM_OK && requests == 1 && !resets,
          "healthy initial inference is not limited by the recovery window");
    pico_llm_result_free(&out);
}

static void TestSharedBudget(bool responses)
{
    Reset();
    PicoProviderRetry recovery = {.cancel = Cancel, .on_delta = Delta};
    attempts[0].http = 503;
    attempts[0].headers = "HTTP/1.1 503 Busy\r\nRetry-After: 500\r\n\r\n";
    attempts[1].http = 401;
    attempt_count = 2;
    PicoLlmResult out = {0};
    Check(PostWithRecovery(responses, &out, &recovery) == PICO_LLM_FAIL && requests == 2,
          "authentication rejection exits recovery for the bounded refresh path");
    pico_llm_result_free(&out);
    /* The outer adapter refreshes credentials then invokes the helper again. */
    attempts[2] = Success(responses); attempts[2].elapsed = 120; attempt_count = 3;
    Check(PostWithRecovery(responses, &out, &recovery) == PICO_LLM_FAIL && requests == 3 &&
          out.error && strstr(out.error, "recovery timed out"),
          "authentication/request-shape reentry cannot restart an existing recovery window");
    pico_llm_result_free(&out);
}

static void TestInternalError(bool responses)
{
    Reset();
    attempts[0] = (Attempt){200, {"{\"error\":{\"code\":\"internal_error\",\"message\":\"Temporary failure\"}}"}, NULL};
    attempts[1] = Success(responses); attempt_count = 2;
    PicoLlmResult out = {0};
    CheckRecovered(Post(responses, &out), &out);
    pico_llm_result_free(&out);
}

int main(void)
{
    for (int responses = 0; responses <= 1; responses++)
    {
        TestHealthyInitialRequest(responses);
        TestSharedBudget(responses);
        TestInternalError(responses);
        TestInterruptedStream(responses, true);
        TestInterruptedStream(responses, false);
        TestPermanentTransport(responses);
        TestQuota(responses);
        TestRetryAfter(responses, false);
        TestRetryAfter(responses, true);
        TestInFlightDeadline(responses, false);
        TestInFlightDeadline(responses, true);
        TestHttpRecovery(responses, 408);
        TestHttpRecovery(responses, 500);
        TestHttpRecovery(responses, 502);
        TestHttpRecovery(responses, 504);
        TestGuardRetryPolicy(responses);
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
    TestRoutingAcrossCapacityRetry();
    JsonBuf_Free(&visible);
    return failed;
}
