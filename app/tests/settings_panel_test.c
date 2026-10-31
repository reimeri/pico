/* Included by host_workspace_test.c: exercise production frames and controls. */
static int TestSettingsPanelInteraction(void)
{
    InteractionFixture f = {0};
    bool good = false;
    const char *phase = "open";
    if (!InteractionFixtureInit(&f)) goto done;
    PicoHost *host = f.host;
    if (!SidebarFrameClick(host, Clay_GetElementData(CLAY_ID("ShellSettings"))) ||
        !PicoSettingsUi_IsOpen(host) || PicoUi_ModalOpen(host) ||
        !Clay_GetElementData(CLAY_ID("SettingsPane")).found) goto done;
    phase = "settings typing is isolated from composer";
    PicoComposer_SetText(host, "chat draft");
    if (!InteractionReplaceText(host, CLAY_ID("SettingsContextLimit"), "12345") ||
        strcmp(host->composer.text, "chat draft")) goto done;
    phase = "Escape leaves panel and busy turn alone";
    PicoAgent *agent = PicoHost_FindAgent(host, f.agent);
    agent->state = PICO_AGENT_LLM_WAIT;
    pico_status_warn(host, "Warning in chat");
    g_find_key = KEY_ESCAPE; PicoHost_Frame(host); g_find_key = -1;
    if (!PicoSettingsUi_IsOpen(host) || PicoAgent_CancelRequested(agent) || !host->status_warn) goto done;
    agent->state = PICO_AGENT_IDLE;
    phase = "chat warnings remain clickable beside settings";
    if (!SidebarFrameClick(host, Clay_GetElementData(CLAY_ID("ExtWarn"))) || host->status_warn ||
        !PicoSettingsUi_IsOpen(host)) goto done;
    phase = "outside click transfers keyboard without dismissing";
    if (!SidebarFrameClick(host, Clay_GetElementData(CLAY_ID("Composer")))) goto done;
    g_find_ctrl = true; g_find_key = KEY_A; PicoHost_Frame(host);
    g_find_ctrl = false; g_find_key = -1;
    g_find_character = 'x'; PicoHost_Frame(host);
    Clay_RenderCommandArray commands = PicoHost_LayoutShell(host, 800, 0);
    if (!PicoSettingsUi_IsOpen(host) || strcmp(host->composer.text, "x") || !FindCardText(&commands, "12345")) goto done;
    phase = "global panel draft survives workspace selection";
    char other_path[4096];
    snprintf(other_path, sizeof(other_path), "%s/other", f.dir);
    Pico_MkdirP(other_path);
    PicoWorkspaceId other_workspace;
    if (pico_workspace_open(host, other_path, &other_workspace) != PICO_OK) goto done;
    PicoAgentId other;
    PicoAgentCreateOptions options = {.kind = PICO_AGENT_MAIN, .select = true, .session_start = PICO_SESSION_NONE};
    if (pico_main_agent_create(host, other_workspace, &options, &other) != PICO_OK) goto done;
    PicoHost_Frame(host);
    commands = PicoHost_LayoutShell(host, 800, 0);
    if (!PicoSettingsUi_IsOpen(host) || !FindCardText(&commands, "12345")) goto done;
    phase = "gear cancels";
    if (!SidebarFrameClick(host, Clay_GetElementData(CLAY_ID("ShellSettings"))) || PicoSettingsUi_IsOpen(host)) goto done;
    phase = "gear reopens";
    if (!SidebarFrameClick(host, Clay_GetElementData(CLAY_ID("ShellSettings"))) || !PicoSettingsUi_IsOpen(host)) goto done;
    phase = "Cancel dismisses the panel";
    if (!SidebarFrameClick(host, Clay_GetElementData(CLAY_ID("SettingsCancel"))) || PicoSettingsUi_IsOpen(host)) goto done;
    good = true;
done:
    if (!good) fprintf(stderr, "settings panel interaction: %s\n", phase);
    InteractionFixtureFree(&f);
    if (!good) Fail("settings panel must be non-modal with isolated focus, global drafts, and explicit dismissal");
    return !good;
}

static int TestSettingsPanelResize(void)
{
    InteractionFixture f = {0};
    bool good = false;
    const char *phase = "open";
    if (!InteractionFixtureInit(&f)) goto done;
    PicoHost *host = f.host;
    PicoSettingsUi_Open(host); PicoHost_Frame(host);
    Clay_BoundingBox pane = Clay_GetElementData(CLAY_ID("SettingsPane")).boundingBox;
    phase = "drag from left edge";
    g_find_pointer = (Vector2){pane.x, pane.y + pane.height / 2};
    g_find_press = g_find_down = true; PicoHost_Frame(host); g_find_press = false;
    g_find_pointer.x += 40; g_find_pointer.y = -10;
    PicoHost_Frame(host);
    Clay_BoundingBox smaller = Clay_GetElementData(CLAY_ID("SettingsPane")).boundingBox;
    if (!host->ui_drag_active || smaller.width >= pane.width ||
        fabsf(smaller.x + smaller.width - pane.x - pane.width) > 0.01f) goto done;
    phase = "Escape ends resize but not panel or captured gesture";
    g_find_key = KEY_ESCAPE; PicoHost_Frame(host); g_find_key = -1;
    g_find_pointer.x -= 300; PicoHost_Frame(host);
    if (host->ui_drag_active || !PicoSettingsUi_IsOpen(host) ||
        !ShellBoxStable(smaller, Clay_GetElementData(CLAY_ID("SettingsPane")).boundingBox) ||
        Clay_GetPointerState().state == CLAY_POINTER_DATA_PRESSED ||
        Clay_GetPointerState().state == CLAY_POINTER_DATA_PRESSED_THIS_FRAME) goto done;
    g_find_down = false; g_find_released = true; PicoHost_Frame(host); g_find_released = false;
    phase = "width survives close and reopen";
    if (!SidebarFrameClick(host, Clay_GetElementData(CLAY_ID("SettingsCancel"))) ||
        !SidebarFrameClick(host, Clay_GetElementData(CLAY_ID("ShellSettings"))) ||
        !ShellBoxStable(smaller, Clay_GetElementData(CLAY_ID("SettingsPane")).boundingBox)) goto done;
    phase = "wide drag leaves chat visible";
    g_find_pointer = (Vector2){smaller.x, smaller.y + smaller.height / 2};
    g_find_press = g_find_down = true; PicoHost_Frame(host); g_find_press = false;
    g_find_pointer.x = -1000; PicoHost_Frame(host);
    g_find_down = false; g_find_released = true; PicoHost_Frame(host); g_find_released = false;
    pane = Clay_GetElementData(CLAY_ID("SettingsPane")).boundingBox;
    Clay_BoundingBox chat = Clay_GetElementData(CLAY_ID("MainColumn")).boundingBox;
    Clay_BoundingBox right = Clay_GetElementData(CLAY_ID("RightColumn")).boundingBox;
    if (chat.width <= 0 || chat.x + chat.width >= pane.x || pane.x + pane.width > right.x + right.width + 0.01f) goto done;
    good = true;
done:
    if (!good) fprintf(stderr, "settings panel resize: %s\n", phase);
    InteractionFixtureFree(&f);
    if (!good) Fail("settings resizing must preserve chat, retain width, and own the complete gesture");
    return !good;
}

static int TestSettingsPanelBrowserSwitch(void)
{
    InteractionFixture f = {0};
    char helper[4096], application[4096], executable[4096];
    bool good = false;
    const char *phase = "setup fake browser";
    if (!InteractionFixtureInit(&f)) goto done;
    PicoHost *host = f.host;
    snprintf(application, sizeof(application), "%s/", f.dir);
    snprintf(helper, sizeof(helper), "%s/browser/runtime", f.dir); Pico_MkdirP(helper);
    snprintf(helper, sizeof(helper), "%s/browser/runtime/pico_browser_helper", f.dir);
    ssize_t length = readlink("/proc/self/exe", executable, sizeof(executable) - 1);
    if (length < 0) goto done;
    executable[length] = 0;
    if (symlink(executable, helper)) goto done;
    setenv("PICO_TEST_BROWSER_FAILURE", "healthy", 1);
    g_browser_application_directory = application;
    PicoAgent *agent = PicoHost_FindAgent(host, f.agent);
    snprintf(agent->session_id, sizeof(agent->session_id), "abcdef123456");
    phase = "settings hides browser without ending its session";
    if (!SidebarFrameClick(host, Clay_GetElementData(CLAY_ID("BrowserToggle"))) ||
        !SidebarFrameClick(host, Clay_GetElementData(CLAY_ID("ShellSettings"))) ||
        !Clay_GetElementData(CLAY_ID("SettingsPane")).found || Clay_GetElementData(CLAY_ID("BrowserPane")).found) goto done;
    phase = "browser replaces clean settings";
    if (!SidebarFrameClick(host, Clay_GetElementData(CLAY_ID("BrowserToggle"))) ||
        PicoSettingsUi_IsOpen(host) || !Clay_GetElementData(CLAY_ID("BrowserPane")).found) goto done;
    phase = "programmatic panel opening does not hide an owned browser modal";
    if (!SidebarFrameClick(host, Clay_GetElementData(CLAY_ID("BrowserProfile")))) goto done;
    PicoSettingsUi_Open(host); PicoHost_Frame(host);
    if (!Clay_GetElementData(CLAY_ID("BrowserProfileCard")).found ||
        !SidebarFrameClick(host, Clay_GetElementData(CLAY_ID("BrowserProfileCancel"))) || PicoUi_ModalOpen(host)) goto done;
    phase = "dirty profile must confirm before switching panels";
    if (!SubagentSettingsOpen(host) || !SubagentSettingsClick(host, CLAY_ID("SubagentNew")) ||
        !SubagentSettingsText(host, 0, 0, "unsaved") ||
        !SidebarFrameClick(host, Clay_GetElementData(CLAY_ID("BrowserToggle"))) ||
        !PicoSettingsUi_IsOpen(host) || !PicoUi_ModalOpen(host) ||
        !Clay_GetElementData(CLAY_ID("SubagentConfirmation")).found) goto done;
    phase = "cancel keeps settings and draft; gear confirmation stays responsive";
    if (!SidebarFrameClick(host, Clay_GetElementData(CLAY_ID("SubagentConfirmNo"))) ||
        !PicoSettingsUi_IsOpen(host) || PicoUi_ModalOpen(host) ||
        !SidebarFrameClick(host, Clay_GetElementData(CLAY_ID("ShellSettings")))) goto done;
    g_find_key = KEY_ESCAPE; PicoHost_Frame(host); g_find_key = -1;
    if (!PicoSettingsUi_IsOpen(host) || PicoUi_ModalOpen(host)) goto done;
    phase = "confirmed browser switch completes";
    if (!SidebarFrameClick(host, Clay_GetElementData(CLAY_ID("BrowserToggle"))) ||
        !SidebarFrameClick(host, Clay_GetElementData(CLAY_ID("SubagentConfirmYes"))) ||
        PicoSettingsUi_IsOpen(host) || PicoUi_ModalOpen(host) ||
        !Clay_GetElementData(CLAY_ID("BrowserPane")).found) goto done;
    good = true;
done:
    if (!good) fprintf(stderr, "settings/browser switch: %s\n", phase);
    InteractionFixtureFree(&f);
    g_browser_application_directory = NULL;
    unsetenv("PICO_TEST_BROWSER_FAILURE");
    if (!good) Fail("browser and settings must share the dock without bypassing or trapping discard confirmations");
    return !good;
}
