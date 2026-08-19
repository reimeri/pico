#ifndef PICO_LLM_H
#define PICO_LLM_H

#include <stdbool.h>
#include <stddef.h>

typedef bool (*PicoLlmCancelFn)(void *user);

typedef enum PicoLlmDeltaKind {
    PICO_LLM_DELTA_TEXT = 0,
    PICO_LLM_DELTA_THINKING,
    PICO_LLM_DELTA_STATUS,
} PicoLlmDeltaKind;

typedef void (*PicoLlmDeltaFn)(void *user, PicoLlmDeltaKind kind, const char *s, size_t n);

enum {
    PICO_LLM_OK = 0,
    PICO_LLM_FAIL = 1,
    PICO_LLM_CANCEL = 2,
};

typedef struct PicoLlmUsage {
    int input_tokens;
    int cached_tokens;
} PicoLlmUsage;

void PicoLlm_ResolveUrl(const char *base, char *out, size_t cap);
int PicoLlm_Stream(const char *url, const char *api_key, const char *body, const char *session_id,
                   PicoLlmCancelFn cancel, PicoLlmDeltaFn on_delta, void *user, char **out_items_json,
                   PicoLlmUsage *out_usage, char **out_error);

#endif
