/* Included by host_workspace_test.c: drive the real shell input and layout. */
static void SidebarResizePress(PicoHost *host)
{
    Clay_BoundingBox pane = Clay_GetElementData(CLAY_ID("Sidebar")).boundingBox;
    g_find_pointer = (Vector2){pane.x + pane.width, pane.y + pane.height / 2};
    g_find_press = g_find_down = true;
    PicoHost_Frame(host);
    g_find_press = false;
}

static void SidebarResizeRelease(PicoHost *host)
{
    g_find_down = false; g_find_released = true;
    PicoHost_Frame(host);
    g_find_released = false;
}

static bool SidebarResizeCannotScroll(PicoHost *host, Clay_ElementId container, Clay_ElementId handle_id)
{
    SidebarResizePress(host);
    Clay_ElementData handle = Clay_GetElementData(handle_id);
    if (!handle.found || handle.boundingBox.height <= 0) return false;
    g_find_pointer = (Vector2){handle.boundingBox.x + handle.boundingBox.width / 2,
                              handle.boundingBox.y + handle.boundingBox.height / 2};
    PicoHost_Frame(host); PicoHost_Frame(host);
    Clay_ScrollContainerData scroll = Clay_GetScrollContainerData(container);
    if (!scroll.found || !scroll.scrollPosition || scroll.contentDimensions.height <= scroll.scrollContainerDimensions.height) return false;
    float scroll_y = scroll.scrollPosition->y;
    g_find_pointer.y += 50; PicoHost_Frame(host);
    g_find_key = KEY_ESCAPE; PicoHost_Frame(host); g_find_key = -1;
    g_find_pointer.y += 30; PicoHost_Frame(host);
    scroll = Clay_GetScrollContainerData(container);
    bool unchanged = scroll.found && scroll.scrollPosition && fabsf(scroll.scrollPosition->y - scroll_y) < 0.01f;
    SidebarResizeRelease(host);
    return unchanged;
}

static int TestSidebarResize(void)
{
    InteractionFixture f = {0};
    bool good = false;
    const char *phase = "setup";
    g_window_focus_override = 1;
    if (!InteractionFixtureInit(&f)) goto done;
    PicoHost *host = f.host;
    Clay_BoundingBox original = Clay_GetElementData(CLAY_ID("Sidebar")).boundingBox;
    Clay_BoundingBox right = Clay_GetElementData(CLAY_ID("RightColumn")).boundingBox;
    phase = "right-edge drag updates the sidebar and remaining shell";
    SidebarResizePress(host);
    if (g_mouse_cursor != MOUSE_CURSOR_RESIZE_EW) goto done;
    g_find_pointer.x += 80; g_find_pointer.y = -10;
    PicoHost_Frame(host);
    Clay_BoundingBox wider = Clay_GetElementData(CLAY_ID("Sidebar")).boundingBox;
    Clay_BoundingBox remaining = Clay_GetElementData(CLAY_ID("RightColumn")).boundingBox;
    if (!host->ui_drag_active || fabsf(wider.width - original.width - 80) > 0.01f ||
        fabsf(wider.x - original.x) > 0.01f || fabsf(remaining.x - right.x - 80) > 0.01f ||
        fabsf(remaining.x + remaining.width - right.x - right.width) > 0.01f) goto done;
    phase = "Escape consumes the held gesture without cancelling a turn or editing the composer";
    PicoAgent *agent = PicoHost_FindAgent(host, f.agent);
    agent->state = PICO_AGENT_LLM_WAIT;
    PicoComposer_SetText(host, "keep this draft");
    g_find_key = KEY_ESCAPE; PicoHost_Frame(host); g_find_key = -1;
    g_find_pointer.x += 100; g_find_character = 'x'; PicoHost_Frame(host);
    if (host->ui_drag_active || PicoAgent_CancelRequested(agent) ||
        strcmp(host->composer.text, "keep this draft") ||
        !ShellBoxStable(wider, Clay_GetElementData(CLAY_ID("Sidebar")).boundingBox) ||
        Clay_GetPointerState().state == CLAY_POINTER_DATA_PRESSED ||
        Clay_GetPointerState().state == CLAY_POINTER_DATA_PRESSED_THIS_FRAME) goto done;
    agent->state = PICO_AGENT_IDLE;
    g_find_character = 'x';
    SidebarResizeRelease(host);
    if (strcmp(host->composer.text, "keep this draft")) goto done;
    g_find_character = 0;
    phase = "release over chat cannot start a selection";
    SidebarResizePress(host);
    g_find_pointer.x += 30; PicoHost_Frame(host);
    Clay_BoundingBox chosen = Clay_GetElementData(CLAY_ID("Sidebar")).boundingBox;
    SidebarResizeRelease(host);
    if (host->ui_drag_active || host->chat_sel.mouse_selecting ||
        !ShellBoxStable(chosen, Clay_GetElementData(CLAY_ID("Sidebar")).boundingBox)) goto done;
    phase = "minimum width keeps sidebar content usable";
    SidebarResizePress(host);
    g_find_pointer.x = -1000; PicoHost_Frame(host);
    Clay_BoundingBox minimum = Clay_GetElementData(CLAY_ID("Sidebar")).boundingBox;
    g_find_pointer.x -= 200; PicoHost_Frame(host);
    if (minimum.width <= 0 || minimum.width >= original.width ||
        !ShellBoxStable(minimum, Clay_GetElementData(CLAY_ID("Sidebar")).boundingBox) ||
        PicoHost_SidebarContentWidth(host) <= 0) goto done;
    SidebarResizeRelease(host);
    phase = "maximum leaves chat visible beside an open right-hand panel";
    PicoSettingsUi_Open(host); PicoHost_Frame(host);
    phase = "focused settings field does not receive captured resize keyboard input";
    if (!InteractionReplaceText(host, CLAY_ID("SettingsContextLimit"), "12345")) goto done;
    SidebarResizePress(host);
    g_find_character = '6'; PicoHost_Frame(host);
    g_find_character = '7'; g_find_key = KEY_ESCAPE; PicoHost_Frame(host); g_find_key = -1;
    g_find_character = '8'; SidebarResizeRelease(host); g_find_character = 0;
    Clay_RenderCommandArray commands = PicoHost_LayoutShell(host, 800, 0);
    if (!FindCardText(&commands, "12345")) goto done;
    phase = "maximum leaves chat visible beside an open right-hand panel";
    SidebarResizePress(host);
    g_find_pointer.x = 3000; PicoHost_Frame(host);
    Clay_BoundingBox maximum = Clay_GetElementData(CLAY_ID("Sidebar")).boundingBox;
    remaining = Clay_GetElementData(CLAY_ID("RightColumn")).boundingBox;
    Clay_BoundingBox chat = Clay_GetElementData(CLAY_ID("MainColumn")).boundingBox;
    Clay_BoundingBox settings = Clay_GetElementData(CLAY_ID("SettingsPane")).boundingBox;
    g_find_pointer.x += 200; PicoHost_Frame(host);
    if (maximum.width <= chosen.width || maximum.width >= remaining.width || chat.width <= 0 ||
        chat.x + chat.width >= settings.x ||
        !ShellBoxStable(maximum, Clay_GetElementData(CLAY_ID("Sidebar")).boundingBox)) goto done;
    SidebarResizeRelease(host);
    PicoSettingsUi_Close(host); PicoHost_Frame(host);
    phase = "temporary narrow viewport cap wins over minimum without losing preference";
    Clay_SetLayoutDimensions((Clay_Dimensions){380, 800});
    (void)PicoHost_LayoutShell(host, 800, 0);
    Clay_BoundingBox narrow = Clay_GetElementData(CLAY_ID("Sidebar")).boundingBox;
    remaining = Clay_GetElementData(CLAY_ID("RightColumn")).boundingBox;
    if (narrow.width >= minimum.width || narrow.width <= 0 || remaining.width <= narrow.width) goto done;
    Clay_SetLayoutDimensions((Clay_Dimensions){1100, 800});
    (void)PicoHost_LayoutShell(host, 800, 0);
    if (!ShellBoxStable(maximum, Clay_GetElementData(CLAY_ID("Sidebar")).boundingBox)) goto done;
    phase = "held resize cannot acquire the chat scrollbar, including after Escape";
    for (int i = 0; i < 80; i++)
        PicoAgent_AddMessage(host, agent, PICO_ROLE_USER, "Overflowing chat for scrollbar capture regression.");
    host->chat_follow_bottom = false;
    for (int i = 0; i < 20; i++) { pico_host_request_redraw(host); PicoHost_Frame(host); }
    if (!SidebarResizeCannotScroll(host, CLAY_ID("ChatScroll"), CLAY_ID("ChatScrollBarHandle"))) goto done;
    phase = "held resize cannot acquire an overflowing composer scrollbar";
    char draft[4096] = {0};
    for (int i = 0; i < 80; i++) strcat(draft, "Composer line\n");
    PicoComposer_SetText(host, draft);
    for (int i = 0; i < 3; i++) { pico_host_request_redraw(host); PicoHost_Frame(host); }
    if (!SidebarResizeCannotScroll(host, CLAY_ID("ComposerScroll"), CLAY_ID("CompScrollBarHandle"))) goto done;
    PicoComposer_SetText(host, "keep this draft");
    phase = "focus loss stops resizing and retains capture until physical release";
    SidebarResizePress(host);
    g_find_pointer.x -= 50; PicoHost_Frame(host);
    chosen = Clay_GetElementData(CLAY_ID("Sidebar")).boundingBox;
    g_window_focus_override = 0; PicoHost_Frame(host);
    g_window_focus_override = 1; g_find_pointer.x -= 100; PicoHost_Frame(host);
    if (host->ui_drag_active || !ShellBoxStable(chosen, Clay_GetElementData(CLAY_ID("Sidebar")).boundingBox) ||
        Clay_GetPointerState().state == CLAY_POINTER_DATA_PRESSED ||
        Clay_GetPointerState().state == CLAY_POINTER_DATA_PRESSED_THIS_FRAME) goto done;
    g_find_character = 'x';
    SidebarResizeRelease(host);
    if (strcmp(host->composer.text, "keep this draft")) goto done;
    g_find_character = 0;
    phase = "modal blocks new resize gestures";
    if (!pico_ui_modal_push(host, "sidebar-resize-test")) goto done;
    SidebarResizePress(host);
    g_find_pointer.x -= 50; PicoHost_Frame(host);
    SidebarResizeRelease(host);
    pico_ui_modal_pop(host, "sidebar-resize-test");
    if (!ShellBoxStable(chosen, Clay_GetElementData(CLAY_ID("Sidebar")).boundingBox)) goto done;
    phase = "sidebar width and unrelated panel preferences survive restart";
    host->browser_panel_width = 375;
    PicoSettings_SavePanelWidth(host, PICO_PANEL_BROWSER);
    if (pico_host_free(host) != PICO_HOST_SHUTDOWN_CLEAN) { f.host = NULL; goto done; }
    f.host = NULL;
    if (pico_host_init(&f.host, NULL, true) != PICO_OK) goto done;
    host = f.host; WaitPluginLoad(host);
    (void)PicoHost_LayoutShell(host, 800, 0);
    if (host->browser_panel_width != 375 ||
        !ShellBoxStable(chosen, Clay_GetElementData(CLAY_ID("Sidebar")).boundingBox)) goto done;
    phase = "hidden sidebar has no resize handle, even without a selected conversation";
    PicoHost_ClearAgentSelection(host);
    host->view_count[PICO_SLOT_SIDEBAR] = 0;
    (void)PicoHost_LayoutShell(host, 800, 0);
    if (Clay_GetElementData(CLAY_ID("SidebarResizeHandle")).found) goto done;
    good = true;
done:
    if (!good) fprintf(stderr, "sidebar resize: %s\n", phase);
    InteractionFixtureFree(&f);
    g_window_focus_override = -1;
    if (!good) Fail("sidebar resize must bound and persist width and own the entire gesture");
    return !good;
}
