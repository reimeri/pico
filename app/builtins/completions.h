#ifndef PICO_COMPLETIONS_H
#define PICO_COMPLETIONS_H

#include "pico/app.h"
#include "pico/http.h"
#include "json.h"

/* Internal Chat Completions helper. Hyper uses it; other OpenAI-compat
 * providers can pass PicoCompletionsBuildOpts rather than forking the wire format. */

typedef enum PicoCompletionsThinking {
    PICO_COMPLETIONS_THINKING_NONE = 0,
    PICO_COMPLETIONS_THINKING_DEEPSEEK,
} PicoCompletionsThinking;

typedef struct PicoCompletionsCall {
    char *id;
    char *name;
    JsonBuf arguments;
    int index;
} PicoCompletionsCall;

typedef struct PicoCompletionsCtx {
    PicoLlmCancelFn cancel;
    PicoLlmDeltaFn on_delta;
    void *user;
    JsonBuf text;
    JsonBuf think;
    PicoCompletionsCall *calls;
    int call_count;
    int input_tokens;
    int cached_tokens;
    char *error;
    bool failed;
    bool saw_text;
    long http;
} PicoCompletionsCtx;

typedef struct PicoCompletionsBuildOpts {
    const char *provider;
    bool store_false;
    PicoCompletionsThinking thinking;
    bool requires_reasoning_content;
    const char *max_tokens_field;
    int max_tokens;
} PicoCompletionsBuildOpts;

void pico_completions_resolve_url(const char *base, const char *fallback, char *out, size_t cap);
bool pico_completions_resolve_canonical_url(const char *base, const char *canonical_base, char *out,
                                            size_t cap);
char *pico_completions_build_request(const PicoLlmTurn *turn, const PicoCompletionsBuildOpts *opts);
char *pico_completions_body_without_thinking(const char *body);

void pico_completions_ctx_init(PicoCompletionsCtx *c);
bool pico_completions_feed(PicoCompletionsCtx *c, const char *json, size_t len);
int pico_completions_post(const char *url, const char *body, const char *bearer,
                          const char *const extra_headers[], int extra_count, PicoLlmCancelFn cancel,
                          PicoLlmDeltaFn on_delta, void *user, PicoCompletionsCtx *ctx);
void pico_completions_fill_result(PicoCompletionsCtx *c, PicoLlmResult *out);
void pico_completions_ctx_free(PicoCompletionsCtx *c);

#endif
