#define _POSIX_C_SOURCE 200809L

#include "pico/plugin.h"
#include "pico/http.h"
#include "pico/auth.h"
#include "json.h"
#include "builtins/completions.h"
#include "builtins/xai_auth.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static const char kDefaultBase[] = "https://api.x.ai/v1";
static const char kClientId[] = "b1a00492-073a-47ea-816f-4c329264a828";
static const char kScope[] = "openid profile email offline_access grok-cli:access api:access";
static const char kDeviceUrl[] = "https://auth.x.ai/oauth2/device/code";
static const char kTokenUrl[] = "https://auth.x.ai/oauth2/token";
static const char kFormContent[] = "Content-Type: application/x-www-form-urlencoded";
static const char kAcceptJson[] = "Accept: application/json";
static const char kReferrer[] = "pico";
static const char kDeviceGrant[] = "urn:ietf:params:oauth:grant-type:device_code";

static pthread_mutex_t g_refresh_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_refresh_cv = PTHREAD_COND_INITIALIZER;
static bool g_refreshing;
static PicoLlmCancelFn g_refresh_owner_cancel;
static void *g_refresh_owner_user;

static char *BuildRequest(const PicoLlmTurn *turn)
{
    PicoCompletionsBuildOpts opts = {
        .provider = "xai",
        .store_false = true,
        .thinking = PICO_COMPLETIONS_THINKING_NONE,
    };
    return pico_completions_build_request(turn, &opts);
}

#define PICO_DEVICE_MAX_NOTES 8
#define PICO_DEVICE_TIMEOUT_SEC 900
#define PICO_DEVICE_MAX_TRANSPORT_FAILS 5
#define PICO_XAI_MIN_INTERVAL_SEC 1
#define PICO_XAI_DEFAULT_INTERVAL_SEC 5
#define PICO_XAI_SLOW_DOWN_INC_SEC 5

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

static bool LoginSleep(int seconds)
{
    if (seconds < PICO_XAI_MIN_INTERVAL_SEC)
    {
        seconds = PICO_XAI_MIN_INTERVAL_SEC;
    }
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

static void Note(PicoApp *app, const char *text)
{
    PicoApp_AddMessage(app, PICO_ROLE_ASSISTANT, text);
}

typedef struct TurnCancel {
    PicoLlmCancelFn fn;
    void *user;
} TurnCancel;

static bool TurnCancelled(void *user)
{
    TurnCancel *t = (TurnCancel *)user;
    return t && t->fn && t->fn(t->user);
}

static int PostForm(const char *url, const char *body, PicoHttpCancelFn cancel, void *cancel_user,
                    long *http, char **out, char **err)
{
    PicoHttpReq req;
    memset(&req, 0, sizeof(req));
    req.url = url;
    req.body = body ? body : "";
    req.headers[0] = kFormContent;
    req.headers[1] = kAcceptJson;
    req.header_count = 2;
    req.cancel = cancel;
    req.user = cancel_user;
    return pico_http_post(&req, http, out, err);
}

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

static char *OauthError(const char *body)
{
    if (!body || !body[0])
    {
        return NULL;
    }
    JsonDoc doc;
    if (JsonParse(&doc, body, strlen(body)) != 0)
    {
        return NULL;
    }
    char *error = JsonObjStr(&doc, 0, "error");
    JsonFree(&doc);
    return error;
}

static bool ApplyTokenBody(PicoApp *app, PicoAgentContext *ctx, PicoAuthEntry *auth, const char *body,
                           const char *fallback_refresh)
{
    const char *previous = fallback_refresh;
    if (!previous && auth)
    {
        previous = auth->refresh_token;
    }
    char *access = NULL;
    char *refresh = NULL;
    long expires_at = 0;
    if (!pico_xai_oauth_parse_token(body, previous, time(NULL), &access, &refresh, &expires_at))
    {
        return false;
    }
    bool saved = ctx ? pico_auth_set_oauth_ctx(ctx, "xai", access, refresh, NULL, expires_at)
                     : pico_auth_set_oauth(app, "xai", access, refresh, NULL, expires_at);
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
            pico_auth_copy_ctx(ctx, "xai", auth);
        }
        else
        {
            pico_auth_copy(app, "xai", auth);
        }
    }
    free(access);
    free(refresh);
    return true;
}

static int ExchangeRefreshToken(PicoApp *app, PicoAgentContext *ctx, PicoAuthEntry *auth,
                                const char *refresh_token, TurnCancel *tc)
{
    const char *keys[] = {"grant_type", "client_id", "refresh_token"};
    const char *vals[] = {"refresh_token", kClientId, refresh_token ? refresh_token : ""};
    char *form = pico_http_form_encode(keys, vals, 3);
    long http = 0;
    char *body = NULL;
    char *err = NULL;
    int rc = PostForm(kTokenUrl, form, ctx ? (tc ? TurnCancelled : NULL) : LoginCancelled, ctx ? tc : NULL,
                      &http, &body, &err);
    free(form);
    int result = PICO_HTTP_FAIL;
    if (rc == PICO_HTTP_CANCEL)
    {
        result = PICO_HTTP_CANCEL;
    }
    else if (rc == PICO_HTTP_OK && http < 400 &&
             ApplyTokenBody(app, ctx, auth, body, refresh_token))
    {
        result = PICO_HTTP_OK;
    }
    free(body);
    free(err);
    return result;
}

static bool RefreshOauth(PicoAgentContext *ctx, PicoAuthEntry *auth, TurnCancel *tc, bool force)
{
    if (!ctx || !auth)
    {
        return false;
    }

    pthread_mutex_lock(&g_refresh_mu);
    while (g_refreshing)
    {
        bool owner_abandoned = g_refresh_owner_cancel && g_refresh_owner_cancel(g_refresh_owner_user);
        if (owner_abandoned || (tc && TurnCancelled(tc)) || pico_agent_context_cancelled(ctx))
        {
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
    if (!pico_auth_copy_ctx(ctx, "xai", &latest))
    {
        pthread_mutex_unlock(&g_refresh_mu);
        return false;
    }
    bool rejected_token_replaced =
        force && pico_xai_oauth_token_replaced(auth->access_token, latest.access_token);
    pico_auth_entry_free(auth);
    *auth = latest;
    if (strcmp(auth->active, PICO_AUTH_OAUTH) != 0)
    {
        pthread_mutex_unlock(&g_refresh_mu);
        return false;
    }
    bool refresh_needed =
        pico_xai_oauth_refresh_needed(auth->access_token, auth->expires_at, time(NULL), false);
    if (rejected_token_replaced && !refresh_needed)
    {
        pthread_mutex_unlock(&g_refresh_mu);
        return true;
    }
    if (!force && !refresh_needed)
    {
        pthread_mutex_unlock(&g_refresh_mu);
        return true;
    }
    if (!auth->refresh_token || !auth->refresh_token[0] || (tc && TurnCancelled(tc)) ||
        pico_agent_context_cancelled(ctx))
    {
        pthread_mutex_unlock(&g_refresh_mu);
        return false;
    }
    g_refreshing = true;
    g_refresh_owner_cancel = tc ? tc->fn : NULL;
    g_refresh_owner_user = tc ? tc->user : NULL;
    pthread_mutex_unlock(&g_refresh_mu);

    int rc = ExchangeRefreshToken(NULL, ctx, auth, auth->refresh_token, tc);
    bool ok = rc == PICO_HTTP_OK;

    pthread_mutex_lock(&g_refresh_mu);
    g_refreshing = false;
    g_refresh_owner_cancel = NULL;
    g_refresh_owner_user = NULL;
    pthread_cond_broadcast(&g_refresh_cv);
    pthread_mutex_unlock(&g_refresh_mu);
    return ok;
}

typedef enum DevicePoll {
    DEVICE_PENDING = 0,
    DEVICE_READY,
    DEVICE_SLOW_DOWN,
    DEVICE_UNREACHABLE,
    DEVICE_FAILED,
    DEVICE_CANCELLED,
} DevicePoll;

static DevicePoll PollDeviceOnce(const char *device_code, char **out_body, int *out_interval,
                                 char **out_error)
{
    const char *keys[] = {"grant_type", "client_id", "device_code"};
    const char *vals[] = {kDeviceGrant, kClientId, device_code ? device_code : ""};
    char *form = pico_http_form_encode(keys, vals, 3);
    long http = 0;
    char *body = NULL;
    char *err = NULL;
    int rc = PostForm(kTokenUrl, form, LoginCancelled, NULL, &http, &body, &err);
    free(form);

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
    else if (http < 400)
    {
        *out_body = body;
        body = NULL;
        state = DEVICE_READY;
    }
    else
    {
        char *error = OauthError(body);
        if (error && strcmp(error, "authorization_pending") == 0)
        {
            state = DEVICE_PENDING;
        }
        else if (error && strcmp(error, "slow_down") == 0)
        {
            JsonDoc doc;
            if (body && JsonParse(&doc, body, strlen(body)) == 0)
            {
                int interval = JsonObjInt(&doc, 0, "interval", 0);
                if (out_interval && interval > 0)
                {
                    *out_interval = interval;
                }
                JsonFree(&doc);
            }
            state = DEVICE_SLOW_DOWN;
        }
        else if (error && (strcmp(error, "access_denied") == 0 || strcmp(error, "authorization_denied") == 0))
        {
            *out_error = JsonDup("xAI device authorization was denied");
            state = DEVICE_FAILED;
        }
        else if (error && strcmp(error, "expired_token") == 0)
        {
            *out_error = JsonDup("xAI device code expired");
            state = DEVICE_FAILED;
        }
        else
        {
            *out_error = HttpDetail(body, err, http);
            state = DEVICE_FAILED;
        }
        free(error);
    }
    free(body);
    free(err);
    return state;
}

static int IntervalOf(const JsonDoc *doc, int obj)
{
    int v = JsonObjInt(doc, obj, "interval", 0);
    return v > 0 ? v : PICO_XAI_DEFAULT_INTERVAL_SEC;
}

static bool RequestDeviceAuth(char *device_code, size_t code_cap, char *user_code, size_t user_cap,
                              char *verify_url, size_t url_cap, int *interval, int *expires_in)
{
    const char *keys[] = {"client_id", "scope", "referrer"};
    const char *vals[] = {kClientId, kScope, kReferrer};
    char *form = pico_http_form_encode(keys, vals, 3);
    long http = 0;
    char *body = NULL;
    char *err = NULL;
    int rc = PostForm(kDeviceUrl, form, LoginCancelled, NULL, &http, &body, &err);
    free(form);
    if (rc == PICO_HTTP_CANCEL)
    {
        free(body);
        free(err);
        return false;
    }
    if (rc != PICO_HTTP_OK || http >= 400)
    {
        char *detail = HttpDetail(body, err, http);
        char buf[512];
        snprintf(buf, sizeof(buf), "Could not start xAI login: %s", detail ? detail : "unknown error");
        free(detail);
        LoginNote(buf);
        free(body);
        free(err);
        return false;
    }
    JsonDoc doc;
    if (!body || JsonParse(&doc, body, strlen(body)) != 0)
    {
        LoginNote("Could not start xAI login: bad response.");
        free(body);
        free(err);
        return false;
    }
    char *got_code = JsonObjStr(&doc, 0, "device_code");
    char *got_user = JsonObjStr(&doc, 0, "user_code");
    char *got_uri = JsonObjStr(&doc, 0, "verification_uri");
    char *got_complete = JsonObjStr(&doc, 0, "verification_uri_complete");
    int got_expires = JsonObjInt(&doc, 0, "expires_in", 0);
    bool ok = got_code && got_code[0] && got_user && got_user[0] && pico_xai_https_uri_ok(got_uri) &&
              got_expires > 0;
    if (ok && got_complete && got_complete[0] && !pico_xai_https_uri_ok(got_complete))
    {
        free(got_complete);
        got_complete = NULL;
    }
    if (ok)
    {
        snprintf(device_code, code_cap, "%s", got_code);
        snprintf(user_code, user_cap, "%s", got_user);
        snprintf(verify_url, url_cap, "%s",
                 got_complete && got_complete[0] ? got_complete : got_uri);
        if (interval)
        {
            *interval = IntervalOf(&doc, 0);
        }
        if (expires_in)
        {
            *expires_in = got_expires;
        }
    }
    else if (!pico_xai_https_uri_ok(got_uri))
    {
        LoginNote("Could not start xAI login: untrusted verification URI.");
    }
    else
    {
        LoginNote("Could not start xAI login: missing device code.");
    }
    free(got_code);
    free(got_user);
    free(got_uri);
    free(got_complete);
    JsonFree(&doc);
    free(body);
    free(err);
    return ok;
}

static void *DeviceLoginMain(void *arg)
{
    PicoApp *app = (PicoApp *)arg;
    char device_code[256] = {0};
    char user_code[64] = {0};
    char verify_url[512] = {0};
    int interval = PICO_XAI_DEFAULT_INTERVAL_SEC;
    int expires_in = PICO_DEVICE_TIMEOUT_SEC;
    if (RequestDeviceAuth(device_code, sizeof(device_code), user_code, sizeof(user_code), verify_url,
                          sizeof(verify_url), &interval, &expires_in))
    {
        char msg[768];
        snprintf(msg, sizeof(msg),
                 "Sign in at %s\nEnter code: `%s`\n\nThe code expires in %d minutes. "
                 "`/login xai cancel` to stop.",
                 verify_url, user_code, expires_in / 60);
        LoginNote(msg);

        time_t deadline = time(NULL) + expires_in;
        int fails = 0;
        while (LoginSleep(interval))
        {
            if (time(NULL) >= deadline)
            {
                LoginNote("xAI login timed out. Run `/login xai` to try again.");
                break;
            }
            char *body = NULL;
            int slow_interval = 0;
            char *error = NULL;
            DevicePoll state = PollDeviceOnce(device_code, &body, &slow_interval, &error);
            bool keep_polling = state == DEVICE_PENDING || state == DEVICE_SLOW_DOWN;
            if (state == DEVICE_READY)
            {
                if (ApplyTokenBody(app, NULL, NULL, body, NULL))
                {
                    LoginNote("Signed in with xAI.");
                }
                else
                {
                    LoginNote("xAI token exchange failed. Run `/login xai` to try again.");
                }
            }
            else if (state == DEVICE_PENDING)
            {
                fails = 0;
            }
            else if (state == DEVICE_SLOW_DOWN)
            {
                fails = 0;
                interval = pico_xai_oauth_slow_down_interval(interval, slow_interval);
            }
            else if (state == DEVICE_UNREACHABLE && ++fails < PICO_DEVICE_MAX_TRANSPORT_FAILS)
            {
                keep_polling = true;
            }
            else if (state == DEVICE_UNREACHABLE || state == DEVICE_FAILED)
            {
                char buf[512];
                snprintf(buf, sizeof(buf), "xAI login failed: %s", error ? error : "unknown error");
                LoginNote(buf);
            }
            free(body);
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

static void StartDeviceLogin(PicoApp *app)
{
    StopDeviceLogin();
    pthread_mutex_lock(&g_login.mu);
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
        Note(app, "Could not start xAI login: thread creation failed.");
    }
}

static void DrainLoginNotes(PicoApp *app)
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

static void XaiLogin(PicoApp *app, const char *args)
{
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
        Note(app, "Usage: `/login xai`, `/login xai key`, or `/login xai cancel`.");
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
        pico_auth_copy(app, "xai", &e);
        if (!e.api_key || !e.api_key[0])
        {
            Note(app, "No API key. Set `XAI_API_KEY`.");
            pico_auth_entry_free(&e);
            return;
        }
        if (pico_auth_set_active(app, "xai", PICO_AUTH_API_KEY))
        {
            Note(app, "Using xAI API key.");
        }
        else
        {
            Note(app, "Using xAI API key, but `~/.config/pico/auth.json` could not be written, so "
                      "this choice will not survive a restart.");
        }
        pico_auth_entry_free(&e);
        return;
    }
    StartDeviceLogin(app);
}

static void XaiLogout(PicoApp *app)
{
    StopDeviceLogin();
    bool saved = pico_auth_clear_oauth(app, "xai");
    PicoAuthEntry e;
    pico_auth_copy(app, "xai", &e);
    if (!saved)
    {
        Note(app, "Logged out of xAI, but `~/.config/pico/auth.json` could not be written, so the "
                  "stored tokens may still be on disk.");
    }
    else if (e.api_key && e.api_key[0])
    {
        Note(app, "Logged out of xAI. Using API key.");
    }
    else
    {
        Note(app, "Logged out of xAI.");
    }
    pico_auth_entry_free(&e);
}

static void XaiFrame(PicoApp *app, float dt)
{
    (void)dt;
    DrainLoginNotes(app);
}

static const char *BearerOf(const PicoAuthEntry *auth, bool oauth)
{
    if (oauth)
    {
        return auth->access_token;
    }
    return auth->api_key;
}

static int XaiStream(PicoAgentContext *agent_ctx, const PicoLlmTurn *turn, PicoLlmCancelFn cancel,
                     PicoLlmDeltaFn on_delta, void *user, PicoLlmResult *out)
{
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
    pico_auth_copy_ctx(agent_ctx, "xai", &auth);
    bool oauth = strcmp(auth.active, PICO_AUTH_OAUTH) == 0;
    if (oauth)
    {
        if (pico_xai_oauth_refresh_needed(auth.access_token, auth.expires_at, time(NULL), false))
        {
            if (!RefreshOauth(agent_ctx, &auth, &tc, false))
            {
                pico_auth_entry_free(&auth);
                if (TurnCancelled(&tc))
                {
                    return PICO_LLM_CANCEL;
                }
                out->error = JsonDup(
                    "Your xAI session is no longer valid. Run `/login xai` to re-authenticate.");
                return PICO_LLM_FAIL;
            }
        }
    }
    else if (!auth.api_key || !auth.api_key[0])
    {
        pico_auth_entry_free(&auth);
        out->error = JsonDup("No xAI credentials. Run `/login xai`, or set `XAI_API_KEY`.");
        return PICO_LLM_FAIL;
    }

    char url[1024];
    if (!pico_completions_resolve_canonical_url(turn->base_url, kDefaultBase, url, sizeof(url)))
    {
        pico_auth_entry_free(&auth);
        out->error = JsonDup("xAI requests must use `https://api.x.ai/v1`.");
        return PICO_LLM_FAIL;
    }
    char *body = BuildRequest(turn);
    if (!body)
    {
        pico_auth_entry_free(&auth);
        out->error = JsonDup("failed to build request");
        return PICO_LLM_FAIL;
    }

    const char *bearer = BearerOf(&auth, oauth);
    PicoCompletionsCtx ctx;
    int rc = pico_completions_post(url, body, bearer, NULL, 0, cancel, on_delta, user, &ctx);
    if (rc == PICO_LLM_FAIL && oauth && ctx.http == 401)
    {
        pico_completions_ctx_free(&ctx);
        if (!RefreshOauth(agent_ctx, &auth, &tc, true))
        {
            free(body);
            pico_auth_entry_free(&auth);
            if (TurnCancelled(&tc))
            {
                return PICO_LLM_CANCEL;
            }
            out->error = JsonDup(
                "Your xAI session is no longer valid. Run `/login xai` to re-authenticate.");
            return PICO_LLM_FAIL;
        }
        bearer = BearerOf(&auth, true);
        rc = pico_completions_post(url, body, bearer, NULL, 0, cancel, on_delta, user, &ctx);
    }
    free(body);
    pico_auth_entry_free(&auth);
    if (rc == PICO_LLM_CANCEL)
    {
        return PICO_LLM_CANCEL;
    }
    pico_completions_fill_result(&ctx, out);
    bool saw_text = ctx.saw_text;
    pico_completions_ctx_free(&ctx);
    if (rc != PICO_LLM_OK)
    {
        if (!out->error)
        {
            out->error = JsonDup("LLM request failed");
        }
        return PICO_LLM_FAIL;
    }
    if (!out->error && !saw_text && !pico_llm_result_has_output(out))
    {
        char buf[sizeof(url) + 32];
        snprintf(buf, sizeof(buf), "empty response from %s", url);
        out->error = JsonDup(buf);
        return PICO_LLM_FAIL;
    }
    return PICO_LLM_OK;
}

static void XaiInit(PicoApp *app)
{
    pico_add_provider(app, &(PicoProvider){.name = "xai", .stream = XaiStream, .map_context = true});
    pico_add_auth(app, &(PicoAuth){.provider = "xai",
                                   .help = "xAI device-code or API key",
                                   .verbs = "key cancel",
                                   .login = XaiLogin,
                                   .logout = XaiLogout});
    const char *key = getenv("XAI_API_KEY");
    pico_auth_set_env_key(app, "xai", (key && key[0]) ? key : NULL);
}

static void XaiShutdown(PicoApp *app)
{
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

PicoExt pico_ext_xai(void)
{
    return (PicoExt){
        .abi = PICO_EXT_ABI,
        .name = "xai",
        .description = "xAI provider",
        .init = XaiInit,
        .shutdown = XaiShutdown,
        .on_frame = XaiFrame,
    };
}
