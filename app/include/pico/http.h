#ifndef PICO_HTTP_H
#define PICO_HTTP_H

#include <stdbool.h>
#include <stddef.h>

enum {
    PICO_HTTP_OK = 0,
    PICO_HTTP_FAIL = 1,
    PICO_HTTP_CANCEL = 2,
};

#define PICO_HTTP_MAX_HEADERS 16

typedef bool (*PicoHttpCancelFn)(void *user);
/* Return false to abort the transfer (e.g. a fatal JSON error event).
 * `event` is the SSE event name, or NULL for a bare JSON body. */
typedef bool (*PicoHttpJsonFn)(void *user, const char *event, const char *json, size_t len);

/* Called synchronously before a retry wait and again before its attempt (delay=0).
 * retry is 1-based; error is borrowed for the callback only. */
typedef void (*PicoHttpRetryFn)(void *user, int retry, int max_retries,
                                int delay_seconds, const char *error);

typedef struct PicoHttpPost {
    const char *url;
    const char *body;
    const char *headers[PICO_HTTP_MAX_HEADERS];
    int header_count;
    PicoHttpCancelFn cancel;
    PicoHttpRetryFn on_retry;
    PicoHttpJsonFn on_json;
    void *user; /* passed to all callbacks */
} PicoHttpPost;

/* Buffered JSON/form request. `body` may be empty; GET ignores it. Header strings and
 * callback data must remain valid until the blocking call returns. Requests follow
 * redirects and time out after 30 seconds per attempt. */
typedef struct PicoHttpReq {
    const char *url;
    const char *body;
    const char *headers[PICO_HTTP_MAX_HEADERS];
    int header_count;
    PicoHttpCancelFn cancel;
    PicoHttpRetryFn on_retry;
    void *user;
} PicoHttpReq;

/* HTTP status is reported through `out_http`; a completed 4xx/5xx transfer still
 * returns PICO_HTTP_OK. When set, `out_body` and `out_error` are malloc'd and caller-owned.
 * Cancellation returns PICO_HTTP_CANCEL. SSE requests follow redirects and time out after
 * 600 seconds per attempt; buffered GET/POST requests use 30 seconds per attempt.
 * Transient handshake/DNS/connect failures before any headers/body are retried up to
 * five times with cancellable 1/2/4/8/16-second waits. HTTP errors, certificate
 * verification failures, timeouts and partial responses are not retried. */
int pico_http_post_sse(const PicoHttpPost *req, long *out_http, char **out_error);
int pico_http_post(const PicoHttpReq *req, long *out_http, char **out_body, char **out_error);
int pico_http_get(const PicoHttpReq *req, long *out_http, char **out_body, char **out_error);
char *pico_http_form_encode(const char *const *keys, const char *const *vals, int n);

#endif
