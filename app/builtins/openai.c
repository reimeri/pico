#define _POSIX_C_SOURCE 200809L

#include "pico/plugin.h"
#include "pico/http.h"
#include "pico/auth.h"
#include "json.h"
#include "builtins/responses.h"
#include "builtins/openai_auth.h"
#include "host_internal.h"

#include <pthread.h>
#include <errno.h>
#include <openssl/crypto.h>
#include <poll.h>
#include <strings.h>
#include <limits.h>
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static const char kDefaultBase[] = "https://api.openai.com/v1";
static const char kCodexResponses[] = "https://chatgpt.com/backend-api/codex/responses";
static const char kClientId[] = "app_EMoamEEZ73f0CkXaXp7hrann";
static const char kIssuer[] = "https://auth.openai.com";

static bool FastAuthAvailable(const PicoAuthEntry *auth)
{
    if (strcmp(auth->active, PICO_AUTH_OAUTH) == 0)
        return (auth->access_token && auth->access_token[0]) ||
               (auth->refresh_token && auth->refresh_token[0]);
    return strcmp(auth->active, PICO_AUTH_API_KEY) == 0 && auth->api_key && auth->api_key[0];
}

static char *BuildRequest(const PicoLlmTurn *turn, bool codex)
{
    /* The ChatGPT Codex backend rejects stored responses, and with store off
     * the server only replays reasoning items that carry their encrypted
     * payload. Plain Responses endpoints (including OpenAI-compatible
     * gateways behind models[].base_url) may reject either field. */
    PicoResponsesBuildOpts opts = {
        .provider = "openai",
        .service_tier = pico_responses_openai_service_tier(turn, codex),
        .store_false = codex,
        .include_encrypted_reasoning = codex,
        .reasoning_summary_auto = true,
    };
    return pico_responses_build_request(turn, &opts);
}

/* A host-tracked task owns each attempt independently of extension generations.
 * Workers only touch their attempt; main-thread on_frame owns UI and auth writes. */
#define PICO_LOGIN_MAX_NOTES 8
#define PICO_LOGIN_TIMEOUT_SEC 900
#define PICO_DEVICE_MAX_TRANSPORT_FAILS 5

typedef struct LoginAttempt {
    pthread_mutex_t mu;
    unsigned refs; /* Host registration + core-owned task. Guarded by mu. */
    bool done;
    bool cancel;
    bool browser;
    double deadline;
    int wake[2];
    PicoAgentId agent_id;
    char *notes[PICO_LOGIN_MAX_NOTES];
    int note_count;
    char *authorize_url;
    char *token_body;
} LoginAttempt;

typedef struct HostAuthState {
    PicoHost *host;
    LoginAttempt *login; /* Main-thread-only; NULL invalidates all pending events. */
} HostAuthState;

static void FreeSecret(char *value)
{
    if (value) OPENSSL_cleanse(value, strlen(value));
    free(value);
}

static void ReleaseLogin(void *user)
{
    LoginAttempt *login = user;
    pthread_mutex_lock(&login->mu);
    bool last = --login->refs == 0;
    pthread_mutex_unlock(&login->mu);
    if (!last) return;
    for (int i = 0; i < login->note_count; i++) free(login->notes[i]);
    free(login->authorize_url);
    FreeSecret(login->token_body);
    close(login->wake[0]);
    close(login->wake[1]);
    pthread_mutex_destroy(&login->mu);
    free(login);
}

static void LoginNote(LoginAttempt *login, const char *text)
{
    if (!text || !text[0]) return;
    pthread_mutex_lock(&login->mu);
    if (!login->cancel && login->note_count < PICO_LOGIN_MAX_NOTES)
    {
        char *copy = JsonDup(text);
        if (copy) login->notes[login->note_count++] = copy;
    }
    pthread_mutex_unlock(&login->mu);
}

static bool LoginCancelled(void *user)
{
    LoginAttempt *login = user;
    pthread_mutex_lock(&login->mu);
    bool cancelled = login->cancel;
    pthread_mutex_unlock(&login->mu);
    return cancelled || pico_openai_monotonic() >= login->deadline;
}

static void CancelLogin(void *user)
{
    LoginAttempt *login = user;
    pthread_mutex_lock(&login->mu);
    login->cancel = true;
    /* Pipe stays alive until both task and registration release the attempt. */
    ssize_t rc;
    do { rc = write(login->wake[1], "x", 1); } while (rc < 0 && errno == EINTR);
    pthread_mutex_unlock(&login->mu);
}

static bool LoginSleep(LoginAttempt *login, int seconds)
{
    double until = pico_openai_monotonic() + seconds;
    while (!LoginCancelled(login))
    {
        double remaining = until - pico_openai_monotonic();
        if (remaining <= 0) return true;
        struct pollfd wake = {.fd = login->wake[0], .events = POLLIN};
        int rc = poll(&wake, 1, remaining >= 0.1 ? 100 : (int)(remaining * 1000) + 1);
        if (rc > 0 || (rc < 0 && errno != EINTR)) return false;
    }
    return false;
}

static void StopLogin(HostAuthState *s)
{
    LoginAttempt *login = s->login;
    s->login = NULL;
    if (!login) return;
    CancelLogin(login);
    ReleaseLogin(login);
}

static void Note(PicoHost *app, PicoAgentId agent_id, const char *text)
{
    PicoHost_AddMessage(app, agent_id, PICO_ROLE_ASSISTANT, text);
}

static int B64UrlVal(char c)
{
    if (c >= 'A' && c <= 'Z')
    {
        return c - 'A';
    }
    if (c >= 'a' && c <= 'z')
    {
        return c - 'a' + 26;
    }
    if (c >= '0' && c <= '9')
    {
        return c - '0' + 52;
    }
    if (c == '-')
    {
        return 62;
    }
    if (c == '_')
    {
        return 63;
    }
    return -1;
}

static char *B64UrlDecode(const char *s, size_t n)
{
    unsigned char *out = (unsigned char *)malloc(n + 1);
    size_t o = 0;
    int val = 0;
    int bits = 0;
    if (!out)
    {
        return NULL;
    }
    for (size_t i = 0; i < n; i++)
    {
        int d = B64UrlVal(s[i]);
        if (d < 0)
        {
            continue;
        }
        val = (val << 6) | d;
        bits += 6;
        if (bits >= 8)
        {
            bits -= 8;
            out[o++] = (unsigned char)((val >> bits) & 0xff);
        }
    }
    out[o] = 0;
    return (char *)out;
}

static char *AccountFromJwt(const char *jwt)
{
    if (!jwt || !jwt[0])
    {
        return NULL;
    }
    const char *dot1 = strchr(jwt, '.');
    if (!dot1)
    {
        return NULL;
    }
    const char *dot2 = strchr(dot1 + 1, '.');
    if (!dot2)
    {
        return NULL;
    }
    char *json = B64UrlDecode(dot1 + 1, (size_t)(dot2 - (dot1 + 1)));
    if (!json)
    {
        return NULL;
    }
    JsonDoc doc;
    char *id = NULL;
    if (JsonParse(&doc, json, strlen(json)) == 0)
    {
        id = JsonObjStr(&doc, 0, "chatgpt_account_id");
        if (!id || !id[0])
        {
            free(id);
            int auth = JsonObjGet(&doc, 0, "https://api.openai.com/auth");
            id = JsonIsObject(&doc, auth) ? JsonObjStr(&doc, auth, "chatgpt_account_id") : NULL;
        }
        if (!id || !id[0])
        {
            free(id);
            id = JsonObjStr(&doc, 0, "account_id");
        }
        JsonFree(&doc);
    }
    free(json);
    if (id && !id[0])
    {
        free(id);
        id = NULL;
    }
    return id;
}

static char *PickAccountId(const char *access, const char *id_token, const char *fallback)
{
    char *id = AccountFromJwt(id_token);
    if (id)
    {
        return id;
    }
    id = AccountFromJwt(access);
    if (id)
    {
        return id;
    }
    return (fallback && fallback[0]) ? JsonDup(fallback) : NULL;
}

static int PostRaw(const char *url, const char *body, const char *content_type,
                   PicoHttpCancelFn cancel, void *cancel_user, long *http, char **out, char **err)
{
    PicoHttpReq req;
    memset(&req, 0, sizeof(req));
    req.url = url;
    req.body = body ? body : "";
    req.headers[0] = content_type;
    req.header_count = content_type ? 1 : 0;
    req.cancel = cancel;
    req.user = cancel_user;
    return pico_http_post(&req, http, out, err);
}

/* Lets a token refresh honour the same cancel the streaming request does. */
typedef struct TurnCancel {
    PicoLlmCancelFn fn;
    void *user;
} TurnCancel;

static bool TurnCancelled(void *user)
{
    TurnCancel *t = (TurnCancel *)user;
    return t && t->fn && t->fn(t->user);
}

/* Prefers the server's own explanation, which pico_http_post captures even for a
 * 4xx; curl's `err` is only set when the request never completed. */
static char *HttpDetail(const char *body, const char *err, long http)
{
    static const char *kFields[] = {"error_description", "detail", "message", "error"};
    if (body && body[0])
    {
        JsonDoc doc;
        if (JsonParse(&doc, body, strlen(body)) == 0)
        {
            for (size_t i = 0; i < sizeof(kFields) / sizeof(kFields[0]); i++)
            {
                int tok = JsonObjGet(&doc, 0, kFields[i]);
                char *s = NULL;
                if (JsonIsObject(&doc, tok))
                {
                    s = JsonObjStr(&doc, tok, "message");
                }
                else if (tok >= 0)
                {
                    s = JsonStrDup(&doc, tok);
                }
                if (s && s[0])
                {
                    JsonFree(&doc);
                    return s;
                }
                free(s);
            }
            JsonFree(&doc);
        }
    }
    if (err && err[0])
    {
        return JsonDup(err);
    }
    char buf[64];
    snprintf(buf, sizeof(buf), "HTTP %ld", http);
    return JsonDup(buf);
}

static char *TokenString(const JsonDoc *doc, const char *key)
{
    int tok = JsonObjGet(doc, 0, key);
    int start = JsonTokStart(doc, tok);
    /* JsonObjStr also converts primitives; credentials must actually be strings. */
    if (start <= 0 || doc->src[start - 1] != '"') return NULL;
    char *value = JsonStrDup(doc, tok);
    if (!value || !value[0]) { free(value); return NULL; }
    return value;
}

static long TokenExpiry(const JsonDoc *doc)
{
    char *raw = JsonObjRaw(doc, 0, "expires_in");
    if (!raw || !raw[0]) { free(raw); return 0; }
    long value = 0;
    for (const char *p = raw; *p; p++)
    {
        if (*p < '0' || *p > '9' || value > (LONG_MAX - (*p - '0')) / 10)
        {
            free(raw);
            return 0;
        }
        value = value * 10 + (*p - '0');
    }
    free(raw);
    long now = (long)time(NULL);
    return value > 0 && value <= LONG_MAX - now ? now + value : 0;
}

static bool ApplyTokenBody(PicoHost *app, PicoAgentContext *ctx, PicoAuthEntry *auth,
                           const char *body, PicoAgentId agent_id)
{
    JsonDoc doc;
    if (!body || !JsonValidSyntax(body, strlen(body)) || JsonParse(&doc, body, strlen(body)) != 0)
    {
        return false;
    }
    char *access = TokenString(&doc, "access_token");
    char *refresh = TokenString(&doc, "refresh_token");
    char *id_token = TokenString(&doc, "id_token");
    char *token_type = TokenString(&doc, "token_type");
    bool type_present = JsonObjGet(&doc, 0, "token_type") >= 0;
    long expires_at = TokenExpiry(&doc);
    /* An initial login must be refreshable. A refresh response may omit rotation.
     * Validate everything before changing existing credentials. */
    bool ok = JsonIsObject(&doc, 0) && access && access[0] && expires_at > 0 &&
              (!type_present || (token_type && strcasecmp(token_type, "Bearer") == 0)) &&
              ((refresh && refresh[0]) || (auth && auth->refresh_token && auth->refresh_token[0]));
    if (ok)
    {
        const char *use_refresh = (refresh && refresh[0]) ? refresh : (auth ? auth->refresh_token : NULL);
        char *account = PickAccountId(access, id_token, auth ? auth->account_id : NULL);
        bool saved = ctx ? pico_auth_set_oauth_ctx(ctx, "openai", access, use_refresh, account, expires_at)
                         : pico_auth_set_oauth(app, "openai", access, use_refresh, account, expires_at);
        if (!saved && !ctx)
        {
            Note(app, agent_id, "Warning: could not write `~/.config/pico/auth.json`. This session stays "
                         "signed in, but the login will not survive a restart.");
        }
        if (auth)
        {
            pico_auth_entry_free(auth);
            if (ctx)
            {
                pico_auth_copy_ctx(ctx, "openai", auth);
            }
            else
            {
                pico_auth_copy(app, "openai", auth);
            }
        }
        free(account);
    }
    free(access);
    free(refresh);
    free(id_token);
    free(token_type);
    JsonFree(&doc);
    return ok;
}

static bool OauthDue(const PicoAuthEntry *auth)
{
    if (!auth || !auth->access_token || !auth->access_token[0])
    {
        return true;
    }
    return auth->expires_at > 0 && time(NULL) + 60 >= auth->expires_at;
}

static bool RefreshOauth(PicoAgentContext *ctx, PicoAuthEntry *auth, TurnCancel *tc)
{
    if (!ctx || !auth)
    {
        return false;
    }

    int res = pico_auth_begin_refresh_ctx(ctx, "openai", tc ? tc->fn : NULL, tc ? tc->user : NULL, auth);
    if (res == PICO_AUTH_REFRESH_ALREADY_VALID)
    {
        return true;
    }
    if (res != PICO_AUTH_REFRESH_OWNER)
    {
        return false;
    }

    const char *keys[] = {"grant_type", "refresh_token", "client_id"};
    const char *vals[] = {"refresh_token", auth->refresh_token, kClientId};
    char *form = pico_http_form_encode(keys, vals, 3);
    char url[256];
    snprintf(url, sizeof(url), "%s/oauth/token", kIssuer);
    long http = 0;
    char *body = NULL;
    char *err = NULL;
    int rc = PostRaw(url, form, "Content-Type: application/x-www-form-urlencoded",
                     tc ? TurnCancelled : NULL, tc, &http, &body, &err);
    free(form);
    bool ok = rc == PICO_HTTP_OK && http < 400 && ApplyTokenBody(NULL, ctx, auth, body, 0);
    free(body);
    free(err);

    pico_auth_end_refresh_ctx(ctx, "openai");
    return ok;
}

static int IntervalOf(const JsonDoc *doc, int obj)
{
    char *s = JsonObjStr(doc, obj, "interval");
    int v = 5;
    if (s && s[0])
    {
        v = atoi(s);
    }
    else
    {
        v = JsonObjInt(doc, obj, "interval", 5);
    }
    free(s);
    return v < 1 ? 5 : v;
}

static bool ExchangeCode(LoginAttempt *login, const char *redirect, const char *code, const char *verifier)
{
    char *form = pico_openai_code_form(kClientId, redirect, code, verifier);
    if (!form) { LoginNote(login, "Could not prepare token exchange."); return false; }
    char url[256];
    snprintf(url, sizeof(url), "%s/oauth/token", kIssuer);
    long http = 0;
    char *body = NULL, *err = NULL;
    int rc = PostRaw(url, form, "Content-Type: application/x-www-form-urlencoded", LoginCancelled,
                     login, &http, &body, &err);
    FreeSecret(form);
    bool ok = rc == PICO_HTTP_OK && http >= 200 && http < 300 && body;
    if (ok)
    {
        pthread_mutex_lock(&login->mu);
        if (!login->cancel)
        {
            login->token_body = body;
            body = NULL;
        }
        pthread_mutex_unlock(&login->mu);
    }
    else if (rc != PICO_HTTP_CANCEL)
    {
        /* Never put a token endpoint response (which may contain secrets) in chat. */
        LoginNote(login, "Token exchange failed. Run `/login openai` to try again.");
    }
    FreeSecret(body);
    free(err);
    return ok;
}

typedef enum DevicePoll {
    DEVICE_PENDING = 0,
    DEVICE_READY,
    /* The request never completed, so retrying may still succeed. */
    DEVICE_UNREACHABLE,
    /* The server answered with a verdict; retrying will not change it. */
    DEVICE_FAILED,
    DEVICE_CANCELLED,
} DevicePoll;

static bool IsPendingError(const char *s)
{
    return s && (strstr(s, "authorization_pending") || strstr(s, "slow_down") || strstr(s, "pending"));
}

/* Anything short of an explicit failure counts as "not approved yet". Guessing
 * wrong that way only costs another poll, whereas treating an unfamiliar pending
 * response as fatal would drop the user out of the flow entirely. */
static DevicePoll PollDeviceOnce(LoginAttempt *s, const char *device_auth_id, const char *user_code,
                                 char **out_code, char **out_verifier, char **out_error)
{
    char url[256];
    snprintf(url, sizeof(url), "%s/api/accounts/deviceauth/token", kIssuer);
    JsonBuf b;
    JsonBuf_Init(&b);
    JsonBuf_Puts(&b, "{\"device_auth_id\":");
    JsonBuf_String(&b, device_auth_id);
    JsonBuf_Puts(&b, ",\"user_code\":");
    JsonBuf_String(&b, user_code);
    JsonBuf_Putc(&b, '}');
    char *req = JsonBuf_Steal(&b);
    long http = 0;
    char *body = NULL;
    char *err = NULL;
    int rc = PostRaw(url, req, "Content-Type: application/json", LoginCancelled, s, &http, &body,
                     &err);
    free(req);

    DevicePoll state = DEVICE_PENDING;
    if (rc == PICO_HTTP_CANCEL)
    {
        state = DEVICE_CANCELLED;
    }
    else if (rc != PICO_HTTP_OK)
    {
        *out_error = HttpDetail(NULL, err, http);
        state = DEVICE_UNREACHABLE;
    }
    else if (http >= 400 && http != 403 && http != 404)
    {
        char *detail = HttpDetail(body, err, http);
        if (IsPendingError(body) || IsPendingError(detail))
        {
            free(detail);
        }
        else
        {
            *out_error = detail;
            state = DEVICE_FAILED;
        }
    }
    else if (body && body[0])
    {
        JsonDoc doc;
        if (JsonParse(&doc, body, strlen(body)) == 0)
        {
            char *code = JsonObjStr(&doc, 0, "authorization_code");
            char *verifier = JsonObjStr(&doc, 0, "code_verifier");
            if (code && code[0] && verifier && verifier[0])
            {
                *out_code = code;
                *out_verifier = verifier;
                state = DEVICE_READY;
            }
            else
            {
                free(code);
                free(verifier);
            }
            JsonFree(&doc);
        }
    }
    free(body);
    free(err);
    return state;
}

static bool RequestUserCode(LoginAttempt *s, char *id, size_t id_cap, char *code, size_t code_cap,
                            int *interval)
{
    char url[256];
    snprintf(url, sizeof(url), "%s/api/accounts/deviceauth/usercode", kIssuer);
    JsonBuf b;
    JsonBuf_Init(&b);
    JsonBuf_Puts(&b, "{\"client_id\":");
    JsonBuf_String(&b, kClientId);
    JsonBuf_Putc(&b, '}');
    char *req = JsonBuf_Steal(&b);
    long http = 0;
    char *body = NULL;
    char *err = NULL;
    int rc = PostRaw(url, req, "Content-Type: application/json", LoginCancelled, s, &http, &body,
                     &err);
    free(req);
    if (rc == PICO_HTTP_CANCEL)
    {
        free(body);
        free(err);
        return false;
    }
    if (rc != PICO_HTTP_OK || http >= 400)
    {
        if (http == 404)
        {
            LoginNote(s, "Device-code login is not enabled for this ChatGPT account. Enable it in your "
                         "ChatGPT security settings, or ask a workspace admin.");
        }
        else
        {
            char *detail = HttpDetail(body, err, http);
            char buf[512];
            snprintf(buf, sizeof(buf), "Could not start device login: %s",
                     detail ? detail : "unknown error");
            free(detail);
            LoginNote(s, buf);
        }
        free(body);
        free(err);
        return false;
    }
    JsonDoc doc;
    if (!body || JsonParse(&doc, body, strlen(body)) != 0)
    {
        LoginNote(s, "Could not start device login: bad response.");
        free(body);
        free(err);
        return false;
    }
    char *got_id = JsonObjStr(&doc, 0, "device_auth_id");
    char *got_code = JsonObjStr(&doc, 0, "user_code");
    if (!got_code || !got_code[0])
    {
        free(got_code);
        got_code = JsonObjStr(&doc, 0, "usercode");
    }
    bool ok = got_id && got_id[0] && strlen(got_id) < id_cap &&
              got_code && got_code[0] && strlen(got_code) < code_cap;
    if (ok)
    {
        snprintf(id, id_cap, "%s", got_id);
        snprintf(code, code_cap, "%s", got_code);
        *interval = IntervalOf(&doc, 0);
    }
    else
    {
        LoginNote(s, "Could not start device login: missing or oversized device code.");
    }
    free(got_id);
    free(got_code);
    JsonFree(&doc);
    free(body);
    free(err);
    return ok;
}

static void DeviceLoginRun(LoginAttempt *login)
{
    char device_auth_id[128] = {0};
    char user_code[64] = {0};
    int interval = 5;
    if (!RequestUserCode(login, device_auth_id, sizeof(device_auth_id), user_code, sizeof(user_code), &interval)) return;
    char msg[512];
    snprintf(msg, sizeof(msg),
             "Sign in at %s/codex/device\nEnter code: `%s`\n\nThe code expires in %d minutes. "
             "`/login openai cancel` to stop.", kIssuer, user_code, PICO_LOGIN_TIMEOUT_SEC / 60);
    LoginNote(login, msg);
    int fails = 0;
    while (LoginSleep(login, interval))
    {
        char *code = NULL, *verifier = NULL, *error = NULL;
        DevicePoll result = PollDeviceOnce(login, device_auth_id, user_code, &code, &verifier, &error);
        bool keep_polling = result == DEVICE_PENDING;
        if (result == DEVICE_READY)
        {
            char redirect[256];
            snprintf(redirect, sizeof(redirect), "%s/deviceauth/callback", kIssuer);
            ExchangeCode(login, redirect, code, verifier);
        }
        else if (result == DEVICE_PENDING) fails = 0;
        else if (result == DEVICE_UNREACHABLE && ++fails < PICO_DEVICE_MAX_TRANSPORT_FAILS) keep_polling = true;
        else if (result == DEVICE_UNREACHABLE || result == DEVICE_FAILED)
        {
            LoginNote(login, "Device login failed. Run `/login openai device` to try again.");
        }
        FreeSecret(code);
        FreeSecret(verifier);
        free(error);
        if (!keep_polling) break;
    }
}

static void BrowserLoginRun(LoginAttempt *login)
{
    PicoOpenAiPkce pkce;
    if (!pico_openai_pkce_generate(&pkce))
    {
        LoginNote(login, "Could not generate secure OAuth login material. Login was not started.");
        return;
    }
    const unsigned short ports[] = {1455, 1457};
    unsigned short port = 0;
    int listener = pico_openai_listen(ports, sizeof(ports) / sizeof(ports[0]), &port);
    if (listener < 0)
    {
        LoginNote(login, errno == EADDRINUSE ?
                  "Browser login needs a local callback, but ports 1455 and 1457 are occupied. "
                  "Close the other login attempt and retry, or use `/login openai device`." :
                  "Could not start the local login callback listener. Retry or use `/login openai device`.");
        OPENSSL_cleanse(&pkce, sizeof(pkce));
        return;
    }
    char redirect[128];
    snprintf(redirect, sizeof(redirect), "http://localhost:%u/auth/callback", (unsigned)port);
    char *url = pico_openai_authorize_url(kIssuer, kClientId, redirect, &pkce);
    if (!url)
    {
        LoginNote(login, "Could not prepare browser login.");
        close(listener);
        OPENSSL_cleanse(&pkce, sizeof(pkce));
        return;
    }
    pthread_mutex_lock(&login->mu);
    if (!login->cancel) { login->authorize_url = url; url = NULL; }
    pthread_mutex_unlock(&login->mu);
    free(url);
    char *code = NULL;
    PicoOpenAiCallback result = pico_openai_await_callback(listener, login->wake[0], login->deadline,
                                                           pkce.state, LoginCancelled, login, &code);
    close(listener);
    if (result == PICO_OPENAI_CALLBACK_CODE && !LoginCancelled(login))
        ExchangeCode(login, redirect, code, pkce.verifier);
    else if (result == PICO_OPENAI_CALLBACK_DENIED)
        LoginNote(login, "OpenAI did not authorize sign-in. Run `/login openai` to try again.");
    else if (result == PICO_OPENAI_CALLBACK_FAILED)
        LoginNote(login, "The local login callback failed. Retry or use `/login openai device`.");
    FreeSecret(code);
    OPENSSL_cleanse(&pkce, sizeof(pkce));
}

static void *LoginMain(void *user)
{
    LoginAttempt *login = user;
    if (login->browser) BrowserLoginRun(login);
    else DeviceLoginRun(login);
    if (pico_openai_monotonic() >= login->deadline)
        LoginNote(login, login->browser ? "Browser login timed out. Run `/login openai` to try again." :
                                         "Device login timed out. Run `/login openai device` to try again.");
    pthread_mutex_lock(&login->mu);
    login->done = true;
    pthread_mutex_unlock(&login->mu);
    return NULL;
}

static void StartLogin(HostAuthState *s, PicoAgentId agent_id, bool browser)
{
    StopLogin(s);
    LoginAttempt *login = calloc(1, sizeof(*login));
    if (!login) { Note(s->host, agent_id, "Could not allocate login attempt."); return; }
    if (pthread_mutex_init(&login->mu, NULL) != 0)
    {
        free(login);
        Note(s->host, agent_id, "Could not initialize login attempt.");
        return;
    }
    if (!pico_openai_wake_pipe(login->wake))
    {
        pthread_mutex_destroy(&login->mu);
        free(login);
        Note(s->host, agent_id, "Could not initialize login cancellation.");
        return;
    }
    login->agent_id = agent_id;
    login->browser = browser;
    login->deadline = pico_openai_monotonic() + PICO_LOGIN_TIMEOUT_SEC;
    login->refs = 2;
    s->login = login;
    if (!PicoHost_StartTask(s->host, LoginMain, login, CancelLogin, ReleaseLogin))
    {
        s->login = NULL;
        ReleaseLogin(login);
        ReleaseLogin(login);
        Note(s->host, agent_id, "Could not start login worker. Try again when the previous login has stopped.");
    }
}

static void DrainLoginNotes(HostAuthState *s)
{
    LoginAttempt *login = s->login;
    if (!login) return;
    pthread_mutex_lock(&login->mu);
    char *url = login->authorize_url;
    login->authorize_url = NULL;
    char *body = login->token_body;
    login->token_body = NULL;
    char *notes[PICO_LOGIN_MAX_NOTES];
    int count = login->note_count;
    memcpy(notes, login->notes, (size_t)count * sizeof(notes[0]));
    login->note_count = 0;
    bool done = login->done;
    bool cancelled = login->cancel || pico_openai_monotonic() >= login->deadline;
    pthread_mutex_unlock(&login->mu);
    /* Only the currently attached attempt can reach here. StopLogin/reload drop
     * its registration reference before any replacement is published. */
    if (url && !cancelled && !done)
    {
        size_t cap = strlen(url) + 256;
        char *message = malloc(cap);
        if (message)
        {
            snprintf(message, cap, "[Sign in with OpenAI](%s)\n\nOpening your browser. "
                     "If it does not open, use the link above. `/login openai cancel` to stop.", url);
            Note(s->host, login->agent_id, message);
            free(message);
        }
        if (!PicoHost_OpenBrowser(s->host, url))
            Note(s->host, login->agent_id, "Could not open your browser automatically. Use the sign-in link above.");
    }
    for (int i = 0; i < count; i++)
    {
        if (!login->cancel) Note(s->host, login->agent_id, notes[i]);
        free(notes[i]);
    }
    if (body && !cancelled)
    {
        if (ApplyTokenBody(s->host, NULL, NULL, body, login->agent_id))
            Note(s->host, login->agent_id, "Signed in with ChatGPT. Pico will use your Codex subscription.");
        else Note(s->host, login->agent_id, "OpenAI returned an invalid token response. Run `/login openai` to try again.");
    }
    free(url);
    FreeSecret(body);
    if (done) { s->login = NULL; ReleaseLogin(login); }
}

static int Fold(int c)
{
    return (c >= 'A' && c <= 'Z') ? c - 'A' + 'a' : c;
}

static bool FoldEq(const char *a, const char *b)
{
    if (!a || !b)
    {
        return false;
    }
    while (*a && *b)
    {
        if (Fold((unsigned char)*a) != Fold((unsigned char)*b))
        {
            return false;
        }
        a++;
        b++;
    }
    return *a == *b;
}

static bool IsKeyArg(const char *s)
{
    return FoldEq(s, "key") || FoldEq(s, "api-key") || FoldEq(s, "apikey");
}

static bool IsCancelArg(const char *s)
{
    return FoldEq(s, "cancel");
}

/* `/login` forwards whatever followed the provider name, so pull out the single
 * verb here and reject anything trailing it. */
static void OpenAiLogin(PicoHost *app, PicoAgentId agent_id, const char *args, void *state)
{
    HostAuthState *s = (HostAuthState *)state;
    const char *p = args ? args : "";
    while (*p == ' ' || *p == '\t')
    {
        p++;
    }
    char verb[32];
    size_t n = 0;
    while (p[n] && p[n] != ' ' && p[n] != '\t' && n + 1 < sizeof(verb))
    {
        verb[n] = p[n];
        n++;
    }
    verb[n] = '\0';
    const char *tail = p + n;
    while (*tail == ' ' || *tail == '\t')
    {
        tail++;
    }
    if (tail[0] || (verb[0] && !IsCancelArg(verb) && !IsKeyArg(verb) && !FoldEq(verb, "browser") && !FoldEq(verb, "device")))
    {
        Note(app, agent_id, "Usage: `/login openai [browser|device|key|cancel]`.");
        return;
    }
    if (IsCancelArg(verb))
    {
        if (s->login)
        {
            StopLogin(s);
            Note(app, agent_id, "Login cancelled.");
        }
        else
        {
            Note(app, agent_id, "No login in progress.");
        }
        return;
    }
    if (IsKeyArg(verb))
    {
        StopLogin(s);
        PicoAuthEntry e;
        pico_auth_copy(app, "openai", &e);
        if (!e.api_key || !e.api_key[0])
        {
            Note(app, agent_id, "No API key. Set `PICO_API_KEY` or `OPENAI_API_KEY`.");
            pico_auth_entry_free(&e);
            return;
        }
        if (pico_auth_set_active(app, "openai", PICO_AUTH_API_KEY))
        {
            Note(app, agent_id, "Using OpenAI API key.");
        }
        else
        {
            Note(app, agent_id, "Using OpenAI API key, but `~/.config/pico/auth.json` could not be written, so "
                                "this choice will not survive a restart.");
        }
        pico_auth_entry_free(&e);
        return;
    }
    StartLogin(s, agent_id, !FoldEq(verb, "device"));
}

static void OpenAiLogout(PicoHost *app, PicoAgentId agent_id, void *state)
{
    HostAuthState *s = (HostAuthState *)state;
    StopLogin(s);
    bool saved = pico_auth_clear_oauth(app, "openai");
    PicoAuthEntry e;
    pico_auth_copy(app, "openai", &e);
    if (!saved)
    {
        Note(app, agent_id, "Logged out of ChatGPT, but `~/.config/pico/auth.json` could not be written, so the "
                            "stored tokens may still be on disk.");
    }
    else if (e.api_key && e.api_key[0])
    {
        Note(app, agent_id, "Logged out of ChatGPT. Using API key.");
    }
    else
    {
        Note(app, agent_id, "Logged out of ChatGPT.");
    }
    pico_auth_entry_free(&e);
}

static void OpenAiFrame(PicoHost *app, void *state, float dt)
{
    (void)app;
    (void)dt;
    HostAuthState *s = (HostAuthState *)state;
    DrainLoginNotes(s);
}

static int PostOnce(const char *url, const char *bearer, const char *account_id, bool oauth,
                    const char *body, const char *session_id, PicoResponsesCtx *ctx,
                    PicoLlmCancelFn cancel, PicoLlmDeltaFn on_delta, void *user)
{
    char *acct = NULL;
    char session_hdr[80];
    const char *extras[4];
    int extra_count = 0;
    if (oauth)
    {
        extras[extra_count++] = "originator: pico";
        extras[extra_count++] = "OpenAI-Beta: responses=experimental";
        if (account_id && account_id[0])
        {
            size_t n = strlen(account_id) + 32;
            acct = (char *)malloc(n);
            if (acct)
            {
                snprintf(acct, n, "chatgpt-account-id: %s", account_id);
                extras[extra_count++] = acct;
            }
        }
    }
    if (session_id && session_id[0])
    {
        snprintf(session_hdr, sizeof(session_hdr), "session_id: %s", session_id);
        extras[extra_count++] = session_hdr;
    }
    int rc = pico_responses_post(url, body, bearer, extras, extra_count, cancel, on_delta, user, ctx);
    free(acct);
    return rc;
}

static const char *BearerOf(const PicoAuthEntry *auth, bool oauth)
{
    if (oauth)
    {
        return auth->access_token;
    }
    return auth->api_key;
}

static int OpenAiStream(PicoAgentContext *agent_ctx, const PicoLlmTurn *turn, PicoLlmCancelFn cancel,
                        PicoLlmDeltaFn on_delta, void *user, PicoLlmResult *out, void *state)
{
    (void)state;
    if (out)
    {
        memset(out, 0, sizeof(*out));
    }
    if (!agent_ctx || !turn || !out)
    {
        return PICO_LLM_FAIL;
    }

    TurnCancel tc = {.fn = cancel, .user = user};
    PicoAuthEntry auth;
    pico_auth_copy_ctx(agent_ctx, "openai", &auth);
    bool oauth = strcmp(auth.active, PICO_AUTH_OAUTH) == 0;
    if (turn->fast && (!FastAuthAvailable(&auth) ||
                       !pico_responses_openai_fast_route(turn->base_url, oauth)))
    {
        pico_auth_entry_free(&auth);
        out->error = JsonDup("Fast mode requires an authenticated OpenAI API or Codex route.");
        return PICO_LLM_FAIL;
    }
    if (oauth)
    {
        if (OauthDue(&auth) || !auth.access_token || !auth.access_token[0])
        {
            if (!RefreshOauth(agent_ctx, &auth, &tc))
            {
                pico_auth_entry_free(&auth);
                if (TurnCancelled(&tc))
                {
                    return PICO_LLM_CANCEL;
                }
                out->error = JsonDup("Codex login expired. Run `/login openai`.");
                return PICO_LLM_FAIL;
            }
        }
    }
    else if (!auth.api_key || !auth.api_key[0])
    {
        pico_auth_entry_free(&auth);
        out->error = JsonDup(
            "No OpenAI credentials. Run `/login openai` for a ChatGPT subscription, or set "
            "`PICO_API_KEY` / `OPENAI_API_KEY`.");
        return PICO_LLM_FAIL;
    }

    char url[1024];
    if (oauth)
    {
        snprintf(url, sizeof(url), "%s", kCodexResponses);
    }
    else
    {
        pico_responses_resolve_url(turn->base_url, kDefaultBase, url, sizeof(url));
    }
    char *body = BuildRequest(turn, oauth);
    if (!body)
    {
        pico_auth_entry_free(&auth);
        out->error = JsonDup("failed to build request");
        return PICO_LLM_FAIL;
    }

    const char *bearer = BearerOf(&auth, oauth);
    PicoResponsesCtx ctx;
    int rc = PostOnce(url, bearer, auth.account_id, oauth, body, turn->cache_key, &ctx, cancel, on_delta,
                      user);
    if (rc == PICO_LLM_FAIL && oauth && ctx.http == 401)
    {
        pico_responses_ctx_free(&ctx);
        if (!RefreshOauth(agent_ctx, &auth, &tc))
        {
            free(body);
            pico_auth_entry_free(&auth);
            if (TurnCancelled(&tc))
            {
                return PICO_LLM_CANCEL;
            }
            out->error = JsonDup("Codex login expired. Run `/login openai`.");
            return PICO_LLM_FAIL;
        }
        bearer = BearerOf(&auth, true);
        rc = PostOnce(url, bearer, auth.account_id, true, body, turn->cache_key, &ctx, cancel, on_delta,
                      user);
    }
    if (rc == PICO_LLM_FAIL && ctx.error && strstr(ctx.error, "easoning"))
    {
        char *stripped = pico_responses_body_without_reasoning(body);
        if (stripped)
        {
            pico_responses_ctx_free(&ctx);
            rc = PostOnce(url, BearerOf(&auth, oauth), auth.account_id, oauth, stripped, turn->cache_key,
                          &ctx, cancel, on_delta, user);
            free(stripped);
        }
    }
    free(body);
    pico_auth_entry_free(&auth);
    if (rc == PICO_LLM_CANCEL)
    {
        return PICO_LLM_CANCEL;
    }
    pico_responses_fill_result(&ctx, out);
    JsonBuf_Free(&ctx.items);
    JsonBuf_Free(&ctx.summary);
    if (rc != PICO_LLM_OK)
    {
        if (!out->error)
        {
            out->error = ctx.error ? ctx.error : JsonDup("LLM request failed");
            ctx.error = NULL;
        }
        free(ctx.error);
        return PICO_LLM_FAIL;
    }
    if (!out->error && !ctx.saw_text && !pico_llm_result_has_output(out))
    {
        char buf[sizeof(url) + 32];
        snprintf(buf, sizeof(buf), "empty response from %s", url);
        out->error = JsonDup(buf);
        free(ctx.error);
        return PICO_LLM_FAIL;
    }
    free(ctx.error);
    return PICO_LLM_OK;
}

static const char *FirstEnv(const char *a, const char *b)
{
    const char *v = getenv(a);
    if (v && v[0])
    {
        return v;
    }
    v = getenv(b);
    return (v && v[0]) ? v : NULL;
}

static int OpenAiHostInit(PicoHost *app, void **state_out)
{
    HostAuthState *s = (HostAuthState *)calloc(1, sizeof(HostAuthState));
    if (!s)
    {
        return 1;
    }
    s->host = app;
    if (state_out)
    {
        *state_out = s;
    }
    pico_add_auth(app, &(PicoAuth){.provider = "openai",
                                   .help = "ChatGPT browser/device login or API key",
                                   .verbs = "browser device key cancel",
                                   .login = OpenAiLogin,
                                   .logout = OpenAiLogout,
                                   .state = s});
    pico_auth_set_env_key(app, "openai", FirstEnv("PICO_API_KEY", "OPENAI_API_KEY"));
    return 0;
}

static void OpenAiHostShutdown(PicoHost *app, void *state)
{
    (void)app;
    HostAuthState *s = (HostAuthState *)state;
    if (!s)
    {
        return;
    }
    StopLogin(s);
    free(s);
}

static bool OpenAiSupportsFast(PicoHost *host, const PicoModel *model, void *state)
{
    (void)state;
    PicoAuthEntry auth;
    pico_auth_copy(host, "openai", &auth);
    bool supported = FastAuthAvailable(&auth) && pico_responses_openai_fast_route(model->base_url,
                         strcmp(auth.active, PICO_AUTH_OAUTH) == 0);
    pico_auth_entry_free(&auth);
    return supported;
}

static int OpenAiWorkspaceInit(PicoWorkspace *workspace, void **state_out)
{
    (void)state_out;
    pico_add_provider(workspace, &(PicoProvider){.name = "openai",
                                                 .stream = OpenAiStream,
                                                 .map_context = true,
                                                 .supports_fast = OpenAiSupportsFast});
    return 0;
}

PicoExt pico_ext_openai(void)
{
    return (PicoExt){
        .abi = PICO_EXT_ABI,
        .name = "openai",
        .description = "OpenAI-compatible provider",
        .host_init = OpenAiHostInit,
        .host_shutdown = OpenAiHostShutdown,
        .host_on_frame = OpenAiFrame,
        .workspace_init = OpenAiWorkspaceInit,
    };
}
