#define _POSIX_C_SOURCE 200809L
#include "builtins/openai_auth.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <unistd.h>

static int failed;
static void Check(bool ok, const char *message)
{
    if (!ok) { fprintf(stderr, "FAIL: %s\n", message); failed = 1; }
}

#ifdef PICO_OPENAI_RANDOM_FAULT_TESTS
static bool random_failure;
int __real_RAND_bytes(unsigned char *buf, int n);
int __wrap_RAND_bytes(unsigned char *buf, int n)
{
    return random_failure ? 0 : __real_RAND_bytes(buf, n);
}
#endif

static void TestPkce(void)
{
    char challenge[44];
    Check(pico_openai_pkce_challenge("dBjftJeZ4CVP-mB92K27uhbUJU1p1r_wW1gFWFOEjXk", challenge) &&
          strcmp(challenge, "E9Melhoa2OwvFrEMTJguCHaoeK1t8URWbuGJSstw-cM") == 0,
          "PKCE S256 interoperates with the RFC 7636 example");
    PicoOpenAiPkce pkce;
    Check(pico_openai_pkce_generate(&pkce), "secure PKCE generation succeeds");
    char *url = pico_openai_authorize_url("https://auth.openai.com", "test-client",
                                          "http://localhost:1457/auth/callback", &pkce);
    char *form = pico_openai_code_form("test-client", "http://localhost:1457/auth/callback", "a+b&c", pkce.verifier);
    Check(url && form && strstr(url, "redirect_uri=http%3A%2F%2Flocalhost%3A1457%2Fauth%2Fcallback") &&
          strstr(form, "redirect_uri=http%3A%2F%2Flocalhost%3A1457%2Fauth%2Fcallback") &&
          strstr(url, "code_challenge_method=S256") && strstr(url, pkce.challenge) && strstr(url, pkce.state) &&
          !strstr(url, pkce.verifier) && strstr(form, "code=a%2Bb%26c") && strstr(form, pkce.verifier),
          "authorization and exchange agree on redirect, encode values, and keep verifier out of browser URL");
    free(url);
    free(form);
#ifdef PICO_OPENAI_RANDOM_FAULT_TESTS
    random_failure = true;
    Check(!pico_openai_pkce_generate(&pkce) && !pkce.verifier[0] && !pkce.state[0],
          "randomness failure cannot produce usable OAuth material");
    random_failure = false;
#endif
}

typedef struct CallbackFixture {
    int listener;
    unsigned short port;
    int wake[2];
    double deadline;
    pthread_t thread;
    char *code;
    PicoOpenAiCallback result;
} CallbackFixture;

static void *Await(void *user)
{
    CallbackFixture *f = user;
    f->result = pico_openai_await_callback(f->listener, f->wake[0], f->deadline,
                                           "expected-state", NULL, NULL, &f->code);
    close(f->listener);
    return NULL;
}

static bool Start(CallbackFixture *f, double timeout)
{
    memset(f, 0, sizeof(*f));
    unsigned short ports[] = {0};
    f->listener = pico_openai_listen(ports, 1, &f->port);
    if (f->listener < 0) return false;
    if (!pico_openai_wake_pipe(f->wake)) { close(f->listener); return false; }
    f->deadline = pico_openai_monotonic() + timeout;
    if (pthread_create(&f->thread, NULL, Await, f) != 0)
    {
        close(f->listener); close(f->wake[0]); close(f->wake[1]);
        return false;
    }
    return true;
}

static void Finish(CallbackFixture *f)
{
    pthread_join(f->thread, NULL);
    close(f->wake[0]); close(f->wake[1]);
}

static int Connect(unsigned short port)
{
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in addr = {.sin_family = AF_INET, .sin_port = htons(port),
                               .sin_addr = {.s_addr = htonl(INADDR_LOOPBACK)}};
    if (fd < 0) return -1;
    struct timeval timeout = {.tv_sec = 3};
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) { close(fd); return -1; }
    return fd;
}

static bool Request(unsigned short port, const char *request, const char *expected)
{
    int fd = Connect(port);
    if (fd < 0) return false;
    size_t len = strlen(request), sent = 0;
    while (sent < len)
    {
        ssize_t n = send(fd, request + sent, len - sent, MSG_NOSIGNAL);
        if (n <= 0) break;
        sent += (size_t)n;
    }
    char response[2048];
    size_t used = 0;
    while (used + 1 < sizeof(response))
    {
        ssize_t n = recv(fd, response + used, sizeof(response) - used - 1, 0);
        if (n <= 0) break;
        used += (size_t)n;
    }
    response[used] = 0;
    close(fd);
    return strstr(response, expected) != NULL && !strstr(response, "secret-code");
}

static void TestCallbackValidation(void)
{
    CallbackFixture f;
    if (!Start(&f, 10)) { Check(false, "start callback fixture"); return; }
    const char *bad[] = {
        "GET /favicon.ico HTTP/1.1\r\n\r\n",
        "POST /auth/callback?state=expected-state&code=x HTTP/1.1\r\n\r\n",
        "GET /auth/callback?state=wrong&code=x HTTP/1.1\r\n\r\n",
        "GET /auth/callback?error=access_denied HTTP/1.1\r\n\r\n",
        "GET /auth/callback?state=expected-state&state=expected-state&code=x HTTP/1.1\r\n\r\n",
        "GET /auth/callback?state=expected-state&code=x&error=denied HTTP/1.1\r\n\r\n",
        "GET /auth/callback?state=expected-state&code=%00secret-code HTTP/1.1\r\n\r\n",
        "GET /auth/callback?state=expected-state&code=%GG HTTP/1.1\r\n\r\n",
    };
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++)
        Check(Request(f.port, bad[i], "400 Bad Request"), "invalid callback rejected without completing login");
    Check(Request(f.port, "GET /auth/callback?state=expected-state&code=secret-code%2B%26 HTTP/1.1\r\nHost: localhost\r\n\r\n",
                  "Sign-in received"), "valid callback acknowledged without exposing code");
    Finish(&f);
    Check(f.result == PICO_OPENAI_CALLBACK_CODE && f.code && strcmp(f.code, "secret-code+&") == 0,
          "validated callback yields strictly decoded authorization code");
    free(f.code);
    int retry = Connect(f.port);
    Check(retry < 0, "callback listener stops after completion");
    if (retry >= 0) close(retry);
}

static void TestDenied(void)
{
    CallbackFixture f;
    if (!Start(&f, 5)) { Check(false, "start denial fixture"); return; }
    Check(Request(f.port, "GET /auth/callback?state=expected-state&error=access_denied&error_description=%3Cscript%3E HTTP/1.1\r\n\r\n",
                  "Sign-in was not authorized"), "provider denial returns a static response");
    Finish(&f);
    Check(f.result == PICO_OPENAI_CALLBACK_DENIED && !f.code, "validated provider denial produces no code");
}

static void TestCancellation(bool partial_client)
{
    CallbackFixture f;
    if (!Start(&f, 10)) { Check(false, "start cancellation fixture"); return; }
    int client = -1;
    if (partial_client)
    {
        client = Connect(f.port);
        if (client >= 0) (void)send(client, "GET /auth", 9, MSG_NOSIGNAL);
    }
    double begin = pico_openai_monotonic();
    Check(write(f.wake[1], "x", 1) == 1, "signal cancellation");
    Finish(&f);
    Check(f.result == PICO_OPENAI_CALLBACK_CANCELLED && pico_openai_monotonic() - begin < 1.0,
          "cancellation promptly interrupts both accept and partial request waits");
    if (client >= 0) close(client);
    unsigned short port;
    int next = pico_openai_listen(&f.port, 1, &port);
    Check(next >= 0, "cancelled listener releases its port");
    if (next >= 0) close(next);
}

static void TestTimeout(void)
{
    CallbackFixture f;
    if (!Start(&f, 0.05)) { Check(false, "start timeout fixture"); return; }
    Finish(&f);
    Check(f.result == PICO_OPENAI_CALLBACK_TIMEOUT, "uncompleted login expires");
}

static void TestPortSelection(void)
{
    unsigned short ports[] = {0, 0};
    unsigned short occupied_port, fallback_port;
    int occupied = pico_openai_listen(ports, 1, &occupied_port);
    if (occupied < 0) { Check(false, "reserve occupied port"); return; }
    ports[0] = occupied_port;
    int fallback = pico_openai_listen(ports, 2, &fallback_port);
    Check(fallback >= 0 && fallback_port != occupied_port, "occupied preferred port selects next candidate");
    if (fallback >= 0)
    {
        ports[1] = fallback_port;
        unsigned short unused;
        int none = pico_openai_listen(ports, 2, &unused);
        Check(none < 0 && errno == EADDRINUSE, "all occupied ports fail cleanly");
        if (none >= 0) close(none);
        /* No requests, including /cancel, may be sent to either occupant. */
        int unexpected = accept(occupied, NULL, NULL);
        Check(unexpected < 0 && (errno == EAGAIN || errno == EWOULDBLOCK), "port selection does not contact occupant");
        if (unexpected >= 0) close(unexpected);
        CallbackFixture f = {.listener = fallback, .port = fallback_port,
                              .deadline = pico_openai_monotonic() + 5};
        if (pico_openai_wake_pipe(f.wake) && pthread_create(&f.thread, NULL, Await, &f) == 0)
        {
            Check(Request(f.port, "GET /auth/callback?state=expected-state&code=fallback HTTP/1.1\r\n\r\n",
                          "200 OK"), "selected fallback uses the same callback flow");
            Finish(&f);
            Check(f.result == PICO_OPENAI_CALLBACK_CODE, "fallback completes login");
            free(f.code);
        }
        else { Check(false, "start fallback fixture"); close(fallback); }
    }
    close(occupied);
}

int main(void)
{
    TestPkce();
    TestPortSelection();
    TestCallbackValidation();
    TestDenied();
    TestCancellation(false);
    TestCancellation(true);
    TestTimeout();
    return failed;
}
