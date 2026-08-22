/* Included by agent_behavior_test.c so manager concurrency uses the same deterministic fake provider host. */

static bool g_close_hook_saw_removed;

static void InspectClosedAgent(PicoApp *app, const PicoHookEvent *event)
{
    PicoAgentInfo info;
    if (event && event->hook == PICO_HOOK_ON_AGENT_DESTROY)
    {
        g_close_hook_saw_removed = !pico_agent_find(app, event->agent_id, &info);
    }
}

static int TestManagerProfileRegistry(void)
{
    const char *name = "manager profile registry";
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

    PicoApp app;
    InitApp(&app);
    PicoAgentManager_LoadProfiles(app.agents);
    PicoSubagentProfileInfo info;
    bool loaded = pico_subagent_profile_count(&app) == 1 &&
                  pico_subagent_profile_info(&app, 0, &info) &&
                  strcmp(info.name, "exploration") == 0 &&
                  strcmp(info.purpose, "Inspect only") == 0 &&
                  info.restricted_tools && info.tool_count == 1 &&
                  strcmp(info.tools[0], "ask_test") == 0;
    PicoAgentId only_id = pico_agent_active(&app);
    bool close_contract = pico_agent_close(&app, only_id) == PICO_AGENT_RESULT_BUSY;
    PicoAgentCreateOptions close_options = {
        .kind = PICO_AGENT_NORMAL,
        .session_start = PICO_SESSION_NONE,
    };
    PicoAgentId close_id = 0;
    close_contract = close_contract &&
                     pico_agent_create(&app, &close_options, &close_id) == PICO_AGENT_RESULT_OK;
    pico_add_hook(&app, PICO_HOOK_ON_AGENT_DESTROY, InspectClosedAgent);
    g_close_hook_saw_removed = false;
    close_contract = close_contract && pico_agent_close(&app, close_id) == PICO_AGENT_RESULT_OK &&
                     g_close_hook_saw_removed;

    bool reservations = PicoAgentManager_ReserveSession(app.agents, 111, "/tmp/one.jsonl") &&
                        PicoAgentManager_ReserveSession(app.agents, 111, "/tmp/one.jsonl") &&
                        !PicoAgentManager_ReserveSession(app.agents, 222, "/tmp/one.jsonl");
    PicoAgentManager_ReleaseSessions(app.agents, 111);
    reservations = reservations && !PicoAgentManager_SessionReserved(app.agents, "/tmp/one.jsonl", 0);
    PicoApp_Free(&app);
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
    const char *name = "manager concurrency and isolation";
    ResetTest(TEST_CONCURRENT_REVERSE, 0);
    g_test.provider_tokens = 10;
    g_test.provider_cached_tokens = 3;
    PicoApp app;
    InitApp(&app);
    PicoAgentId first_id = pico_agent_active(&app);
    PicoAgentCreateOptions options = {
        .kind = PICO_AGENT_NORMAL,
        .session_start = PICO_SESSION_NONE,
        .select = false,
    };
    PicoAgentId second_id = 0;
    if (pico_agent_create(&app, &options, &second_id) != PICO_AGENT_RESULT_OK ||
        pico_agent_count(&app) != 2 || second_id == first_id)
    {
        PicoApp_Free(&app);
        return Fail(name, "could not create two independent agents");
    }
    PicoAgent *first = PicoAgentManager_Find(app.agents, first_id);
    PicoAgent *second = PicoAgentManager_Find(app.agents, second_id);
    PicoAgent_StartTurn(&app, first, "first");
    PicoAgent_StartTurn(&app, second, "second");

    bool reverse_observed = false;
    for (int i = 0; i < 3000; i++)
    {
        PicoAgentManager_Pump(app.agents);
        pthread_mutex_lock(&g_test.mu);
        int entered = g_test.provider_entered_count;
        PicoAgentId blocked_id = g_test.first_provider_id;
        pthread_mutex_unlock(&g_test.mu);
        if (entered >= 2)
        {
            PicoAgent *blocked = PicoAgentManager_Find(app.agents, blocked_id);
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
        PicoApp_Free(&app);
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
        PicoApp_Free(&app);
        return Fail(name, "selection retained stale transcript state");
    }
    pthread_mutex_lock(&g_test.mu);
    g_test.block_release = true;
    pthread_cond_broadcast(&g_test.cv);
    pthread_mutex_unlock(&g_test.mu);
    for (int i = 0; i < 3000 && (PicoAgent_IsBusy(first) || PicoAgent_IsBusy(second)); i++)
    {
        PicoAgentManager_Pump(app.agents);
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
        PicoAgentManager_Pump(app.agents);
        SleepOneMs();
    }
    bool cancel_isolated = !PicoAgent_IsBusy(first) && PicoAgent_IsBusy(second);
    PicoAgent_Cancel(second);
    for (int i = 0; i < 3000 && PicoAgent_IsBusy(second); i++)
    {
        PicoAgentManager_Pump(app.agents);
        SleepOneMs();
    }
    PicoApp_Free(&app);
    return isolated && cancel_isolated
               ? 0
               : Fail(name, "events, usage, transcript, or cancellation crossed agent boundaries");
}

