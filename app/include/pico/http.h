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

int pico_http_post_sse(const PicoHttpPost *req, long *out_http, char **out_error);

#endif
