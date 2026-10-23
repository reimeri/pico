#ifndef PICO_PROVIDER_RETRY_H
#define PICO_PROVIDER_RETRY_H

#include "pico/app.h"
#include "json.h"

#define PICO_PROVIDER_MAX_RETRIES 5

/* Inspect an error object/string, not arbitrary successful response content. */
bool pico_provider_capacity_error(const JsonDoc *doc, int error);
bool pico_provider_capacity_message(const char *message);
/* Discard the current attempt, publish activity, and wait cancellably. */
bool pico_provider_retry_wait(int retry, PicoLlmCancelFn cancel,
                              PicoLlmDeltaFn on_delta, void *user);

#endif
