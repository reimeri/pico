/* Included by host_workspace_test.c. Exercise the registered OpenAI provider;
 * only HTTP is replaced, with synthetic credentials and no network access. */
#include <strings.h>
static pthread_mutex_t hint_mu = PTHREAD_MUTEX_INITIALIZER;
static struct {
    bool active, failed, reasoning_retry;
    int requests;
    const char *model, *tier, *hint, *url;
} hint_wire;

static bool RoutingHintWire(const PicoHttpPost *req, long *http, char **error, int *result)
{
    pthread_mutex_lock(&hint_mu);
    if (!hint_wire.active) { pthread_mutex_unlock(&hint_mu); return false; }
    int hints = 0;
    for (int i = 0; i < req->header_count; ++i) {
        const char *header = req->headers[i];
        if (!strncasecmp(header, "x-codex-routing-hint:", 21)) {
            hints++;
            hint_wire.failed |= !hint_wire.hint || strcmp(header, hint_wire.hint);
        }
        hint_wire.failed |= strchr(header, '\r') != NULL || strchr(header, '\n') != NULL;
    }
    hint_wire.failed |= hints != (hint_wire.hint ? 1 : 0) || strcmp(req->url, hint_wire.url);
    JsonDoc doc;
    if (JsonParse(&doc, req->body, strlen(req->body))) hint_wire.failed = true;
    else {
        int tier = JsonObjGet(&doc, 0, "service_tier");
        char *model = JsonObjStr(&doc, 0, "model");
        hint_wire.failed |= !model || strcmp(model, hint_wire.model) ||
            (hint_wire.tier ? !JsonEq(&doc, tier, hint_wire.tier) : tier >= 0);
        free(model); JsonFree(&doc);
    }
    bool retry = hint_wire.reasoning_retry && hint_wire.requests == 0;
    hint_wire.requests++;
    pthread_mutex_unlock(&hint_mu);
    *http = retry ? 400 : 200; *error = NULL; *result = PICO_HTTP_OK;
    const char *reply = retry ? "{\"error\":{\"message\":\"reasoning is unsupported\"}}" :
        "{\"type\":\"response.completed\",\"response\":{\"status\":\"completed\",\"output\":["
        "{\"type\":\"message\",\"role\":\"assistant\",\"content\":[{\"type\":\"output_text\",\"text\":\"done\"}]}]}}";
    req->on_json(req->user, NULL, reply, strlen(reply));
    return true;
}

static int TestCodexRoutingHint(void)
{
    const struct {
        const char *name, *model, *base, *tier, *hint;
        bool oauth, fast, reasoning_retry;
    } cases[] = {
        {"OAuth priority and reasoning retry", "hint-model", "", "priority",
         "x-codex-routing-hint: model=hint-model;tier=priority", true, true, true},
        {"OAuth standard", "other-hint-model", "", NULL,
         "x-codex-routing-hint: model=other-hint-model", true, false, false},
        {"API priority", "hint-model", "", "priority", NULL, false, true, false},
        {"custom gateway", "hint-model", "https://gateway.example/v1", NULL, NULL, false, false, false},
        {"invalid header bytes", "hint-model\r\nX-Injected: yes", "", NULL, NULL, true, false, false},
    };
    char config[] = "/tmp/pico-hint-config-XXXXXX", directory[] = "/tmp/pico-hint-workspace-XXXXXX";
    PicoHost *host = NULL; PicoWorkspaceId ws_id = 0; PicoAgentId id = 0;
    bool ok = false;
    const char *phase = "fixture";
    if (!mkdtemp(config) || !mkdtemp(directory)) return 1;
    char *old_config = getenv("XDG_CONFIG_HOME") ? strdup(getenv("XDG_CONFIG_HOME")) : NULL;
    setenv("XDG_CONFIG_HOME", config, 1);
    if (pico_host_init(&host, NULL, true) != PICO_OK ||
        pico_workspace_open(host, directory, &ws_id) != PICO_OK) goto done;
    WaitPluginLoad(host);
    PicoWorkspace *ws = PicoHost_FindWorkspace(host, ws_id);
    PicoModel *model = realloc(ws->models, sizeof(*model));
    if (!model) goto done;
    ws->models = model; ws->model_count = 1; memset(model, 0, sizeof(*model));
    snprintf(model->id, sizeof(model->id), "hint-model");
    snprintf(model->provider, sizeof(model->provider), "openai");
    model->supports_fast = true; model->context_limit = 128000;
    ws->settings.compact_enabled = false;
    pico_auth_set_env_key(host, "openai", "hint-key");
    if (!pico_auth_set_oauth(host, "openai", "hint-access", "hint-refresh", "hint-account", time(NULL)+3600)) goto done;
    PicoAgentCreateOptions options = {.kind=PICO_AGENT_MAIN, .session_start=PICO_SESSION_NEW, .model="hint-model"};
    if (pico_main_agent_create(host, ws_id, &options, &id) != PICO_OK) goto done;
    PicoAgent *agent = PicoHost_FindAgent(host, id);
    for (size_t i = 0; i < sizeof(cases)/sizeof(cases[0]); ++i) {
        phase = cases[i].name;
        snprintf(model->id, sizeof(model->id), "%s", cases[i].model);
        snprintf(model->base_url, sizeof(model->base_url), "%s", cases[i].base);
        if (!pico_auth_set_active(host, "openai", cases[i].oauth ? PICO_AUTH_OAUTH : PICO_AUTH_API_KEY) ||
            !PicoSettings_SetModel(agent, model->id) || !PicoSettings_SetFast(agent, cases[i].fast) ||
            !PicoSettings_SetEffort(agent, "high")) goto done;
        pthread_mutex_lock(&hint_mu);
        memset(&hint_wire, 0, sizeof(hint_wire));
        hint_wire.active = true; hint_wire.reasoning_retry = cases[i].reasoning_retry;
        hint_wire.model = cases[i].model; hint_wire.tier = cases[i].tier; hint_wire.hint = cases[i].hint;
        hint_wire.url = cases[i].oauth ? "https://chatgpt.com/backend-api/codex/responses" :
            cases[i].base[0] ? "https://gateway.example/v1/responses" : "https://api.openai.com/v1/responses";
        pthread_mutex_unlock(&hint_mu);
        if (pico_agent_submit(host, id, "check routing hint", NULL) != PICO_OK ||
            !PumpUntilIdle(host, agent) || agent->state != PICO_AGENT_IDLE) goto done;
        pthread_mutex_lock(&hint_mu);
        bool matched = !hint_wire.failed && hint_wire.requests == (cases[i].reasoning_retry ? 2 : 1);
        hint_wire.active = false;
        pthread_mutex_unlock(&hint_mu);
        if (!matched) goto done;
    }
    ok = DrainSessionForAssertion(host, agent);
done:
    if (host) pico_host_free(host);
    pthread_mutex_lock(&hint_mu); memset(&hint_wire, 0, sizeof(hint_wire)); pthread_mutex_unlock(&hint_mu);
    if (old_config) { setenv("XDG_CONFIG_HOME", old_config, 1); free(old_config); } else unsetenv("XDG_CONFIG_HOME");
    RmRf(config); RmRf(directory);
    if (!ok) { fprintf(stderr, "Routing hint phase: %s\n", phase); Fail("Codex OAuth routing hint contract"); }
    return !ok;
}
