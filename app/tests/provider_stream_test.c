#include "builtins/completions.h"
#include "builtins/responses.h"
#ifdef PICO_TEST_CLOCK
#include "test_clock.h"
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct Stream {
    bool completions;
    PicoResponsesCtx responses;
    PicoCompletionsCtx chat;
} Stream;

static int failed;
static void Check(bool ok, const char *message)
{
    if (!ok) { fprintf(stderr, "FAIL: %s\n", message); failed = 1; }
}

static Stream NewStream(bool completions)
{
    Stream s = {.completions = completions};
    if (completions) pico_completions_ctx_init(&s.chat);
    return s;
}
static void FreeStream(Stream *s)
{
    if (s->completions) pico_completions_ctx_free(&s->chat);
    else pico_responses_ctx_free(&s->responses);
}
static void Result(Stream *s, PicoLlmResult *result)
{
    if (s->completions) pico_completions_fill_result(&s->chat, result);
    else pico_responses_fill_result(&s->responses, result);
}

/* Encode the actual wire formats so these tests cover both builtin adapters,
 * including guard checks when the caller has no live delta subscriber. */
static bool Fragment(Stream *s, PicoStreamKind kind, int index, const char *text)
{
    JsonBuf b = {0};
    if (s->completions)
    {
        JsonBuf_Puts(&b, "{\"choices\":[{\"delta\":{");
        if (kind == PICO_STREAM_ARGUMENTS)
        {
            char field[160];
            snprintf(field, sizeof(field), "\"tool_calls\":[{\"index\":%d,\"id\":\"call%d\","
                     "\"function\":{\"name\":\"sh\",\"arguments\":", index, index);
            JsonBuf_Puts(&b, field);
            JsonBuf_String(&b, text);
            JsonBuf_Puts(&b, "}}]");
        }
        else
        {
            JsonBuf_String(&b, kind == PICO_STREAM_TEXT ? "content" : "reasoning_content");
            JsonBuf_Putc(&b, ':');
            JsonBuf_String(&b, text);
        }
        JsonBuf_Puts(&b, "}}]}");
    }
    else
    {
        const char *type = kind == PICO_STREAM_ARGUMENTS ? "response.function_call_arguments.delta" :
                           kind == PICO_STREAM_TEXT ? "response.output_text.delta" :
                           kind == PICO_STREAM_SUMMARY ? "response.reasoning_summary_text.delta" :
                           "response.reasoning_text.delta";
        char field[80];
        JsonBuf_Puts(&b, "{\"type\":");
        JsonBuf_String(&b, type);
        snprintf(field, sizeof(field), ",\"output_index\":%d,\"delta\":", index);
        JsonBuf_Puts(&b, field);
        JsonBuf_String(&b, text);
        JsonBuf_Putc(&b, '}');
    }
    bool ok = s->completions ? pico_completions_feed(&s->chat, b.data, b.len) :
                              pico_responses_feed(&s->responses, b.data, b.len);
    JsonBuf_Free(&b);
    return ok;
}

/* Responses can carry full arguments in three different event shapes. */
static bool Snapshot(Stream *s, int shape, const char *arguments)
{
    JsonBuf b = {0};
    if (shape == 0)
        JsonBuf_Puts(&b, "{\"type\":\"response.function_call_arguments.done\",\"output_index\":0,\"arguments\":");
    else
    {
        JsonBuf_Puts(&b, shape == 1 ? "{\"type\":\"response.output_item.done\",\"output_index\":0,\"item\":" :
                     "{\"type\":\"response.completed\",\"response\":{\"status\":\"completed\",\"output\":[");
        JsonBuf_Puts(&b, "{\"type\":\"function_call\",\"call_id\":\"call0\",\"name\":\"sh\",\"arguments\":");
    }
    JsonBuf_String(&b, arguments);
    JsonBuf_Puts(&b, shape == 0 ? "}" : shape == 1 ? "}}" : "}]}}");
    bool ok = pico_responses_feed(&s->responses, b.data, b.len);
    JsonBuf_Free(&b);
    return ok;
}

static void CheckFailure(Stream *s, const char *reason)
{
    PicoLlmResult result = {0};
    Result(s, &result);
    Check(result.error && strstr(result.error, reason) && result.item_count == 0,
          "guard explains the failure and projects no executable tool calls");
    pico_llm_result_free(&result);
}

static void TestArgumentLimits(void)
{
    /* The boundary is the decoded argument string, not escaped wire bytes. */
    size_t limit = PICO_STREAM_TOOL_ARGUMENT_LIMIT;
    char *args = malloc(limit + 2);
    if (!args) { Check(false, "allocate argument fixture"); return; }
    memset(args, 'a', limit);
    memcpy(args, "{\"x\":\"", 6);
    memcpy(args + limit - 2, "\"}", 2);
    args[limit] = '\0';
    for (int chat = 0; chat < 2; chat++)
    {
        Stream s = NewStream(chat);
        Check(Fragment(&s, PICO_STREAM_ARGUMENTS, 0, args) &&
              Fragment(&s, PICO_STREAM_ARGUMENTS, 1, args),
              "each separate call may reach the argument limit");
        if (!chat)
            Check(Snapshot(&s, 0, args) && Snapshot(&s, 1, args),
                  "done snapshots do not double-count streamed arguments");
        Check(!Fragment(&s, PICO_STREAM_ARGUMENTS, 0, " "),
              "a fragment crossing the per-call argument limit aborts the stream");
        CheckFailure(&s, "1 MiB");
        FreeStream(&s);
    }
    args[limit] = ' '; args[limit + 1] = '\0';
    for (int shape = 0; shape < 3; shape++)
    {
        Stream s = NewStream(false);
        Check(!Snapshot(&s, shape, args), "oversized full arguments cannot bypass the limit");
        CheckFailure(&s, "1 MiB");
        FreeStream(&s);
    }
    free(args);
}

#ifdef PICO_TEST_CLOCK
static void TestRepetition(void)
{
    /* A paragraph-sized cycle, deliberately not aligned with transport fragments. */
    char pattern[701], run[21001];
    unsigned state = 7;
    for (int i = 0; i < 700; i++)
    {
        state = state * 1664525u + 1013904223u;
        pattern[i] = (char)('a' + (state >> 16) % 26);
    }
    pattern[700] = 0;
    for (int i = 0; i < 21000; i++) run[i] = pattern[i % 700];
    run[21000] = 0;
    for (int chat = 0; chat < 2; chat++)
    for (int kind = PICO_STREAM_TEXT; kind <= PICO_STREAM_ARGUMENTS; kind++)
    {
        if (chat && kind == PICO_STREAM_SUMMARY) continue;
        Stream s = NewStream(chat);
        PicoTestClock_Set(100);
        bool ok = true;
        for (int i = 0; i < 30 && ok; i++)
            ok = Fragment(&s, kind, 0, pattern);
        Check(ok, "large repetition alone is not sufficient to stop legitimate output");
        PicoTestClock_Advance(159);
        Check(Fragment(&s, kind, 0, pattern), "repetition has a sustained-time grace period");
        PicoTestClock_Advance(160);
        Check(!Fragment(&s, kind, 0, pattern), "sustained paragraph repetition stops generation");
        CheckFailure(&s, "repetitive");
        FreeStream(&s);

        /* A mismatch really breaks the run; earlier repetition is not a quota. */
        s = NewStream(chat);
        PicoTestClock_Set(100);
        Check(Fragment(&s, kind, 0, run), "start a fresh repetitive run");
        PicoTestClock_Advance(159);
        Check(Fragment(&s, kind, 0, "!"), "novel output interrupts repetition");
        PicoTestClock_Advance(160);
        Check(Fragment(&s, kind, 0, run), "an interrupted run receives a new grace period");
        FreeStream(&s);
    }
    PicoTestClock_Reset();
}

static void IgnoreDelta(void *user, const PicoLlmDelta *delta)
{
    (void)user; (void)delta;
}

static void TestCompletionSnapshot(void)
{
    Stream s = NewStream(false);
    s.responses.on_delta = IgnoreDelta;
    char repeated[17001]; memset(repeated, 'x', sizeof(repeated) - 1); repeated[17000] = 0;
    PicoTestClock_Set(100);
    Check(Fragment(&s, PICO_STREAM_TEXT, -1, repeated), "stream repetitive but time-bounded text");
    PicoTestClock_Advance(160);
    JsonBuf b = {0};
    JsonBuf_Puts(&b, "{\"type\":\"response\",\"status\":\"completed\",\"output\":["
                    "{\"type\":\"message\",\"content\":[{\"type\":\"output_text\",\"text\":");
    JsonBuf_String(&b, repeated);
    JsonBuf_Puts(&b, "}]}]}");
    Check(pico_responses_feed(&s.responses, b.data, b.len),
          "a completed full-text snapshot does not extend a repetition watch");
    PicoLlmResult result = {0}; Result(&s, &result);
    Check(!result.error && result.item_count > 0, "completion remains successful");
    pico_llm_result_free(&result);
    JsonBuf_Free(&b);
    FreeStream(&s);
    PicoTestClock_Reset();
}

static void TestChannelIsolation(void)
{
    for (int chat = 0; chat < 2; chat++)
    {
        Stream s = NewStream(chat);
        char repeated[17001]; memset(repeated, 'x', sizeof(repeated) - 1); repeated[17000] = 0;
        PicoTestClock_Set(100);
        Check(Fragment(&s, PICO_STREAM_ARGUMENTS, 0, repeated), "first tool starts a repetition watch");
        PicoTestClock_Advance(160);
        Check(Fragment(&s, PICO_STREAM_ARGUMENTS, 1, "x") &&
              Fragment(&s, PICO_STREAM_TEXT, -1, "x"),
              "another call and text do not inherit an earlier stream's repetition");
        Check(!Fragment(&s, PICO_STREAM_ARGUMENTS, 0, "x"),
              "interleaving another call does not hide an ongoing loop");
        FreeStream(&s);
    }
    PicoTestClock_Reset();
}
#endif

int main(void)
{
    TestArgumentLimits();
#ifdef PICO_TEST_CLOCK
    TestRepetition();
    TestChannelIsolation();
    TestCompletionSnapshot();
#endif
    return failed;
}
