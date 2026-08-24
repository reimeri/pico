#define _POSIX_C_SOURCE 200809L

#include "pico/plugin.h"
#include "pico/http.h"
#include "pico/auth.h"
#include "json.h"
#include "builtins/completions.h"
#include "builtins/hyper_auth.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static const char kDefaultBase[] = "https://hyper.charm.land/v1";
static const char kHyperOrigin[] = "https://hyper.charm.land";
static const char kJsonContent[] = "Content-Type: application/json";

static pthread_mutex_t g_refresh_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_refresh_cv = PTHREAD_COND_INITIALIZER;
static bool g_refreshing;
static PicoLlmCancelFn g_refresh_owner_cancel;
static void *g_refresh_owner_user;

static bool EffortOn(const char *effort)
{
    return effort && effort[0] && strcmp(effort, "none") != 0 && strcmp(effort, "off") != 0;
}

static char *BuildRequest(const PicoLlmTurn *turn)
{
    PicoCompletionsBuildOpts opts = {
        .provider = "hyper",
        .store_false = true,
        .thinking = PICO_COMPLETIONS_THINKING_DEEPSEEK,
        .requires_reasoning_content = EffortOn(turn->effort),
        .max_tokens_field = "max_tokens",
    };
    return pico_completions_build_request(turn, &opts);
}

#define PICO_DEVICE_MAX_NOTES 8
#define PICO_DEVICE_TIMEOUT_SEC 900
#define PICO_DEVICE_MAX_TRANSPORT_FAILS 5
#define PICO_HYPER_MIN_INTERVAL_SEC 1
#define PICO_HYPER_SLOW_DOWN_INC_SEC 5
#define PICO_HYPER_EXPIRY_BUFFER_SEC 30

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
    if (seconds < PICO_HYPER_MIN_INTERVAL_SEC)
    {
        seconds = PICO_HYPER_MIN_INTERVAL_SEC;
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

static bool RejectedRefresh(long http, const char *body)
{
    if (http != 401 || !body || !body[0])
    {
        return false;
    }
    JsonDoc doc;
    if (JsonParse(&doc, body, strlen(body)) != 0)
    {
        return false;
    }
    char *error = JsonObjStr(&doc, 0, "error");
    bool match = error && strcmp(error, "could not get refresh token: not found") == 0;
    free(error);
    JsonFree(&doc);
    return match;
}

static void UrlEncode(JsonBuf *b, const char *s)
{
    static const char kHex[] = "0123456789ABCDEF";
    if (!s)
    {
        return;
    }
    for (; *s; s++)
    {
        unsigned char c = (unsigned char)*s;
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' ||
            c == '_' || c == '.' || c == '~')
        {
            JsonBuf_Putc(b, (char)c);
        }
        else
        {
            JsonBuf_Putc(b, '%');
            JsonBuf_Putc(b, kHex[c >> 4]);
            JsonBuf_Putc(b, kHex[c & 15]);
        }
    }
}

static void DeviceName(char *out, size_t cap)
{
    char host[256];
    if (gethostname(host, sizeof(host) - 1) != 0)
    {
        host[0] = '\0';
    }
    host[sizeof(host) - 1] = '\0';
    if (host[0])
    {
        snprintf(out, cap, "Pico (%s)", host);
    }
    else
    {
        snprintf(out, cap, "Pico");
    }
}

static long TokenExpiry(const JsonDoc *doc, int obj)
{
    time_t now = time(NULL);
    int expires_in = JsonObjInt(doc, obj, "expires_in", 0);
    long expires_at = 0;
    if (expires_in > 0)
    {
        expires_at = (long)now + expires_in;
    }
    else
    {
        expires_at = (long)JsonObjInt(doc, obj, "expires_at", 0);
    }
    if (expires_at <= (long)now)
    {
        return 0;
    }
    long remaining = expires_at - (long)now;
    long buffer = PICO_HYPER_EXPIRY_BUFFER_SEC;
    if (remaining / 2 < buffer)
    {
        buffer = remaining / 2;
    }
    if (buffer < 0)
    {
        buffer = 0;
    }
    return expires_at - buffer;
}

static bool ApplyTokenBody(PicoApp *app, PicoAgentContext *ctx, PicoAuthEntry *auth, const char *body,
                           const char *fallback_refresh, const char *team_id)
{
    JsonDoc doc;
    if (!body || JsonParse(&doc, body, strlen(body)) != 0)
    {
        return false;
    }
    char *access = JsonObjStr(&doc, 0, "access_token");
    char *refresh = JsonObjStr(&doc, 0, "refresh_token");
    char *got_team = JsonObjStr(&doc, 0, "team_id");
    long expires_at = TokenExpiry(&doc, 0);
    bool ok = access && access[0] && expires_at > 0;
    if (ok)
    {
        const char *use_refresh = (refresh && refresh[0]) ? refresh : fallback_refresh;
        if (!use_refresh && auth)
        {
            use_refresh = auth->refresh_token;
        }
        const char *account = (got_team && got_team[0]) ? got_team : team_id;
        if (!account && auth)
        {
            account = auth->account_id;
        }
        bool saved = ctx ? pico_auth_set_oauth_ctx(ctx, "hyper", access, use_refresh, account, expires_at)
                         : pico_auth_set_oauth(app, "hyper", access, use_refresh, account, expires_at);
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
                pico_auth_copy_ctx(ctx, "hyper", auth);
            }
            else
            {
                pico_auth_copy(app, "hyper", auth);
            }
        }
    }
    free(access);
    free(refresh);
    free(got_team);
    JsonFree(&doc);
    return ok;
}

static int ExchangeRefreshToken(PicoApp *app, PicoAgentContext *ctx, PicoAuthEntry *auth,
                                const char *refresh_token, const char *team_id, TurnCancel *tc)
{
    JsonBuf b;
    JsonBuf_Init(&b);
    JsonBuf_Puts(&b, "{\"refresh_token\":");
    JsonBuf_String(&b, refresh_token ? refresh_token : "");
    JsonBuf_Putc(&b, '}');
    char *req_body = JsonBuf_Steal(&b);
    char url[256];
    snprintf(url, sizeof(url), "%s/token/exchange", kHyperOrigin);
    PicoHttpReq req;
    memset(&req, 0, sizeof(req));
    req.url = url;
    req.body = req_body;
    req.headers[0] = kJsonContent;
    req.header_count = 1;
    req.cancel = ctx ? (tc ? TurnCancelled : NULL) : LoginCancelled;
    req.user = ctx ? tc : NULL;
    long http = 0;
    char *body = NULL;
    char *err = NULL;
    int rc = pico_http_post(&req, &http, &body, &err);
    free(req_body);
    int result = PICO_HTTP_FAIL;
    if (rc == PICO_HTTP_CANCEL)
    {
        result = PICO_HTTP_CANCEL;
    }
    else if (rc == PICO_HTTP_OK && http < 400 &&
             ApplyTokenBody(app, ctx, auth, body, refresh_token, team_id))
    {
        result = PICO_HTTP_OK;
    }
    else if (RejectedRefresh(http, body))
    {
        result = -401;
    }
    else
    {
        result = PICO_HTTP_FAIL;
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
    if (!pico_auth_copy_ctx(ctx, "hyper", &latest))
    {
        pthread_mutex_unlock(&g_refresh_mu);
        return false;
    }
    bool rejected_token_replaced =
        force && pico_hyper_oauth_token_replaced(auth->access_token, latest.access_token);
    pico_auth_entry_free(auth);
    *auth = latest;
    if (strcmp(auth->active, PICO_AUTH_OAUTH) != 0)
    {
        pthread_mutex_unlock(&g_refresh_mu);
        return false;
    }
    bool refresh_needed =
        pico_hyper_oauth_refresh_needed(auth->access_token, auth->expires_at, time(NULL), false);
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

    int rc = ExchangeRefreshToken(NULL, ctx, auth, auth->refresh_token, auth->account_id, tc);
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

static DevicePoll PollDeviceOnce(const char *device_code, char **out_refresh, char **out_team,
                                 int *out_interval, char **out_error)
{
    JsonBuf path;
    JsonBuf_Init(&path);
    JsonBuf_Puts(&path, kHyperOrigin);
    JsonBuf_Puts(&path, "/device/auth/");
    UrlEncode(&path, device_code);
    char *url = JsonBuf_Steal(&path);
    PicoHttpReq req;
    memset(&req, 0, sizeof(req));
    req.url = url;
    req.headers[0] = kJsonContent;
    req.header_count = 1;
    req.cancel = LoginCancelled;
    long http = 0;
    char *body = NULL;
    char *err = NULL;
    int rc = pico_http_get(&req, &http, &body, &err);
    free(url);

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
    else if (body && body[0])
    {
        JsonDoc doc;
        if (JsonParse(&doc, body, strlen(body)) == 0)
        {
            char *refresh = JsonObjStr(&doc, 0, "refresh_token");
            if (refresh && refresh[0])
            {
                *out_refresh = refresh;
                *out_team = JsonObjStr(&doc, 0, "team_id");
                state = DEVICE_READY;
            }
            else
            {
                free(refresh);
                char *error = JsonObjStr(&doc, 0, "error");
                if (error && strcmp(error, "authorization_pending") == 0)
                {
                    state = DEVICE_PENDING;
                }
                else if (error && strcmp(error, "slow_down") == 0)
                {
                    int interval = JsonObjInt(&doc, 0, "interval", 0);
                    if (out_interval && interval > 0)
                    {
                        *out_interval = interval;
                    }
                    state = DEVICE_SLOW_DOWN;
                }
                else
                {
                    char *desc = JsonObjStr(&doc, 0, "error_description");
                    *out_error = desc && desc[0] ? desc : JsonDup(error ? error : "unknown error");
                    if (desc && desc[0])
                    {
                        /* owned by *out_error */
                    }
                    else
                    {
                        free(desc);
                    }
                    state = DEVICE_FAILED;
                }
                free(error);
            }
            JsonFree(&doc);
        }
        else if (http >= 400)
        {
            *out_error = HttpDetail(body, err, http);
            state = DEVICE_FAILED;
        }
    }
    else if (http >= 400)
    {
        *out_error = HttpDetail(body, err, http);
        state = DEVICE_FAILED;
    }
    free(body);
    free(err);
    return state;
}

static bool RequestDeviceAuth(char *device_code, size_t code_cap, char *user_code, size_t user_cap,
                              char *verify_url, size_t url_cap, int *interval, int *expires_in)
{
    char name[320];
    DeviceName(name, sizeof(name));
    JsonBuf b;
    JsonBuf_Init(&b);
    JsonBuf_Puts(&b, "{\"device_name\":");
    JsonBuf_String(&b, name);
    JsonBuf_Putc(&b, '}');
    char *req_body = JsonBuf_Steal(&b);
    char url[256];
    snprintf(url, sizeof(url), "%s/device/auth", kHyperOrigin);
    PicoHttpReq req;
    memset(&req, 0, sizeof(req));
    req.url = url;
    req.body = req_body;
    req.headers[0] = kJsonContent;
    req.header_count = 1;
    req.cancel = LoginCancelled;
    long http = 0;
    char *body = NULL;
    char *err = NULL;
    int rc = pico_http_post(&req, &http, &body, &err);
    free(req_body);
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
        snprintf(buf, sizeof(buf), "Could not start Hyper login: %s", detail ? detail : "unknown error");
        free(detail);
        LoginNote(buf);
        free(body);
        free(err);
        return false;
    }
    JsonDoc doc;
    if (!body || JsonParse(&doc, body, strlen(body)) != 0)
    {
        LoginNote("Could not start Hyper login: bad response.");
        free(body);
        free(err);
        return false;
    }
    char *got_code = JsonObjStr(&doc, 0, "device_code");
    char *got_user = JsonObjStr(&doc, 0, "user_code");
    char *got_url = JsonObjStr(&doc, 0, "verification_url");
    bool ok = got_code && got_code[0] && got_user && got_user[0] && got_url && got_url[0];
    if (ok)
    {
        snprintf(device_code, code_cap, "%s", got_code);
        snprintf(user_code, user_cap, "%s", got_user);
        snprintf(verify_url, url_cap, "%s", got_url);
        *interval = JsonObjInt(&doc, 0, "interval", 5);
        if (*interval < PICO_HYPER_MIN_INTERVAL_SEC)
        {
            *interval = PICO_HYPER_MIN_INTERVAL_SEC;
        }
        *expires_in = JsonObjInt(&doc, 0, "expires_in", PICO_DEVICE_TIMEOUT_SEC);
        if (*expires_in < 1)
        {
            *expires_in = PICO_DEVICE_TIMEOUT_SEC;
        }
    }
    else
    {
        LoginNote("Could not start Hyper login: missing device code.");
    }
    free(got_code);
    free(got_user);
    free(got_url);
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
    int interval = 5;
    int expires_in = PICO_DEVICE_TIMEOUT_SEC;
    if (RequestDeviceAuth(device_code, sizeof(device_code), user_code, sizeof(user_code), verify_url,
                          sizeof(verify_url), &interval, &expires_in))
    {
        char msg[768];
        snprintf(msg, sizeof(msg),
                 "Sign in at %s\nEnter code: `%s`\n\nThe code expires in %d minutes. "
                 "`/login hyper cancel` to stop.",
                 verify_url, user_code, expires_in / 60);
        LoginNote(msg);

        time_t deadline = time(NULL) + expires_in;
        int fails = 0;
        while (LoginSleep(interval))
        {
            if (time(NULL) >= deadline)
            {
                LoginNote("Hyper login timed out. Run `/login hyper` to try again.");
                break;
            }
            char *refresh = NULL;
            char *team = NULL;
            int slow_interval = 0;
            char *error = NULL;
            DevicePoll state = PollDeviceOnce(device_code, &refresh, &team, &slow_interval, &error);
            bool keep_polling = state == DEVICE_PENDING || state == DEVICE_SLOW_DOWN;
            if (state == DEVICE_READY)
            {
                int rc = ExchangeRefreshToken(app, NULL, NULL, refresh, team, NULL);
                if (rc == PICO_HTTP_OK)
                {
                    LoginNote("Signed in with Charm Hyper.");
                }
                else if (rc != PICO_HTTP_CANCEL)
                {
                    LoginNote("Hyper token exchange failed. Run `/login hyper` to try again.");
                }
            }
            else if (state == DEVICE_PENDING)
            {
                fails = 0;
            }
            else if (state == DEVICE_SLOW_DOWN)
            {
                fails = 0;
                if (slow_interval > 0)
                {
                    interval = slow_interval;
                }
                else
                {
                    interval += PICO_HYPER_SLOW_DOWN_INC_SEC;
                }
                if (interval < PICO_HYPER_MIN_INTERVAL_SEC)
                {
                    interval = PICO_HYPER_MIN_INTERVAL_SEC;
                }
            }
            else if (state == DEVICE_UNREACHABLE && ++fails < PICO_DEVICE_MAX_TRANSPORT_FAILS)
            {
                keep_polling = true;
            }
            else if (state == DEVICE_UNREACHABLE || state == DEVICE_FAILED)
            {
                char buf[512];
                snprintf(buf, sizeof(buf), "Hyper login failed: %s", error ? error : "unknown error");
                LoginNote(buf);
            }
            free(refresh);
            free(team);
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
        Note(app, "Could not start Hyper login: thread creation failed.");
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

static void HyperLogin(PicoApp *app, const char *args)
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
        Note(app, "Usage: `/login hyper`, `/login hyper key`, or `/login hyper cancel`.");
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
        pico_auth_copy(app, "hyper", &e);
        if (!e.api_key || !e.api_key[0])
        {
            Note(app, "No API key. Set `HYPER_API_KEY`.");
            pico_auth_entry_free(&e);
            return;
        }
        if (pico_auth_set_active(app, "hyper", PICO_AUTH_API_KEY))
        {
            Note(app, "Using Hyper API key.");
        }
        else
        {
            Note(app, "Using Hyper API key, but `~/.config/pico/auth.json` could not be written, so "
                      "this choice will not survive a restart.");
        }
        pico_auth_entry_free(&e);
        return;
    }
    StartDeviceLogin(app);
}

static void HyperLogout(PicoApp *app)
{
    StopDeviceLogin();
    bool saved = pico_auth_clear_oauth(app, "hyper");
    PicoAuthEntry e;
    pico_auth_copy(app, "hyper", &e);
    if (!saved)
    {
        Note(app, "Logged out of Hyper, but `~/.config/pico/auth.json` could not be written, so the "
                  "stored tokens may still be on disk.");
    }
    else if (e.api_key && e.api_key[0])
    {
        Note(app, "Logged out of Hyper. Using API key.");
    }
    else
    {
        Note(app, "Logged out of Hyper.");
    }
    pico_auth_entry_free(&e);
}

static void HyperFrame(PicoApp *app, float dt)
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

static int HyperStream(PicoAgentContext *agent_ctx, const PicoLlmTurn *turn, PicoLlmCancelFn cancel,
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
    pico_auth_copy_ctx(agent_ctx, "hyper", &auth);
    bool oauth = strcmp(auth.active, PICO_AUTH_OAUTH) == 0;
    if (oauth)
    {
        if (pico_hyper_oauth_refresh_needed(auth.access_token, auth.expires_at, time(NULL), false))
        {
            if (!RefreshOauth(agent_ctx, &auth, &tc, false))
            {
                pico_auth_entry_free(&auth);
                if (TurnCancelled(&tc))
                {
                    return PICO_LLM_CANCEL;
                }
                out->error = JsonDup(
                    "Your Hyper session is no longer valid. Run `/login hyper` to re-authenticate.");
                return PICO_LLM_FAIL;
            }
        }
    }
    else if (!auth.api_key || !auth.api_key[0])
    {
        pico_auth_entry_free(&auth);
        out->error = JsonDup("No Hyper credentials. Run `/login hyper`, or set `HYPER_API_KEY`.");
        return PICO_LLM_FAIL;
    }

    char url[1024];
    if (!pico_completions_resolve_canonical_url(turn->base_url, kDefaultBase, url, sizeof(url)))
    {
        pico_auth_entry_free(&auth);
        out->error = JsonDup("Hyper requests must use `https://hyper.charm.land/v1`.");
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
                "Your Hyper session is no longer valid. Run `/login hyper` to re-authenticate.");
            return PICO_LLM_FAIL;
        }
        bearer = BearerOf(&auth, true);
        rc = pico_completions_post(url, body, bearer, NULL, 0, cancel, on_delta, user, &ctx);
    }
    if (rc == PICO_LLM_FAIL && ctx.error && strstr(ctx.error, "easoning"))
    {
        char *stripped = pico_completions_body_without_thinking(body);
        if (stripped)
        {
            pico_completions_ctx_free(&ctx);
            rc = pico_completions_post(url, stripped, BearerOf(&auth, oauth), NULL, 0, cancel, on_delta,
                                       user, &ctx);
            free(stripped);
        }
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

static void HyperInit(PicoApp *app)
{
    pico_add_provider(app, &(PicoProvider){.name = "hyper", .stream = HyperStream});
    pico_add_auth(app, &(PicoAuth){.provider = "hyper",
                                   .help = "Hyper device-code or API key",
                                   .verbs = "key cancel",
                                   .login = HyperLogin,
                                   .logout = HyperLogout});
    const char *key = getenv("HYPER_API_KEY");
    pico_auth_set_env_key(app, "hyper", (key && key[0]) ? key : NULL);
}

static void HyperShutdown(PicoApp *app)
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

PicoExt pico_ext_hyper(void)
{
    return (PicoExt){
        .abi = PICO_EXT_ABI,
        .name = "hyper",
        .description = "Charm Hyper provider",
        .init = HyperInit,
        .shutdown = HyperShutdown,
        .on_frame = HyperFrame,
    };
}
