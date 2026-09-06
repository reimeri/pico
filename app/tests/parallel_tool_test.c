/* Included by agent_behavior_test.c; workers are released explicitly, never by a timing assumption. */
#include <errno.h>
#include <signal.h>
#include <sys/wait.h>

typedef struct ParallelFixture {
    pthread_mutex_t mu;
    pthread_cond_t cv;
    unsigned entered;
    unsigned released;
    unsigned exited;
    int calls;
    int count;
    int barrier;
    int error_index;
    bool ask;
    bool processes;
    pid_t children[2];
    int child_status[2];
    bool delegate;
    bool child_calls;
    bool resume;
    int rewrite_mode;
    char applied_order[64];
    char history_order[64];
} ParallelFixture;

static ParallelFixture g_parallel = {
    .mu = PTHREAD_MUTEX_INITIALIZER,
    .cv = PTHREAD_COND_INITIALIZER,
};

static void ParallelRelease(unsigned mask)
{
    pthread_mutex_lock(&g_parallel.mu);
    g_parallel.released |= mask;
    pthread_cond_broadcast(&g_parallel.cv);
    pthread_mutex_unlock(&g_parallel.mu);
}

static bool ParallelWait(PicoHost *app, unsigned mask, bool exited)
{
    for (int i = 0; i < 3000; i++)
    {
        PicoWorkspace_Pump(TestWs(app));
        pthread_mutex_lock(&g_parallel.mu);
        unsigned value = exited ? g_parallel.exited : g_parallel.entered;
        pthread_mutex_unlock(&g_parallel.mu);
        if ((value & mask) == mask) return true;
        SleepOneMs();
    }
    return false;
}

static void ParallelTool(PicoAgentContext *ctx, const char *args, PicoToolResult *out, void *state)
{
    (void)state;
    JsonDoc doc;
    JsonParse(&doc, args, strlen(args));
    int index = JsonObjInt(&doc, 0, "index", 0);
    JsonFree(&doc);
    unsigned bit = 1u << index;
    if (g_parallel.processes)
    {
        pid_t pid = fork();
        if (pid == 0)
        {
            setpgid(0, 0);
            for (;;) pause();
        }
        if (pid > 0)
        {
            setpgid(pid, pid);
            pico_tool_set_child(ctx, pid);
        }
        pthread_mutex_lock(&g_parallel.mu);
        g_parallel.children[index] = pid;
        g_parallel.entered |= bit;
        pthread_mutex_unlock(&g_parallel.mu);
        int status = 0;
        if (pid > 0)
        {
            while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {}
            pico_tool_set_child(ctx, 0);
        }
        pthread_mutex_lock(&g_parallel.mu);
        g_parallel.child_status[index] = status;
        g_parallel.exited |= bit;
        pthread_mutex_unlock(&g_parallel.mu);
        out->output = JsonDup("process finished");
        return;
    }
    pthread_mutex_lock(&g_parallel.mu);
    g_parallel.entered |= bit;
    pthread_cond_broadcast(&g_parallel.cv);
    bool ask = g_parallel.ask;
    while (!ask && !(g_parallel.released & bit))
        pthread_cond_wait(&g_parallel.cv, &g_parallel.mu);
    pthread_mutex_unlock(&g_parallel.mu);
    if (ask)
    {
        char request[128];
        snprintf(request, sizeof(request), "{\"type\":\"confirm\",\"message\":\"call %d\"}", index);
        out->is_error = pico_tool_ask(ctx, request, &out->output) != PICO_ASK_OK;
    }
    else
    {
        out->output = JsonDup(args);
        out->details_json = JsonDup(args);
        out->is_error = index == g_parallel.error_index;
    }
    pthread_mutex_lock(&g_parallel.mu);
    g_parallel.exited |= bit;
    pthread_cond_broadcast(&g_parallel.cv);
    pthread_mutex_unlock(&g_parallel.mu);
}

static bool ParallelApply(PicoWorkspace *workspace, PicoAgentId id, const char *details, bool replay, void *state)
{
    (void)workspace; (void)id; (void)replay; (void)state;
    JsonDoc doc;
    if (JsonParse(&doc, details, strlen(details)) != 0) return false;
    int index = JsonObjInt(&doc, 0, "index", -1);
    JsonFree(&doc);
    size_t n = strlen(g_parallel.applied_order);
    snprintf(g_parallel.applied_order + n, sizeof(g_parallel.applied_order) - n, "%d,", index);
    return true;
}

static int ParallelProvider(PicoAgentContext *ctx, const PicoLlmTurn *turn,
                            PicoLlmCancelFn cancel, PicoLlmDeltaFn delta, void *user,
                            PicoLlmResult *out, void *state)
{
    (void)cancel; (void)delta; (void)user; (void)state;
    bool child = pico_agent_context_profile(ctx)[0] != '\0';
    int child_index = 0;
    bool has_results = false;
    for (int i = 0; i < turn->input_count; i++)
    {
        const char *task = strstr(turn->input_json[i], "child-");
        if (task) (void)sscanf(task, "child-%d", &child_index);
        if (strstr(turn->input_json[i], "\"type\":\"tool_result\"")) has_results = true;
    }
    pthread_mutex_lock(&g_parallel.mu);
    int round = child ? (has_results ? 2 : 1) : ++g_parallel.calls;
    if (!child && round > 1)
    {
        for (int i = 0; i < turn->input_count; i++)
        {
            JsonDoc doc;
            JsonParse(&doc, turn->input_json[i], strlen(turn->input_json[i]));
            if (JsonEq(&doc, JsonObjGet(&doc, 0, "type"), "tool_result"))
            {
                char *id = JsonObjStr(&doc, 0, "call_id");
                size_t n = strlen(g_parallel.history_order);
                snprintf(g_parallel.history_order + n, sizeof(g_parallel.history_order) - n,
                         "%s,", id ? id : "?");
                free(id);
            }
            JsonFree(&doc);
        }
    }
    bool delegate = g_parallel.delegate;
    bool child_calls = g_parallel.child_calls;
    int count = child ? (child_calls ? 2 : 1) : g_parallel.count;
    int barrier = g_parallel.barrier;
    pthread_mutex_unlock(&g_parallel.mu);
    if (round > 1)
    {
        pico_llm_result_add_text(out, "done");
        return PICO_LLM_OK;
    }
    for (int i = 0; i < count; i++)
    {
        int index = child ? child_index * 2 + i : i;
        char id[32], args[128];
        snprintf(id, sizeof(id), "parallel-%d", index);
        if (delegate && !child)
            snprintf(args, sizeof(args), "{\"profile\":\"%s\",\"task\":\"child-%d\"%s}",
                     i == barrier ? "worker" : "exploration", i, g_parallel.resume ? ",\"session_id\":\"continued-child\"" : "");
        else
            snprintf(args, sizeof(args), "{\"index\":%d}", index);
        pico_llm_result_add_tool_call(out, id, delegate && !child ? "subagent" :
                                     i == barrier ? "serial_probe" : "parallel_probe", args, NULL);
    }
    return PICO_LLM_OK;
}

static void ParallelInit(PicoHost *app, int count, int limit)
{
    ResetTest(TEST_SINGLE, 0);
    InitApp(app);
    pthread_mutex_lock(&g_parallel.mu);
    g_parallel.entered = g_parallel.released = g_parallel.exited = 0;
    g_parallel.calls = 0;
    g_parallel.count = count;
    g_parallel.barrier = -1;
    g_parallel.rewrite_mode = -1;
    g_parallel.error_index = -1;
    g_parallel.ask = g_parallel.delegate = g_parallel.child_calls = false;
    g_parallel.processes = g_parallel.resume = false;
    g_parallel.applied_order[0] = '\0';
    memset(g_parallel.children, 0, sizeof(g_parallel.children));
    memset(g_parallel.child_status, 0, sizeof(g_parallel.child_status));
    g_parallel.history_order[0] = '\0';
    pthread_mutex_unlock(&g_parallel.mu);
    PicoWorkspace *ws = TestWs(app);
    ws->settings.max_parallel_tools = limit;
    PicoHost_BeginRegistration(app, PICO_REG_WORKSPACE, ws);
    pico_add_tool(ws, "parallel_probe", "parallel test", "{}", ParallelTool, ParallelApply, PICO_TOOL_PARALLEL);
    pico_add_tool(ws, "serial_probe", "serial test", "{}", ParallelTool, ParallelApply, PICO_TOOL_SEQUENTIAL);
    pico_add_provider(ws, &(PicoProvider){.name = "parallel", .stream = ParallelProvider, .map_context = true});
    PicoHost_PublishRegistration(app, NULL);
    snprintf(ws->models[0].provider, sizeof(ws->models[0].provider), "parallel");
}

static bool ParallelResult(PicoHost *app, int index)
{
    char id[32];
    snprintf(id, sizeof(id), "parallel-%d", index);
    for (int i = 0; i < 3000; i++)
    {
        PicoWorkspace_Pump(TestWs(app));
        PicoTraceLine *row = ToolTraceByCallId(app, id);
        if (row && row->tool_output) return true;
        SleepOneMs();
    }
    return false;
}

static int TestParallelScheduling(void)
{
    PicoHost app;
    ParallelInit(&app, 5, 2);
    g_parallel.barrier = 3;
    g_parallel.error_index = 1;
    PicoAgent *agent = TestAgent(&app);
    PicoAgent_StartTurn(&app, agent, "parallel batch");
    bool overlap = ParallelWait(&app, 3, false);
    bool bounded = PicoAgent_ToolCallProgress(agent, "parallel-2") == PICO_TOOL_CALL_QUEUED;
    /* A mid-turn settings change must not resize this batch. */
    TestWs(&app)->settings.max_parallel_tools = 1;
    ParallelRelease(2);
    bool refilled = ParallelWait(&app, 7, false) && ParallelResult(&app, 1);
    PicoTraceLine *second = ToolTraceByCallId(&app, "parallel-1");
    bool isolated = second && second->tool_error && PicoAgent_IsBusy(agent) &&
                    PicoAgent_ToolCallProgress(agent, "parallel-0") == PICO_TOOL_CALL_RUNNING;
    bool persisted = strcmp(g_test.logged_tool_ids, "parallel-1,") == 0;
    ParallelRelease(1);
    bool first_done = ParallelResult(&app, 0);
    ParallelRelease(4);
    bool barrier = ParallelWait(&app, 8, false) &&
                   PicoAgent_ToolCallProgress(agent, "parallel-4") == PICO_TOOL_CALL_QUEUED;
    ParallelRelease(8);
    bool tail = ParallelWait(&app, 16, false);
    ParallelRelease(~0u);
    bool idle = WaitForIdle(&app);
    bool ordered = strcmp(g_parallel.history_order,
                          "parallel-1,parallel-0,parallel-2,parallel-3,parallel-4,") == 0 &&
                   strcmp(g_parallel.applied_order, "0,2,3,4,") == 0;
    bool rows_ordered = false;
    for (int m = 0; m < agent->message_count; m++)
    {
        int next = 0;
        for (int t = 0; t < agent->messages[m].trace_count; t++)
        {
            PicoTraceLine *row = &agent->messages[m].trace[t];
            if (!row->is_tool) continue;
            char id[32];
            snprintf(id, sizeof(id), "parallel-%d", next++);
            if (!row->tool_call_id || strcmp(row->tool_call_id, id) != 0) next = -100;
        }
        if (next == 5) rows_ordered = true;
    }
    PicoHost_Shutdown(&app);
    return overlap && bounded && refilled && isolated && persisted && first_done && barrier && tail &&
           idle && ordered && rows_ordered ? 0 : Fail("parallel scheduling", "overlap, limit, barrier, failure isolation, or result ordering failed");
}

static int TestParallelLimitOne(void)
{
    PicoHost app;
    ParallelInit(&app, 2, 1);
    PicoAgent_StartTurn(&app, TestAgent(&app), "one slot");
    bool queued = ParallelWait(&app, 1, false) &&
                  PicoAgent_ToolCallProgress(TestAgent(&app), "parallel-1") == PICO_TOOL_CALL_QUEUED;
    ParallelRelease(1);
    bool next = ParallelWait(&app, 2, false);
    ParallelRelease(~0u);
    bool idle = WaitForIdle(&app);
    PicoHost_Shutdown(&app);
    return queued && next && idle ? 0 : Fail("parallel limit one", "eligible calls did not execute sequentially");
}

static int TestParallelAsks(void)
{
    PicoHost app;
    ParallelInit(&app, 2, 2);
    g_parallel.ask = true;
    PicoAgent_StartTurn(&app, TestAgent(&app), "two asks");
    bool overlap = ParallelWait(&app, 3, false);
    PicoToolAsk first = {0}, second = {0};
    bool pending = WaitForPending(&app, 0, &first);
    int first_index = pending && strstr(first.request_json, "call 1") ? 1 : 0;
    bool answered = pending && pico_tool_answer(&app, first.id, "{\"answer\":\"first\"}");
    bool independent = answered && ParallelResult(&app, first_index) &&
                       WaitForPending(&app, first.id, &second) && second.id != first.id;
    bool stale = !pico_tool_answer(&app, first.id, "{}");
    PicoAgent_Cancel(TestAgent(&app));
    bool idle = WaitForIdle(&app);
    PicoTraceLine *a = ToolTraceByCallId(&app, first_index ? "parallel-1" : "parallel-0");
    PicoTraceLine *b = ToolTraceByCallId(&app, first_index ? "parallel-0" : "parallel-1");
    bool routed = a && a->tool_output && strstr(a->tool_output, "first") && !a->tool_error && b && b->tool_error;
    PicoHost_Shutdown(&app);
    return overlap && independent && stale && idle && routed ? 0 : Fail("parallel asks", "ask identity, sibling isolation, or cancellation failed");
}

static int TestParallelCancelKeepsCompleted(void)
{
    PicoHost app;
    ParallelInit(&app, 2, 2);
    PicoAgent_StartTurn(&app, TestAgent(&app), "finish last call first");
    bool overlap = ParallelWait(&app, 3, false);
    ParallelRelease(2);
    bool completed = ParallelResult(&app, 1);
    PicoAgent_ForceCancel(&app, TestAgent(&app));
    PicoTraceLine *last = ToolTraceByCallId(&app, "parallel-1");
    bool preserved = last && !last->tool_error && last->tool_output && strstr(last->tool_output, "index");
    ParallelRelease(~0u);
    PicoHost_Shutdown(&app);
    return overlap && completed && preserved ? 0 : Fail("parallel cancelled batch", "force cancellation overwrote an already completed result");
}

static int TestParallelProcessCancellation(void)
{
    PicoHost app;
    ParallelInit(&app, 2, 2);
    g_parallel.processes = true;
    PicoAgent_StartTurn(&app, TestAgent(&app), "two tracked process groups");
    bool overlap = ParallelWait(&app, 3, false);
    PicoAgent_ForceCancel(&app, TestAgent(&app));
    bool exited = ParallelWait(&app, 3, true);
    pthread_mutex_lock(&g_parallel.mu);
    bool killed = true;
    for (int i = 0; i < 2; i++)
    {
        killed = killed && g_parallel.children[i] > 0 && WIFSIGNALED(g_parallel.child_status[i]) &&
                 WTERMSIG(g_parallel.child_status[i]) == SIGKILL;
        if (!exited && g_parallel.children[i] > 0) kill(g_parallel.children[i], SIGKILL);
    }
    pthread_mutex_unlock(&g_parallel.mu);
    PicoHost_Shutdown(&app);
    return overlap && exited && killed ? 0 : Fail("parallel process cancellation", "force cancellation did not kill every call's process group");
}

static void ParallelBeforeAsk(PicoAgentContext *ctx, PicoToolEvent *event, void *state)
{
    (void)state;
    /* A permission-style interceptor can itself block independently in each call. */
    PicoToolResult result = {0};
    ParallelTool(ctx, event->args_json, &result, NULL);
    event->deny = true;
    event->result = result.output;
}

static int TestParallelBeforeHooks(void)
{
    PicoHost app;
    ParallelInit(&app, 2, 2);
    g_parallel.ask = true;
    TestAddToolBeforeHook(&app, ParallelBeforeAsk);
    PicoAgent_StartTurn(&app, TestAgent(&app), "two permission prompts");
    bool overlap = ParallelWait(&app, 3, false);
    PicoToolAsk ask = {0};
    bool pending = WaitForPending(&app, 0, &ask);
    PicoAgent_Cancel(TestAgent(&app));
    bool idle = WaitForIdle(&app);
    bool both = (g_parallel.exited & 3) == 3 && !pico_tool_pending_ask(&app, &ask);
    PicoHost_Shutdown(&app);
    return overlap && pending && idle && both ? 0 : Fail("parallel before hooks", "before callbacks did not overlap or cancellation failed to wake all asks");
}

static int TestParallelForceCancelReload(void)
{
    PicoHost app;
    ParallelInit(&app, 3, 2);
    PicoAgent *agent = TestAgent(&app);
    PicoWorkspace *ws = TestWs(&app);
    PicoAgent_StartTurn(&app, agent, "abandon two calls");
    bool overlap = ParallelWait(&app, 3, false);
    PicoAgent_ForceCancel(&app, agent);
    PicoAgent_StartTurn(&app, agent, "replacement generation");
    bool replacement = WaitForIdle(&app);
    PicoHost_RequestReload(&app);
    ParallelRelease(1);
    bool one_done = ParallelWait(&app, 1, true);
    pico_host_pump(&app);
    bool retained = ws->state == PICO_WORKSPACE_RELOADING && !PicoWorkspace_AcceptsNewWork(ws);
    ParallelRelease(~0u);
    bool reloaded = false;
    for (int i = 0; i < 3000; i++)
    {
        pico_host_pump(&app);
        if (ws->state == PICO_WORKSPACE_OPEN) { reloaded = true; break; }
        SleepOneMs();
    }
    bool no_queued_work = (g_parallel.entered & 4) == 0;
    bool no_late_results = strcmp(g_test.logged_tool_ids, "parallel-0,parallel-1,parallel-2,") == 0;
    PicoHost_Shutdown(&app);
    return overlap && replacement && one_done && retained && reloaded && no_queued_work && no_late_results
               ? 0 : Fail("parallel force cancel/reload", "retirement did not retain all callbacks or isolate late results");
}

static int TestParallelDelegations(void)
{
    char temp[] = "/tmp/pico-parallel-child-XXXXXX";
    if (!mkdtemp(temp)) return Fail("parallel delegations", "temporary directory failed");
    char dir[4096], path[4096];
    bool wrote = WriteSubagentProfile(temp,
        "{\"purpose\":\"Read only\",\"parallel_safe\":true,\"tools\":[\"parallel_probe\"],\"max_parallel_tools\":1}",
        dir, sizeof(dir), path, sizeof(path));
    snprintf(g_config_dir, sizeof(g_config_dir), "%s", temp);
    PicoHost app;
    ParallelInit(&app, 2, 2);
    g_parallel.delegate = g_parallel.child_calls = true;
    InitExt(&app, TestWs(&app), pico_ext_subagent(), NULL, NULL);
    PicoWorkspace_LoadProfiles(TestWs(&app));
    PicoAgent_StartTurn(&app, TestAgent(&app), "independent children");
    bool overlap = ParallelWait(&app, 5, false);
    bool override = false;
    PicoAgentId first_child = 0, second_child = 0;
    PicoTraceLine *a = ToolTraceByCallId(&app, "parallel-0");
    PicoTraceLine *b = ToolTraceByCallId(&app, "parallel-1");
    if (a && b) { first_child = a->child_id; second_child = b->child_id; }
    pthread_mutex_lock(&g_parallel.mu);
    override = g_parallel.entered == 5;
    pthread_mutex_unlock(&g_parallel.mu);
    ParallelRelease(1);
    bool next = ParallelWait(&app, 2, false);
    ParallelRelease(2);
    bool first_done = ParallelResult(&app, 0) && PicoAgent_IsBusy(TestAgent(&app));
    /* Cancel the still-running sibling; its ordinary callback remains cooperative. */
    PicoAgent_Cancel(TestAgent(&app));
    ParallelRelease(~0u);
    bool drained = WaitForManagerIdle(&app);
    a = ToolTraceByCallId(&app, "parallel-0");
    b = ToolTraceByCallId(&app, "parallel-1");
    bool results = a && b && a->tool_output && strstr(a->tool_output, "completed") &&
                   b->tool_output && b->tool_error;
    PicoHost_Shutdown(&app);
    unlink(path); rmdir(dir); rmdir(temp);
    snprintf(g_config_dir, sizeof(g_config_dir), "/tmp/pico-agent-behavior");
    return wrote && overlap && override && first_child && second_child && first_child != second_child &&
           next && first_done && drained && results
               ? 0 : Fail("parallel delegations", "sibling execution, profile limit, row linkage, or cancellation failed");
}

static int TestParallelResumeReservation(void)
{
    char temp[] = "/tmp/pico-parallel-resume-XXXXXX";
    if (!mkdtemp(temp)) return Fail("parallel resume reservation", "temporary directory failed");
    char dir[4096], path[4096];
    bool wrote = WriteSubagentProfile(temp, "{\"purpose\":\"Resume exclusively\",\"parallel_safe\":true,\"tools\":[\"parallel_probe\"]}",
                                      dir, sizeof(dir), path, sizeof(path));
    snprintf(g_config_dir, sizeof(g_config_dir), "%s", temp);
    PicoHost app;
    ParallelInit(&app, 2, 2);
    g_parallel.delegate = g_parallel.resume = true;
    InitExt(&app, TestWs(&app), pico_ext_subagent(), NULL, NULL);
    PicoWorkspace_LoadProfiles(TestWs(&app));
    ConfigureFakeSession("exploration");
    PicoAgent_StartTurn(&app, TestAgent(&app), "two continuations of the same child");
    bool exclusive = false;
    for (int i = 0; i < 3000; i++)
    {
        PicoWorkspace_Pump(TestWs(&app));
        PicoTraceLine *a = ToolTraceByCallId(&app, "parallel-0");
        PicoTraceLine *b = ToolTraceByCallId(&app, "parallel-1");
        if (a && b && ((a->tool_output && a->tool_error) || (b->tool_output && b->tool_error)))
        {
            exclusive = g_fake_session.replay_count == 1 && pico_agent_count(&app) == 2 &&
                        (a->child_id != 0) != (b->child_id != 0);
            break;
        }
        SleepOneMs();
    }
    ParallelRelease(~0u);
    bool drained = WaitForManagerIdle(&app);
    bool released = !PicoWorkspace_SessionReserved(TestWs(&app), g_fake_session.path, 0);
    PicoHost_Shutdown(&app);
    unlink(path); rmdir(dir); rmdir(temp);
    snprintf(g_config_dir, sizeof(g_config_dir), "/tmp/pico-agent-behavior");
    return wrote && exclusive && drained && released ? 0 : Fail("parallel resume reservation", "competing calls did not retain exclusive child-session ownership");
}

static int TestSubagentProfileBarrier(void)
{
    char temp[] = "/tmp/pico-profile-barrier-XXXXXX";
    if (!mkdtemp(temp)) return Fail("profile barrier", "temporary directory failed");
    char dir[4096], path[4096], worker_path[4096];
    bool wrote = WriteSubagentProfile(temp,
        "{\"purpose\":\"Explore\",\"parallel_safe\":true,\"tools\":[\"parallel_probe\"]}",
        dir, sizeof(dir), path, sizeof(path));
    /* Missing parallel_safe is intentionally conservative for a writing worker. */
    wrote = WriteConfigProfile(dir, "worker.json",
        "{\"purpose\":\"Make changes\",\"tools\":[\"parallel_probe\"]}") && wrote;
    snprintf(worker_path, sizeof(worker_path), "%s/subagents/worker.json", temp);
    snprintf(g_config_dir, sizeof(g_config_dir), "%s", temp);
    PicoHost app;
    ParallelInit(&app, 3, 3);
    g_parallel.delegate = true;
    g_parallel.barrier = 1;
    InitExt(&app, TestWs(&app), pico_ext_subagent(), NULL, NULL);
    PicoWorkspace_LoadProfiles(TestWs(&app));
    PicoAgent *parent = TestAgent(&app);
    PicoAgent_StartTurn(&app, parent, "explore, write, then explore");
    bool first = ParallelWait(&app, 1, false) &&
                 PicoAgent_ToolCallProgress(parent, "parallel-1") == PICO_TOOL_CALL_QUEUED &&
                 PicoAgent_ToolCallProgress(parent, "parallel-2") == PICO_TOOL_CALL_QUEUED;
    ParallelRelease(1);
    bool worker = ParallelWait(&app, 4, false) &&
                  PicoAgent_ToolCallProgress(parent, "parallel-0") == PICO_TOOL_CALL_IDLE &&
                  PicoAgent_ToolCallProgress(parent, "parallel-2") == PICO_TOOL_CALL_QUEUED;
    ParallelRelease(4);
    bool last = ParallelWait(&app, 16, false) &&
                PicoAgent_ToolCallProgress(parent, "parallel-1") == PICO_TOOL_CALL_IDLE;
    ParallelRelease(~0u);
    bool idle = WaitForManagerIdle(&app);
    PicoHost_Shutdown(&app);
    unlink(path); unlink(worker_path); rmdir(dir); rmdir(temp);
    snprintf(g_config_dir, sizeof(g_config_dir), "/tmp/pico-agent-behavior");
    return wrote && first && worker && last && idle ? 0 :
        Fail("profile barrier", "a non-opted-in profile overlapped earlier or later calls");
}

static void RewriteDelegation(PicoAgentContext *ctx, PicoToolEvent *event, void *state)
{
    (void)ctx; (void)state;
    if (strcmp(event->name, "subagent") != 0) return;
    const char *rewrites[] = {
        "{\"profile\":\"worker\",\"task\":\"child-0\"}",
        "{\"profile\":\"exploration\",\"task\":\"child-0\",\"session_id\":\"continued-child\"}",
        "{\"profile\":\"exploration\",\"task\":\"child-0\"}",
        "{\"task\":\"child-1\",\"profile\":\"explor\\u0061tion\"}",
    };
    event->args_json_out = JsonDup(rewrites[g_parallel.rewrite_mode]);
}

static int TestSubagentIdentityRewrite(void)
{
    char temp[] = "/tmp/pico-profile-rewrite-XXXXXX";
    if (!mkdtemp(temp)) return Fail("delegation identity rewrite", "temporary directory failed");
    char dir[4096], path[4096], worker_path[4096];
    bool ok = WriteSubagentProfile(temp,
        "{\"purpose\":\"Explore\",\"parallel_safe\":true,\"tools\":[\"parallel_probe\"]}",
        dir, sizeof(dir), path, sizeof(path));
    ok = WriteConfigProfile(dir, "worker.json", "{\"purpose\":\"Write\",\"tools\":[\"parallel_probe\"]}") && ok;
    snprintf(worker_path, sizeof(worker_path), "%s/subagents/worker.json", temp);
    snprintf(g_config_dir, sizeof(g_config_dir), "%s", temp);
    for (int mode = 0; mode < 4; mode++)
    {
        PicoHost app;
        ParallelInit(&app, 1, 2);
        g_parallel.delegate = true;
        g_parallel.rewrite_mode = mode;
        g_parallel.resume = mode == 2;
        InitExt(&app, TestWs(&app), pico_ext_subagent(), NULL, NULL);
        PicoWorkspace_LoadProfiles(TestWs(&app));
        ConfigureFakeSession("exploration");
        TestAddToolBeforeHook(&app, RewriteDelegation);
        ParallelRelease(~0u);
        PicoAgent_StartTurn(&app, TestAgent(&app), "intercept delegation");
        bool idle = WaitForManagerIdle(&app);
        PicoTraceLine *row = ToolTraceByCallId(&app, "parallel-0");
        bool outcome = row && row->tool_output && (mode == 3
            ? !row->tool_error && g_parallel.entered == 4 && strstr(row->tool_output, "completed")
            : row->tool_error && g_parallel.entered == 0 &&
              strstr(row->tool_output, "cannot change profile or session_id"));
        ok = idle && outcome && ok;
        PicoHost_Shutdown(&app);
    }
    unlink(path); unlink(worker_path); rmdir(dir); rmdir(temp);
    snprintf(g_config_dir, sizeof(g_config_dir), "/tmp/pico-agent-behavior");
    return ok ? 0 : Fail("delegation identity rewrite", "identity changed or an equivalent identity/task-only edit was rejected");
}
