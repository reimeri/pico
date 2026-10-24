#include "pico/http.h"
#include "canonical.h"
#include "image_fixtures.h"
/* Included by host_workspace_test.c; the actual provider and persistence run,
 * only the network boundary is replaced. No credentials or network required. */
static pthread_mutex_t native_wire_mu = PTHREAD_MUTEX_INITIALIZER;
static struct {
    bool active;
    int requests, compactions;
    char *body;
    char url[1024];
} native_wire;

static bool NativeCompactionWire(const PicoHttpPost *req, long *http, char **error, int *result)
{
    pthread_mutex_lock(&native_wire_mu);
    if (!native_wire.active) { pthread_mutex_unlock(&native_wire_mu); return false; }
    native_wire.requests++;
    free(native_wire.body); native_wire.body = JsonDup(req->body);
    snprintf(native_wire.url, sizeof(native_wire.url), "%s", req->url);
    bool compact = false;
    JsonDoc doc;
    if (!JsonParse(&doc, req->body, strlen(req->body))) {
        int input = JsonObjGet(&doc, 0, "input");
        int last = JsonArrayAt(&doc, input, JsonArrayLen(&doc, input) - 1);
        compact = JsonEq(&doc, JsonObjGet(&doc, last, "type"), "compaction_trigger");
        JsonFree(&doc);
    }
    if (compact) native_wire.compactions++;
    pthread_mutex_unlock(&native_wire_mu);
    *http = 200; *error = NULL; *result = PICO_HTTP_OK;
    const char *done = "{\"type\":\"response.output_item.done\",\"item\":{\"type\":\"compaction\",\"id\":\"cmp_native\",\"encrypted_content\":\"integration-encrypted-state\"}}";
    const char *completion = compact ?
        "{\"type\":\"response.completed\",\"response\":{\"status\":\"completed\",\"usage\":{\"input_tokens\":1000},\"output\":[{\"type\":\"compaction\",\"id\":\"cmp_native\",\"encrypted_content\":\"integration-encrypted-state\"}]}}" :
        "{\"type\":\"response.completed\",\"response\":{\"status\":\"completed\",\"usage\":{\"input_tokens\":100},\"output\":[{\"type\":\"message\",\"role\":\"assistant\",\"content\":[{\"type\":\"output_text\",\"text\":\"integration answer\"}]}]}}";
    if (compact) (void)req->on_json(req->user, NULL, done, strlen(done));
    (void)req->on_json(req->user, NULL, completion, strlen(completion));
    return true;
}

static void NativeEphemeralContext(PicoWorkspace *workspace, PicoAgentId id,
                                   PicoContextEvent *event, void *state)
{
    (void)workspace; (void)id; (void)state;
    event->extra_context = JsonDup("EPHEMERAL_NATIVE_CONTEXT");
}

static int NativeRequestCount(void)
{
    pthread_mutex_lock(&native_wire_mu); int n = native_wire.requests; pthread_mutex_unlock(&native_wire_mu);
    return n;
}

static int TestOpenAiNativeCompaction(bool oauth)
{
    char config[] = "/tmp/pico-native-config-XXXXXX", directory[] = "/tmp/pico-native-workspace-XXXXXX";
    PicoHost *host = NULL; PicoWorkspaceId ws_id = 0; PicoAgentId id = 0;
    bool ok = false;
    char *normal_instructions = NULL, *normal_tools = NULL;
    const char *phase = "fixture";
    char missing_image[4096];
    if (!mkdtemp(config) || !mkdtemp(directory)) return 1;
    snprintf(missing_image, sizeof(missing_image), "%s/missing.png", directory);
    char *old_config = getenv("XDG_CONFIG_HOME") ? strdup(getenv("XDG_CONFIG_HOME")) : NULL;
    setenv("XDG_CONFIG_HOME", config, 1);
    if (pico_host_init(&host, NULL, true) != PICO_OK ||
        pico_workspace_open(host, directory, &ws_id) != PICO_OK) goto done;
    WaitPluginLoad(host);
    PicoWorkspace *ws = PicoHost_FindWorkspace(host, ws_id);
    PicoModel *models = realloc(ws->models, 2 * sizeof(*models));
    if (!models) goto done;
    ws->models = models; ws->model_count = 2;
    memset(models, 0, 2 * sizeof(*models));
    snprintf(models[0].id, sizeof(models[0].id), "native-first");
    snprintf(models[0].provider, sizeof(models[0].provider), "openai");
    models[0].context_limit = 256000;
    models[0].vision = true;
    models[1] = models[0]; snprintf(models[1].id, sizeof(models[1].id), "native-second");
    ws->settings.compact_enabled = false;
    PicoHost_BeginRegistration(host, PICO_REG_WORKSPACE, ws);
    pico_add_context_hook(ws, NativeEphemeralContext);
    PicoHost_PublishRegistration(host, NULL);
    pico_auth_set_env_key(host, "openai", "native-original-key");
    if (oauth) pico_auth_set_oauth(host, "openai", "native-original-access", "native-refresh", "native-account", time(NULL)+3600);
    pico_auth_set_active(host, "openai", oauth ? PICO_AUTH_OAUTH : PICO_AUTH_API_KEY);
    PicoAgentCreateOptions options = {.kind=PICO_AGENT_MAIN, .session_start=PICO_SESSION_NEW, .model="native-first"};
    if (pico_main_agent_create(host, ws_id, &options, &id) != PICO_OK) goto done;
    PicoAgent *agent = PicoHost_FindAgent(host, id);
    pthread_mutex_lock(&native_wire_mu); native_wire.active = true; pthread_mutex_unlock(&native_wire_mu);
    phase = "initial turn";
    PicoLlmPart image = {.kind=PICO_LLM_PART_IMAGE, .path=missing_image, .mime="image/png"};
    char *parts = oauth ? NULL : pico_canonical_parts_json(&image, 1);
    PicoResult submitted = pico_agent_submit(host, id, "native original user task", parts);
    free(parts);
    if (submitted != PICO_OK || !PumpUntilIdle(host, agent)) goto done;
    JsonDoc ordinary;
    if (JsonParse(&ordinary, native_wire.body, strlen(native_wire.body))) goto done;
    normal_instructions = JsonObjStr(&ordinary, 0, "instructions");
    normal_tools = JsonObjRaw(&ordinary, 0, "tools");
    JsonFree(&ordinary);
    if (!normal_instructions || !normal_tools) goto done;
    char original_cache_key[65];
    snprintf(original_cache_key, sizeof(original_cache_key), "%s", PicoAgent_CacheKey(agent));
    phase = "native compaction";
    PicoAgent_Compact(host, agent);
    if (!PumpUntilIdle(host, agent) || agent->state != PICO_AGENT_IDLE || !agent->tokens_unknown) goto done;
    pthread_mutex_lock(&native_wire_mu);
    bool native_request = native_wire.compactions == 1 && native_wire.body &&
        !strstr(native_wire.body, "EPHEMERAL_NATIVE_CONTEXT") && strstr(native_wire.body, "compaction_trigger") &&
        strstr(native_wire.body, "\"store\":false") && strstr(native_wire.body, "reasoning.encrypted_content") &&
        strstr(native_wire.url, oauth ? "chatgpt.com/backend-api/codex/responses" : "api.openai.com/v1/responses") &&
        (oauth || (!strstr(native_wire.body, missing_image) &&
                   !strstr(native_wire.body, "input_image")));
    pthread_mutex_unlock(&native_wire_mu);
    if (!native_request || strcmp(original_cache_key, PicoAgent_CacheKey(agent)) ||
        !DrainSessionForAssertion(host, agent)) goto done;
    JsonDoc compacted;
    if (JsonParse(&compacted, native_wire.body, strlen(native_wire.body))) goto done;
    char *compact_instructions = JsonObjStr(&compacted, 0, "instructions");
    char *compact_tools = JsonObjRaw(&compacted, 0, "tools");
    bool same_prefix = compact_instructions && compact_tools &&
        !strcmp(normal_instructions, compact_instructions) && !strcmp(normal_tools, compact_tools);
    free(compact_instructions); free(compact_tools); JsonFree(&compacted);
    if (!same_prefix) goto done;
    size_t length; char *saved = Pico_ReadFile(agent->session_path, &length);
    bool persisted = saved && strstr(saved, "integration-encrypted-state") && !strstr(saved, "native-original-key") &&
        !strstr(saved, "native-original-access"); free(saved);
    if (!persisted) goto done;
    phase = "cross-model continuation";
    if (!PicoSettings_SetModel(agent, "native-second")) goto done;
    if (oauth) pico_auth_set_oauth(host, "openai", "native-rotated-access", "rotated-refresh", "native-account", time(NULL)+3600);
    if (pico_agent_submit(host, id, "continue with another model", NULL) != PICO_OK ||
        !PumpUntilIdle(host, agent) || agent->state != PICO_AGENT_IDLE || agent->tokens_unknown) goto done;
    pthread_mutex_lock(&native_wire_mu);
    bool replayed = native_wire.body && strstr(native_wire.body, "integration-encrypted-state") &&
        strstr(native_wire.body, "native-second") && !strstr(native_wire.body, "provider_state");
    pthread_mutex_unlock(&native_wire_mu);
    if (!replayed || !DrainSessionForAssertion(host, agent)) goto done;
    phase = "session resume";
    options.session_start = PICO_SESSION_NONE;
    PicoAgentId resumed_id = 0;
    if (pico_main_agent_create(host, ws_id, &options, &resumed_id) != PICO_OK) goto done;
    PicoAgent *resumed = PicoHost_FindAgent(host, resumed_id);
    if (PicoSession_Replay(host, resumed, agent->session_path, false) != 0) goto done;
    /* Resume has history but no in-memory prompt snapshot. A standalone compact
     * must rebuild the full normal prefix, not send empty/stale instructions. */
    PicoAgent_Compact(host, resumed);
    if (!PumpUntilIdle(host, resumed) || resumed->state != PICO_AGENT_IDLE) goto done;
    JsonDoc restored;
    if (JsonParse(&restored, native_wire.body, strlen(native_wire.body))) goto done;
    char *restored_instructions = JsonObjStr(&restored, 0, "instructions");
    bool normal_fallback = restored_instructions && !strcmp(normal_instructions, restored_instructions);
    free(restored_instructions); JsonFree(&restored);
    if (!normal_fallback || pico_agent_submit(host, resumed_id, "resumed continuation", NULL) != PICO_OK ||
        !PumpUntilIdle(host, resumed) || resumed->state != PICO_AGENT_IDLE) goto done;
    pthread_mutex_lock(&native_wire_mu);
    replayed = native_wire.body && strstr(native_wire.body, "integration-encrypted-state");
    pthread_mutex_unlock(&native_wire_mu);
    if (!replayed || strcmp(original_cache_key, PicoAgent_CacheKey(resumed))) goto done;
    if (!oauth) {
        phase = "restore missing image after native checkpoint and session resume";
        int warnings = 0;
        for (int i = 0; i < resumed->message_count; ++i)
            if (resumed->messages[i].role == PICO_ROLE_NOTICE && resumed->messages[i].source &&
                strstr(resumed->messages[i].source, missing_image)) warnings++;
        FILE *file = fopen(missing_image, "wb");
        if (!file) goto done;
        bool written = fwrite(image_fixtures[0].data, 1, image_fixtures[0].size, file) == image_fixtures[0].size;
        if (fclose(file) || !written || warnings != 1 ||
            pico_agent_submit(host, resumed_id, "restored attachment", NULL) != PICO_OK ||
            !PumpUntilIdle(host, resumed) || resumed->state != PICO_AGENT_IDLE) goto done;
        pthread_mutex_lock(&native_wire_mu);
        bool restored_image = native_wire.body && strstr(native_wire.body, "input_image");
        pthread_mutex_unlock(&native_wire_mu);
        if (!restored_image) goto done;
    }
    phase = "account/key mismatch refused before HTTP";
    int requests = NativeRequestCount();
    if (oauth) pico_auth_set_oauth(host, "openai", "other-access", "other-refresh", "different-account", time(NULL)+3600);
    else pico_auth_set_env_key(host, "openai", "different-key");
    if (pico_agent_submit(host, id, "must refuse changed authority", NULL) != PICO_OK ||
        !PumpUntilIdle(host, agent) || agent->state != PICO_AGENT_ERROR || NativeRequestCount() != requests) goto done;
    phase = "backend mismatch refused before HTTP";
    PicoAgent_DismissError(agent);
    if (oauth) pico_auth_set_active(host, "openai", PICO_AUTH_API_KEY);
    else {
        pico_auth_set_oauth(host, "openai", "other-access", "other-refresh", "native-account", time(NULL)+3600);
        pico_auth_set_active(host, "openai", PICO_AUTH_OAUTH);
    }
    if (pico_agent_submit(host, id, "must refuse changed backend", NULL) != PICO_OK ||
        !PumpUntilIdle(host, agent) || agent->state != PICO_AGENT_ERROR || NativeRequestCount() != requests) goto done;
    if (oauth) {
        phase = "reset invalidates an outstanding native checkpoint";
        PicoAgent_DismissError(agent);
        pico_auth_set_oauth(host, "openai", "restored-access", "restored-refresh", "native-account", time(NULL)+3600);
        pico_auth_set_active(host, "openai", PICO_AUTH_OAUTH);
        int ready[2], proceed[2];
        if (pipe(ready) || pipe(proceed)) goto done;
        g_replay_gate_stage = "catalog_before_upsert";
        g_replay_ready_fd = ready[1]; g_replay_continue_fd = proceed[0];
        PicoAgent_Compact(host, agent);
        PICO_TEST_WAIT(agent->state != PICO_AGENT_ERROR && strcmp(agent->activity, "Saving compaction…")) {
            pico_host_pump(host);
        }
        bool pending = agent->state == PICO_AGENT_COMPACT_WAIT && TransferTestByte(ready[0], false);
        if (pending) PicoSession_Reset(host, agent);
        bool released = TransferTestByte(proceed[1], true);
        g_replay_gate_stage = "async_replay_before_read";
        g_replay_ready_fd = g_replay_continue_fd = -1;
        close(ready[0]); close(ready[1]); close(proceed[0]); close(proceed[1]);
        if (!pending || !released || pico_agent_submit(host, id, "fresh session after pending checkpoint", NULL) != PICO_OK ||
            !PumpUntilIdle(host, agent) || agent->state != PICO_AGENT_IDLE) goto done;
        pthread_mutex_lock(&native_wire_mu);
        bool fresh = native_wire.body && !strstr(native_wire.body, "integration-encrypted-state");
        pthread_mutex_unlock(&native_wire_mu);
        if (!fresh) goto done;
    }
    if (!oauth) {
        phase = "custom gateway retains text compaction";
        pico_auth_set_env_key(host, "openai", "native-original-key");
        pico_auth_set_active(host, "openai", PICO_AUTH_API_KEY);
        snprintf(models[0].base_url, sizeof(models[0].base_url), "https://gateway.example/v1");
        PicoAgent_DismissError(agent);
        /* Existing native history must not be sent to a custom gateway. */
        if (!PicoSettings_SetModel(agent, "native-first") ||
            pico_agent_submit(host, id, "must refuse gateway", NULL) != PICO_OK ||
            !PumpUntilIdle(host, agent) || agent->state != PICO_AGENT_ERROR || NativeRequestCount() != requests) goto done;
        PicoSession_Reset(host, agent);
        if (!strcmp(original_cache_key, PicoAgent_CacheKey(agent))) goto done;
        PicoAgent_PushHistoryUser(agent, "new gateway task");
        PicoAgent_Compact(host, agent);
        if (!PumpUntilIdle(host, agent) || agent->state != PICO_AGENT_IDLE) goto done;
        pthread_mutex_lock(&native_wire_mu);
        bool text = native_wire.body && strstr(native_wire.body, "Handoff brief:") &&
            !strstr(native_wire.body, "compaction_trigger") && !strstr(native_wire.body, "\"store\":false");
        pthread_mutex_unlock(&native_wire_mu);
        if (!text) goto done;
    }
    ok = true;
done:
    free(normal_instructions); free(normal_tools);
    if (host) pico_host_free(host);
    pthread_mutex_lock(&native_wire_mu);
    free(native_wire.body); memset(&native_wire, 0, sizeof(native_wire));
    pthread_mutex_unlock(&native_wire_mu);
    if (old_config) { setenv("XDG_CONFIG_HOME", old_config, 1); free(old_config); } else unsetenv("XDG_CONFIG_HOME");
    RmRf(config); RmRf(directory);
    if (!ok) { fprintf(stderr, "Native compaction phase: %s (oauth=%d)\n", phase, oauth); Fail("OpenAI native compaction contract"); }
    return ok ? 0 : 1;
}
