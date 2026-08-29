#include "host_internal.h"
/* Included by agent_behavior_test.c so workspace concurrency uses the same deterministic fake provider host. */

static bool g_close_hook_saw_removed;

static void InspectClosedAgent(PicoWorkspace *workspace, const PicoHookEvent *event, void *state)
{
    PicoHost *app = workspace ? workspace->host : NULL;
    (void)state;
    PicoAgentInfo info;
    if (event && event->hook == PICO_HOOK_ON_AGENT_DESTROY)
    {
        g_close_hook_saw_removed = !pico_agent_find(app, event->agent_id, &info);
    }
}

static int TestManagerProfileRegistry(void)
{
    const char *name = "workspace profile registry";
    char temp[] = "/tmp/pico-agent-profiles-XXXXXX";
    if (!mkdtemp(temp))
    {
        return Fail(name, "could not create config directory");
    }
    snprintf(g_config_dir, sizeof(g_config_dir), "%s", temp);
    char dir[4096];
    char valid_path[4096];
    char invalid_path[4096];
    if (!PicoPath_Format(dir, sizeof(dir), "%s/subagents", temp) ||
        !PicoPath_Format(valid_path, sizeof(valid_path), "%s/exploration.json", dir) ||
        !PicoPath_Format(invalid_path, sizeof(invalid_path), "%s/broken.json", dir))
    {
        rmdir(temp);
        return Fail(name, "temporary profile path was too long");
    }
    Pico_MkdirP(dir);
    FILE *file = fopen(valid_path, "wb");
    if (file)
    {
        fputs("{/* comment */\"description\":\"Explore\",\"purpose\":\"Inspect only\",\"tools\":[\"ask_test\"]}", file);
        fclose(file);
    }
    file = fopen(invalid_path, "wb");
    if (file)
    {
        fputs("{\"description\":\"missing purpose\"}", file);
        fclose(file);
    }

    PicoHost app;
    InitApp(&app);
    PicoWorkspace_LoadProfiles(PicoHost_PrimaryWorkspace(&app));
    PicoSubagentProfileInfo info;
    bool loaded = pico_subagent_profile_count(&app) == 1 &&
                  pico_subagent_profile_info(&app, 0, &info) &&
                  strcmp(info.name, "exploration") == 0 &&
                  strcmp(info.purpose, "Inspect only") == 0 &&
                  info.restricted_tools && info.tool_count == 1 &&
                  strcmp(info.tools[0], "ask_test") == 0;
    PicoAgentId only_id = pico_agent_active(&app);
    PicoHost_BeginRegistration(&app, PICO_REG_WORKSPACE, PicoHost_PrimaryWorkspace(&app));
    pico_workspace_add_hook(PicoHost_PrimaryWorkspace(&app), PICO_HOOK_ON_AGENT_DESTROY, InspectClosedAgent);
    PicoHost_PublishRegistration(&app, NULL);
    g_close_hook_saw_removed = false;
    bool close_contract = pico_agent_close(&app, only_id) == PICO_AGENT_RESULT_OK &&
                         g_close_hook_saw_removed &&
                         PicoHost_PrimaryWorkspace(&app)->count == 0;

    PicoWorkspace *ws = PicoHost_PrimaryWorkspace(&app);
    bool reservations = PicoWorkspace_ReserveSession(ws, 111, "/tmp/one.jsonl") &&
                        PicoWorkspace_ReserveSession(ws, 111, "/tmp/one.jsonl") &&
                        !PicoWorkspace_ReserveSession(ws, 222, "/tmp/one.jsonl");
    PicoWorkspace_ReleaseSessions(ws, 111);
    reservations = reservations && !PicoWorkspace_SessionReserved(ws, "/tmp/one.jsonl", 0);
    PicoHost_Shutdown(&app);
    unlink(valid_path);
    unlink(invalid_path);
    rmdir(dir);
    rmdir(temp);
    snprintf(g_config_dir, sizeof(g_config_dir), "/tmp/pico-agent-behavior");
    return loaded && reservations && close_contract
               ? 0
               : Fail(name, "valid/invalid profiles or writer reservations were not isolated");
}

static int TestManagerConcurrencyAndIsolation(void)
{
    const char *name = "workspace concurrency and isolation";
    ResetTest(TEST_CONCURRENT_REVERSE, 0);
    g_test.provider_tokens = 10;
    g_test.provider_cached_tokens = 3;
    PicoHost app;
    InitApp(&app);
    PicoAgentId first_id = pico_agent_active(&app);
    PicoAgentCreateOptions options = {
        .kind = PICO_AGENT_MAIN,
        .session_start = PICO_SESSION_NONE,
        .select = false,
    };
    PicoAgentId second_id = 0;
    if (pico_agent_create(&app, &options, &second_id) != PICO_AGENT_RESULT_OK ||
        pico_agent_count(&app) != 2 || second_id == first_id)
    {
        PicoHost_Shutdown(&app);
        return Fail(name, "could not create two independent agents");
    }
    PicoAgent *first = PicoHost_FindAgent(&app, first_id);
    PicoAgent *second = PicoHost_FindAgent(&app, second_id);
    PicoAgent_StartTurn(&app, first, "first");
    PicoAgent_StartTurn(&app, second, "second");

    bool reverse_observed = false;
    for (int i = 0; i < 3000; i++)
    {
        PicoWorkspace_Pump(PicoHost_PrimaryWorkspace(&app));
        pthread_mutex_lock(&g_test.mu);
        int entered = g_test.provider_entered_count;
        PicoAgentId blocked_id = g_test.first_provider_id;
        pthread_mutex_unlock(&g_test.mu);
        if (entered >= 2)
        {
            PicoAgent *blocked = PicoHost_FindAgent(&app, blocked_id);
            PicoAgent *completed = blocked_id == first_id ? second : first;
            if (PicoAgent_IsBusy(blocked) && !PicoAgent_IsBusy(completed))
            {
                reverse_observed = true;
                break;
            }
        }
        SleepOneMs();
    }
    if (!reverse_observed)
    {
        pthread_mutex_lock(&g_test.mu);
        g_test.block_release = true;
        pthread_cond_broadcast(&g_test.cv);
        pthread_mutex_unlock(&g_test.mu);
        PicoHost_Shutdown(&app);
        return Fail(name, "provider callbacks did not overlap and complete in reverse order");
    }

    app.chat_sel.msg = 9;
    app.chat_follow_bottom = false;
    if (!pico_agent_select(&app, second_id) || app.chat_sel.msg != -1 || !app.chat_follow_bottom)
    {
        pthread_mutex_lock(&g_test.mu);
        g_test.block_release = true;
        pthread_cond_broadcast(&g_test.cv);
        pthread_mutex_unlock(&g_test.mu);
        PicoHost_Shutdown(&app);
        return Fail(name, "selection retained stale transcript state");
    }
    pthread_mutex_lock(&g_test.mu);
    g_test.block_release = true;
    pthread_cond_broadcast(&g_test.cv);
    pthread_mutex_unlock(&g_test.mu);
    for (int i = 0; i < 3000 && (PicoAgent_IsBusy(first) || PicoAgent_IsBusy(second)); i++)
    {
        PicoWorkspace_Pump(PicoHost_PrimaryWorkspace(&app));
        SleepOneMs();
    }
    bool isolated = !PicoAgent_IsBusy(first) && !PicoAgent_IsBusy(second) &&
                    first->session_input_tokens == 10 && second->session_input_tokens == 10 &&
                    first->message_count > 0 && second->message_count > 0;

    ResetTest(TEST_PROVIDER_BLOCK, 0);
    PicoAgent_StartTurn(&app, first, "cancel first");
    PicoAgent_StartTurn(&app, second, "leave second running");
    for (int i = 0; i < 3000; i++)
    {
        pthread_mutex_lock(&g_test.mu);
        int entered = g_test.provider_tools_issued;
        pthread_mutex_unlock(&g_test.mu);
        if (entered >= 2) break;
        SleepOneMs();
    }
    PicoAgent_Cancel(first);
    for (int i = 0; i < 3000 && PicoAgent_IsBusy(first); i++)
    {
        PicoWorkspace_Pump(PicoHost_PrimaryWorkspace(&app));
        SleepOneMs();
    }
    bool cancel_isolated = !PicoAgent_IsBusy(first) && PicoAgent_IsBusy(second);
    PicoAgent_Cancel(second);
    for (int i = 0; i < 3000 && PicoAgent_IsBusy(second); i++)
    {
        PicoWorkspace_Pump(PicoHost_PrimaryWorkspace(&app));
        SleepOneMs();
    }
    PicoHost_Shutdown(&app);
    return isolated && cancel_isolated
               ? 0
               : Fail(name, "events, usage, transcript, or cancellation crossed agent boundaries");
}

static int TestSubmitTargetsExplicitAgentWithoutChangingSelection(void)
{
    const char *name = "submit targets explicit agent without changing selection";
    ResetTest(TEST_PROVIDER_BLOCK, 0);
    PicoHost app;
    InitApp(&app);
    PicoAgentId first_id = pico_agent_active(&app);
    PicoAgentCreateOptions options = {
        .kind = PICO_AGENT_MAIN,
        .session_start = PICO_SESSION_NONE,
        .select = false,
    };
    PicoAgentId second_id = 0;
    if (pico_agent_create(&app, &options, &second_id) != PICO_AGENT_RESULT_OK ||
        pico_agent_active(&app) != first_id)
    {
        PicoHost_Shutdown(&app);
        return Fail(name, "creating a second main agent changed UI selection");
    }
    if (pico_agent_submit(&app, second_id, "second", NULL) != PICO_OK)
    {
        PicoHost_Shutdown(&app);
        return Fail(name, "explicit submit failed");
    }
    PicoAgent *first = PicoHost_FindAgent(&app, first_id);
    PicoAgent *second = PicoHost_FindAgent(&app, second_id);
    bool targeted = false;
    for (int i = 0; i < 3000; i++)
    {
        PicoWorkspace_Pump(PicoHost_PrimaryWorkspace(&app));
        if (PicoAgent_IsBusy(second) && !PicoAgent_IsBusy(first) && pico_agent_active(&app) == first_id)
        {
            targeted = true;
            break;
        }
        SleepOneMs();
    }
    pthread_mutex_lock(&g_test.mu);
    g_test.block_release = true;
    pthread_cond_broadcast(&g_test.cv);
    pthread_mutex_unlock(&g_test.mu);
    for (int i = 0; i < 3000 && PicoAgent_IsBusy(second); i++)
    {
        PicoWorkspace_Pump(PicoHost_PrimaryWorkspace(&app));
        SleepOneMs();
    }
    PicoHost_Shutdown(&app);
    return targeted ? 0 : Fail(name, "submit retargeted UI selection or ran on the selected agent");
}

static int TestSubmitIsCompleteExplicitTurn(void)
{
    const char *name = "explicit submit records the user turn from provided parts";
    ResetTest(TEST_SINGLE, 0);
    PicoHost app;
    InitApp(&app);
    PicoAgentId first_id = pico_agent_active(&app);
    PicoAgentCreateOptions options = {
        .kind = PICO_AGENT_MAIN,
        .session_start = PICO_SESSION_NONE,
        .select = false,
    };
    PicoAgentId second_id = 0;
    const char *parts = "[{\"type\":\"text\",\"text\":\"second\"}]";
    if (pico_agent_create(&app, &options, &second_id) != PICO_AGENT_RESULT_OK)
    {
        PicoHost_Shutdown(&app);
        return Fail(name, "could not create a second main agent");
    }
    app.agent_parts = JsonDup("[{\"type\":\"text\",\"text\":\"ambient\"}]");
    if (pico_agent_submit(&app, second_id, "second", parts) != PICO_OK)
    {
        PicoHost_Shutdown(&app);
        return Fail(name, "explicit submit failed");
    }
    PicoAgent *second = PicoHost_FindAgent(&app, second_id);
    for (int i = 0; i < 3000 && second && PicoAgent_IsBusy(second); i++)
    {
        PicoWorkspace_Pump(PicoHost_PrimaryWorkspace(&app));
        SleepOneMs();
    }
    const PicoMessage *user = pico_agent_message(&app, second_id, 0);
    pthread_mutex_lock(&g_test.mu);
    bool used_parts = g_test.last_input && strstr(g_test.last_input, "second") &&
                      !strstr(g_test.last_input, "ambient");
    pthread_mutex_unlock(&g_test.mu);
    bool ok = pico_agent_active(&app) == first_id &&
              pico_agent_message_count(&app, first_id) == 0 &&
              user && user->role == PICO_ROLE_USER && user->source &&
              strcmp(user->source, "second") == 0 &&
              app.agent_parts && strstr(app.agent_parts, "ambient") && used_parts;
    PicoHost_Shutdown(&app);
    return ok ? 0 : Fail(name, "submit consumed ambient parts or skipped the user transcript");
}

static int TestSubmitReportsResultCodes(void)
{
    const char *name = "explicit submit reports missing, empty, and busy results";
    ResetTest(TEST_PROVIDER_BLOCK, 0);
    PicoHost app;
    InitApp(&app);
    PicoAgentId first_id = pico_agent_active(&app);
    bool codes = pico_agent_submit(&app, 0, "x", NULL) == PICO_NOT_FOUND &&
                 pico_agent_submit(&app, 999, "x", NULL) == PICO_NOT_FOUND &&
                 pico_agent_submit(&app, first_id, "", NULL) == PICO_INVALID;
    if (!codes || pico_agent_submit(&app, first_id, "go", NULL) != PICO_OK)
    {
        pthread_mutex_lock(&g_test.mu);
        g_test.block_release = true;
        pthread_cond_broadcast(&g_test.cv);
        pthread_mutex_unlock(&g_test.mu);
        PicoHost_Shutdown(&app);
        return Fail(name, "submit did not report missing/empty results or start the turn");
    }
    PicoAgent *first = PicoHost_FindAgent(&app, first_id);
    for (int i = 0; i < 3000 && first && !PicoAgent_IsBusy(first); i++)
    {
        PicoWorkspace_Pump(PicoHost_PrimaryWorkspace(&app));
        SleepOneMs();
    }
    bool busy = pico_agent_submit(&app, first_id, "again", NULL) == PICO_BUSY;
    pthread_mutex_lock(&g_test.mu);
    g_test.block_release = true;
    pthread_cond_broadcast(&g_test.cv);
    pthread_mutex_unlock(&g_test.mu);
    for (int i = 0; i < 3000 && first && PicoAgent_IsBusy(first); i++)
    {
        PicoWorkspace_Pump(PicoHost_PrimaryWorkspace(&app));
        SleepOneMs();
    }
    PicoHost_Shutdown(&app);
    return busy ? 0 : Fail(name, "busy submit did not return PICO_BUSY");
}

static PicoAgentId g_login_other;
static PicoAgentId g_login_seen;

static void FakeLogin(PicoHost *host, PicoAgentId agent_id, const char *args, void *state)
{
    (void)args;
    (void)state;
    g_login_seen = agent_id;
    pico_agent_select(host, g_login_other);
    PicoHost_AddMessage(host, agent_id, PICO_ROLE_ASSISTANT, "logged in");
}

static void LoginOnSubmit(PicoWorkspace *workspace, const PicoHookEvent *event, void *state)
{
    PicoHost *app = workspace ? workspace->host : NULL;
    (void)state;
    if (!app || app->submit_cancel || !app->composer.text || strncmp(app->composer.text, "/login", 6) != 0)
    {
        return;
    }
    if (app->auth_count > 0 && app->auths[0].login)
    {
        app->auths[0].login(app, event->agent_id, "", app->auths[0].state);
    }
    app->submit_cancel = true;
}

static int TestLoginRoutesToSnapshottedAgent(void)
{
    const char *name = "login routes notes to the snapshotted agent";
    ResetTest(TEST_SINGLE, 0);
    PicoHost app;
    InitApp(&app);
    PicoAgentId first_id = pico_agent_active(&app);
    PicoAgentCreateOptions options = {
        .kind = PICO_AGENT_MAIN,
        .session_start = PICO_SESSION_NONE,
        .select = false,
    };
    PicoAgentId second_id = 0;
    if (pico_agent_create(&app, &options, &second_id) != PICO_AGENT_RESULT_OK)
    {
        PicoHost_Shutdown(&app);
        return Fail(name, "could not create a second main agent");
    }
    g_login_other = second_id;
    g_login_seen = 0;
    app.auths[0] = (PicoAuth){.provider = "testauth", .login = FakeLogin};
    app.auth_count = 1;
    PicoHost_BeginRegistration(&app, PICO_REG_WORKSPACE, PicoHost_PrimaryWorkspace(&app));
    pico_workspace_add_hook(PicoHost_PrimaryWorkspace(&app), PICO_HOOK_BEFORE_SUBMIT, LoginOnSubmit);
    PicoHost_PublishRegistration(&app, NULL);
    app.composer.text = JsonDup("/login testauth");
    app.composer.length = (int)strlen(app.composer.text);
    app.composer.capacity = app.composer.length + 1;
    PicoHost_Submit(&app);
    const PicoMessage *note = pico_agent_message(&app, first_id, 0);
    bool ok = g_login_seen == first_id && pico_agent_active(&app) == second_id &&
              note && note->source && strstr(note->source, "logged in") &&
              pico_agent_message_count(&app, second_id) == 0;
    PicoHost_Shutdown(&app);
    return ok ? 0 : Fail(name, "login notes followed UI selection instead of the command snapshot");
}

static int TestResumeMissingAgentReturnsNotFound(void)
{
    const char *name = "resume of a stale agent id returns not found";
    ResetTest(TEST_SINGLE, 0);
    PicoHost app;
    InitApp(&app);
    PicoAgentResult result = PicoWorkspace_Resume(&app, 999, "missing", false);
    PicoHost_Shutdown(&app);
    return result == PICO_AGENT_RESULT_NOT_FOUND
               ? 0
               : Fail(name, "stale resume id was not PICO_AGENT_RESULT_NOT_FOUND");
}

static int TestResumeLeavesUnselectedAgentSelection(void)
{
    const char *name = "resume of an unselected agent leaves UI selection";
    ResetTest(TEST_SINGLE, 0);
    PicoHost app;
    InitApp(&app);
    PicoAgentId first_id = pico_agent_active(&app);
    PicoAgentCreateOptions options = {
        .kind = PICO_AGENT_MAIN,
        .session_start = PICO_SESSION_NONE,
        .select = false,
    };
    PicoAgentId second_id = 0;
    PicoAgentInfo info;
    if (pico_agent_create(&app, &options, &second_id) != PICO_AGENT_RESULT_OK)
    {
        PicoHost_Shutdown(&app);
        return Fail(name, "could not create a second main agent");
    }
    g_fake_session.enabled = true;
    g_fake_session.resolve_ok = true;
    snprintf(g_fake_session.id, sizeof(g_fake_session.id), "resume-target");
    snprintf(g_fake_session.path, sizeof(g_fake_session.path), "/tmp/resume-target.jsonl");
    snprintf(g_fake_session.replayed_model, sizeof(g_fake_session.replayed_model), "test-model");
    if (PicoWorkspace_Resume(&app, second_id, "resume-target", false) != PICO_AGENT_RESULT_OK)
    {
        PicoHost_Shutdown(&app);
        return Fail(name, "explicit resume failed");
    }
    bool ok = pico_agent_active(&app) == first_id && !pico_agent_find(&app, second_id, &info) &&
              pico_agent_count(&app) == 2;
    PicoHost_Shutdown(&app);
    return ok ? 0 : Fail(name, "resume changed UI selection or failed to replace the target");
}
