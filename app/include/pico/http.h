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

typedef struct PicoHttpPost {
    const char *url;
    const char *body;
    const char *headers[PICO_HTTP_MAX_HEADERS];
    int header_count;
    PicoHttpCancelFn cancel;
    PicoHttpJsonFn on_json;
    void *user; /* passed to both callbacks */
} PicoHttpPost;

/* Buffered JSON/form request. `body` may be empty; GET ignores it. Header strings and
 * callback data must remain valid until the blocking call returns. Requests follow
 * redirects and time out after 30 seconds. */
typedef struct PicoHttpReq {
    const char *url;
    const char *body;
    const char *headers[PICO_HTTP_MAX_HEADERS];
    int header_count;
    PicoHttpCancelFn cancel;
    void *user;
} PicoHttpReq;

/* HTTP status is reported through `out_http`; a completed 4xx/5xx transfer still
 * returns PICO_HTTP_OK. When set, `out_body` and `out_error` are malloc'd and caller-owned.
 * Cancellation returns PICO_HTTP_CANCEL. SSE requests follow redirects and time out after
 * 300 seconds; buffered GET/POST requests time out after 30 seconds. */
int pico_http_post_sse(const PicoHttpPost *req, long *out_http, char **out_error);
int pico_http_post(const PicoHttpReq *req, long *out_http, char **out_body, char **out_error);
int pico_http_get(const PicoHttpReq *req, long *out_http, char **out_body, char **out_error);
char *pico_http_form_encode(const char *const *keys, const char *const *vals, int n);

#endif
