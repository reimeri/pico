/* Included by agent_behavior_test.c to reuse its deterministic provider and worker barriers. */

static bool WriteSubagentProfile(const char *root, const char *json,
                                 char *dir, size_t dir_cap, char *path, size_t path_cap)
{
    snprintf(dir, dir_cap, "%s/subagents", root);
    Pico_MkdirP(dir);
    snprintf(path, path_cap, "%s/exploration.json", dir);
    FILE *file = fopen(path, "wb");
    if (!file)
    {
        return false;
    }
    bool ok = fputs(json, file) >= 0 && fclose(file) == 0;
    return ok;
}

static bool WaitForManagerIdle(PicoApp *app)
{
    for (int i = 0; i < 4000; i++)
    {
        PicoAgentManager_Pump(app->agents);
        PicoAgent *parent = PicoApp_ActiveAgent(app);
        if (parent && !PicoAgent_IsBusy(parent) && pico_agent_count(app) == 1)
        {
            return true;
        }
        SleepOneMs();
    }
    return false;
}

static int TestNamedSubagentDelegation(void)
{
    const char *name = "named subagent delegation";
    char temp[] = "/tmp/pico-subagent-test-XXXXXX";
    if (!mkdtemp(temp))
    {
        return Fail(name, "could not create config directory");
    }
    char dir[4096];
    char path[4096];
    if (!WriteSubagentProfile(temp,
                              "{/* jsonc */\"description\":\"Explore\",\"purpose\":\"Inspect only\",\"tools\":[]}",
                              dir, sizeof(dir), path, sizeof(path)))
    {
        rmdir(temp);
        return Fail(name, "could not write profile");
    }
    snprintf(g_config_dir, sizeof(g_config_dir), "%s", temp);
    ResetTest(TEST_DELEGATION, 1);
    PicoApp app;
    InitApp(&app);
    PicoExt extension = pico_ext_subagent();
    extension.init(&app);
    PicoAgentManager_LoadProfiles(app.agents);
    snprintf(g_test.issue_tool_name, sizeof(g_test.issue_tool_name), "subagent");
    snprintf(g_test.issue_tool_args, sizeof(g_test.issue_tool_args),
             "{\"profile\":\"exploration\",\"task\":\"delegated question\"}");
    PicoAgent *parent = PicoApp_ActiveAgent(&app);
    PicoAgent_StartTurn(&app, parent, "parent request");
    bool completed = WaitForManagerIdle(&app);
    PicoTraceLine *trace = LastToolTrace(&app);
    pthread_mutex_lock(&g_test.mu);
    bool child_context = g_test.child_instructions &&
                         strstr(g_test.child_instructions, "Subagent profile: exploration") &&
                         strstr(g_test.child_instructions, "Inspect only") &&
                         g_test.child_input && strstr(g_test.child_input, "delegated question") &&
                         !strstr(g_test.child_input, "parent request") &&
                         g_test.child_tools[0] == '\0' &&
                         strcmp(g_test.child_model, "test-model") == 0 &&
                         strcmp(g_test.child_effort, "none") == 0;
    pthread_mutex_unlock(&g_test.mu);
    bool output = trace && trace->tool_output &&
                  strstr(trace->tool_output, "\"status\":\"completed\"") &&
                  strstr(trace->tool_output, "\"profile\":\"exploration\"") &&
                  strstr(trace->tool_output, "\"final_answer\":\"done\"");
    bool isolated = strcmp(parent->model, "test-model") == 0 &&
                    strcmp(parent->effort, "none") == 0 && pico_agent_count(&app) == 1;
    PicoApp_Free(&app);
    unlink(path);
    rmdir(dir);
    rmdir(temp);
    snprintf(g_config_dir, sizeof(g_config_dir), "/tmp/pico-agent-behavior");
    return completed && child_context && output && isolated
               ? 0 : Fail(name, "child context, policy, result, or cleanup was incorrect");
}

static int TestSubagentParentCancellation(void)
{
    const char *name = "subagent parent cancellation";
    char temp[] = "/tmp/pico-subagent-cancel-XXXXXX";
    if (!mkdtemp(temp))
    {
        return Fail(name, "could not create config directory");
    }
    char dir[4096];
    char path[4096];
    if (!WriteSubagentProfile(temp, "{\"purpose\":\"Block until cancelled\",\"tools\":[]}",
                              dir, sizeof(dir), path, sizeof(path)))
    {
        rmdir(temp);
        return Fail(name, "could not write profile");
    }
    snprintf(g_config_dir, sizeof(g_config_dir), "%s", temp);
    ResetTest(TEST_DELEGATION_CHILD_BLOCK, 1);
    PicoApp app;
    InitApp(&app);
    PicoExt extension = pico_ext_subagent();
    extension.init(&app);
    PicoAgentManager_LoadProfiles(app.agents);
    snprintf(g_test.issue_tool_name, sizeof(g_test.issue_tool_name), "subagent");
    snprintf(g_test.issue_tool_args, sizeof(g_test.issue_tool_args),
             "{\"profile\":\"exploration\",\"task\":\"block\"}");
    PicoAgent *parent = PicoApp_ActiveAgent(&app);
    PicoAgent_StartTurn(&app, parent, "delegate and cancel");
    bool child_running = false;
    for (int i = 0; i < 3000; i++)
    {
        PicoAgentManager_Pump(app.agents);
        if (pico_agent_count(&app) == 2)
        {
            child_running = true;
            break;
        }
        SleepOneMs();
    }
    PicoAgent_Cancel(parent);
    bool cancelled = WaitForManagerIdle(&app);
    PicoApp_Free(&app);
    unlink(path);
    rmdir(dir);
    rmdir(temp);
    snprintf(g_config_dir, sizeof(g_config_dir), "/tmp/pico-agent-behavior");
    return child_running && cancelled
               ? 0 : Fail(name, "parent did not wake and cascade cancellation to the child");
}

static int TestSubagentCancellationBeforeEnqueue(void)
{
    const char *name = "subagent cancellation before enqueue";
    char temp[] = "/tmp/pico-subagent-late-cancel-XXXXXX";
    if (!mkdtemp(temp))
    {
        return Fail(name, "could not create config directory");
    }
    char dir[4096];
    char path[4096];
    if (!WriteSubagentProfile(temp, "{\"purpose\":\"Late work\",\"tools\":[]}",
                              dir, sizeof(dir), path, sizeof(path)))
    {
        rmdir(temp);
        return Fail(name, "could not write profile");
    }

    snprintf(g_config_dir, sizeof(g_config_dir), "%s", temp);
    ResetTest(TEST_SINGLE, 1);
    PicoApp app;
    InitApp(&app);
    pico_add_tool(&app, "late_delegate", "delegate after a barrier", "{}",
                  LateDelegateTool, NULL);
    PicoAgentManager_LoadProfiles(app.agents);
    snprintf(g_test.issue_tool_name, sizeof(g_test.issue_tool_name), "late_delegate");
    PicoAgent *parent = PicoApp_ActiveAgent(&app);
    PicoAgent_StartTurn(&app, parent, "delegate after cancellation");

    bool entered = WaitForBlock(&app);
    if (entered)
    {
        PicoAgent_Cancel(parent);
    }
    pthread_mutex_lock(&g_test.mu);
    g_test.block_release = true;
    pthread_cond_broadcast(&g_test.cv);
    pthread_mutex_unlock(&g_test.mu);
    bool idle = WaitForManagerIdle(&app);
    pthread_mutex_lock(&g_test.mu);
    bool no_child_started = g_test.child_input == NULL;
    pthread_mutex_unlock(&g_test.mu);

    PicoApp_Free(&app);
    unlink(path);
    rmdir(dir);
    rmdir(temp);
    snprintf(g_config_dir, sizeof(g_config_dir), "/tmp/pico-agent-behavior");
    return entered && idle && no_child_started
               ? 0 : Fail(name, "cancelled callback enqueued or started a child");
}

static void ConfigureFakeSession(const char *profile)
{
    g_fake_session.enabled = true;
    g_fake_session.resolve_ok = true;
    snprintf(g_fake_session.id, sizeof(g_fake_session.id), "continued-child");
    snprintf(g_fake_session.path, sizeof(g_fake_session.path), "/tmp/continued-child.jsonl");
    snprintf(g_fake_session.profile, sizeof(g_fake_session.profile), "%s", profile);
    snprintf(g_fake_session.purpose, sizeof(g_fake_session.purpose), "Old profile purpose");
    snprintf(g_fake_session.header_model, sizeof(g_fake_session.header_model), "test-model");
    snprintf(g_fake_session.replayed_model, sizeof(g_fake_session.replayed_model), "previous-model");
    snprintf(g_fake_session.cache_key, sizeof(g_fake_session.cache_key), "previous-cache-key");
}

static int TestSubagentSessionContinuation(void)
{
    const char *name = "subagent exact session continuation";
    char temp[] = "/tmp/pico-subagent-resume-XXXXXX";
    if (!mkdtemp(temp))
    {
        return Fail(name, "could not create config directory");
    }
    char dir[4096];
    char path[4096];
    if (!WriteSubagentProfile(temp,
                              "{\"purpose\":\"Current profile purpose\"}",
                              dir, sizeof(dir), path, sizeof(path)))
    {
        rmdir(temp);
        return Fail(name, "could not write profile");
    }

    snprintf(g_config_dir, sizeof(g_config_dir), "%s", temp);
    ResetTest(TEST_DELEGATION, 1);
    PicoApp app;
    InitApp(&app);
    PicoExt extension = pico_ext_subagent();
    extension.init(&app);
    PicoAgentManager_LoadProfiles(app.agents);
    ConfigureFakeSession("exploration");
    int random_before = g_random_hex_calls;
    snprintf(g_test.issue_tool_name, sizeof(g_test.issue_tool_name), "subagent");
    snprintf(g_test.issue_tool_args, sizeof(g_test.issue_tool_args),
             "{\"profile\":\"exploration\",\"task\":\"continue review\","
             "\"session_id\":\"continued-child\"}");
    PicoAgent *parent = PicoApp_ActiveAgent(&app);
    PicoAgent_StartTurn(&app, parent, "parent continuation request");
    bool completed = WaitForManagerIdle(&app);
    PicoTraceLine *trace = LastToolTrace(&app);

    pthread_mutex_lock(&g_test.mu);
    bool restored_context = g_test.child_input &&
                            strstr(g_test.child_input, "previous delegated context") &&
                            strstr(g_test.child_input, "continue review") &&
                            !strstr(g_test.child_input, "parent continuation request");
    bool refreshed_policy = g_test.child_instructions &&
                            strstr(g_test.child_instructions, "Current profile purpose") &&
                            !strstr(g_test.child_instructions, "Old profile purpose") &&
                            strstr(g_test.child_tools, "ask_test") &&
                            strstr(g_test.child_tools, "subagent");
    pthread_mutex_unlock(&g_test.mu);
    bool output = trace && !trace->tool_error && trace->tool_output &&
                  strstr(trace->tool_output, "\"status\":\"completed\"") &&
                  strstr(trace->tool_output, "\"session_id\":\"continued-child\"") &&
                  strstr(trace->tool_output, "\"resumable\":true");
    bool durable = g_fake_session.replay_count == 1 &&
                   g_fake_session.log_user_count == 1 &&
                   g_fake_session.append_interrupted_count == 1 &&
                   !PicoAgentManager_SessionReserved(app.agents, g_fake_session.path, 0);
    bool rotated_latest_model = g_random_hex_calls == random_before + 2;

    PicoApp_Free(&app);
    unlink(path);
    rmdir(dir);
    rmdir(temp);
    snprintf(g_config_dir, sizeof(g_config_dir), "/tmp/pico-agent-behavior");
    return completed && restored_context && refreshed_policy && output && durable &&
                   rotated_latest_model
               ? 0 : Fail(name, "resume did not restore context, refresh policy, rotate cache, or clean up");
}

static int TestSubagentContinuationEmptyAnswer(void)
{
    const char *name = "subagent continuation empty answer";
    char temp[] = "/tmp/pico-subagent-empty-answer-XXXXXX";
    if (!mkdtemp(temp))
    {
        return Fail(name, "could not create config directory");
    }
    char dir[4096];
    char path[4096];
    if (!WriteSubagentProfile(temp, "{\"purpose\":\"Current profile purpose\"}",
                              dir, sizeof(dir), path, sizeof(path)))
    {
        rmdir(temp);
        return Fail(name, "could not write profile");
    }

    snprintf(g_config_dir, sizeof(g_config_dir), "%s", temp);
    ResetTest(TEST_DELEGATION_CHILD_EMPTY, 1);
    PicoApp app;
    InitApp(&app);
    PicoExt extension = pico_ext_subagent();
    extension.init(&app);
    PicoAgentManager_LoadProfiles(app.agents);
    ConfigureFakeSession("exploration");
    snprintf(g_test.issue_tool_name, sizeof(g_test.issue_tool_name), "subagent");
    snprintf(g_test.issue_tool_args, sizeof(g_test.issue_tool_args),
             "{\"profile\":\"exploration\",\"task\":\"return nothing\","
             "\"session_id\":\"continued-child\"}");
    PicoAgent_StartTurn(&app, PicoApp_ActiveAgent(&app), "continue with empty result");
    bool completed = WaitForManagerIdle(&app);
    PicoTraceLine *trace = LastToolTrace(&app);
    bool current_answer = trace && !trace->tool_error && trace->tool_output &&
                          strstr(trace->tool_output, "\"status\":\"completed\"") &&
                          strstr(trace->tool_output, "\"final_answer\":\"\"") &&
                          !strstr(trace->tool_output, "previous answer");

    PicoApp_Free(&app);
    unlink(path);
    rmdir(dir);
    rmdir(temp);
    snprintf(g_config_dir, sizeof(g_config_dir), "/tmp/pico-agent-behavior");
    return completed && current_answer
               ? 0 : Fail(name, "continuation reused an assistant answer from replayed history");
}

static int TestSubagentResumeFailures(void)
{
    const char *name = "subagent resume failures";
    char temp[] = "/tmp/pico-subagent-resume-fail-XXXXXX";
    if (!mkdtemp(temp))
    {
        return Fail(name, "could not create config directory");
    }
    char dir[4096];
    char path[4096];
    if (!WriteSubagentProfile(temp, "{\"purpose\":\"Current purpose\",\"tools\":[]}",
                              dir, sizeof(dir), path, sizeof(path)))
    {
        rmdir(temp);
        return Fail(name, "could not write profile");
    }

    snprintf(g_config_dir, sizeof(g_config_dir), "%s", temp);
    ResetTest(TEST_DELEGATION, 1);
    PicoApp app;
    InitApp(&app);
    PicoExt extension = pico_ext_subagent();
    extension.init(&app);
    PicoAgentManager_LoadProfiles(app.agents);

    ConfigureFakeSession("review");
    snprintf(g_test.issue_tool_name, sizeof(g_test.issue_tool_name), "subagent");
    snprintf(g_test.issue_tool_args, sizeof(g_test.issue_tool_args),
             "{\"profile\":\"exploration\",\"task\":\"mismatch\","
             "\"session_id\":\"continued-child\"}");
    PicoAgent_StartTurn(&app, PicoApp_ActiveAgent(&app), "mismatch");
    bool mismatch_done = WaitForManagerIdle(&app);
    PicoTraceLine *trace = LastToolTrace(&app);
    bool mismatch = trace && trace->tool_error && trace->tool_output &&
                    strstr(trace->tool_output, "profile does not match") &&
                    g_fake_session.replay_count == 0 && pico_agent_count(&app) == 1 &&
                    !PicoAgentManager_SessionReserved(app.agents, g_fake_session.path, 0);

    ResetTest(TEST_DELEGATION, 1);
    snprintf(g_test.issue_tool_name, sizeof(g_test.issue_tool_name), "subagent");
    snprintf(g_test.issue_tool_args, sizeof(g_test.issue_tool_args),
             "{\"profile\":\"missing\",\"task\":\"unknown\"}");
    PicoAgent_StartTurn(&app, PicoApp_ActiveAgent(&app), "unknown");
    bool unknown_done = WaitForManagerIdle(&app);
    trace = LastToolTrace(&app);
    bool unknown = trace && trace->tool_error && trace->tool_output &&
                   strstr(trace->tool_output, "\"profile\":\"missing\"") &&
                   strstr(trace->tool_output, "unknown subagent profile");

    ResetTest(TEST_DELEGATION, 1);
    ConfigureFakeSession("exploration");
    g_fake_session.resolve_ok = false;
    snprintf(g_test.issue_tool_name, sizeof(g_test.issue_tool_name), "subagent");
    snprintf(g_test.issue_tool_args, sizeof(g_test.issue_tool_args),
             "{\"profile\":\"exploration\",\"task\":\"missing session\","
             "\"session_id\":\"continued-child\"}");
    PicoAgent_StartTurn(&app, PicoApp_ActiveAgent(&app), "missing session");
    bool missing_done = WaitForManagerIdle(&app);
    trace = LastToolTrace(&app);
    bool missing = trace && trace->tool_error && trace->tool_output &&
                   strstr(trace->tool_output, "not found or is invalid") &&
                   pico_agent_count(&app) == 1 &&
                   !PicoAgentManager_SessionReserved(app.agents, g_fake_session.path, 0);

    PicoApp_Free(&app);
    unlink(path);
    rmdir(dir);
    rmdir(temp);
    snprintf(g_config_dir, sizeof(g_config_dir), "/tmp/pico-agent-behavior");
    return mismatch_done && mismatch && unknown_done && unknown && missing_done && missing
               ? 0 : Fail(name, "resume rejection leaked a child/reservation or lost its controlled error");
}

static int TestSubagentChildAsk(void)
{
    const char *name = "subagent child ask while parent waits";
    char temp[] = "/tmp/pico-subagent-ask-XXXXXX";
    if (!mkdtemp(temp))
    {
        return Fail(name, "could not create config directory");
    }
    char dir[4096];
    char path[4096];
    if (!WriteSubagentProfile(temp,
                              "{\"purpose\":\"Ask when needed\",\"tools\":[\"ask_test\"]}",
                              dir, sizeof(dir), path, sizeof(path)))
    {
        rmdir(temp);
        return Fail(name, "could not write profile");
    }

    snprintf(g_config_dir, sizeof(g_config_dir), "%s", temp);
    ResetTest(TEST_DELEGATION_CHILD_ASK, 2);
    PicoApp app;
    InitApp(&app);
    PicoExt extension = pico_ext_subagent();
    extension.init(&app);
    PicoAgentManager_LoadProfiles(app.agents);
    snprintf(g_test.issue_tool_name, sizeof(g_test.issue_tool_name), "subagent");
    snprintf(g_test.issue_tool_args, sizeof(g_test.issue_tool_args),
             "{\"profile\":\"exploration\",\"task\":\"ask a question\"}");
    PicoAgent *parent = PicoApp_ActiveAgent(&app);
    PicoAgent_StartTurn(&app, parent, "delegate with ask");

    PicoToolAsk ask;
    bool pending = false;
    for (int i = 0; i < 4000; i++)
    {
        PicoAgentManager_Pump(app.agents);
        if (pico_tool_pending_ask(&app, &ask))
        {
            pending = true;
            break;
        }
        SleepOneMs();
    }
    bool routed = pending && ask.agent_id != parent->id &&
                  strcmp(ask.profile, "exploration") == 0 &&
                  parent->state == PICO_AGENT_TOOL_WAIT &&
                  pico_agent_count(&app) == 2;
    bool answered = pending && pico_tool_answer(&app, ask.id, "{\"answer\":\"continue\"}");
    bool completed = answered && WaitForManagerIdle(&app);
    PicoTraceLine *trace = LastToolTrace(&app);
    pthread_mutex_lock(&g_test.mu);
    bool child_received = g_test.ask_rc[0][0] == PICO_ASK_OK &&
                          g_test.ask_answer[0][0] &&
                          strcmp(g_test.ask_answer[0][0], "{\"answer\":\"continue\"}") == 0 &&
                          strcmp(g_test.child_tools, "ask_test") == 0;
    pthread_mutex_unlock(&g_test.mu);
    bool output = trace && !trace->tool_error && trace->tool_output &&
                  strstr(trace->tool_output, "\"status\":\"completed\"");

    if (!answered && parent && PicoAgent_IsBusy(parent))
    {
        PicoAgent_Cancel(parent);
        WaitForManagerIdle(&app);
    }
    PicoApp_Free(&app);
    unlink(path);
    rmdir(dir);
    rmdir(temp);
    snprintf(g_config_dir, sizeof(g_config_dir), "/tmp/pico-agent-behavior");
    return routed && completed && child_received && output
               ? 0 : Fail(name, "child ask was not routable while the parent remained in tool wait");
}

static bool WaitForAgentIdle(PicoApp *app, PicoAgentId id, int expected_count)
{
    for (int i = 0; i < 4000; i++)
    {
        PicoAgentManager_Pump(app->agents);
        PicoAgent *agent = PicoAgentManager_Find(app->agents, id);
        if (agent && !PicoAgent_IsBusy(agent) && pico_agent_count(app) == expected_count)
        {
            return true;
        }
        SleepOneMs();
    }
    return false;
}

static int TestSubagentDelegationCaps(void)
{
    const char *name = "subagent delegation caps";
    char temp[] = "/tmp/pico-subagent-caps-XXXXXX";
    if (!mkdtemp(temp))
    {
        return Fail(name, "could not create config directory");
    }
    char dir[4096];
    char path[4096];
    if (!WriteSubagentProfile(temp, "{\"purpose\":\"Nested work\",\"tools\":[]}",
                              dir, sizeof(dir), path, sizeof(path)))
    {
        rmdir(temp);
        return Fail(name, "could not write profile");
    }
    snprintf(g_config_dir, sizeof(g_config_dir), "%s", temp);

    ResetTest(TEST_DELEGATION, 1);
    PicoApp depth_app;
    InitApp(&depth_app);
    PicoExt extension = pico_ext_subagent();
    extension.init(&depth_app);
    PicoAgentManager_LoadProfiles(depth_app.agents);
    PicoAgentId parent_id = pico_agent_active(&depth_app);
    PicoAgentId deepest_id = 0;
    for (int depth = 1; depth <= PICO_MAX_DELEGATION_DEPTH; depth++)
    {
        PicoAgentCreateOptions options = {
            .kind = PICO_AGENT_SUBAGENT,
            .parent_id = parent_id,
            .profile = "manual",
            .purpose = "manual chain",
            .session_start = PICO_SESSION_NONE,
        };
        if (pico_agent_create(&depth_app, &options, &deepest_id) != PICO_AGENT_RESULT_OK)
        {
            PicoApp_Free(&depth_app);
            unlink(path);
            rmdir(dir);
            rmdir(temp);
            return Fail(name, "could not build the boundary-depth chain");
        }
        parent_id = deepest_id;
    }
    pico_agent_select(&depth_app, deepest_id);
    snprintf(g_test.issue_tool_name, sizeof(g_test.issue_tool_name), "subagent");
    snprintf(g_test.issue_tool_args, sizeof(g_test.issue_tool_args),
             "{\"profile\":\"exploration\",\"task\":\"too deep\"}");
    PicoAgent_StartTurn(&depth_app, PicoAgentManager_Find(depth_app.agents, deepest_id), "depth");
    bool depth_done = WaitForAgentIdle(&depth_app, deepest_id,
                                       PICO_MAX_DELEGATION_DEPTH + 1);
    PicoTraceLine *trace = LastToolTrace(&depth_app);
    bool depth_limited = trace && trace->tool_error && trace->tool_output &&
                         strstr(trace->tool_output, "depth limit");
    PicoApp_Free(&depth_app);

    ResetTest(TEST_DELEGATION, 1);
    PicoApp count_app;
    InitApp(&count_app);
    extension = pico_ext_subagent();
    extension.init(&count_app);
    PicoAgentManager_LoadProfiles(count_app.agents);
    PicoAgentId root_id = pico_agent_active(&count_app);
    for (int i = 1; i < PICO_MAX_AGENTS; i++)
    {
        PicoAgentCreateOptions options = {
            .kind = PICO_AGENT_NORMAL,
            .session_start = PICO_SESSION_NONE,
        };
        PicoAgentId id = 0;
        if (pico_agent_create(&count_app, &options, &id) != PICO_AGENT_RESULT_OK)
        {
            PicoApp_Free(&count_app);
            unlink(path);
            rmdir(dir);
            rmdir(temp);
            return Fail(name, "could not fill the agent boundary");
        }
    }
    snprintf(g_test.issue_tool_name, sizeof(g_test.issue_tool_name), "subagent");
    snprintf(g_test.issue_tool_args, sizeof(g_test.issue_tool_args),
             "{\"profile\":\"exploration\",\"task\":\"one too many\"}");
    PicoAgent_StartTurn(&count_app, PicoAgentManager_Find(count_app.agents, root_id), "limit");
    bool count_done = WaitForAgentIdle(&count_app, root_id, PICO_MAX_AGENTS);
    trace = LastToolTrace(&count_app);
    bool count_limited = trace && trace->tool_error && trace->tool_output &&
                         strstr(trace->tool_output, "agent or delegation depth limit reached");
    PicoApp_Free(&count_app);

    unlink(path);
    rmdir(dir);
    rmdir(temp);
    snprintf(g_config_dir, sizeof(g_config_dir), "/tmp/pico-agent-behavior");
    return depth_done && depth_limited && count_done && count_limited
               ? 0 : Fail(name, "agent or depth cap did not return a controlled delegation error");
}

static int TestSubagentDirectChildCancellation(void)
{
    const char *name = "direct subagent child cancellation";
    char temp[] = "/tmp/pico-subagent-child-cancel-XXXXXX";
    if (!mkdtemp(temp))
    {
        return Fail(name, "could not create config directory");
    }
    char dir[4096];
    char path[4096];
    if (!WriteSubagentProfile(temp, "{\"purpose\":\"Block until cancelled\",\"tools\":[]}",
                              dir, sizeof(dir), path, sizeof(path)))
    {
        rmdir(temp);
        return Fail(name, "could not write profile");
    }
    snprintf(g_config_dir, sizeof(g_config_dir), "%s", temp);
    ResetTest(TEST_DELEGATION_CHILD_BLOCK, 1);
    PicoApp app;
    InitApp(&app);
    PicoExt extension = pico_ext_subagent();
    extension.init(&app);
    PicoAgentManager_LoadProfiles(app.agents);
    snprintf(g_test.issue_tool_name, sizeof(g_test.issue_tool_name), "subagent");
    snprintf(g_test.issue_tool_args, sizeof(g_test.issue_tool_args),
             "{\"profile\":\"exploration\",\"task\":\"block\"}");
    PicoAgent *parent = PicoApp_ActiveAgent(&app);
    PicoAgent_StartTurn(&app, parent, "delegate and cancel child");

    PicoAgentId child_id = 0;
    for (int i = 0; i < 3000 && !child_id; i++)
    {
        PicoAgentManager_Pump(app.agents);
        for (int a = 0; a < app.agents->count; a++)
        {
            if (app.agents->agents[a]->id != parent->id)
            {
                child_id = app.agents->agents[a]->id;
                break;
            }
        }
        SleepOneMs();
    }
    bool cancelled = child_id &&
                     pico_agent_cancel(&app, child_id) == PICO_AGENT_RESULT_OK &&
                     WaitForManagerIdle(&app);
    PicoTraceLine *trace = LastToolTrace(&app);
    bool reported = trace && trace->tool_error && trace->tool_output &&
                    strstr(trace->tool_output, "\"status\":\"cancelled\"") &&
                    strstr(trace->tool_output, "\"profile\":\"exploration\"");

    if (!cancelled && PicoAgent_IsBusy(parent))
    {
        PicoAgent_Cancel(parent);
        WaitForManagerIdle(&app);
    }
    PicoApp_Free(&app);
    unlink(path);
    rmdir(dir);
    rmdir(temp);
    snprintf(g_config_dir, sizeof(g_config_dir), "/tmp/pico-agent-behavior");
    return cancelled && reported
               ? 0 : Fail(name, "direct child cancellation was not published as cancelled");
}
