#ifndef PICO_OPENAI_AUTH_H
#define PICO_OPENAI_AUTH_H

/* Private builtin helpers, not an extension API. */
#include <stdbool.h>
#include <stddef.h>
#include <sys/types.h>

#include "pico/http.h"

typedef struct PicoOpenAiPkce {
    char verifier[87];
    char challenge[44];
    char state[44];
} PicoOpenAiPkce;

typedef enum PicoOpenAiCallback {
    PICO_OPENAI_CALLBACK_CODE,
    PICO_OPENAI_CALLBACK_DENIED,
    PICO_OPENAI_CALLBACK_CANCELLED,
    PICO_OPENAI_CALLBACK_TIMEOUT,
    PICO_OPENAI_CALLBACK_FAILED,
} PicoOpenAiCallback;

bool pico_openai_pkce_challenge(const char *verifier, char out[44]);
bool pico_openai_pkce_generate(PicoOpenAiPkce *out);
/* Returned forms/URLs are malloc'd. The same redirect must be used for both. */
char *pico_openai_authorize_url(const char *issuer, const char *client_id,
                               const char *redirect, const PicoOpenAiPkce *pkce);
char *pico_openai_code_form(const char *client_id, const char *redirect,
                           const char *code, const char *verifier);

/* Bind only IPv4 loopback; advance through candidates ONLY on EADDRINUSE.
 * Caller owns the returned nonblocking, close-on-exec descriptor. */
int pico_openai_listen(const unsigned short *ports, size_t count, unsigned short *port);
bool pico_openai_wake_pipe(int fds[2]);
double pico_openai_monotonic(void);
/* A validated callback consumes the attempt. Invalid requests leave it pending.
 * Neither descriptor is closed here. `code` is malloc'd on CODE only. */
PicoOpenAiCallback pico_openai_await_callback(int listener, int wake_fd, double deadline,
                                             const char *state, PicoHttpCancelFn cancel,
                                             void *user, char **code);

#endif
