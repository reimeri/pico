/* Included by agent_behavior_test.c so manager concurrency uses the same deterministic fake provider host. */

static bool g_close_hook_saw_removed;
static bool g_close_hook_saw_workspace;

static void InspectClosedAgent(PicoApp *app, const PicoHookEvent *event)
{
    PicoAgentInfo info;
    if (event && event->hook == PICO_HOOK_ON_AGENT_DESTROY)
    {
        g_close_hook_saw_removed = !pico_agent_find(app, event->agent_id, &info);
        g_close_hook_saw_workspace = event->workspace_key[0] && event->workspace_path[0];
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
    g_close_hook_saw_workspace = false;
    close_contract = close_contract && pico_agent_close(&app, close_id) == PICO_AGENT_RESULT_OK &&
                     g_close_hook_saw_removed && g_close_hook_saw_workspace;

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

static int TestCrossWorkspaceTargeting(void)
{
    const char *name = "cross-workspace targeting";
    char first_path[] = "/tmp/pico-target-first-XXXXXX";
    char second_path[] = "/tmp/pico-target-second-XXXXXX";
    if (!mkdtemp(first_path) || !mkdtemp(second_path))
    {
        return Fail(name, "could not create workspace roots");
    }
    ResetTest(TEST_SINGLE, 0);
    PicoApp app;
    InitApp(&app);
    PicoWorkspace workspaces[2];
    memset(workspaces, 0, sizeof(workspaces));
    snprintf(workspaces[0].key, sizeof(workspaces[0].key), "first");
    snprintf(workspaces[0].path, sizeof(workspaces[0].path), "%s", first_path);
    snprintf(workspaces[1].key, sizeof(workspaces[1].key), "second");
    snprintf(workspaces[1].path, sizeof(workspaces[1].path), "%s", second_path);
    workspaces[0].available = true;
    workspaces[1].available = true;
    PicoWorkspaceRegistry registry = {.items = workspaces, .count = 2, .capacity = 2};
    app.workspaces = &registry;
    PicoAgent *first = PicoApp_ActiveAgent(&app);
    snprintf(first->workspace_key, sizeof(first->workspace_key), "%s", workspaces[0].key);
    snprintf(first->workspace_path, sizeof(first->workspace_path), "%s", workspaces[0].path);
    snprintf(app.workspace, sizeof(app.workspace), "%s", first_path);
    PicoAgentCreateOptions options = {
        .kind = PICO_AGENT_NORMAL,
        .workspace_key = workspaces[1].key,
        .session_start = PICO_SESSION_NONE,
    };
    PicoAgentId second_id = 0;
    bool created = pico_agent_create(&app, &options, &second_id) == PICO_AGENT_RESULT_OK;
    PicoAgent *second = PicoAgentManager_Find(app.agents, second_id);
    if (created)
    {
        PicoAgent_StartTurn(&app, first, "first workspace");
        for (int i = 0; i < 3000 && PicoAgent_IsBusy(first); i++)
        {
            PicoAgentManager_Pump(app.agents);
            SleepOneMs();
        }
        PicoAgent_StartTurn(&app, second, "second workspace");
        for (int i = 0; i < 3000 && PicoAgent_IsBusy(second); i++)
        {
            PicoAgentManager_Pump(app.agents);
            SleepOneMs();
        }
    }
    bool first_seen = false;
    bool second_seen = false;
    pthread_mutex_lock(&g_test.mu);
    for (int i = 0; i < g_test.context_seen_count; i++)
    {
        if (g_test.context_ids[i] == first->id &&
            strcmp(g_test.context_workspaces[i], first_path) == 0) first_seen = true;
        if (g_test.context_ids[i] == second_id &&
            strcmp(g_test.context_workspaces[i], second_path) == 0) second_seen = true;
    }
    pthread_mutex_unlock(&g_test.mu);
    bool selected = created && pico_agent_select(&app, second_id) &&
                    strcmp(app.workspace, second_path) == 0;
    PicoAgentCreateOptions child_options = {
        .kind = PICO_AGENT_SUBAGENT,
        .parent_id = second_id,
        .session_start = PICO_SESSION_NONE,
    };
    PicoAgentId child_id = 0;
    bool inherited = pico_agent_create(&app, &child_options, &child_id) == PICO_AGENT_RESULT_OK;
    PicoAgentInfo child_info;
    inherited = inherited && pico_agent_find(&app, child_id, &child_info) &&
                strcmp(child_info.workspace_path, second_path) == 0 &&
                strcmp(child_info.workspace_key, workspaces[1].key) == 0;
    app.workspaces = NULL;
    PicoApp_Free(&app);
    rmdir(first_path);
    rmdir(second_path);
    return created && first_seen && second_seen && selected && inherited
               ? 0
               : Fail(name, "worker context, selection alias, or child inheritance crossed workspaces");
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
    PicoAgentInfo first_info;
    PicoAgentInfo second_info;
    if (pico_agent_create(&app, &options, &second_id) != PICO_AGENT_RESULT_OK ||
        pico_agent_count(&app) != 2 || second_id == first_id ||
        !pico_agent_find(&app, first_id, &first_info) ||
        !pico_agent_find(&app, second_id, &second_info) ||
        !first_info.workspace_key[0] || !first_info.workspace_path[0] ||
        strcmp(first_info.workspace_key, second_info.workspace_key) != 0 ||
        strcmp(first_info.workspace_path, second_info.workspace_path) != 0)
    {
        PicoApp_Free(&app);
        return Fail(name, "could not create two workspace-owned independent agents");
    }
    PicoAgentCreateOptions invalid_child = {
        .kind = PICO_AGENT_SUBAGENT,
        .parent_id = first_id,
        .workspace_key = first_info.workspace_key,
        .session_start = PICO_SESSION_NONE,
    };
    PicoAgentId invalid_id = 0;
    if (pico_agent_create(&app, &invalid_child, &invalid_id) != PICO_AGENT_RESULT_INVALID)
    {
        PicoApp_Free(&app);
        return Fail(name, "subagent workspace override was accepted");
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

    first->ui.chat_sel.msg = 9;
    first->ui.chat_follow_bottom = false;
    if (!pico_agent_select(&app, second_id) || second->ui.chat_sel.msg != -1 ||
        !second->ui.chat_follow_bottom || first->ui.chat_sel.msg != 9 || first->ui.chat_follow_bottom)
    {
        pthread_mutex_lock(&g_test.mu);
        g_test.block_release = true;
        pthread_cond_broadcast(&g_test.cv);
        pthread_mutex_unlock(&g_test.mu);
        PicoApp_Free(&app);
        return Fail(name, "selection did not keep per-agent transcript state");
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

static int TestAgentUiAndPresentation(void)
{
    const char *name = "per-agent ui and descendant presentation";
    ResetTest(TEST_SINGLE, 0);
    PicoApp app;
    InitApp(&app);
    PicoAgentId first_id = pico_agent_active(&app);
    PicoAgentCreateOptions options = {
        .kind = PICO_AGENT_NORMAL,
        .session_start = PICO_SESSION_NONE,
        .select = false,
    };
    PicoAgentId second_id = 0;
    if (pico_agent_create(&app, &options, &second_id) != PICO_AGENT_RESULT_OK)
    {
        PicoApp_Free(&app);
        return Fail(name, "could not create a second agent");
    }
    PicoAgent *first = PicoAgentManager_Find(app.agents, first_id);
    PicoAgent *second = PicoAgentManager_Find(app.agents, second_id);
    if (!first || !second || !first->ui.composer.text || !second->ui.composer.text)
    {
        PicoApp_Free(&app);
        return Fail(name, "agents were missing composer state");
    }
    snprintf(first->ui.composer.text, (size_t)first->ui.composer.capacity, "alpha");
    first->ui.composer.length = 5;
    first->ui.chat_sel.msg = 4;
    first->ui.chat_follow_bottom = false;
    snprintf(second->ui.composer.text, (size_t)second->ui.composer.capacity, "beta");
    second->ui.composer.length = 4;
    if (!pico_agent_select(&app, second_id) || first->ui.composer.length != 5 ||
        strcmp(first->ui.composer.text, "alpha") != 0 || first->ui.chat_sel.msg != 4 ||
        first->ui.chat_follow_bottom || pico_app_composer(&app) != &second->ui.composer)
    {
        PicoApp_Free(&app);
        return Fail(name, "selection leaked composer or chat state between agents");
    }

    PicoAgent_StartTurn(&app, first, "background");
    for (int i = 0; i < 3000 && PicoAgent_IsBusy(first); i++)
    {
        PicoAgentManager_Pump(app.agents);
        SleepOneMs();
    }
    PicoAgentInfo first_info;
    if (PicoAgent_IsBusy(first) || !pico_agent_find(&app, first_id, &first_info) ||
        !first_info.unread_completion || first_info.presentation != PICO_AGENT_PRESENT_COMPLETED)
    {
        PicoApp_Free(&app);
        return Fail(name, "background completion did not mark the hidden session unread");
    }
    if (!pico_agent_select(&app, first_id) || !pico_agent_find(&app, first_id, &first_info) ||
        first_info.unread_completion || first_info.presentation != PICO_AGENT_PRESENT_IDLE)
    {
        PicoApp_Free(&app);
        return Fail(name, "selecting a completed session did not clear unread status");
    }

    PicoAgentCreateOptions child_options = {
        .kind = PICO_AGENT_SUBAGENT,
        .parent_id = first_id,
        .profile = "test",
        .purpose = "child",
        .session_start = PICO_SESSION_NONE,
    };
    PicoAgentId child_id = 0;
    if (pico_agent_create(&app, &child_options, &child_id) != PICO_AGENT_RESULT_OK)
    {
        PicoApp_Free(&app);
        return Fail(name, "could not create a descendant");
    }
    PicoAgent *child = PicoAgentManager_Find(app.agents, child_id);
    child->state = PICO_AGENT_ERROR;
    child->error = JsonDup("child failed");
    child->unread_completion = true;
    if (!pico_agent_find(&app, first_id, &first_info) ||
        first_info.presentation != PICO_AGENT_PRESENT_ERROR || first_info.unread_completion)
    {
        PicoApp_Free(&app);
        return Fail(name, "live descendant error did not bubble without child completion");
    }
    PicoApp_Free(&app);
    return 0;
}

