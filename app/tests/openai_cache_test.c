/* Included by host_workspace_test.c. Real agent/provider lifecycle; only the
 * HTTP boundary is mocked. Two first requests overlap before either completes. */
static void RoutingEnableRefresh(void);
static bool RoutingDisableRefresh(void);

static pthread_mutex_t routing_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t routing_cv = PTHREAD_COND_INITIALIZER;
static struct {
    bool active, failed, change_account;
    int round, entered;
    struct { char key[65], token[128]; int calls; } sessions[2];
} routing_wire;

static bool RoutingWire(const PicoHttpPost *req, long *http, char **error, int *result)
{
    pthread_mutex_lock(&routing_mu);
    if (!routing_wire.active) { pthread_mutex_unlock(&routing_mu); return false; }
    JsonDoc doc;
    if (JsonParse(&doc, req->body, strlen(req->body))) {
        routing_wire.failed = true;
        pthread_mutex_unlock(&routing_mu);
        *http = 400; *error = NULL; *result = PICO_HTTP_FAIL; return true;
    }
    char *key = JsonObjStr(&doc, 0, "prompt_cache_key");
    int input = JsonObjGet(&doc, 0, "input");
    int last = JsonArrayAt(&doc, input, JsonArrayLen(&doc, input) - 1);
    bool compact = JsonEq(&doc, JsonObjGet(&doc, last, "type"), "compaction_trigger");
    JsonFree(&doc);
    int slot = key && !strcmp(key, routing_wire.sessions[0].key) ? 0 : 1;
    if (!key || strcmp(key, routing_wire.sessions[slot].key)) routing_wire.failed = true;
    int call = routing_wire.sessions[slot].calls++;
    const char *session_header = NULL, *token_header = NULL;
    for (int i = 0; i < req->header_count; ++i) {
        const char *h = req->headers[i];
        if (!strncmp(h, "session-id: ", 12)) session_header = h + 12;
        if (!strncmp(h, "session_id:", 11)) routing_wire.failed = true;
        if (!strncmp(h, "x-codex-turn-state: ", 20)) token_header = h + 20;
    }
    bool fresh = call == 0 || (routing_wire.round == 3 && call == 1);
    routing_wire.failed |= !session_header || !key || strcmp(session_header, key) ||
        (fresh ? token_header != NULL : (!token_header || strcmp(token_header, routing_wire.sessions[slot].token))) ||
        !req->on_headers || strstr(req->body, "route-secret-") != NULL;
    char issued[128];
    snprintf(issued, sizeof(issued), "route-secret-%d-%d-%d", routing_wire.round, slot, call);
    if (fresh) snprintf(routing_wire.sessions[slot].token, sizeof(routing_wire.sessions[slot].token), "%s", issued);
    if (routing_wire.round == 0 && call == 0) {
        routing_wire.entered++;
        pthread_cond_broadcast(&routing_cv);
        while (routing_wire.entered < 2) pthread_cond_wait(&routing_cv, &routing_mu);
    }
    bool unauthorized = routing_wire.round == 4 && call == 1;
    pthread_mutex_unlock(&routing_mu);
    free(key);
    char headers[256];
    snprintf(headers, sizeof(headers), "HTTP/1.1 200 OK\r\nx-codex-turn-state: %s\r\n\r\n", issued);
    if (req->on_headers) req->on_headers(req->user, unauthorized ? 401 : 200, headers, strlen(headers));
    *http = unauthorized ? 401 : 200; *error = NULL; *result = PICO_HTTP_OK;
    const char *reply = unauthorized ? "{\"detail\":\"access token rejected\"}" : compact ?
        "{\"type\":\"response.completed\",\"response\":{\"status\":\"completed\",\"output\":[{\"type\":\"compaction\",\"id\":\"cmp_route\",\"encrypted_content\":\"route-checkpoint\"}]}}" :
        call == 0 ?
        "{\"type\":\"response.completed\",\"response\":{\"status\":\"completed\",\"output\":[{\"type\":\"function_call\",\"call_id\":\"cache-call\",\"name\":\"cache_echo\",\"arguments\":\"{}\"}]}}" :
        "{\"type\":\"response.completed\",\"response\":{\"status\":\"completed\",\"usage\":{\"input_tokens\":600},\"output\":[{\"type\":\"message\",\"role\":\"assistant\",\"content\":[{\"type\":\"output_text\",\"text\":\"done\"}]}]}}";
    req->on_json(req->user, NULL, reply, strlen(reply));
    return true;
}

static void RoutingEcho(PicoAgentContext *ctx, const char *args, PicoToolResult *out, void *state)
{
    (void)ctx; (void)args; (void)state;
    out->output = JsonDup("echo");
    out->details_json = JsonDup("{}");
}

static bool RoutingApply(PicoWorkspace *ws, PicoAgentId id, const char *json, bool replay, void *state)
{
    (void)id; (void)json; (void)state;
    if (!replay && routing_wire.change_account) {
        routing_wire.change_account = false;
        return pico_auth_set_oauth(ws->host, "openai", "new-access", "new-refresh", "new-account", time(NULL)+3600);
    }
    return true;
}

static void RoutingRound(int round, PicoAgent *first, PicoAgent *second)
{
    pthread_mutex_lock(&routing_mu);
    routing_wire.round = round;
    routing_wire.entered = 0;
    memset(routing_wire.sessions, 0, sizeof(routing_wire.sessions));
    snprintf(routing_wire.sessions[0].key, sizeof(routing_wire.sessions[0].key), "%s", PicoAgent_CacheKey(first));
    if (second) snprintf(routing_wire.sessions[1].key, sizeof(routing_wire.sessions[1].key), "%s", PicoAgent_CacheKey(second));
    pthread_mutex_unlock(&routing_mu);
}

static int TestCodexCacheAffinity(void)
{
    char config[] = "/tmp/pico-routing-config-XXXXXX", directory[] = "/tmp/pico-routing-workspace-XXXXXX";
    PicoHost *host = NULL; PicoWorkspaceId ws_id = 0; PicoAgentId ids[2] = {0};
    bool ok = false;
    if (!mkdtemp(config) || !mkdtemp(directory)) return 1;
    char *old_config = getenv("XDG_CONFIG_HOME") ? strdup(getenv("XDG_CONFIG_HOME")) : NULL;
    setenv("XDG_CONFIG_HOME", config, 1);
    if (pico_host_init(&host, NULL, true) != PICO_OK || pico_workspace_open(host, directory, &ws_id) != PICO_OK) goto done;
    WaitPluginLoad(host);
    PicoWorkspace *ws = PicoHost_FindWorkspace(host, ws_id);
    PicoModel *model = realloc(ws->models, sizeof(*model));
    if (!model) goto done;
    ws->models = model; ws->model_count = 1; memset(model, 0, sizeof(*model));
    snprintf(model->id, sizeof(model->id), "routing-model");
    snprintf(model->provider, sizeof(model->provider), "openai");
    model->context_limit = 4096;
    ws->settings.compact_enabled = false;
    PicoHost_BeginRegistration(host, PICO_REG_WORKSPACE, ws);
    pico_add_tool(ws, "cache_echo", "echo", "{}", RoutingEcho, RoutingApply, PICO_TOOL_PARALLEL);
    PicoHost_PublishRegistration(host, NULL);
    if (!pico_auth_set_oauth(host, "openai", "access", "refresh", "account", time(NULL)+3600) ||
        !pico_auth_set_active(host, "openai", PICO_AUTH_OAUTH)) goto done;
    PicoAgentCreateOptions options = {.kind=PICO_AGENT_MAIN, .session_start=PICO_SESSION_NEW, .model="routing-model"};
    for (int i = 0; i < 2; ++i)
        if (pico_main_agent_create(host, ws_id, &options, &ids[i]) != PICO_OK) goto done;
    PicoAgent *a = PicoHost_FindAgent(host, ids[0]), *b = PicoHost_FindAgent(host, ids[1]);
    a->compact_enabled = b->compact_enabled = true;
    a->compact_ratio = b->compact_ratio = 0.1;
    RoutingRound(0, a, b);
    pthread_mutex_lock(&routing_mu); routing_wire.active = true; pthread_mutex_unlock(&routing_mu);
    if (pico_agent_submit(host, ids[0], "first", NULL) != PICO_OK ||
        pico_agent_submit(host, ids[1], "second", NULL) != PICO_OK ||
        !PumpUntilIdle(host, a) || !PumpUntilIdle(host, b) ||
        a->state != PICO_AGENT_IDLE || b->state != PICO_AGENT_IDLE ||
        routing_wire.sessions[0].calls != 3 || routing_wire.sessions[1].calls != 3) goto done;
    /* New user turns and standalone compaction must not inherit old routing. */
    RoutingRound(1, a, NULL);
    if (pico_agent_submit(host, ids[0], "next turn", NULL) != PICO_OK || !PumpUntilIdle(host, a) ||
        a->state != PICO_AGENT_IDLE || routing_wire.sessions[0].calls != 3) goto done;
    RoutingRound(2, a, NULL);
    PicoAgent_Compact(host, a);
    if (!PumpUntilIdle(host, a) || a->state != PICO_AGENT_IDLE || routing_wire.sessions[0].calls != 1) goto done;
    /* Durable history must contain neither this token nor its transport envelope. */
    if (!DrainSessionForAssertion(host, a)) goto done;
    size_t size; char *saved = Pico_ReadFile(a->session_path, &size);
    bool private = saved && !strstr(saved, "route-secret-") && !strstr(saved, "turn_state"); free(saved);
    if (!private) goto done;
    /* Account changes within tool execution discard old-owner routing, even
     * though the agent is still in the same turn. Use a fresh canonical history. */
    PicoSession_Reset(host, a); a->compact_enabled = false;
    RoutingRound(3, a, NULL); routing_wire.change_account = true;
    if (pico_agent_submit(host, ids[0], "change authority", NULL) != PICO_OK || !PumpUntilIdle(host, a) ||
        a->state != PICO_AGENT_IDLE || routing_wire.sessions[0].calls != 2) goto done;
    /* 401 retries must keep routing when only the bearer token rotates. */
    RoutingRound(4, a, NULL); RoutingEnableRefresh();
    if (pico_agent_submit(host, ids[0], "refresh credentials", NULL) != PICO_OK || !PumpUntilIdle(host, a) ||
        a->state != PICO_AGENT_IDLE || routing_wire.sessions[0].calls != 3 || !RoutingDisableRefresh()) goto done;
    ok = !routing_wire.failed;
done:
    (void)RoutingDisableRefresh();
    pthread_mutex_lock(&routing_mu); routing_wire.entered = 2; pthread_cond_broadcast(&routing_cv); pthread_mutex_unlock(&routing_mu);
    if (host) pico_host_free(host);
    pthread_mutex_lock(&routing_mu); memset(&routing_wire, 0, sizeof(routing_wire)); pthread_mutex_unlock(&routing_mu);
    if (old_config) { setenv("XDG_CONFIG_HOME", old_config, 1); free(old_config); } else unsetenv("XDG_CONFIG_HOME");
    RmRf(config); RmRf(directory);
    if (!ok) Fail("Codex affinity must isolate concurrent turns, preserve routing through tools/compaction, and clear at boundaries");
    return !ok;
}
