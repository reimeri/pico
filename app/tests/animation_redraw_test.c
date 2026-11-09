#include "overlay.h"

/* Included by host_workspace_test.c; exercise real builtin callbacks and frame
 * presentation, rather than asserting the implementation's redraw flags. */
static int TestBusyFramesFollowVisibleAnimationDemand(void)
{
    InteractionFixture f = {0};
    PicoAgent *agent = NULL;
    bool good = false;
    const char *phase = "fixture";
    double saved_time = g_frame_time;
    int saved_focus = g_window_focus_override;
    if (!InteractionFixtureInit(&f)) goto done;
    agent = PicoHost_FindAgent(f.host, f.agent);
    /* A quiescent worker state with a real working indicator, but no stream
     * or tool completions that might legitimately invalidate the window. */
    agent->state = PICO_AGENT_COMPACT_WAIT;
    g_frame_time = GetTime() + 1.0;
    g_window_focus_override = 1;
    pico_host_request_redraw(f.host);
    PicoHost_Frame(f.host);
    int frames = g_presented_frames;
    phase = "visible focused animation follows presentation";
    PicoHost_Frame(f.host);
    if (g_presented_frames != frames + 1) goto done;

    phase = "unfocused animations wait instead of repainting every pump";
    g_window_focus_override = 0;
    PicoHost_Frame(f.host); /* Focus change is a real invalidation. */
    frames = g_presented_frames;
    for (int i = 0; i < 4; i++) PicoHost_Frame(f.host);
    if (g_presented_frames != frames) goto done;

    phase = "animation throttle does not delay real invalidation";
    pico_host_request_redraw(f.host);
    PicoHost_Frame(f.host);
    if (g_presented_frames != frames + 1) goto done;

    phase = "unfocused animation advances without new content";
    frames = g_presented_frames;
    g_frame_time += 1.0;
    PicoHost_Frame(f.host);
    if (g_presented_frames != frames + 1) goto done;

    phase = "busy agents with hidden indicators do not animate";
    f.host->view_count[PICO_SLOT_SIDEBAR] = 0;
    f.host->view_count[PICO_SLOT_FOOTER] = 0;
    pico_host_request_redraw(f.host);
    PicoHost_Frame(f.host);
    /* A previously scheduled visible frame may still be pending. Consume it
     * once, then verify that hidden decorations do not renew their demand. */
    g_frame_time += 1.0;
    PicoHost_Frame(f.host);
    frames = g_presented_frames;
    for (int i = 0; i < 4; i++)
    {
        g_frame_time += 0.1;
        PicoHost_Frame(f.host);
    }
    good = g_presented_frames == frames && PicoAgent_IsBusy(agent);

done:
    if (agent) agent->state = PICO_AGENT_IDLE;
    g_frame_time = saved_time;
    g_window_focus_override = saved_focus;
    InteractionFixtureFree(&f);
    if (!good)
    {
        fprintf(stderr, "busy animation phase: %s\n", phase);
        Fail("busy frames must follow visible animation demand, not agent activity");
    }
    return good ? 0 : 1;
}

static int TestWaitingQuestionDoesNotAnimate(void)
{
    InteractionFixture f = {0};
    MatrixProviderState state;
    MatrixStateInit(&state, MATRIX_PROVIDER_ASK);
    state.ask_request = "{\"type\":\"questionnaire\",\"ui\":\"custom\",\"questions\":["
        "{\"id\":\"choice\",\"kind\":\"select\",\"question\":\"Choose one\",\"options\":[\"One\",\"Two\"]}]}";
    PicoAgent *agent = NULL;
    PicoToolAsk ask = {0};
    bool good = false;
    const char *phase = "fixture";
    double saved_time = g_frame_time;
    int saved_focus = g_window_focus_override;
    if (!InteractionFixtureInit(&f)) goto done;
    agent = PicoHost_FindAgent(f.host, f.agent);
    if (!ConfigureMatrixWorkspace(f.host, PicoHost_FindWorkspace(f.host, f.workspace), &state, true) ||
        !PicoSettings_SetModel(agent, "matrix-model")) goto done;
    g_window_focus_override = 0; /* No focused text caret is entitled to blink. */
    PicoAgent_StartTurn(f.host, agent, "Ask for a choice");
    PICO_TEST_WAIT(!ask.id)
    {
        pico_host_pump(f.host);
        pico_tool_pending_ask(f.host, &ask);
    }
    phase = "pending question";
    if (!ask.id || !WaitHostTasks(f.host)) goto done;
    PicoOverlay_Notify(f.host, NULL); /* Model-selection feedback is unrelated animation. */
    g_frame_time = GetTime() + 1.0;
    PicoHost_Frame(f.host);
    /* Finish the inbox's one-time state transition and pending UI adoption. */
    g_frame_time += 1.0;
    PicoHost_Frame(f.host);
    PicoHost_Frame(f.host);
    phase = "static yellow waiting indicator";
    if (!SidebarInboxStatusColor(&g_last_presented_commands, COLOR_STATUS_RUN)) goto done;
    int frames = g_presented_frames;
    for (int i = 0; i < 4; i++)
    {
        g_frame_time += 0.1;
        PicoHost_Frame(f.host);
    }
    phase = "unchanged question stays idle";
    if (g_presented_frames != frames || !PicoAgent_AskUiOpen(agent)) goto done;

    phase = "answer wakes the UI without the busy redraw loop";
    if (!pico_tool_answer(f.host, ask.id, "{\"choice\":\"One\"}") || !PumpUntilIdle(f.host, agent)) goto done;
    PicoHost_Frame(f.host);
    good = g_presented_frames > frames && !PicoAgent_AskUiOpen(agent);

done:
    if (agent && PicoAgent_IsBusy(agent))
    {
        PicoAgent_Cancel(agent);
        (void)PumpUntilIdle(f.host, agent);
    }
    g_frame_time = saved_time;
    g_window_focus_override = saved_focus;
    InteractionFixtureFree(&f);
    MatrixStateDestroy(&state);
    if (!good)
    {
        fprintf(stderr, "waiting animation phase: %s\n", phase);
        Fail("pending user input must keep static working indicators without continuous redraw");
    }
    return good ? 0 : 1;
}

static int TestUnmountedThinkingDoesNotAnimate(void)
{
    InteractionFixture f = {0};
    PicoAgent *agent = NULL;
    bool good = false;
    const char *phase = "fixture";
    double saved_time = g_frame_time;
    int saved_focus = g_window_focus_override;
    g_frame_time = 100;
    g_window_focus_override = 1;
    if (!InteractionFixtureInit(&f)) goto done;
    f.host->view_count[PICO_SLOT_SIDEBAR] = 0;
    f.host->view_count[PICO_SLOT_FOOTER] = 0;
    agent = PicoHost_FindAgent(f.host, f.agent);
    const int rows = 120;
    for (int i = 0; i < rows; i++)
        PicoAgent_AddMessage(f.host, agent, PICO_ROLE_USER,
            i ? "Historical message\n\nAnother paragraph\n\nMore history" : "Oldest message");
    PicoAgent_AddMessage(f.host, agent, PICO_ROLE_ASSISTANT, "");
    agent->state = PICO_AGENT_LLM_WAIT;
    f.host->chat_follow_bottom = true;
    pico_host_request_redraw(f.host);
    phase = "visible thinking animation";
    /* Finish cold refinement before checking that only the animation remains. */
    for (int i = 0; i < rows; i++)
    {
        g_frame_time += 1;
        PicoHost_Frame(f.host);
    }
    if (!SidebarCommandsContain(&g_last_presented_commands, "Thinking…")) goto done;
    int frames = g_presented_frames;
    PicoHost_Frame(f.host);
    if (g_presented_frames != frames + 1) goto done;

    phase = "jump from live thinking to distant history";
    Clay_ElementData chat = Clay_GetElementData(CLAY_ID("ChatScroll"));
    if (!chat.found) goto done;
    g_find_pointer = (Vector2){chat.boundingBox.x + 20, chat.boundingBox.y + 20};
    Clay_SetPointerState((Clay_Vector2){g_find_pointer.x, g_find_pointer.y}, false);
    g_find_wheel.y = 1;
    PicoHost_Frame(f.host);
    g_find_wheel.y = 0;
    Clay_ScrollContainerData scroll = Clay_GetScrollContainerData(CLAY_ID("ChatScroll"));
    if (!scroll.found || !scroll.scrollPosition || f.host->chat_follow_bottom) goto done;
    scroll.scrollPosition->y = 0;
    pico_host_request_redraw(f.host);
    g_frame_time += 1;
    PicoHost_Frame(f.host);
    if (!SidebarCommandsContain(&g_last_presented_commands, "Oldest message") ||
        SidebarCommandsContain(&g_last_presented_commands, "Thinking…")) goto done;

    phase = "unmounted live row no longer demands presentation";
    /* Drain the frame scheduled before the jump. It must not be renewed using
     * the thinking label's old, once-visible Clay bounds. */
    g_frame_time += 1;
    PicoHost_Frame(f.host);
    frames = g_presented_frames;
    for (int i = 0; i < 4; i++)
    {
        g_frame_time += 0.1;
        PicoHost_Frame(f.host);
    }
    good = g_presented_frames == frames;

done:
    g_find_wheel = (Vector2){0};
    if (agent) agent->state = PICO_AGENT_IDLE;
    g_frame_time = saved_time;
    g_window_focus_override = saved_focus;
    InteractionFixtureFree(&f);
    if (!good)
    {
        fprintf(stderr, "thinking animation phase: %s\n", phase);
        Fail("unmounted thinking labels must not animate from stale Clay bounds");
    }
    return good ? 0 : 1;
}
