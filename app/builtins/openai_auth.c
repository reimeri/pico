#define _GNU_SOURCE

#include "builtins/openai_auth.h"
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <openssl/rand.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

static void Base64Url(const unsigned char *bytes, int length, char *out)
{
    EVP_EncodeBlock((unsigned char *)out, bytes, length);
    for (char *p = out; *p; p++)
    {
        if (*p == '+') *p = '-';
        else if (*p == '/') *p = '_';
        else if (*p == '=') { *p = '\0'; break; }
    }
}

bool pico_openai_pkce_challenge(const char *verifier, char out[44])
{
    unsigned char digest[EVP_MAX_MD_SIZE];
    unsigned int n = 0;
    char encoded[45];
    if (!verifier || !EVP_Digest(verifier, strlen(verifier), digest, &n, EVP_sha256(), NULL) || n != 32)
    {
        out[0] = '\0';
        return false;
    }
    Base64Url(digest, (int)n, encoded);
    memcpy(out, encoded, 44);
    return true;
}

bool pico_openai_pkce_generate(PicoOpenAiPkce *out)
{
    unsigned char random[96];
    char verifier[89], state[45];
    memset(out, 0, sizeof(*out));
    if (RAND_bytes(random, sizeof(random)) != 1)
    {
        OPENSSL_cleanse(random, sizeof(random));
        return false;
    }
    Base64Url(random, 64, verifier);
    Base64Url(random + 64, 32, state);
    memcpy(out->verifier, verifier, sizeof(out->verifier));
    memcpy(out->state, state, sizeof(out->state));
    OPENSSL_cleanse(random, sizeof(random));
    OPENSSL_cleanse(verifier, sizeof(verifier));
    if (!pico_openai_pkce_challenge(out->verifier, out->challenge))
    {
        OPENSSL_cleanse(out, sizeof(*out));
        return false;
    }
    return true;
}

char *pico_openai_authorize_url(const char *issuer, const char *client_id,
                               const char *redirect, const PicoOpenAiPkce *pkce)
{
    const char *keys[] = {"response_type", "client_id", "redirect_uri", "scope", "code_challenge",
                          "code_challenge_method", "state", "id_token_add_organizations",
                          "codex_cli_simplified_flow", "originator"};
    const char *vals[] = {"code", client_id, redirect, "openid profile email offline_access", pkce->challenge,
                          "S256", pkce->state, "true", "true", "pico"};
    char *query = pico_http_form_encode(keys, vals, 10);
    if (!query) return NULL;
    size_t cap = strlen(issuer) + strlen(query) + sizeof("/oauth/authorize?");
    char *url = malloc(cap);
    if (url) snprintf(url, cap, "%s/oauth/authorize?%s", issuer, query);
    free(query);
    return url;
}

char *pico_openai_code_form(const char *client_id, const char *redirect,
                           const char *code, const char *verifier)
{
    const char *keys[] = {"grant_type", "code", "redirect_uri", "client_id", "code_verifier"};
    const char *vals[] = {"authorization_code", code, redirect, client_id, verifier};
    return pico_http_form_encode(keys, vals, 5);
}

bool pico_openai_wake_pipe(int fds[2])
{
    return pipe2(fds, O_NONBLOCK | O_CLOEXEC) == 0;
}

int pico_openai_listen(const unsigned short *ports, size_t count, unsigned short *port)
{
    for (size_t i = 0; i < count; i++)
    {
        int fd = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
        if (fd < 0) return -1;
        int reuse = 1;
        struct sockaddr_in address = {.sin_family = AF_INET, .sin_port = htons(ports[i]),
                                      .sin_addr = {.s_addr = htonl(INADDR_LOOPBACK)}};
        if (setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse)) != 0)
        {
            int error = errno;
            close(fd);
            errno = error;
            return -1;
        }
        if (bind(fd, (struct sockaddr *)&address, sizeof(address)) != 0)
        {
            int error = errno;
            close(fd);
            if (error == EADDRINUSE && i + 1 < count) continue;
            errno = error;
            return -1;
        }
        socklen_t len = sizeof(address);
        if (listen(fd, 8) != 0 || getsockname(fd, (struct sockaddr *)&address, &len) != 0)
        {
            int error = errno;
            close(fd);
            errno = error;
            return -1;
        }
        *port = ntohs(address.sin_port);
        return fd;
    }
    errno = EINVAL;
    return -1;
}

double pico_openai_monotonic(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

/* 1 ready, 0 deadline, -1 cancelled, -2 I/O failure. */
static int WaitFd(int fd, short events, int wake_fd, double deadline,
                  PicoHttpCancelFn cancel, void *user)
{
    for (;;)
    {
        if (cancel && cancel(user)) return -1;
        double remaining = deadline - pico_openai_monotonic();
        if (remaining <= 0) return 0;
        struct pollfd fds[2] = {{.fd = fd, .events = events}, {.fd = wake_fd, .events = POLLIN}};
        int ms = remaining >= 0.1 ? 100 : (int)(remaining * 1000) + 1;
        int rc = poll(fds, 2, ms);
        if (rc < 0)
        {
            if (errno == EINTR) continue;
            return -2;
        }
        if (fds[1].revents) return -1;
        if (fds[0].revents & events) return 1;
        if (fds[0].revents & (POLLERR | POLLHUP | POLLNVAL)) return -2;
    }
}

static int Hex(unsigned char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    return -1;
}

static bool Decode(char *text)
{
    unsigned char *src = (unsigned char *)text, *dst = src;
    while (*src)
    {
        unsigned char c = *src++;
        if (c == '%')
        {
            if (!src[0] || !src[1]) return false;
            int a = Hex(src[0]), b = Hex(src[1]);
            if (a < 0 || b < 0) return false;
            c = (unsigned char)(16 * a + b);
            src += 2;
        }
        else if (c == '+') c = ' ';
        if (c < 32 || c == 127) return false;
        *dst++ = c;
    }
    *dst = 0;
    return true;
}

/* Returns 0 for invalid, 1 for code, 2 for provider denial. */
static int ParseCallback(char *request, const char *expected_state, char **out_code)
{
    char *line_end = strstr(request, "\r\n");
    if (!line_end) return 0;
    *line_end = 0;
    if (strncmp(request, "GET /auth/callback?", 19) != 0) return 0;
    char *query = request + 19;
    char *version = strchr(query, ' ');
    if (!version || (strcmp(version, " HTTP/1.1") && strcmp(version, " HTTP/1.0"))) return 0;
    *version = 0;
    char *state = NULL, *code = NULL, *error = NULL;
    char *part = query;
    while (part)
    {
        char *next = strchr(part, '&');
        if (next) *next++ = 0;
        char *value = strchr(part, '=');
        if (!value) return 0;
        *value++ = 0;
        if (!Decode(part) || !Decode(value)) return 0;
        char **field = NULL;
        if (strcmp(part, "state") == 0) field = &state;
        else if (strcmp(part, "code") == 0) field = &code;
        else if (strcmp(part, "error") == 0) field = &error;
        if (field)
        {
            if (*field || !value[0] || strlen(value) > 4096) return 0;
            *field = value;
        }
        part = next;
    }
    if (!state || strlen(state) != strlen(expected_state) ||
        CRYPTO_memcmp(state, expected_state, strlen(state)) != 0 || (!code == !error)) return 0;
    if (error) return 2;
    *out_code = strdup(code);
    return *out_code ? 1 : 0;
}

static void Reply(int fd, int wake_fd, double deadline, PicoHttpCancelFn cancel, void *user, int result)
{
    const char *body = result == 1 ? "Sign-in received. Return to Pico to finish signing in.\n" :
                       result == 2 ? "Sign-in was not authorized. Return to Pico.\n" :
                                     "Invalid callback. Continue signing in using the link in Pico.\n";
    char response[768];
    int n = snprintf(response, sizeof(response),
                     "HTTP/1.1 %s\r\nContent-Type: text/plain; charset=utf-8\r\n"
                     "Content-Length: %zu\r\nCache-Control: no-store\r\n"
                     "Referrer-Policy: no-referrer\r\nContent-Security-Policy: default-src 'none'\r\n"
                     "Connection: close\r\n\r\n%s", result ? "200 OK" : "400 Bad Request", strlen(body), body);
    size_t sent = 0;
    while (sent < (size_t)n && WaitFd(fd, POLLOUT, wake_fd, deadline, cancel, user) == 1)
    {
        ssize_t count = send(fd, response + sent, (size_t)n - sent, MSG_NOSIGNAL);
        if (count > 0) sent += (size_t)count;
        else if (count < 0 && (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK)) continue;
        else break;
    }
}

PicoOpenAiCallback pico_openai_await_callback(int listener, int wake_fd, double deadline,
                                             const char *state, PicoHttpCancelFn cancel,
                                             void *user, char **code)
{
    *code = NULL;
    for (;;)
    {
        int ready = WaitFd(listener, POLLIN, wake_fd, deadline, cancel, user);
        if (ready <= 0)
            return ready == -1 ? PICO_OPENAI_CALLBACK_CANCELLED :
                   ready == 0 ? PICO_OPENAI_CALLBACK_TIMEOUT : PICO_OPENAI_CALLBACK_FAILED;
        int client = accept4(listener, NULL, NULL, SOCK_NONBLOCK | SOCK_CLOEXEC);
        if (client < 0)
        {
            if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK) continue;
            return PICO_OPENAI_CALLBACK_FAILED;
        }
        /* A stalled client cannot occupy the entire login deadline. */
        double slice = pico_openai_monotonic() + 1.0;
        if (slice > deadline) slice = deadline;
        char request[16384];
        size_t used = 0;
        bool complete = false;
        while (used + 1 < sizeof(request) && WaitFd(client, POLLIN, wake_fd, slice, cancel, user) == 1)
        {
            ssize_t n = recv(client, request + used, sizeof(request) - used - 1, 0);
            if (n < 0 && (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK)) continue;
            if (n <= 0) break;
            if (memchr(request + used, 0, (size_t)n)) break;
            used += (size_t)n;
            request[used] = 0;
            if (strstr(request, "\r\n\r\n")) { complete = true; break; }
        }
        int result = complete ? ParseCallback(request, state, code) : 0;
        Reply(client, wake_fd, slice, cancel, user, result);
        close(client);
        if (result) return result == 1 ? PICO_OPENAI_CALLBACK_CODE : PICO_OPENAI_CALLBACK_DENIED;
    }
}

