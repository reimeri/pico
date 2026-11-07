/* Included by host_workspace_test.c after the real-frame interaction fixture. */
static bool *g_cold_seen;
static int g_cold_rows;
static Clay_Dimensions ColdMeasureText(Clay_StringSlice text, Clay_TextElementConfig *config, void *data)
{
    if (g_cold_seen && text.length > 0 && text.length < 256)
    {
        char copy[256];
        memcpy(copy, text.chars, (size_t)text.length); copy[text.length] = '\0';
        int row;
        if (sscanf(copy, "history-%d", &row) == 1 && row >= 0 && row < g_cold_rows)
            g_cold_seen[row] = true;
    }
    return ShellMeasureText(text, config, data);
}

/* Cold history may refine its extent, but must never be a prerequisite for
 * displaying the intended tail. Use real frames so same-frame correction and
 * idle admission share the production lifecycle. */
static int TestColdTranscriptRefinement(bool with_sidebar)
{
    InteractionFixture f = {0};
    const int rows = 120;
    bool seen[120] = {0};
    bool good = false;
    double saved_time = g_frame_time;
    const char *phase = "setup";
    g_frame_time = 100;
    if (!InteractionFixtureInit(&f)) goto done;
    if (!with_sidebar) f.host->view_count[PICO_SLOT_SIDEBAR] = 0;
    PicoAgent *agent = PicoHost_FindAgent(f.host, f.agent);
    for (int i = 0; i < rows; i++)
    {
        char text[256];
        snprintf(text, sizeof(text), "history-%d %s", i,
            i % 3 ? "Short history row" : "Tall history row\n\nparagraph\n\nparagraph\n\nparagraph\n\nparagraph\n\nparagraph");
        PicoAgent_AddMessage(f.host, agent, i % 2 ? PICO_ROLE_USER : PICO_ROLE_ASSISTANT, text);
    }
    g_cold_seen = seen; g_cold_rows = rows;
    Clay_SetMeasureTextFunction(ColdMeasureText, NULL);
    RichText_SetMeasureFunction(ColdMeasureText, NULL);
    f.host->chat_follow_bottom = true;
    pico_host_request_redraw(f.host);
    phase = "first tail presentation";
    PICO_TEST_WAIT_LOOP("cold viewport becomes presentable")
    {
        PicoHost_Frame(f.host);
        if (f.host->frame_presented) break;
        g_frame_time += 1;
    }
    Clay_ElementData tail = Clay_GetElementData(CLAY_IDI("MsgMain", rows - 1));
    ChatStabilitySnapshot initial;
    if (!tail.found || !CaptureChatStabilitySnapshot(with_sidebar, &initial) ||
        seen[0] ||
        !ShellVerticallyContains(initial.chat, tail.boundingBox)) goto done;
    int observed = 0;
    phase = "stable gradual refinement";
    PICO_TEST_WAIT_LOOP("every historical row eventually gets measured")
    {
        int total = 0;
        for (int i = 0; i < rows; i++) total += seen[i];
        int added = total - observed;
        observed = total;
        if (observed == rows) break;
        /* Admission must not turn a correction loop into a whole-history
         * layout. This is a relationship to this fixture, not a batch value. */
        if (added >= rows / 2) goto done;
        g_frame_time += 1;
        PicoHost_Frame(f.host);
        if (!f.host->frame_presented) continue;
        ChatStabilitySnapshot now;
        Clay_ElementData next_tail = Clay_GetElementData(CLAY_IDI("MsgMain", rows - 1));
        if (!CaptureChatStabilitySnapshot(with_sidebar, &now) ||
            !ChatShellBoundsStable(&initial, &now, with_sidebar) || !next_tail.found ||
            !ShellBoxStable(tail.boundingBox, next_tail.boundingBox) ||
            fabsf(now.scroll_y - fminf(0, now.container_height - now.content_height)) > 0.01f)
            goto done;
    }
    /* An unrelated redraw after convergence must not finally repair the
     * scrollbar. Estimated heights must have converged without more input. */
    phase = "settled extent";
    ChatStabilitySnapshot settled, again;
    if (!CaptureChatStabilitySnapshot(with_sidebar, &settled)) goto done;
    g_frame_time += 1;
    pico_host_request_redraw(f.host);
    PicoHost_Frame(f.host);
    if (!CaptureChatStabilitySnapshot(with_sidebar, &again) ||
        !ChatStabilitySnapshotStable(&settled, &again, with_sidebar)) goto done;
    good = true;
done:
    g_cold_seen = NULL; g_cold_rows = 0;
    RichText_SetMeasureFunction(ShellMeasureText, NULL);
    g_frame_time = saved_time;
    InteractionFixtureFree(&f);
    if (!good) { fprintf(stderr, "cold refinement phase: %s\n", phase);
        Fail("cold tail, progressive refinement and shell bounds must remain stable"); }
    return good ? 0 : 1;
}

static int ColdSeenCount(void)
{
    int count = 0;
    for (int i = 0; i < g_cold_rows; i++) count += g_cold_seen[i];
    return count;
}

static void SlowRefinementSidebar(PicoHost *host, void *state)
{
    ShellTestSidebar(host, state);
#ifdef PICO_TEST_CLOCK
    /* Model expensive foreground work without sleeping or relying on the
     * configured refinement deadline. Idle work must not starve behind it. */
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    PicoTestClock_Set((double)now.tv_sec + now.tv_nsec / 1e9 + 60.0);
#endif
}

static int TestRefinementScheduling(void)
{
    InteractionFixture f = {0};
    bool seen[80] = {0}, good = false;
    double saved_time = g_frame_time;
    const char *phase = "setup";
    g_frame_time = 200;
    if (!InteractionFixtureInit(&f)) goto done;
    char path[4096];
    snprintf(path, sizeof(path), "%s/offscreen.png", f.dir);
    Image image = GenImageColor(24, 24, RED);
    bool saved = ExportImage(image, path);
    UnloadImage(image);
    if (!saved) goto done;
    PicoAgent *agent = PicoHost_FindAgent(f.host, f.agent);
    for (int i = 0; i < 80; i++)
    {
        char text[128];
        snprintf(text, sizeof(text), "history-%d %s", i, i ? "ordinary row" : "\n\n![offscreen](offscreen.png)");
        PicoAgent_AddMessage(f.host, agent, PICO_ROLE_USER, text);
    }
    g_cold_seen = seen; g_cold_rows = 80;
    Clay_SetMeasureTextFunction(ColdMeasureText, NULL);
    RichText_SetMeasureFunction(ColdMeasureText, NULL);
    g_chat_image_test = true;
    unsigned uploads = g_chat_image_texture_id;
    pico_host_request_redraw(f.host);
    PICO_TEST_WAIT_LOOP("first viewport without offscreen image loading")
    {
        PicoHost_Frame(f.host);
        if (f.host->frame_presented) break;
        g_frame_time += 1;
    }
    phase = "unrelated redraws do not grant idle work";
    int first = ColdSeenCount();
    for (int i = 0; i < 12; i++)
    {
        pico_host_request_redraw(f.host);
        PicoHost_Frame(f.host);
    }
    if (seen[0] || ColdSeenCount() != first || g_chat_image_texture_id != uploads) goto done;
    phase = "interaction takes precedence";
    g_find_down = true;
    for (int i = 0; i < 4; i++) { g_frame_time += 1; PicoHost_Frame(f.host); }
    g_find_down = false;
    if (seen[0] || ColdSeenCount() != first || g_chat_image_texture_id != uploads) goto done;
    phase = "idle work progresses despite expensive foreground layout";
    ShellTestAddView(f.host, PICO_SLOT_SIDEBAR, SlowRefinementSidebar, NULL);
    PICO_TEST_WAIT_LOOP("idle offscreen row becomes measured")
    {
        g_frame_time += 1;
        PicoHost_Frame(f.host);
        if (seen[0]) break;
    }
    if (g_chat_image_texture_id == uploads) goto done;
    phase = "hidden transcript stays inactive";
    PicoAgentId empty;
    PicoAgentCreateOptions options = {.kind=PICO_AGENT_MAIN, .session_start=PICO_SESSION_NONE, .select=true};
    if (pico_main_agent_create(f.host, f.workspace, &options, &empty) != PICO_OK) goto done;
    PicoHost_Frame(f.host);
    int before = ColdSeenCount();
    for (int i = 0; i < 8; i++) { g_frame_time += 1; PicoHost_Frame(f.host); }
    if (ColdSeenCount() != before) goto done;
    phase = "search includes cold history immediately";
    if (!pico_agent_select(f.host, f.agent)) goto done;
    pico_host_request_redraw(f.host);
    PicoHost_Frame(f.host);
    PicoChatFind_Open(f.host);
    PicoChatFind_SetQuery(f.host, "history-40");
    f.host->find.focused = false; /* The caret draw requires a real GPU. */
    PICO_TEST_WAIT_LOOP("cold search target is presented")
    {
        pico_host_request_redraw(f.host);
        PicoHost_Frame(f.host);
        if (f.host->frame_presented && f.host->find.search.count > 0 && !f.host->find.nearest && !f.host->find.reveal) break;
        g_frame_time += 1;
    }
    Clay_ElementData target = Clay_GetElementData(CLAY_IDI("MsgMain", 40));
    Clay_ElementData viewport = Clay_GetElementData(CLAY_ID("ChatScroll"));
    if (!target.found || !viewport.found || !ShellVerticallyContains(viewport.boundingBox, target.boundingBox)) goto done;
    good = true;
done:
#ifdef PICO_TEST_CLOCK
    PicoTestClock_Reset();
#endif
    g_find_down = false;
    g_cold_seen = NULL; g_cold_rows = 0;
    RichText_SetMeasureFunction(ShellMeasureText, NULL);
    g_frame_time = saved_time;
    InteractionFixtureFree(&f);
    g_chat_image_test = false;
    if (!good) { fprintf(stderr,"refinement scheduling phase: %s\n",phase);
        Fail("cold refinement must respect presentation, input, idle cadence and search demand"); }
    return good ? 0 : 1;
}

static int TestReadingAnchorDuringRefinement(void)
{
    InteractionFixture f = {0};
    bool seen[100] = {0}, good = false;
    double saved_time = g_frame_time;
    const char *phase = "setup";
    g_frame_time = 300;
    if (!InteractionFixtureInit(&f)) goto done;
    f.host->view_count[PICO_SLOT_SIDEBAR] = 0;
    PicoAgent *agent = PicoHost_FindAgent(f.host, f.agent);
    for (int i = 0; i < 100; i++)
    {
        char text[256];
        snprintf(text, sizeof(text), "history-%d %s", i, i % 2 ? "short" :
            "long\n\nparagraph\n\nparagraph\n\nparagraph\n\nparagraph\n\nparagraph");
        PicoAgent_AddMessage(f.host, agent, PICO_ROLE_USER, text);
    }
    g_cold_seen = seen; g_cold_rows = 100;
    Clay_SetMeasureTextFunction(ColdMeasureText, NULL);
    RichText_SetMeasureFunction(ColdMeasureText, NULL);
    /* Land in a cold middle viewport using real scroll state, not Find (which
     * explicitly demands the full search index). */
    pico_host_request_redraw(f.host);
    PICO_TEST_WAIT_LOOP("first cold tail")
    {
        PicoHost_Frame(f.host);
        if (f.host->frame_presented) break;
        g_frame_time += 1;
    }
    Clay_ScrollContainerData scroll = Clay_GetScrollContainerData(CLAY_ID("ChatScroll"));
    if (!scroll.found || !scroll.scrollPosition) goto done;
    f.host->chat_follow_bottom = false;
    PicoChat_CancelScrollRestore(f.host);
    scroll.scrollPosition->y *= 0.5f;
    pico_host_request_redraw(f.host);
    PICO_TEST_WAIT_LOOP("cold middle viewport")
    {
        PicoHost_Frame(f.host);
        if (f.host->frame_presented) break;
        g_frame_time += 1;
    }
    int anchor = -1;
    Clay_BoundingBox expected = {0};
    Clay_ElementData viewport = Clay_GetElementData(CLAY_ID("ChatScroll"));
    for (int i = 0; i < 100; i++)
    {
        Clay_ElementData row = Clay_GetElementData(CLAY_IDI("MsgMain", i));
        if (row.found && row.boundingBox.y + row.boundingBox.height > viewport.boundingBox.y &&
            row.boundingBox.y < viewport.boundingBox.y + viewport.boundingBox.height)
        { anchor = i; expected = row.boundingBox; break; }
    }
    if (anchor <= 0 || seen[0]) goto done;
    phase = "prefix growth and shrink preserve reading anchor";
    PICO_TEST_WAIT_LOOP("reading viewport remains stable through refinement")
    {
        if (ColdSeenCount() == 100) break;
        g_frame_time += 1;
        PicoHost_Frame(f.host);
        if (!f.host->frame_presented) continue;
        Clay_ElementData row = Clay_GetElementData(CLAY_IDI("MsgMain", anchor));
        if (!row.found || !ShellBoxStable(expected, row.boundingBox) || f.host->chat_follow_bottom) goto done;
    }
    good = true;
done:
    g_cold_seen = NULL; g_cold_rows = 0;
    RichText_SetMeasureFunction(ShellMeasureText, NULL);
    g_frame_time = saved_time;
    InteractionFixtureFree(&f);
    if (!good) { fprintf(stderr,"reading refinement phase: %s\n",phase);
        Fail("refining estimates above a reading viewport must preserve its message offset"); }
    return good ? 0 : 1;
}

static int TestInspectRefinementReflow(void)
{
    InteractionFixture f = {0};
    bool good = false;
    double saved_time = g_frame_time;
    int saved_width = g_clay_frame_width;
    const char *phase = "setup";
    g_frame_time = 400;
    if (!InteractionFixtureInit(&f)) goto done;
    PicoWorkspace *workspace = PicoHost_FindWorkspace(f.host, f.workspace);
    PicoAgentCreateOptions options = {.kind=PICO_AGENT_SUBAGENT, .parent_id=f.agent, .session_start=PICO_SESSION_NONE};
    PicoAgentId child_id;
    if (PicoWorkspace_CreateAgent(workspace, &options, &child_id) != PICO_OK) goto done;
    PicoAgent *child = PicoHost_FindAgent(f.host, child_id);
    for (int i = 0; i < 100; i++)
        PicoAgent_AddMessage(f.host, child, PICO_ROLE_USER, i % 2 ? "Short inspection row" :
            "Long inspection paragraph that wraps when the viewport narrows. "
            "More words so the height changes on reflow.\n\nAnother paragraph.");
    PicoToolRowEvent inspect = {.name="subagent", .agent_id=f.agent, .child_id=child_id};
    PicoChat_SubagentToolRow(workspace, &inspect, NULL);
    pico_host_request_redraw(f.host);
    PICO_TEST_WAIT_LOOP("inspect tail presentation")
    {
        PicoHost_Frame(f.host);
        if (f.host->frame_presented) break;
        g_frame_time += 1;
    }
    Clay_ElementData clip = Clay_GetElementData(CLAY_ID("SubagentChatScroll"));
    if (!clip.found) goto done;
    g_find_pointer = (Vector2){clip.boundingBox.x + 20, clip.boundingBox.y + 20};
    Clay_SetPointerState((Clay_Vector2){g_find_pointer.x, g_find_pointer.y}, false);
    g_find_wheel.y = 1;
    PicoHost_Frame(f.host);
    g_find_wheel.y = 0;
    Clay_ScrollContainerData scroll = Clay_GetScrollContainerData(CLAY_ID("SubagentChatScroll"));
    if (!scroll.scrollPosition) goto done;
    scroll.scrollPosition->y *= 0.5f;
    pico_host_request_redraw(f.host);
    PICO_TEST_WAIT_LOOP("inspect reading viewport")
    {
        PicoHost_Frame(f.host);
        if (f.host->frame_presented) break;
        g_frame_time += 1;
    }
    int anchor = -1;
    float offset = 0;
    clip = Clay_GetElementData(CLAY_ID("SubagentChatScroll"));
    for (int i = 0; i < 100; i++)
    {
        Clay_ElementData row = Clay_GetElementData(CLAY_IDI("MsgInspect1", i));
        if (row.found && row.boundingBox.y + row.boundingBox.height > clip.boundingBox.y &&
            row.boundingBox.y < clip.boundingBox.y + clip.boundingBox.height)
        { anchor = i; offset = row.boundingBox.y - clip.boundingBox.y; break; }
    }
    if (anchor <= 0) goto done;
    phase = "inspect reflow preserves message offset";
    g_clay_frame_width = 700;
    pico_host_request_redraw(f.host);
    bool presented = false;
    for (int frame = 0; frame < 30; frame++)
    {
        g_frame_time += 1;
        PicoHost_Frame(f.host);
        if (!f.host->frame_presented) continue;
        presented = true;
        clip = Clay_GetElementData(CLAY_ID("SubagentChatScroll"));
        Clay_ElementData row = Clay_GetElementData(CLAY_IDI("MsgInspect1", anchor));
        if (!clip.found || !row.found || fabsf(row.boundingBox.y - clip.boundingBox.y - offset) > 0.01f) goto done;
    }
    good = presented;
done:
    g_find_wheel = (Vector2){0};
    g_clay_frame_width = saved_width;
    g_frame_time = saved_time;
    InteractionFixtureFree(&f);
    if (!good) { fprintf(stderr,"inspect refinement phase: %s\n",phase);
        Fail("inspect reflow and refinement must preserve a reading anchor"); }
    return good ? 0 : 1;
}

static void MutateColdViewport(PicoHost *host, const PicoHookEvent *event, void *opaque)
{
    (void)event;
    bool *mutated = opaque;
    if (*mutated) return;
    *mutated = true;
    PicoAgent_AppendAssistant(host, PicoHost_SelectedAgent(host), "\n\nafter-layout-marker");
}

static int TestRefinementAfterLayoutMutation(void)
{
    InteractionFixture f = {0};
    bool good = false, mutated = false;
    if (!InteractionFixtureInit(&f)) goto done;
    PicoAgent *agent = PicoHost_FindAgent(f.host, f.agent);
    for (int i = 0; i < 100; i++) PicoAgent_AddMessage(f.host, agent, PICO_ROLE_ASSISTANT, "History");
    PicoHost_BeginRegistration(f.host, PICO_REG_HOST, NULL);
    pico_host_add_hook(f.host, PICO_HOOK_AFTER_LAYOUT, MutateColdViewport);
    PicoHost_PublishRegistration(f.host, &mutated);
    pico_host_request_redraw(f.host);
    PICO_TEST_WAIT_LOOP("after-layout mutation reaches presentation")
    {
        PicoHost_Frame(f.host);
        if (f.host->frame_presented) break;
    }
    if (!mutated || !SidebarCommandsContain(&g_last_presented_commands, "after-layout-marker")) goto done;
    good = true;
done:
    InteractionFixtureFree(&f);
    if (!good) Fail("an after-layout edit must not be cleaned or presented with stale geometry");
    return good ? 0 : 1;
}
