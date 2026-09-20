#ifndef PICO_RESPONSES_H
#define PICO_RESPONSES_H

#include "pico/app.h"
#include "pico/http.h"
#include "json.h"

/* Internal Responses-API helper used by the openai builtin. Completions
 * helpers for Hyper live in completions.c. */

typedef struct PicoResponsesCtx {
    PicoLlmCancelFn cancel;
    PicoLlmDeltaFn on_delta;
    void *user;
    JsonBuf items;
    JsonBuf summary;
    int summary_output_index;
    int summary_index;
    int item_count;
    int input_tokens;
    int cached_tokens;
    char service_tier[32];
    char *error;
    bool failed;
    bool saw_text;
    bool saw_summary;
    long http;
} PicoResponsesCtx;

typedef struct PicoResponsesBuildOpts {
    const char *provider;
    const char *service_tier; /* NULL omits the field; provider chooses wire policy */
    bool store_false;
    bool include_encrypted_reasoning;
    bool reasoning_summary_auto;
} PicoResponsesBuildOpts;

void pico_responses_resolve_url(const char *base, const char *fallback, char *out, size_t cap);
bool pico_responses_openai_fast_route(const char *base, bool codex);
const char *pico_responses_openai_service_tier(const PicoLlmTurn *turn, bool codex);
char *pico_responses_build_request(const PicoLlmTurn *turn, const PicoResponsesBuildOpts *opts);
char *pico_responses_body_without_reasoning(const char *body);

bool pico_responses_feed(PicoResponsesCtx *c, const char *json, size_t len);
int pico_responses_post(const char *url, const char *body, const char *bearer,
                        const char *const extra_headers[], int extra_count, PicoLlmCancelFn cancel,
                        PicoLlmDeltaFn on_delta, void *user, PicoResponsesCtx *ctx);
void pico_responses_fill_result(PicoResponsesCtx *c, PicoLlmResult *out);
void pico_responses_ctx_free(PicoResponsesCtx *c);

#endif
