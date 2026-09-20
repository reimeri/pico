/* Included by host_workspace_test.c: exercise auth registrations and observable
 * state without network credentials, actual browsers, or fixed test ports. */
#include "pico/auth.h"
#include "json.h"
#include "builtins/openai_auth.h"
#include <arpa/inet.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <signal.h>

static pthread_mutex_t oauth_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t oauth_cv = PTHREAD_COND_INITIALIZER;
static struct {
    bool active, random_failure, hold_token;
    int callbacks, launches, binds, tokens, device_requests;
    unsigned short port;
    char state[128];
    char *url, *form;
    const char *token_response;
} oauth_test;

static const char oauth_good_token[] =
    "{\"access_token\":\"synthetic-access\",\"refresh_token\":\"synthetic-refresh\","
    "\"token_type\":\"Bearer\",\"expires_in\":3600}";

int __real_RAND_bytes(unsigned char *buf, int n);
int __wrap_RAND_bytes(unsigned char *buf, int n)
{
    pthread_mutex_lock(&oauth_mu);
    bool fail = oauth_test.active && oauth_test.random_failure;
    pthread_mutex_unlock(&oauth_mu);
    return fail ? 0 : __real_RAND_bytes(buf, n);
}

bool __real_PicoHost_OpenBrowser(PicoHost *host, const char *url);
bool __wrap_PicoHost_OpenBrowser(PicoHost *host, const char *url)
{
    pthread_mutex_lock(&oauth_mu);
    bool active = oauth_test.active;
    if (active)
    {
        oauth_test.launches++;
        free(oauth_test.url);
        oauth_test.url = strdup(url);
    }
    pthread_mutex_unlock(&oauth_mu);
    return active ? true : __real_PicoHost_OpenBrowser(host, url);
}

int __real_pico_openai_listen(const unsigned short *ports, size_t count, unsigned short *port);
int __wrap_pico_openai_listen(const unsigned short *ports, size_t count, unsigned short *port)
{
    pthread_mutex_lock(&oauth_mu);
    bool active = oauth_test.active;
    if (active) oauth_test.binds++;
    pthread_mutex_unlock(&oauth_mu);
    unsigned short ephemeral = 0;
    return active ? __real_pico_openai_listen(&ephemeral, 1, port) :
                    __real_pico_openai_listen(ports, count, port);
}

PicoOpenAiCallback __real_pico_openai_await_callback(int listener, int wake_fd, double deadline,
                                                     const char *state, PicoHttpCancelFn cancel,
                                                     void *user, char **code);
PicoOpenAiCallback __wrap_pico_openai_await_callback(int listener, int wake_fd, double deadline,
                                                     const char *state, PicoHttpCancelFn cancel,
                                                     void *user, char **code)
{
    pthread_mutex_lock(&oauth_mu);
    if (oauth_test.active)
    {
        struct sockaddr_in address;
        socklen_t size = sizeof(address);
        getsockname(listener, (struct sockaddr *)&address, &size);
        oauth_test.port = ntohs(address.sin_port);
        snprintf(oauth_test.state, sizeof(oauth_test.state), "%s", state);
        oauth_test.callbacks++;
    }
    pthread_mutex_unlock(&oauth_mu);
    return __real_pico_openai_await_callback(listener, wake_fd, deadline, state, cancel, user, code);
}

int __real_pico_http_post(const PicoHttpReq *req, long *http, char **body, char **error);
int __wrap_pico_http_post(const PicoHttpReq *req, long *http, char **body, char **error)
{
    pthread_mutex_lock(&oauth_mu);
    if (!oauth_test.active || strncmp(req->url, "https://auth.openai.com/", 24) != 0)
    {
        pthread_mutex_unlock(&oauth_mu);
        return __real_pico_http_post(req, http, body, error);
    }
    *http = 200;
    *error = NULL;
    if (strstr(req->url, "/deviceauth/usercode"))
    {
        oauth_test.device_requests++;
        *body = strdup("{\"device_auth_id\":\"device-id\",\"user_code\":\"123456\",\"interval\":1}");
    }
    else if (strstr(req->url, "/deviceauth/token"))
        *body = strdup("{\"authorization_code\":\"synthetic-code\",\"code_verifier\":\"device-verifier\"}");
    else
    {
        oauth_test.tokens++;
        free(oauth_test.form);
        oauth_test.form = strdup(req->body);
        /* Deliberately allow a successful response AFTER cancellation. Production
         * must suppress the stale result, not rely on the transport to do so. */
        while (oauth_test.hold_token) pthread_cond_wait(&oauth_cv, &oauth_mu);
        *body = strdup(oauth_test.token_response);
    }
    pthread_mutex_unlock(&oauth_mu);
    return PICO_HTTP_OK;
}

static int OauthCount(const int *value)
{
    pthread_mutex_lock(&oauth_mu);
    int result = *value;
    pthread_mutex_unlock(&oauth_mu);
    return result;
}

static bool OauthWait(PicoHost *host, const int *value, int minimum, bool pump)
{
    for (int i = 0; i < 3000; i++)
    {
        if (OauthCount(value) >= minimum) return true;
        if (pump) pico_host_pump(host);
        usleep(1000);
    }
    return false;
}

static bool OauthDrain(PicoHost *host)
{
    for (int i = 0; i < 3000; i++)
    {
        pico_host_pump(host);
        if (!host->tasks) return true;
        usleep(1000);
    }
    return false;
}

static PicoAuth *OauthRegistration(PicoHost *host)
{
    for (int i = 0; i < host->auth_count; i++)
        if (strcmp(host->auths[i].provider, "openai") == 0) return &host->auths[i];
    return NULL;
}

static bool OauthMessage(PicoHost *host, PicoAgentId id, const char *text)
{
    PicoAgent *agent = PicoHost_FindAgent(host, id);
    for (int i = 0; agent && i < agent->message_count; i++)
        if (agent->messages[i].source && strstr(agent->messages[i].source, text)) return true;
    return false;
}

static bool OauthSendCallback(void)
{
    pthread_mutex_lock(&oauth_mu);
    struct sockaddr_in address = {.sin_family = AF_INET, .sin_port = htons(oauth_test.port),
                                  .sin_addr = {.s_addr = htonl(INADDR_LOOPBACK)}};
    char request[512];
    snprintf(request, sizeof(request), "GET /auth/callback?state=%s&code=synthetic-code HTTP/1.1\r\nHost: localhost\r\n\r\n", oauth_test.state);
    pthread_mutex_unlock(&oauth_mu);
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return false;
    struct timeval timeout = {.tv_sec = 2};
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    bool ok = connect(fd, (struct sockaddr *)&address, sizeof(address)) == 0 &&
              send(fd, request, strlen(request), MSG_NOSIGNAL) == (ssize_t)strlen(request);
    char response[1024];
    if (ok) ok = recv(fd, response, sizeof(response), 0) > 0;
    close(fd);
    return ok;
}

static bool OauthCredentials(PicoHost *host, bool oauth)
{
    PicoAuthEntry entry;
    pico_auth_copy(host, "openai", &entry);
    bool ok = oauth ? entry.access_token && strcmp(entry.access_token, "synthetic-access") == 0 &&
                      entry.refresh_token && strcmp(entry.refresh_token, "synthetic-refresh") == 0 &&
                      strcmp(entry.active, PICO_AUTH_OAUTH) == 0 :
                      !entry.access_token && strcmp(entry.active, PICO_AUTH_API_KEY) == 0;
    pico_auth_entry_free(&entry);
    return ok;
}

static int TestOpenAiLogin(void)
{
    char config[] = "/tmp/pico-oauth-config-XXXXXX";
    char workspace[] = "/tmp/pico-oauth-workspace-XXXXXX";
    if (!mkdtemp(config) || !mkdtemp(workspace)) return 1;
    char *old_config = getenv("XDG_CONFIG_HOME") ? strdup(getenv("XDG_CONFIG_HOME")) : NULL;
    setenv("XDG_CONFIG_HOME", config, 1);
    char *old_key = getenv("PICO_API_KEY") ? strdup(getenv("PICO_API_KEY")) : NULL;
    setenv("PICO_API_KEY", "synthetic-api-key", 1);
    PicoHost *host = NULL;
    PicoWorkspaceId ws = 0;
    PicoAgentId origin = 0, other = 0;
    PicoAgentCreateOptions options = {.kind = PICO_AGENT_MAIN, .session_start = PICO_SESSION_NONE};
    bool ok = pico_host_init(&host, NULL, true) == PICO_OK &&
              pico_workspace_open(host, workspace, &ws) == PICO_OK &&
              pico_main_agent_create(host, ws, &options, &origin) == PICO_OK &&
              pico_main_agent_create(host, ws, &options, &other) == PICO_OK;
    if (!ok) { Fail("OpenAI login fixture"); return 1; }
    WaitPluginLoad(host);
    PicoAuth *auth = OauthRegistration(host);
    if (!auth) { Fail("OpenAI auth registration"); return 1; }
    pico_auth_set_env_key(host, "openai", "synthetic-api-key");
    pico_auth_set_active(host, "openai", PICO_AUTH_API_KEY);
    pthread_mutex_lock(&oauth_mu);
    oauth_test.active = true;
    oauth_test.token_response = oauth_good_token;
    pthread_mutex_unlock(&oauth_mu);

    /* Default browser login, real loopback callback, fake token HTTP exchange. */
    auth->login(host, origin, "", auth->state);
    pico_agent_select(host, other);
    ok &= OauthWait(host, &oauth_test.launches, 1, true) &&
          OauthWait(host, &oauth_test.callbacks, 1, false) && OauthSendCallback() && OauthDrain(host);
    ok &= OauthCredentials(host, true) && OauthMessage(host, origin, "Signed in with ChatGPT") &&
          !OauthMessage(host, other, "Signed in with ChatGPT") &&
          !OauthMessage(host, origin, "synthetic-access") && !OauthMessage(host, origin, "synthetic-code");
    pthread_mutex_lock(&oauth_mu);
    char *redirect = oauth_test.url ? strstr(oauth_test.url, "redirect_uri=") : NULL;
    char *verifier = oauth_test.form ? strstr(oauth_test.form, "code_verifier=") : NULL;
    char challenge[44];
    char redirect_field[256] = {0};
    if (redirect) snprintf(redirect_field, sizeof(redirect_field), "%.*s", (int)strcspn(redirect, "&"), redirect);
    ok &= redirect && oauth_test.form && strstr(oauth_test.form, redirect_field) && verifier &&
          pico_openai_pkce_challenge(verifier + strlen("code_verifier="), challenge) &&
          strstr(oauth_test.url, challenge) && strstr(oauth_test.form, "code=synthetic-code");
    pthread_mutex_unlock(&oauth_mu);
    if (!ok) Fail("browser login must exchange matching PKCE/redirect and commit only to originating agent");

    /* Queued ready event must not open an obsolete page after explicit cancel. */
    int callbacks = OauthCount(&oauth_test.callbacks), launches = OauthCount(&oauth_test.launches);
    auth->login(host, origin, "browser", auth->state);
    ok = OauthWait(host, &oauth_test.callbacks, callbacks + 1, false);
    auth->login(host, origin, "cancel", auth->state);
    ok &= OauthDrain(host) && OauthCount(&oauth_test.launches) == launches && OauthCredentials(host, true);
    if (!ok) Fail("cancel must discard queued browser launches without dropping existing credentials");

    /* Logout while exchange is blocked cannot be undone by its late response. */
    callbacks = OauthCount(&oauth_test.callbacks);
    int tokens = OauthCount(&oauth_test.tokens);
    pthread_mutex_lock(&oauth_mu); oauth_test.hold_token = true; pthread_mutex_unlock(&oauth_mu);
    auth->login(host, origin, "browser", auth->state);
    ok = OauthWait(host, &oauth_test.callbacks, callbacks + 1, true) && OauthSendCallback() &&
         OauthWait(host, &oauth_test.tokens, tokens + 1, false);
    if (!ok) Fail("logout fixture must reach the blocked token exchange");
    /* Returning before we release the exchange proves logout does not wait for
     * transport. Do not impose a latency budget: logout also persists credentials
     * with fsync, whose duration depends on disk load in the build environment.
     * The alarm is only a deadlock watchdog for a regressed synchronous wait. */
    alarm(30);
    auth->logout(host, origin, auth->state);
    alarm(0);
    if (!OauthCredentials(host, false)) Fail("logout must clear OAuth credentials before transport completes");
    pthread_mutex_lock(&oauth_mu); oauth_test.hold_token = false; pthread_cond_broadcast(&oauth_cv); pthread_mutex_unlock(&oauth_mu);
    if (!OauthDrain(host)) Fail("released token exchange must finish after logout");
    if (!OauthCredentials(host, false)) Fail("stale token completion must not log back in after logout");

    /* Host replacement invalidates pending launches as well as old auth state. */
    callbacks = OauthCount(&oauth_test.callbacks); launches = OauthCount(&oauth_test.launches);
    auth->login(host, origin, "browser", auth->state);
    ok = OauthWait(host, &oauth_test.callbacks, callbacks + 1, false);
    PicoHost_RequestReload(host);
    pico_host_pump(host);
    auth = OauthRegistration(host);
    ok &= auth && OauthDrain(host) && OauthCount(&oauth_test.launches) == launches && OauthCredentials(host, false);
    if (!ok) Fail("host reload cancels login and suppresses stale browser launch");
    if (!auth) return 1;

    /* Secure entropy failure is fail-closed all the way through the command. */
    int binds = OauthCount(&oauth_test.binds);
    launches = OauthCount(&oauth_test.launches); tokens = OauthCount(&oauth_test.tokens);
    pthread_mutex_lock(&oauth_mu); oauth_test.random_failure = true; pthread_mutex_unlock(&oauth_mu);
    auth->login(host, origin, "browser", auth->state);
    ok = OauthDrain(host) && OauthCount(&oauth_test.binds) == binds &&
         OauthCount(&oauth_test.launches) == launches && OauthCount(&oauth_test.tokens) == tokens;
    pthread_mutex_lock(&oauth_mu); oauth_test.random_failure = false; pthread_mutex_unlock(&oauth_mu);
    if (!ok) Fail("randomness failure must not bind, launch, or exchange tokens");

    /* Malformed credentials cannot replace working API-key selection. Each
     * case protects a distinct validation rule at the auth-store boundary. */
    const char *invalid_tokens[] = {
        "{\"access_token\":\"incomplete\",\"expires_in\":3600}",
        "{\"access_token\":42,\"refresh_token\":\"r\",\"expires_in\":3600}",
        "{\"access_token\":\"a\",\"refresh_token\":\"r\",\"expires_in\":0}",
        "{\"access_token\":\"a\",\"refresh_token\":\"r\",\"expires_in\":\"3600junk\"}",
        "{\"access_token\":\"a\",\"refresh_token\":\"r\",\"expires_in\":3600,\"token_type\":\"MAC\"}",
    };
    for (size_t i = 0; i < sizeof(invalid_tokens) / sizeof(invalid_tokens[0]); i++)
    {
        pthread_mutex_lock(&oauth_mu);
        oauth_test.token_response = invalid_tokens[i];
        pthread_mutex_unlock(&oauth_mu);
        callbacks = OauthCount(&oauth_test.callbacks);
        auth->login(host, origin, "browser", auth->state);
        ok = OauthWait(host, &oauth_test.callbacks, callbacks + 1, true) && OauthSendCallback() &&
             OauthDrain(host) && OauthCredentials(host, false);
        if (!ok) Fail("invalid initial token response must not replace existing credentials");
    }

    /* Explicit device mode retains its own redirect and never opens a browser. */
    pthread_mutex_lock(&oauth_mu); oauth_test.token_response = oauth_good_token; pthread_mutex_unlock(&oauth_mu);
    binds = OauthCount(&oauth_test.binds); launches = OauthCount(&oauth_test.launches);
    auth->login(host, origin, "device", auth->state);
    ok = OauthDrain(host) && OauthCredentials(host, true) && OauthCount(&oauth_test.binds) == binds &&
         OauthCount(&oauth_test.launches) == launches && OauthCount(&oauth_test.device_requests) == 1;
    pthread_mutex_lock(&oauth_mu);
    ok &= oauth_test.form && strstr(oauth_test.form, "redirect_uri=https%3A%2F%2Fauth.openai.com%2Fdeviceauth%2Fcallback");
    pthread_mutex_unlock(&oauth_mu);
    if (!ok) Fail("explicit device login must retain device exchange without browser work");
    auth->login(host, origin, "key", auth->state);
    PicoAuthEntry entry;
    pico_auth_copy(host, "openai", &entry);
    if (strcmp(entry.active, PICO_AUTH_API_KEY) != 0) Fail("key mode selects API-key auth");
    pico_auth_entry_free(&entry);

    /* Clean shutdown also cancels a pending callback before curl teardown. */
    callbacks = OauthCount(&oauth_test.callbacks);
    auth->login(host, origin, "browser", auth->state);
    ok = OauthWait(host, &oauth_test.callbacks, callbacks + 1, false);
    ok &= pico_host_free(host) == PICO_HOST_SHUTDOWN_CLEAN;
    if (!ok) Fail("host shutdown must quiesce a pending browser login");
    pthread_mutex_lock(&oauth_mu);
    free(oauth_test.url); free(oauth_test.form);
    memset(&oauth_test, 0, sizeof(oauth_test));
    pthread_mutex_unlock(&oauth_mu);
    if (old_config) { setenv("XDG_CONFIG_HOME", old_config, 1); free(old_config); }
    else unsetenv("XDG_CONFIG_HOME");
    if (old_key) { setenv("PICO_API_KEY", old_key, 1); free(old_key); }
    else unsetenv("PICO_API_KEY");
    RmRf(config); RmRf(workspace);
    return g_failed ? 1 : 0;
}

static int TestOpenAiBlockedShutdownChild(void)
{
    alarm(10);
    char config[] = "/tmp/pico-oauth-retained-XXXXXX";
    if (!mkdtemp(config)) return 1;
    setenv("XDG_CONFIG_HOME", config, 1);
    PicoHost *host = NULL;
    if (pico_host_init(&host, NULL, true) != PICO_OK) return 1;
    WaitPluginLoad(host);
    PicoAuth *auth = OauthRegistration(host);
    if (!auth) return 1;
    pthread_mutex_lock(&oauth_mu);
    oauth_test.active = true;
    oauth_test.hold_token = true;
    oauth_test.token_response = oauth_good_token;
    pthread_mutex_unlock(&oauth_mu);
    auth->login(host, 0, "browser", auth->state);
    if (!OauthWait(host, &oauth_test.callbacks, 1, true) || !OauthSendCallback() ||
        !OauthWait(host, &oauth_test.tokens, 1, false)) return 1;
    PicoHostShutdownResult result = pico_host_free(host);
    PicoHost *replacement = NULL;
    bool ok = result == PICO_HOST_SHUTDOWN_RETAINED &&
              pico_host_init(&replacement, NULL, true) != PICO_OK;
    /* Leave the deliberately blocked worker to process exit, as required by a
     * retained shutdown. In particular no host/auth/curl teardown is allowed. */
    RmRf(config);
    return ok ? 0 : 1;
}

static int TestOpenAiBlockedShutdown(void)
{
    pid_t child = fork();
    if (child == 0)
    {
        execl("/proc/self/exe", "pico_host_workspace_tests", "--openai-retained-shutdown", (char *)NULL);
        _exit(127);
    }
    int status = 0;
    pid_t result;
    do { result = child > 0 ? waitpid(child, &status, 0) : -1; } while (result < 0 && errno == EINTR);
    if (result < 0 || !WIFEXITED(status) || WEXITSTATUS(status) != 0)
    {
        Fail("a blocked login exchange must retain host/auth/curl state at process shutdown");
        return 1;
    }
    return 0;
}

static int TestOpenAiBrowserLauncher(void)
{
    char directory[] = "/tmp/pico-browser-launch-XXXXXX";
    if (!mkdtemp(directory)) return 1;
    char executable[512], marker[512], release[512];
    snprintf(executable, sizeof(executable), "%s/xdg-open", directory);
    snprintf(marker, sizeof(marker), "%s/entered", directory);
    snprintf(release, sizeof(release), "%s/release", directory);
    const char *script = "#!/bin/sh\n"
        "printf '%s\\n%s\\n' \"$$\" \"$1\" > \"$PICO_BROWSER_TEST_MARKER\"\n"
        "while [ ! -f \"$PICO_BROWSER_TEST_RELEASE\" ]; do sleep 0.01; done\n";
    char *old_path = strdup(getenv("PATH") ? getenv("PATH") : "");
    char *old_config = getenv("XDG_CONFIG_HOME") ? strdup(getenv("XDG_CONFIG_HOME")) : NULL;
    char path[8192];
    snprintf(path, sizeof(path), "%s:%s", directory, old_path ? old_path : "");
    if (WriteFile(executable, script) != 0 || chmod(executable, 0755) != 0) return 1;
    setenv("PATH", path, 1);
    setenv("XDG_CONFIG_HOME", directory, 1);
    setenv("PICO_BROWSER_TEST_MARKER", marker, 1);
    setenv("PICO_BROWSER_TEST_RELEASE", release, 1);
    PicoHost *host = NULL;
    bool ok = pico_host_init(&host, NULL, true) == PICO_OK;
    const char *url = "https://example.invalid/?literal=$(printf NOT_LITERAL)&quote='";
    ok &= host && __real_PicoHost_OpenBrowser(host, url);
    pid_t pid = 0;
    for (int i = 0; ok && i < 3000 && pid == 0; i++)
    {
        char *text = Pico_ReadFile(marker, NULL);
        if (text && strstr(text, url)) pid = (pid_t)strtol(text, NULL, 10);
        free(text);
        if (!pid) usleep(1000);
    }
    ok &= pid > 0;
    /* Failure to exec also reports synchronously, without a shell or wait. */
    setenv("PATH", "/nonexistent-pico-browser-launcher", 1);
    ok &= host && !__real_PicoHost_OpenBrowser(host, url);
    setenv("PATH", path, 1);
    double before = pico_openai_monotonic();
    if (host) ok &= pico_host_free(host) == PICO_HOST_SHUTDOWN_CLEAN;
    ok &= pico_openai_monotonic() - before < 1.0;
    WriteFile(release, "release\n");
    /* Observe reaping without stealing the reaper's waitpid result. Zombies
     * still respond to kill(pid, 0); a reaped child disappears. */
    for (int i = 0; pid > 0 && i < 3000 && kill(pid, 0) == 0; i++) usleep(1000);
    ok &= pid > 0 && kill(pid, 0) < 0 && errno == ESRCH;
    setenv("PATH", old_path ? old_path : "", 1);
    free(old_path);
    if (old_config) { setenv("XDG_CONFIG_HOME", old_config, 1); free(old_config); }
    else unsetenv("XDG_CONFIG_HOME");
    unsetenv("PICO_BROWSER_TEST_MARKER"); unsetenv("PICO_BROWSER_TEST_RELEASE");
    RmRf(directory);
    if (!ok) Fail("browser launcher must receive literal argv, outlive host without blocking, and be reaped");
    return ok ? 0 : 1;
}
