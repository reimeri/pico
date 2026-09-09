#define _POSIX_C_SOURCE 200809L
#include "pico/http.h"
#include <arpa/inet.h>
#include <curl/curl.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

typedef struct Server { int fd; const char *type; const char *body; const char *header; } Server;
static void *Serve(void *arg)
{
    Server *s = arg;
    int fd = accept(s->fd, NULL, NULL);
    if (fd < 0) return NULL;
    char request[4096] = {0};
    size_t len = 0;
    while (len + 1 < sizeof(request))
    {
        ssize_t n = read(fd, request + len, sizeof(request) - len - 1);
        if (n <= 0) break;
        len += (size_t)n;
        request[len] = 0;
        char *end = strstr(request, "\r\n\r\n");
        if (end && (strncmp(request, "GET ", 4) == 0 || len >= (size_t)(end - request) + 6)) break;
    }
    char header[512];
    int n = snprintf(header, sizeof(header), "HTTP/1.1 200 OK\r\n%s%s%sContent-Length: %zu\r\nConnection: close\r\n\r\n",
                     s->type ? "Content-Type: " : "", s->type ? s->type : "",
                     s->type ? "\r\n" : "", strlen(s->body));
    if (s->header) send(fd, s->header, strlen(s->header), MSG_NOSIGNAL);
    else send(fd, header, (size_t)n, MSG_NOSIGNAL);
    /* A CR/LF pair can cross any transport boundary. */
    for (const char *p = s->body; *p; p++) send(fd, p, 1, MSG_NOSIGNAL);
    close(fd);
    return NULL;
}
typedef struct Result { int count; bool abort; char json[3][256]; char event[3][64]; } Result;
static bool Got(void *arg, const char *event, const char *json, size_t len)
{
    Result *r = arg;
    if (r->count < 3)
    {
        snprintf(r->json[r->count], 256, "%.*s", (int)len, json);
        snprintf(r->event[r->count], 64, "%s", event ? event : "");
    }
    r->count++;
    return !r->abort;
}
static int Request(const char *type, const char *body, Result *r)
{
    Server s = {.fd = socket(AF_INET, SOCK_STREAM, 0), .type = type, .body = body};
    struct sockaddr_in addr = {.sin_family = AF_INET, .sin_addr.s_addr = htonl(INADDR_LOOPBACK)};
    socklen_t size = sizeof(addr);
    if (s.fd < 0 || bind(s.fd, (void *)&addr, sizeof(addr)) || listen(s.fd, 1) ||
        getsockname(s.fd, (void *)&addr, &size)) return 1;
    pthread_t thread;
    if (pthread_create(&thread, NULL, Serve, &s)) { close(s.fd); return 1; }
    char url[128];
    snprintf(url, sizeof(url), "http://127.0.0.1:%u/", ntohs(addr.sin_port));
    PicoHttpPost req = {.url = url, .body = "{}", .on_json = Got, .user = r};
    long http;
    int result = pico_http_post_sse(&req, &http, NULL);
    pthread_join(thread, NULL);
    close(s.fd);
    return result != PICO_HTTP_OK || http != 200;
}
typedef struct RetryResult {
    Server server;
    pthread_t thread;
    int waits, attempts, last_delay;
    bool recover, cancel, cancelled, invalid;
} RetryResult;

static void Retrying(void *user, int retry, int max_retries, int delay, const char *error)
{
    RetryResult *r = user;
    r->invalid |= retry < 1 || retry > max_retries || !error || !error[0];
    if (!delay) { r->attempts++; return; }
    r->invalid |= delay <= r->last_delay;
    r->last_delay = delay;
    r->waits++;
    if (r->cancel) r->cancelled = true;
    if (r->recover && r->waits == 1)
    {
        r->invalid |= listen(r->server.fd, 1) != 0;
        r->invalid |= pthread_create(&r->thread, NULL, Serve, &r->server) != 0;
    }
}
static bool RetryCancelled(void *user) { return ((RetryResult *)user)->cancelled; }

static int RetryRequest(bool sse, bool recover, bool cancel)
{
    RetryResult r = {.recover = recover, .cancel = cancel,
                     .server = {.fd = socket(AF_INET, SOCK_STREAM, 0),
                                .type = "application/json", .body = "{}"}};
    struct sockaddr_in addr = {.sin_family = AF_INET, .sin_addr.s_addr = htonl(INADDR_LOOPBACK)};
    socklen_t size = sizeof(addr);
    if (r.server.fd < 0 || bind(r.server.fd, (void *)&addr, size) ||
        getsockname(r.server.fd, (void *)&addr, &size)) return 1;
    /* Bound but not listening: first connection is refused. The retry callback
     * brings the server online, without relying on wall-clock scheduling. */
    char url[128];
    snprintf(url, sizeof(url), "http://127.0.0.1:%u/", ntohs(addr.sin_port));
    long http = 0;
    char *error = NULL, *body = NULL;
    int rc;
    if (sse)
    {
        PicoHttpPost req = {.url = url, .body = "{}", .on_retry = Retrying,
                            .cancel = RetryCancelled, .user = &r};
        rc = pico_http_post_sse(&req, &http, &error);
    }
    else
    {
        PicoHttpReq req = {.url = url, .on_retry = Retrying, .cancel = RetryCancelled, .user = &r};
        rc = pico_http_get(&req, &http, &body, &error);
    }
    if (recover) pthread_join(r.thread, NULL);
    close(r.server.fd);
    int fail = r.invalid;
    if (cancel) fail |= rc != PICO_HTTP_CANCEL || r.waits != 1 || r.attempts != 0;
    else if (recover) fail |= rc != PICO_HTTP_OK || http != 200 || r.waits != 1 || r.attempts != 1;
    else fail |= rc != PICO_HTTP_FAIL || !error || r.waits != 5 || r.attempts != r.waits;
    if (fail) fprintf(stderr, "retry sse=%d recover=%d cancel=%d rc=%d http=%ld waits=%d attempts=%d invalid=%d error=%s\n", sse, recover, cancel, rc, http, r.waits, r.attempts, r.invalid, error ? error : "");
    free(error);
    free(body);
    return fail;
}

static void *RejectTls(void *user)
{
    Server *s = user;
    int fd = accept(s->fd, NULL, NULL);
    if (fd >= 0)
    {
        char hello[4096];
        ssize_t n = read(fd, hello, sizeof(hello));
        (void)n;
        const char reply[] = "HTTP/1.1 400 Bad Request\r\n\r\n";
        send(fd, reply, sizeof(reply) - 1, MSG_NOSIGNAL);
        close(fd);
    }
    return NULL;
}

static int RetryTlsHandshake(void)
{
    RetryResult r = {.cancel = true, .server = {.fd = socket(AF_INET, SOCK_STREAM, 0)}};
    struct sockaddr_in addr = {.sin_family = AF_INET, .sin_addr.s_addr = htonl(INADDR_LOOPBACK)};
    socklen_t size = sizeof(addr);
    if (r.server.fd < 0 || bind(r.server.fd, (void *)&addr, size) || listen(r.server.fd, 1) ||
        getsockname(r.server.fd, (void *)&addr, &size)) return 1;
    if (pthread_create(&r.thread, NULL, RejectTls, &r.server)) { close(r.server.fd); return 1; }
    char url[128];
    snprintf(url, sizeof(url), "https://127.0.0.1:%u/", ntohs(addr.sin_port));
    PicoHttpPost req = {.url = url, .body = "{}", .on_retry = Retrying,
                        .cancel = RetryCancelled, .user = &r};
    int rc = pico_http_post_sse(&req, NULL, NULL);
    pthread_join(r.thread, NULL);
    close(r.server.fd);
    return rc != PICO_HTTP_CANCEL || r.waits != 1 || r.invalid;
}

static int NoReplayAfterHeaders(void)
{
    RetryResult r = {.server = {.fd = socket(AF_INET, SOCK_STREAM, 0), .body = ""}};
    struct sockaddr_in addr = {.sin_family = AF_INET, .sin_addr.s_addr = htonl(INADDR_LOOPBACK)};
    socklen_t size = sizeof(addr);
    int closed = socket(AF_INET, SOCK_STREAM, 0);
    if (closed < 0 || bind(closed, (void *)&addr, size) || getsockname(closed, (void *)&addr, &size)) return 1;
    char header[256];
    snprintf(header, sizeof(header), "HTTP/1.1 302 Found\r\nLocation: http://127.0.0.1:%u/\r\nContent-Length: 0\r\nConnection: close\r\n\r\n", ntohs(addr.sin_port));
    r.server.header = header;
    addr.sin_port = 0;
    if (r.server.fd < 0 || bind(r.server.fd, (void *)&addr, size) || listen(r.server.fd, 1) ||
        getsockname(r.server.fd, (void *)&addr, &size)) return 1;
    pthread_create(&r.thread, NULL, Serve, &r.server);
    char url[128];
    snprintf(url, sizeof(url), "http://127.0.0.1:%u/", ntohs(addr.sin_port));
    PicoHttpPost req = {.url = url, .body = "{}", .on_retry = Retrying, .user = &r};
    int rc = pico_http_post_sse(&req, NULL, NULL);
    pthread_join(r.thread, NULL);
    close(r.server.fd);
    close(closed);
    return rc != PICO_HTTP_FAIL || r.waits != 0;
}

int main(void)
{
    setenv("NO_PROXY", "127.0.0.1", 1);
    curl_global_init(CURL_GLOBAL_DEFAULT);
    Result json = {0}, sse = {0}, stopped = {.abort = true};
    const char *error = "{\"error\":\"invalid data: bad event: request\"}";
    const char *events = "event: first\r\ndata: {\"a\":1}\r\n\r\ndata: {\"b\":\n"
                         "data: 2}\n\ndata: {\"c\":3}\r\r";
    int fail = Request("application/json", error, &json) || json.count != 1 || strcmp(json.json[0], error);
    fail |= Request("text/event-stream; charset=utf-8", events, &sse) || sse.count != 3 ||
            strcmp(sse.json[0], "{\"a\":1}") || strcmp(sse.event[0], "first") ||
            strcmp(sse.json[1], "{\"b\":\n2}") || strcmp(sse.json[2], "{\"c\":3}");
    fail |= Request("text/event-stream", events, &stopped) || stopped.count != 1;
    Result untyped = {0}, untyped_json = {0};
    fail |= Request(NULL, events, &untyped) || untyped.count != 3 ||
            strcmp(untyped.event[0], "first") || strcmp(untyped.json[0], "{\"a\":1}") ||
            strcmp(untyped.json[1], "{\"b\":\n2}") || strcmp(untyped.json[2], "{\"c\":3}");
    fail |= Request(NULL, error, &untyped_json) || untyped_json.count != 1 ||
            strcmp(untyped_json.json[0], error);
    Result explicit_json = {0};
    const char *sse_body = "data: {}\n\n";
    fail |= Request("application/json", sse_body, &explicit_json) || explicit_json.count != 1 ||
            strcmp(explicit_json.json[0], sse_body);
    fail |= RetryRequest(true, true, false);
    fail |= RetryRequest(false, true, false);
    fail |= RetryRequest(true, false, true);
    fail |= RetryRequest(false, false, false);
    fail |= NoReplayAfterHeaders();
    fail |= RetryTlsHandshake();
    curl_global_cleanup();
    if (fail) fprintf(stderr, "HTTP framing or callback cancellation failed\n");
    return fail;
}
