#ifndef PICO_HTTP_INTERNAL_H
#define PICO_HTTP_INTERNAL_H
#include <stdbool.h>
#include "pico/http.h"
/* Register a host after curl_global_init succeeds. */
void PicoHttp_HostStarted(void);
/* Close idle handles when the last host exits, before curl_global_cleanup. */
/* False means a caller still owns a live transfer; defer curl global cleanup. */
bool PicoHttp_ShutdownConnections(void);
/* Private worker helper for non-idempotent, potentially long-running operations.
 * One attempt, no redirects, bounded response bytes, and caller-selected total
 * timeout. HTTP errors remain completed transfers; cancellation stays cooperative. */
int PicoHttp_PostOnce(const PicoHttpReq *req, long timeout_seconds, size_t max_body,
                      long *out_http, char **out_body, char **out_error);
#endif
