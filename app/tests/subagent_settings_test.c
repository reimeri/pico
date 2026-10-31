/* Included by host_workspace_test.c, using its real input-frame fixture. */

static bool SubagentSettingsClick(PicoHost *host, Clay_ElementId id)
{
    Clay_ElementData element = Clay_GetElementData(id);
    Clay_ElementData view = Clay_GetElementData(CLAY_ID("SettingsModalScroll"));
    Clay_ScrollContainerData scroll = Clay_GetScrollContainerData(CLAY_ID("SettingsModalScroll"));
    if (!element.found) return false;
    if (view.found && scroll.found && scroll.scrollPosition &&
        (element.boundingBox.y < view.boundingBox.y ||
         element.boundingBox.y + element.boundingBox.height > view.boundingBox.y + view.boundingBox.height)) {
        scroll.scrollPosition->y -= element.boundingBox.y - view.boundingBox.y;
        pico_host_request_redraw(host);
        PicoHost_Frame(host);
        element = Clay_GetElementData(id);
    }
    return SidebarFrameClick(host, element);
}

static bool SubagentSettingsText(PicoHost *host, int field, const char *text)
{
    if (!SubagentSettingsClick(host, CLAY_IDI("SubagentField", field))) return false;
    return InteractionReplaceText(host, CLAY_IDI("SubagentField", field), text);
}

static bool SubagentSettingsDrain(PicoHost *host)
{
    if (!PicoCatalog_DrainPersistBefore(host, NULL)) return false;
    pico_host_pump(host);
    PicoHost_Frame(host);
    return !host->settings_reload_blocked;
}

static bool SubagentSettingsStoredProfile(const char *name, PicoSubagentProfileInfo *out)
{
    PicoSubagentDocument *documents = NULL;
    int count = 0;
    char error[512];
    bool found = false;
    if (PicoSubagentConfig_List(&documents, &count, error, sizeof(error))) {
        for (int i = 0; i < count; i++) {
            if (documents[i].valid && !strcmp(documents[i].profile.name, name)) {
                if (out) *out = documents[i].profile;
                found = true;
                break;
            }
        }
    }
    PicoSubagentConfig_FreeDocuments(documents, count);
    return found;
}

static int TestSubagentSettingsWorkflow(bool open_workspaces)
{
    InteractionFixture f = {0};
    char other[] = "/tmp/pico-subagent-other-XXXXXX";
    bool good = false;
    const char *phase = "setup";
    if (!InteractionFixtureInit(&f) || !mkdtemp(other)) goto done;
    PicoHost *host = f.host;
    PicoWorkspaceId other_id = 0;
    if (open_workspaces) {
        if (pico_workspace_open(host, other, &other_id) != PICO_OK) goto done;
        PicoAgent *agent = PicoHost_FindAgent(host, f.agent);
        agent->state = PICO_AGENT_LLM_WAIT;
        phase = "busy command opens Settings section";
        PicoComposer_SetText(host, "/subagents");
        PicoHost_Submit(host);
        if (!PicoSettingsUi_IsOpen(host) || host->composer.length || agent->state != PICO_AGENT_LLM_WAIT) goto done;
        agent->state = PICO_AGENT_IDLE;
        PicoHost_Frame(host);
    } else {
        phase = "close all workspaces and open global Settings";
        int count = pico_workspace_count(host);
        for (int i = 0; i < count; i++) {
            PicoWorkspaceInfo info;
            if (!pico_workspace_info(host, i, &info) || pico_workspace_request_close(host, info.id) != PICO_OK) goto done;
        }
        PICO_TEST_WAIT(pico_workspace_count(host)) pico_host_pump(host);
        PicoSettingsUi_Open(host);
        PicoHost_Frame(host);
        if (!SidebarFrameClick(host, Clay_GetElementData(CLAY_ID("SettingsSubagentsTab")))) goto done;
    }
    phase = "create and validate";
    if (!SubagentSettingsClick(host, CLAY_ID("SubagentNew")) ||
        !SubagentSettingsText(host, 0, "alpha") ||
        !SubagentSettingsClick(host, CLAY_ID("SubagentSave"))) goto done;
    char created_path[4096], directory[4096];
    PicoSubagentConfig_Directory(directory, sizeof(directory));
    PicoPath_Format(created_path, sizeof(created_path), "%s/alpha.json", directory);
    if (access(created_path, F_OK) == 0 || !Clay_GetElementData(CLAY_ID("SubagentError")).found) goto done;
    if (!SubagentSettingsText(host, 2, "Review carefully") ||
        !SubagentSettingsClick(host, CLAY_ID("SubagentRestricted")) ||
        !SubagentSettingsClick(host, CLAY_ID("SubagentParallel"))) goto done;
    if (!open_workspaces && (!SubagentSettingsText(host, 3, "future-model") ||
        !SubagentSettingsText(host, 4, "research") || !SubagentSettingsText(host, 5, "future-tool"))) goto done;
    if (!SubagentSettingsClick(host, CLAY_ID("SubagentSave")) || !SubagentSettingsDrain(host)) goto done;
    PicoWorkspace *ws = PicoHost_FindWorkspace(host, f.workspace);
    PicoWorkspace *second = PicoHost_FindWorkspace(host, other_id);
    PicoSubagentProfileInfo profile;
    if (!SubagentSettingsStoredProfile("alpha", &profile) || !profile.restricted_tools || !profile.parallel_safe) goto done;
    if (open_workspaces) {
        if (profile.tool_count || !PicoSubagentConfig_Find(ws, "alpha") || !PicoSubagentConfig_Find(second, "alpha")) goto done;
    } else {
        if (pico_workspace_count(host) || !profile.has_model || strcmp(profile.model, "future-model") ||
            !profile.has_effort || strcmp(profile.effort, "research") || profile.tool_count != 1 ||
            strcmp(profile.tools[0], "future-tool")) goto done;
        /* Reopening must rediscover saved files even with no current workspace. */
        PicoSettingsUi_Close(host);
        PicoSettingsUi_Open(host);
        PicoHost_Frame(host);
        if (!SidebarFrameClick(host, Clay_GetElementData(CLAY_ID("SettingsSubagentsTab"))) ||
            !Clay_GetElementData(CLAY_IDI("SubagentRow", 0)).found) goto done;
    }
    phase = "duplicate";
    if (!SubagentSettingsClick(host, CLAY_IDI("SubagentRow", 0)) ||
        !SubagentSettingsClick(host, CLAY_ID("SubagentDuplicate")) ||
        !SubagentSettingsText(host, 0, "beta") ||
        !SubagentSettingsClick(host, CLAY_ID("SubagentSave")) || !SubagentSettingsDrain(host) ||
        !SubagentSettingsStoredProfile("alpha", NULL) || !SubagentSettingsStoredProfile("beta", NULL)) goto done;
    phase = "rename";
    if (!SubagentSettingsClick(host, CLAY_IDI("SubagentRow", 0)) ||
        !SubagentSettingsText(host, 0, "gamma") ||
        !SubagentSettingsClick(host, CLAY_ID("SubagentSave")) || !SubagentSettingsDrain(host) ||
        SubagentSettingsStoredProfile("alpha", NULL) || !SubagentSettingsStoredProfile("gamma", NULL)) goto done;
    phase = "confirmed deletion and Escape cancellation";
    if (!SubagentSettingsClick(host, CLAY_IDI("SubagentRow", 0)) ||
        !SubagentSettingsClick(host, CLAY_ID("SubagentDelete"))) goto done;
    g_find_key = KEY_ESCAPE; PicoHost_Frame(host); g_find_key = -1;
    if (!PicoSettingsUi_IsOpen(host) || Clay_GetElementData(CLAY_ID("SubagentConfirmation")).found ||
        !SubagentSettingsStoredProfile("beta", NULL)) goto done;
    if (!SubagentSettingsClick(host, CLAY_ID("SubagentDelete")) ||
        !SidebarFrameClick(host, Clay_GetElementData(CLAY_ID("SubagentConfirmYes"))) ||
        !SubagentSettingsDrain(host) || SubagentSettingsStoredProfile("beta", NULL)) goto done;
    if (open_workspaces && (PicoSubagentConfig_Find(ws, "beta") || PicoSubagentConfig_Find(second, "beta"))) goto done;
    phase = "Settings tab navigation";
    if (!SidebarFrameClick(host, Clay_GetElementData(CLAY_ID("SettingsGeneralTab"))) ||
        !Clay_GetElementData(CLAY_ID("SettingsContextLimit")).found ||
        !SidebarFrameClick(host, Clay_GetElementData(CLAY_ID("SettingsSubagentsTab"))) ||
        !Clay_GetElementData(CLAY_IDI("SubagentRow", 0)).found) goto done;
    good = open_workspaces || pico_workspace_count(host) == 0;
done:
    if (!good) fprintf(stderr, "subagent Settings workflow: %s (open workspaces: %d)\n", phase, open_workspaces);
    InteractionFixtureFree(&f);
    RmRf(other);
    if (!good) Fail("Subagents Settings must manage global profiles with or without open workspaces");
    return !good;
}

static int TestSubagentSettingsDiscardAndRepair(void)
{
    InteractionFixture f = {0};
    bool good = false;
    const char *phase = "setup";
    if (!InteractionFixtureInit(&f)) goto done;
    PicoHost *host = f.host;
    char directory[4096], path[4096];
    PicoSubagentConfig_Directory(directory, sizeof(directory));
    Pico_MkdirP(directory);
    for (int i = 0; i < 12; i++) {
        PicoPath_Format(path, sizeof(path), "%s/profile-%02d.json", directory, i);
        if (WriteFile(path, "{\"purpose\":\"Existing role\"}\n")) goto done;
    }
    PicoPath_Format(path, sizeof(path), "%s/broken.json", directory);
    FILE *bad = fopen(path, "wb");
    static const char malformed[] = "{\"purpose\":\"prefix\"}\0trailing bytes";
    if (!bad) goto done;
    bool written = fwrite(malformed, 1, sizeof(malformed) - 1, bad) == sizeof(malformed) - 1;
    if (fclose(bad) || !written) goto done;
    PicoComposer_SetText(host, "/subagents"); PicoHost_Submit(host); PicoHost_Frame(host);
    phase = "malformed file repair offers external editor";
    if (!SubagentSettingsClick(host, CLAY_IDI("SubagentRow", 0)) ||
        !Clay_GetElementData(CLAY_ID("SubagentOpenFile")).found ||
        !SubagentSettingsText(host, 2, "Repaired role") ||
        !SubagentSettingsClick(host, CLAY_ID("SubagentSave"))) goto done;
    if (!Clay_GetElementData(CLAY_ID("SubagentRepairFile")).found ||
        !SidebarFrameClick(host, Clay_GetElementData(CLAY_ID("SubagentConfirmYes"))) ||
        !SubagentSettingsDrain(host) ||
        !PicoSubagentConfig_Find(PicoHost_FindWorkspace(host, f.workspace), "broken")) goto done;
    phase = "discard confirmation remains visible far from editor";
    if (!SubagentSettingsClick(host, CLAY_IDI("SubagentRow", 0)) ||
        !SubagentSettingsText(host, 1, "Unsaved description") ||
        !SubagentSettingsClick(host, CLAY_ID("SubagentNew"))) goto done;
    Clay_ElementData confirmation = Clay_GetElementData(CLAY_ID("SubagentConfirmation"));
    if (!confirmation.found || confirmation.boundingBox.y < 0 ||
        confirmation.boundingBox.y + confirmation.boundingBox.height > GetScreenHeight()) goto done;
    g_find_key = KEY_ESCAPE; PicoHost_Frame(host); g_find_key = -1;
    if (!PicoSettingsUi_IsOpen(host) || Clay_GetElementData(CLAY_ID("SubagentConfirmation")).found) goto done;
    phase = "close confirms discard; Escape keeps editing";
    PicoSettingsUi_Close(host); PicoHost_Frame(host);
    if (!PicoSettingsUi_IsOpen(host) || !Clay_GetElementData(CLAY_ID("SubagentConfirmation")).found) goto done;
    g_find_key = KEY_ESCAPE; PicoHost_Frame(host); g_find_key = -1;
    if (!PicoSettingsUi_IsOpen(host) || Clay_GetElementData(CLAY_ID("SubagentConfirmation")).found) goto done;
    PicoSettingsUi_Close(host); PicoHost_Frame(host);
    if (!SidebarFrameClick(host, Clay_GetElementData(CLAY_ID("SubagentConfirmYes"))) || PicoSettingsUi_IsOpen(host)) goto done;
    phase = "reopen resets stale confirmation state";
    PicoComposer_SetText(host, "/subagents"); PicoHost_Submit(host); PicoHost_Frame(host);
    if (!PicoSettingsUi_IsOpen(host) || Clay_GetElementData(CLAY_ID("SubagentConfirmation")).found) goto done;
    good = true;
done:
    if (!good) fprintf(stderr, "subagent Settings discard/repair: %s\n", phase);
    InteractionFixtureFree(&f);
    if (!good) Fail("Subagent repairs and discard confirmations must be visible, cancellable, and safe");
    return !good;
}

static int TestSubagentSettingsDraftSurvivesReload(void)
{
    InteractionFixture f = {0};
    bool good = false;
    if (!InteractionFixtureInit(&f)) goto done;
    PicoHost *host = f.host;
    PicoComposer_SetText(host, "/subagents"); PicoHost_Submit(host); PicoHost_Frame(host);
    if (!SubagentSettingsClick(host, CLAY_ID("SubagentNew")) ||
        !SubagentSettingsText(host, 0, "unsaved")) goto done;
    PicoHost_RequestHostReload(host);
    pico_host_pump(host);
    InteractionOmitDrawHooks(host);
    PicoHost_Frame(host);
    Clay_RenderCommandArray commands = PicoHost_LayoutShell(host, 800, 0);
    if (!PicoSettingsUi_IsOpen(host) || !SidebarCommandsContain(&commands, "unsaved")) goto done;
    PicoSettingsUi_Close(host); PicoHost_Frame(host);
    if (!SidebarFrameClick(host, Clay_GetElementData(CLAY_ID("SubagentConfirmYes")))) goto done;
    PICO_TEST_WAIT(host->plugin_reload_pending || host->plugin_compile) {
        pico_host_pump(host);
        InteractionOmitDrawHooks(host);
    }
    good = !PicoSettingsUi_IsOpen(host);
done:
    InteractionFixtureFree(&f);
    if (!good) Fail("Host reload must defer replacement of unsaved subagent profile drafts until confirmed discard");
    return !good;
}

static int TestSubagentProfileRefreshWaitsForWork(void)
{
    InteractionFixture f = {0};
    MatrixProviderState state;
    MatrixStateInit(&state, MATRIX_PROVIDER_BLOCK);
    bool good = false;
    if (!InteractionFixtureInit(&f)) goto done;
    PicoHost *host = f.host;
    PicoWorkspace *ws = PicoHost_FindWorkspace(host, f.workspace);
    if (!ConfigureMatrixWorkspace(host, ws, &state, false)) goto done;
    PicoAgent *agent = PicoHost_FindAgent(host, f.agent);
    if (!PicoSettings_SetModel(agent, "matrix-model")) goto done;
    char directory[4096], path[4096];
    PicoSubagentConfig_Directory(directory, sizeof(directory));
    Pico_MkdirP(directory);
    PicoPath_Format(path, sizeof(path), "%s/refresh.json", directory);
    if (WriteFile(path, "{\"purpose\":\"Before save\"}\n")) goto done;
    PicoWorkspace_LoadProfiles(ws);
    PicoAgent_StartTurn(host, agent, "Keep running during profile edit");
    PICO_TEST_WAIT(PicoAgent_IsBusy(agent) && !MatrixStateFlag(&state, false)) pico_host_pump(host);
    if (!MatrixStateFlag(&state, false) || WriteFile(path, "{\"purpose\":\"After save\"}\n")) goto done;
    PicoSubagentConfig_RequestRefresh(host);
    pico_host_pump(host);
    const PicoSubagentProfileInfo *profile = PicoSubagentConfig_Find(ws, "refresh");
    if (!profile || strcmp(profile->purpose, "Before save") || !PicoAgent_IsBusy(agent) ||
        !PicoWorkspace_AcceptsNewWork(ws)) goto done;
    MatrixStateRelease(&state);
    if (!PumpUntilIdle(host, agent)) goto done;
    pico_host_pump(host);
    profile = PicoSubagentConfig_Find(ws, "refresh");
    good = profile && !strcmp(profile->purpose, "After save");
done:
    MatrixStateRelease(&state);
    InteractionFixtureFree(&f);
    MatrixStateDestroy(&state);
    if (!good) Fail("Profile refresh must wait for current work without blocking or cancelling it");
    return !good;
}

static int TestSubagentSettingsPresentation(void)
{
    InteractionFixture f = {0};
    bool good = false;
    const char *phase = "setup";
    if (!InteractionFixtureInit(&f)) goto done;
    PicoHost *host = f.host;
    char directory[4096], path[4096];
    PicoSubagentConfig_Directory(directory, sizeof(directory));
    Pico_MkdirP(directory);
    PicoPath_Format(path, sizeof(path), "%s/review.team.json", directory);
    if (WriteFile(path, "{\"purpose\":\"Review changes\",\"description\":\"Review profile\"}\n")) goto done;

    PicoSettingsUi_Open(host);
    PicoHost_Frame(host);
    Clay_ElementData general_field = Clay_GetElementData(CLAY_ID("SettingsContextLimit"));
    Clay_ElementData general_next = Clay_GetElementData(CLAY_ID("SettingsMaxParallelTools"));
    Clay_ElementData general_toggle = Clay_GetElementData(CLAY_ID("SettingsResumeLast"));
    Clay_ElementData general_save = Clay_GetElementData(CLAY_ID("SettingsApply"));
    if (!general_field.found || !general_next.found || !general_toggle.found || !general_save.found) goto done;
    float spacing = general_next.boundingBox.y - general_field.boundingBox.y;
    if (!SidebarFrameClick(host, Clay_GetElementData(CLAY_ID("SettingsSubagentsTab")))) goto done;
    Clay_RenderCommandArray commands = PicoHost_LayoutShell(host, 800, 0);
    phase = "profile names omit only the file extension";
    if (!FindCardText(&commands, "review.team") || FindCardText(&commands, "review.team.json")) goto done;
    if (!SubagentSettingsClick(host, CLAY_IDI("SubagentRow", 0))) goto done;
    phase = "fields and toggles match General Settings";
    Clay_ElementData name = Clay_GetElementData(CLAY_IDI("SubagentField", 0));
    Clay_ElementData description = Clay_GetElementData(CLAY_IDI("SubagentField", 1));
    Clay_ElementData toggle = Clay_GetElementData(CLAY_ID("SubagentFast"));
    if (!name.found || !description.found || !toggle.found ||
        fabsf(description.boundingBox.y - name.boundingBox.y - spacing) > 0.01f ||
        fabsf(name.boundingBox.height - general_field.boundingBox.height) > 0.01f ||
        fabsf(toggle.boundingBox.height - general_toggle.boundingBox.height) > 0.01f ||
        fabsf(toggle.boundingBox.x - name.boundingBox.x) > 0.01f ||
        fabsf(toggle.boundingBox.width - name.boundingBox.width) > 0.01f) goto done;

    phase = "single aligned action row";
    Clay_ElementData duplicate = Clay_GetElementData(CLAY_ID("SubagentDuplicate"));
    Clay_ElementData remove = Clay_GetElementData(CLAY_ID("SubagentDelete"));
    Clay_ElementData cancel = Clay_GetElementData(CLAY_ID("SubagentCancel"));
    Clay_ElementData save = Clay_GetElementData(CLAY_ID("SubagentSave"));
    if (!duplicate.found || !remove.found || !cancel.found || !save.found ||
        fabsf(duplicate.boundingBox.y - save.boundingBox.y) > 0.01f ||
        fabsf(remove.boundingBox.y - cancel.boundingBox.y) > 0.01f ||
        duplicate.boundingBox.x + duplicate.boundingBox.width > remove.boundingBox.x ||
        remove.boundingBox.x + remove.boundingBox.width >= cancel.boundingBox.x ||
        cancel.boundingBox.x + cancel.boundingBox.width > save.boundingBox.x ||
        fabsf(save.boundingBox.height - general_save.boundingBox.height) > 0.01f ||
        fabsf(duplicate.boundingBox.x - name.boundingBox.x) > 0.01f ||
        fabsf(save.boundingBox.x + save.boundingBox.width - name.boundingBox.x - name.boundingBox.width) > 0.01f) goto done;

    phase = "narrow action groups stay within the form";
    g_clay_frame_width = 390;
    for (int pass = 0; pass < 3; pass++) {
        Clay_SetLayoutDimensions((Clay_Dimensions){390, 800});
        pico_host_request_redraw(host);
        PicoHost_Frame(host);
    }
    Clay_ElementData card = Clay_GetElementData(CLAY_ID("SettingsModalCard"));
    Clay_ElementData actions = Clay_GetElementData(CLAY_ID("SubagentActions"));
    duplicate = Clay_GetElementData(CLAY_ID("SubagentDuplicate"));
    remove = Clay_GetElementData(CLAY_ID("SubagentDelete"));
    cancel = Clay_GetElementData(CLAY_ID("SubagentCancel"));
    save = Clay_GetElementData(CLAY_ID("SubagentSave"));
    bool stacked = cancel.boundingBox.y >= duplicate.boundingBox.y + duplicate.boundingBox.height;
    bool inline_groups = fabsf(cancel.boundingBox.y - duplicate.boundingBox.y) < 0.01f &&
                         remove.boundingBox.x + remove.boundingBox.width < cancel.boundingBox.x;
    if (!card.found || !actions.found || !duplicate.found || !remove.found || !cancel.found || !save.found ||
        (!stacked && !inline_groups) || actions.boundingBox.x < card.boundingBox.x ||
        actions.boundingBox.x + actions.boundingBox.width > card.boundingBox.x + card.boundingBox.width + 0.01f ||
        remove.boundingBox.x + remove.boundingBox.width > actions.boundingBox.x + actions.boundingBox.width + 0.01f ||
        save.boundingBox.x + save.boundingBox.width > actions.boundingBox.x + actions.boundingBox.width + 0.01f) goto done;
    good = true;
done:
    g_clay_frame_width = 1100;
    if (!good) fprintf(stderr, "subagent Settings presentation: %s\n", phase);
    InteractionFixtureFree(&f);
    if (!good) Fail("Subagent Settings must use standard controls/spacing and aligned responsive actions");
    return !good;
}
