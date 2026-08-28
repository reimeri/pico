#define _POSIX_C_SOURCE 200809L

#include "pico/plugin.h"
#include "pico/http.h"
#include "pico/auth.h"
#include "json.h"
#include "builtins/responses.h"
#include "host_internal.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static const char kDefaultBase[] = "https://api.openai.com/v1";
static const char kCodexResponses[] = "https://chatgpt.com/backend-api/codex/responses";
static const char kClientId[] = "app_EMoamEEZ73f0CkXaXp7hrann";
static const char kIssuer[] = "https://auth.openai.com";
static pthread_mutex_t g_refresh_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_refresh_cv = PTHREAD_COND_INITIALIZER;
static bool g_refreshing;
static PicoLlmCancelFn g_refresh_owner_cancel;
static void *g_refresh_owner_user;

static char *BuildRequest(const PicoLlmTurn *turn, bool codex)
{
    /* The ChatGPT Codex backend rejects stored responses, and with store off
     * the server only replays reasoning items that carry their encrypted
     * payload. Plain Responses endpoints (including OpenAI-compatible
     * gateways behind models[].base_url) may reject either field. */
    PicoResponsesBuildOpts opts = {
        .provider = "openai",
        .store_false = codex,
        .include_encrypted_reasoning = codex,
        .reasoning_summary_auto = true,
    };
    return pico_responses_build_request(turn, &opts);
}

/* Device-code polling runs on its own thread: every request is a blocking curl
 * call, and the render loop cannot afford to stall on the network. The thread
 * only touches the auth store (mutex-guarded) and this struct; user-visible text
 * is queued here and emitted by the render thread in OpenAiFrame. */
#define PICO_DEVICE_MAX_NOTES 8
#define PICO_DEVICE_TIMEOUT_SEC 900
#define PICO_DEVICE_MAX_TRANSPORT_FAILS 5

typedef struct DeviceLogin {
    pthread_mutex_t mu;
    pthread_t thread;
    bool joinable;
    bool running;
    bool cancel;
    char *notes[PICO_DEVICE_MAX_NOTES];
    int note_count;
} DeviceLogin;

static DeviceLogin g_login = {.mu = PTHREAD_MUTEX_INITIALIZER};

static void LoginNote(const char *text)
{
    if (!text || !text[0])
    {
        return;
    }
    pthread_mutex_lock(&g_login.mu);
    if (g_login.note_count < PICO_DEVICE_MAX_NOTES)
    {
        g_login.notes[g_login.note_count] = JsonDup(text);
        if (g_login.notes[g_login.note_count])
        {
            g_login.note_count++;
        }
    }
    pthread_mutex_unlock(&g_login.mu);
}

static bool LoginCancelled(void *user)
{
    (void)user;
    pthread_mutex_lock(&g_login.mu);
    bool c = g_login.cancel;
    pthread_mutex_unlock(&g_login.mu);
    return c;
}

static bool LoginActive(void)
{
    pthread_mutex_lock(&g_login.mu);
    bool r = g_login.running;
    pthread_mutex_unlock(&g_login.mu);
    return r;
}

/* Sleeps in short slices so `/login openai cancel` and shutdown are not held up by the
 * poll interval. */
static bool LoginSleep(int seconds)
{
    for (int i = 0; i < seconds * 5; i++)
    {
        if (LoginCancelled(NULL))
        {
            return false;
        }
        struct timespec ts = {.tv_sec = 0, .tv_nsec = 200 * 1000 * 1000};
        nanosleep(&ts, NULL);
    }
    return !LoginCancelled(NULL);
}

static void StopDeviceLogin(void)
{
    pthread_mutex_lock(&g_login.mu);
    g_login.cancel = true;
    bool joinable = g_login.joinable;
    pthread_t t = g_login.thread;
    g_login.joinable = false;
    pthread_mutex_unlock(&g_login.mu);
    if (joinable)
    {
        pthread_join(t, NULL);
    }
}

static void Note(PicoHost *app, const char *text)
{
    PicoHost_AddMessage(app, PICO_ROLE_ASSISTANT, text);
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

static bool ApplyTokenBody(PicoHost *app, PicoAgentContext *ctx, PicoAuthEntry *auth,
                           const char *body)
{
    JsonDoc doc;
    if (!body || JsonParse(&doc, body, strlen(body)) != 0)
    {
        return false;
    }
    char *access = JsonObjStr(&doc, 0, "access_token");
    char *refresh = JsonObjStr(&doc, 0, "refresh_token");
    char *id_token = JsonObjStr(&doc, 0, "id_token");
    int expires_in = JsonObjInt(&doc, 0, "expires_in", 3600);
    if (expires_in < 1)
    {
        expires_in = 3600;
    }
    bool ok = access && access[0];
    if (ok)
    {
        const char *use_refresh = (refresh && refresh[0]) ? refresh : (auth ? auth->refresh_token : NULL);
        char *account = PickAccountId(access, id_token, auth ? auth->account_id : NULL);
        long expires_at = (long)time(NULL) + expires_in;
        bool saved = ctx ? pico_auth_set_oauth_ctx(ctx, "openai", access, use_refresh, account, expires_at)
                         : pico_auth_set_oauth(app, "openai", access, use_refresh, account, expires_at);
        if (!saved && !ctx)
        {
            LoginNote("Warning: could not write `~/.config/pico/auth.json`. This session stays "
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
    JsonFree(&doc);
    return ok;
}

static bool OauthDue(const PicoAuthEntry *auth);

static bool RefreshOauth(PicoAgentContext *ctx, PicoAuthEntry *auth, TurnCancel *tc)
{
    if (!ctx || !auth)
    {
        return false;
    }

    pthread_mutex_lock(&g_refresh_mu);
    while (g_refreshing)
    {
        bool owner_abandoned = g_refresh_owner_cancel &&
                               g_refresh_owner_cancel(g_refresh_owner_user);
        if (owner_abandoned || (tc && TurnCancelled(tc)) ||
            pico_agent_context_cancelled(ctx))
        {
            /* Do not race a rotating token. A replacement fails promptly while
             * the abandoned owner unwinds and releases the refresh slot. */
            pthread_mutex_unlock(&g_refresh_mu);
            return false;
        }
        struct timespec until;
        clock_gettime(CLOCK_REALTIME, &until);
        until.tv_nsec += 100000000L;
        if (until.tv_nsec >= 1000000000L)
        {
            until.tv_sec++;
            until.tv_nsec -= 1000000000L;
        }
        (void)pthread_cond_timedwait(&g_refresh_cv, &g_refresh_mu, &until);
    }

    PicoAuthEntry latest;
    if (!pico_auth_copy_ctx(ctx, "openai", &latest))
    {
        pthread_mutex_unlock(&g_refresh_mu);
        return false;
    }
    pico_auth_entry_free(auth);
    *auth = latest;
    if (strcmp(auth->active, PICO_AUTH_OAUTH) != 0)
    {
        pthread_mutex_unlock(&g_refresh_mu);
        return false;
    }
    if (auth->access_token && auth->access_token[0] && !OauthDue(auth))
    {
        pthread_mutex_unlock(&g_refresh_mu);
        return true;
    }
    if (!auth->refresh_token || !auth->refresh_token[0] ||
        (tc && TurnCancelled(tc)) || pico_agent_context_cancelled(ctx))
    {
        pthread_mutex_unlock(&g_refresh_mu);
        return false;
    }
    g_refreshing = true;
    g_refresh_owner_cancel = tc ? tc->fn : NULL;
    g_refresh_owner_user = tc ? tc->user : NULL;
    pthread_mutex_unlock(&g_refresh_mu);

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
    bool ok = rc == PICO_HTTP_OK && http < 400 && ApplyTokenBody(NULL, ctx, auth, body);
    free(body);
    free(err);

    pthread_mutex_lock(&g_refresh_mu);
    g_refreshing = false;
    g_refresh_owner_cancel = NULL;
    g_refresh_owner_user = NULL;
    pthread_cond_broadcast(&g_refresh_cv);
    pthread_mutex_unlock(&g_refresh_mu);
    return ok;
}

static bool OauthDue(const PicoAuthEntry *auth)
{
    if (!auth || auth->expires_at <= 0)
    {
        return false;
    }
    return time(NULL) + 60 >= auth->expires_at;
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

static bool ExchangeDeviceCode(PicoHost *app, const char *code, const char *verifier)
{
    const char *keys[] = {"grant_type", "code", "redirect_uri", "client_id", "code_verifier"};
    char redirect[256];
    snprintf(redirect, sizeof(redirect), "%s/deviceauth/callback", kIssuer);
    const char *vals[] = {"authorization_code", code, redirect, kClientId, verifier};
    char *form = pico_http_form_encode(keys, vals, 5);
    char url[256];
    snprintf(url, sizeof(url), "%s/oauth/token", kIssuer);
    long http = 0;
    char *body = NULL;
    char *err = NULL;
    int rc = PostRaw(url, form, "Content-Type: application/x-www-form-urlencoded", LoginCancelled,
                     NULL, &http, &body, &err);
    free(form);
    bool ok = rc == PICO_HTTP_OK && http < 400 && ApplyTokenBody(app, NULL, NULL, body);
    if (ok)
    {
        LoginNote("Signed in with ChatGPT. Pico will use your Codex subscription.");
    }
    else if (rc != PICO_HTTP_CANCEL)
    {
        char *detail = HttpDetail(body, err, http);
        char buf[512];
        snprintf(buf, sizeof(buf), "Token exchange failed: %s", detail ? detail : "unknown error");
        free(detail);
        LoginNote(buf);
    }
    free(body);
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
static DevicePoll PollDeviceOnce(const char *device_auth_id, const char *user_code, char **out_code,
                                 char **out_verifier, char **out_error)
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
    int rc = PostRaw(url, req, "Content-Type: application/json", LoginCancelled, NULL, &http, &body,
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

static bool RequestUserCode(char *id, size_t id_cap, char *code, size_t code_cap, int *interval)
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
    int rc = PostRaw(url, req, "Content-Type: application/json", LoginCancelled, NULL, &http, &body,
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
            LoginNote("Device-code login is not enabled for this ChatGPT account. Enable it in your "
                      "ChatGPT security settings, or ask a workspace admin.");
        }
        else
        {
            char *detail = HttpDetail(body, err, http);
            char buf[512];
            snprintf(buf, sizeof(buf), "Could not start device login: %s",
                     detail ? detail : "unknown error");
            free(detail);
            LoginNote(buf);
        }
        free(body);
        free(err);
        return false;
    }
    JsonDoc doc;
    if (!body || JsonParse(&doc, body, strlen(body)) != 0)
    {
        LoginNote("Could not start device login: bad response.");
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
    bool ok = got_id && got_id[0] && got_code && got_code[0];
    if (ok)
    {
        snprintf(id, id_cap, "%s", got_id);
        snprintf(code, code_cap, "%s", got_code);
        *interval = IntervalOf(&doc, 0);
    }
    else
    {
        LoginNote("Could not start device login: missing user code.");
    }
    free(got_id);
    free(got_code);
    JsonFree(&doc);
    free(body);
    free(err);
    return ok;
}

static void *DeviceLoginMain(void *arg)
{
    PicoHost *app = (PicoHost *)arg;
    char device_auth_id[128] = {0};
    char user_code[64] = {0};
    int interval = 5;
    if (RequestUserCode(device_auth_id, sizeof(device_auth_id), user_code, sizeof(user_code), &interval))
    {
        char msg[512];
        snprintf(msg, sizeof(msg),
                 "Sign in at %s/codex/device\nEnter code: `%s`\n\nThe code expires in %d minutes. "
                 "`/login openai cancel` to stop.",
                 kIssuer, user_code, PICO_DEVICE_TIMEOUT_SEC / 60);
        LoginNote(msg);

        time_t deadline = time(NULL) + PICO_DEVICE_TIMEOUT_SEC;
        int fails = 0;
        while (LoginSleep(interval))
        {
            if (time(NULL) >= deadline)
            {
                LoginNote("Device login timed out. Run `/login openai` to try again.");
                break;
            }
            char *code = NULL;
            char *verifier = NULL;
            char *error = NULL;
            DevicePoll state = PollDeviceOnce(device_auth_id, user_code, &code, &verifier, &error);
            bool keep_polling = state == DEVICE_PENDING;
            if (state == DEVICE_READY)
            {
                ExchangeDeviceCode(app, code, verifier);
            }
            else if (state == DEVICE_PENDING)
            {
                fails = 0;
            }
            else if (state == DEVICE_UNREACHABLE && ++fails < PICO_DEVICE_MAX_TRANSPORT_FAILS)
            {
                /* The user may already have entered the code, so ride out a few
                 * dropped requests rather than abandoning the login. */
                keep_polling = true;
            }
            else if (state == DEVICE_UNREACHABLE || state == DEVICE_FAILED)
            {
                char buf[512];
                snprintf(buf, sizeof(buf), "Device login failed: %s", error ? error : "unknown error");
                LoginNote(buf);
            }
            free(code);
            free(verifier);
            free(error);
            if (!keep_polling)
            {
                break;
            }
        }
    }
    pthread_mutex_lock(&g_login.mu);
    g_login.running = false;
    pthread_mutex_unlock(&g_login.mu);
    return NULL;
}

static void StartDeviceLogin(PicoHost *app)
{
    StopDeviceLogin();
    pthread_mutex_lock(&g_login.mu);
    /* Drop anything the previous attempt queued so it cannot mix into this flow. */
    for (int i = 0; i < g_login.note_count; i++)
    {
        free(g_login.notes[i]);
        g_login.notes[i] = NULL;
    }
    g_login.note_count = 0;
    g_login.cancel = false;
    g_login.running = true;
    bool spawned = pthread_create(&g_login.thread, NULL, DeviceLoginMain, app) == 0;
    g_login.joinable = spawned;
    if (!spawned)
    {
        g_login.running = false;
    }
    pthread_mutex_unlock(&g_login.mu);
    if (!spawned)
    {
        Note(app, "Could not start device login: thread creation failed.");
    }
}

/* The render thread owns the message list, so queued login text surfaces here. */
static void DrainLoginNotes(PicoHost *app)
{
    for (;;)
    {
        pthread_mutex_lock(&g_login.mu);
        char *text = NULL;
        if (g_login.note_count > 0)
        {
            text = g_login.notes[0];
            for (int i = 1; i < g_login.note_count; i++)
            {
                g_login.notes[i - 1] = g_login.notes[i];
            }
            g_login.note_count--;
        }
        bool reap = !text && !g_login.running && g_login.joinable;
        pthread_mutex_unlock(&g_login.mu);
        if (text)
        {
            Note(app, text);
            free(text);
            continue;
        }
        if (reap)
        {
            StopDeviceLogin();
        }
        return;
    }
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
static void OpenAiLogin(PicoHost *app, const char *args, void *state)
{
    (void)state;
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
    if (tail[0] || (verb[0] && !IsCancelArg(verb) && !IsKeyArg(verb)))
    {
        Note(app, "Usage: `/login openai`, `/login openai key`, or `/login openai cancel`.");
        return;
    }
    if (IsCancelArg(verb))
    {
        if (LoginActive())
        {
            StopDeviceLogin();
            Note(app, "Login cancelled.");
        }
        else
        {
            Note(app, "No login in progress.");
        }
        return;
    }
    if (IsKeyArg(verb))
    {
        StopDeviceLogin();
        PicoAuthEntry e;
        pico_auth_copy(app, "openai", &e);
        if (!e.api_key || !e.api_key[0])
        {
            Note(app, "No API key. Set `PICO_API_KEY` or `OPENAI_API_KEY`.");
            pico_auth_entry_free(&e);
            return;
        }
        if (pico_auth_set_active(app, "openai", PICO_AUTH_API_KEY))
        {
            Note(app, "Using OpenAI API key.");
        }
        else
        {
            Note(app, "Using OpenAI API key, but `~/.config/pico/auth.json` could not be written, so "
                      "this choice will not survive a restart.");
        }
        pico_auth_entry_free(&e);
        return;
    }
    StartDeviceLogin(app);
}

static void OpenAiLogout(PicoHost *app, void *state)
{
    (void)state;
    StopDeviceLogin();
    bool saved = pico_auth_clear_oauth(app, "openai");
    PicoAuthEntry e;
    pico_auth_copy(app, "openai", &e);
    if (!saved)
    {
        Note(app, "Logged out of ChatGPT, but `~/.config/pico/auth.json` could not be written, so the "
                  "stored tokens may still be on disk.");
    }
    else if (e.api_key && e.api_key[0])
    {
        Note(app, "Logged out of ChatGPT. Using API key.");
    }
    else
    {
        Note(app, "Logged out of ChatGPT.");
    }
    pico_auth_entry_free(&e);
}

static void OpenAiFrame(PicoHost *app, void *state, float dt)
{
    (void)dt;
    DrainLoginNotes(app);
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

static int OpenAiInit(PicoHost *app, void **state_out)
{
    (void)state_out;
    pico_add_provider(PicoHost_PrimaryWorkspace(app), &(PicoProvider){.name = "openai", .stream = OpenAiStream, .map_context = true});
    pico_add_auth(app, &(PicoAuth){.provider = "openai",
                                   .help = "ChatGPT device-code or API key",
                                   .verbs = "key cancel",
                                   .login = OpenAiLogin,
                                   .logout = OpenAiLogout});
    pico_auth_set_env_key(app, "openai", FirstEnv("PICO_API_KEY", "OPENAI_API_KEY"));
    return 0;
}

static void OpenAiShutdown(PicoHost *app, void *state)
{
    (void)state;
    (void)app;
    StopDeviceLogin();
    pthread_mutex_lock(&g_login.mu);
    for (int i = 0; i < g_login.note_count; i++)
    {
        free(g_login.notes[i]);
        g_login.notes[i] = NULL;
    }
    g_login.note_count = 0;
    pthread_mutex_unlock(&g_login.mu);
}

PicoExt pico_ext_openai(void)
{
    return (PicoExt){
        .abi = PICO_EXT_ABI,
        .name = "openai",
        .description = "OpenAI-compatible provider",
        .host_init = OpenAiInit,
        .host_shutdown = OpenAiShutdown,
        .host_on_frame = OpenAiFrame,
    };
}
