#ifndef PICO_PROVIDER_RETRY_H
#define PICO_PROVIDER_RETRY_H

#include "pico/app.h"
#include "json.h"

/* One logical request, shared across auth refresh and request-shape fallbacks.
 * The initial attempt is not timed by this budget; first recovery starts it. */
typedef struct PicoProviderRetry {
    double deadline;
    double retry_after;
    unsigned attempts;
    unsigned jitter;
    PicoLlmCancelFn cancel;
    PicoLlmDeltaFn on_delta;
    void *user;
} PicoProviderRetry;

#define PICO_PROVIDER_RECOVERY_ERROR "Automatic recovery timed out; the last model request could not be completed. Completed tool work is preserved."
bool pico_provider_capacity_error(const JsonDoc *doc, int error);
bool pico_provider_capacity_message(const char *message);
bool pico_provider_permanent_error(const JsonDoc *doc, int error);
bool pico_provider_retry_http(long status);
bool pico_provider_retry_expired(const PicoProviderRetry *retry);
bool pico_provider_retry_cancel(void *user);
void pico_provider_retry_delta(void *user, const PicoLlmDelta *delta);
void pico_provider_retry_headers(PicoProviderRetry *retry, long status, const char *headers, size_t len);
/* Return OK to retry, FAIL on exhaustion, CANCEL only on user cancellation. */
int pico_provider_retry_wait(PicoProviderRetry *retry);
#endif
