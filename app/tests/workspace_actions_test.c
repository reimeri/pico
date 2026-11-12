/* Included by host_workspace_test.c. Real footer input and fake desktop
 * launchers keep this independent of the developer's installed applications. */
static void WorkspaceActionsKey(PicoHost *host, int key)
{
    g_find_key = key;
    PicoHost_Frame(host);
    g_find_key = -1;
}

static bool WorkspaceActionsChoose(PicoHost *host, int index, bool keyboard)
{
    if (!SidebarFrameClick(host, Clay_GetElementData(CLAY_ID("FooterActions"))) ||
        !pico_ui_modal_is_top(host, "footer-menu")) return false;
    if (keyboard)
    {
        /* Move off the popup so pointer hover does not override keyboard focus. */
        g_find_pointer = (Vector2){0, 0};
        for (int i = 0; i < index; i++) WorkspaceActionsKey(host, KEY_DOWN);
        if (index > 0)
        {
            WorkspaceActionsKey(host, KEY_UP);
            WorkspaceActionsKey(host, KEY_DOWN);
        }
        WorkspaceActionsKey(host, KEY_ENTER);
    }
    else if (!SidebarFrameClick(host, Clay_GetElementData(CLAY_IDI("FooterMenuItem", index)))) return false;
    return !pico_ui_modal_is_top(host, "footer-menu");
}

static bool WorkspaceActionsMarkerIs(const char *marker, const char *expected)
{
    char *text = Pico_ReadFile(marker, NULL);
    bool matches = text && !strcmp(text, expected);
    free(text);
    return matches;
}

/* Clay may split long notices into several text commands. Check the visible
 * toast's runs in order, allowing the whitespace dropped at line wraps. */
static bool WorkspaceActionsToastIs(PicoHost *host, const char *expected)
{
    Clay_RenderCommandArray commands = PicoHost_LayoutShell(host, 800, 0);
    Clay_ElementData toast = Clay_GetElementData(CLAY_ID("NotifyToastBody"));
    if (!toast.found) return false;
    const char *remaining = expected;
    for (int i = 0; i < commands.length; i++)
    {
        Clay_RenderCommand *command = Clay_RenderCommandArray_Get(&commands, i);
        if (command->commandType != CLAY_RENDER_COMMAND_TYPE_TEXT ||
            !ShellVerticallyContains(toast.boundingBox, command->boundingBox) ||
            command->boundingBox.x < toast.boundingBox.x ||
            command->boundingBox.x + command->boundingBox.width > toast.boundingBox.x + toast.boundingBox.width) continue;
        while (*remaining == ' ') remaining++;
        Clay_StringSlice text = command->renderData.text.stringContents;
        if (strlen(remaining) < (size_t)text.length || strncmp(remaining, text.chars, (size_t)text.length)) return false;
        remaining += text.length;
    }
    return !*remaining;
}

static int TestWorkspaceActions(void)
{
    InteractionFixture f = {0};
    char launchers[] = "/tmp/pico-workspace-actions-XXXXXX";
    const char *names[] = {"xdg-open", "xdg-terminal-exec", "code", "x-terminal-emulator"};
    const char *start_errors[] = {
        "Could not open workspace in file manager: xdg-open could not be started.",
        "Could not open workspace in terminal: install xdg-terminal-exec or x-terminal-emulator.",
        "Could not open workspace in VS Code: code could not be started. Check that it is in PATH.",
    };
    char executable_path[8192], decoy[4096], executables[4][4096], marker[4096] = {0}, checkout[4096], cwd[4096], expected[16384];
    char *old_path = strdup(getenv("PATH") ? getenv("PATH") : "");
    bool good = false;
    const char *phase = "setup";
    if (!old_path || !InteractionFixtureInit(&f) || !mkdtemp(launchers) || !getcwd(cwd, sizeof(cwd))) goto done;
    snprintf(marker, sizeof(marker), "%s/entered", launchers);
    snprintf(checkout, sizeof(checkout), "%s/Alice's $(not-a-command) & checkout", f.dir);
    if (mkdir(checkout, 0755)) goto done;
    PicoWorkspaceId workspace = 0;
    PicoAgentId agent = 0;
    PicoAgentCreateOptions options = {.kind = PICO_AGENT_MAIN, .select = true, .session_start = PICO_SESSION_NONE};
    if (pico_workspace_open(f.host, checkout, &workspace) != PICO_OK ||
        pico_main_agent_create(f.host, workspace, &options, &agent) != PICO_OK) goto done;
    /* A linked checkout displays its project in the footer, but actions must
     * use the selected session's directory, not that project's root or cwd. */
    PicoWorkspace *ws = PicoHost_FindWorkspace(f.host, workspace);
    ws->worktree = ws->checkout_root = true;
    snprintf(ws->project_path, sizeof(ws->project_path), "%s", f.dir);
    for (int i = 0; i < 4; i++)
    {
        char script[8192];
        snprintf(executables[i], sizeof(executables[i]), "%s/%s", launchers, names[i]);
        snprintf(script, sizeof(script), "#!/bin/sh\n{ printf '%%s\\n%%s\\n%%s\\n' '%s' \"$PWD\" \"$#\"; "
            "for arg do printf '%%s\\n' \"$arg\"; done; } > \"$PICO_ACTION_TEST_MARKER\"\n"
            "exit \"${PICO_ACTION_TEST_STATUS:-0}\"\n", names[i]);
        if (WriteFile(executables[i], script) || chmod(executables[i], 0755)) goto done;
    }
    setenv("PICO_ACTION_TEST_MARKER", marker, 1);
    snprintf(executable_path, sizeof(executable_path), ".:%s", launchers);
    setenv("PATH", executable_path, 1);
    if (!PicoPath_Format(decoy, sizeof(decoy), "%s/xdg-terminal-exec", checkout)) goto done;
    if (WriteFile(decoy, "#!/bin/sh\nprintf 'decoy\\n' > \"$PICO_ACTION_TEST_MARKER\"\n") || chmod(decoy, 0755)) goto done;
    f.host->view_count[PICO_SLOT_SIDEBAR] = 0;
    PicoHost_Frame(f.host);
    phase = "popup presentation and dismissal";
    if (!SidebarFrameClick(f.host, Clay_GetElementData(CLAY_ID("FooterActions")))) goto done;
    Clay_RenderCommandArray commands = PicoHost_LayoutShell(f.host, 800, 0);
    if (!FindCardText(&commands, "Open in file manager") || !FindCardText(&commands, "Open in terminal") ||
        !FindCardText(&commands, "Open in VS Code")) goto done;
    Clay_ElementData popup = Clay_GetElementData(CLAY_ID("FooterMenu"));
    Clay_ElementData button = Clay_GetElementData(CLAY_ID("FooterActions"));
    if (!popup.found || !button.found || popup.boundingBox.y + popup.boundingBox.height > button.boundingBox.y ||
        popup.boundingBox.x < 0 || popup.boundingBox.x + popup.boundingBox.width > 1100) goto done;
    WorkspaceActionsKey(f.host, KEY_ESCAPE);
    if (pico_ui_modal_is_top(f.host, "footer-menu")) goto done;
    if (!SidebarFrameClick(f.host, Clay_GetElementData(CLAY_ID("FooterActions"))) ||
        !SidebarFrameClick(f.host, Clay_GetElementData(CLAY_ID("FooterActions"))) ||
        pico_ui_modal_is_top(f.host, "footer-menu")) goto done;
    if (!SidebarFrameClick(f.host, Clay_GetElementData(CLAY_ID("FooterActions")))) goto done;
    g_find_pointer = (Vector2){0, 0}; g_find_press = true;
    PicoHost_Frame(f.host); g_find_press = false;
    if (pico_ui_modal_is_top(f.host, "footer-menu") || access(marker, F_OK) == 0) goto done;

    phase = "literal workspace launches from mouse and keyboard";
    for (int i = 0; i < 3; i++)
    {
        unlink(marker);
        if (!WorkspaceActionsChoose(f.host, i, i == 1)) goto done;
        snprintf(expected, sizeof(expected), "%s\n%s\n1\n%s%s\n", names[i], i == 1 ? checkout : cwd,
                 i == 1 ? "--dir=" : "", checkout);
        PICO_TEST_WAIT(!WorkspaceActionsMarkerIs(marker, expected) && !WorkspaceActionsMarkerIs(marker, "decoy\n")) {}
        if (!WorkspaceActionsMarkerIs(marker, expected)) goto done;
    }
    char after_cwd[4096];
    if (!getcwd(after_cwd, sizeof(after_cwd)) || strcmp(cwd, after_cwd)) goto done;
    phase = "terminal fallback inherits workspace cwd";
    if (unlink(executables[1])) goto done;
    unlink(marker);
    if (!WorkspaceActionsChoose(f.host, 1, false)) goto done;
    snprintf(expected, sizeof(expected), "x-terminal-emulator\n%s\n0\n", checkout);
    PICO_TEST_WAIT(!WorkspaceActionsMarkerIs(marker, expected)) {}

    phase = "asynchronous failure notice";
    setenv("PICO_ACTION_TEST_STATUS", "7", 1);
    unlink(marker);
    if (!WorkspaceActionsChoose(f.host, 2, false)) goto done;
    PICO_TEST_WAIT_LOOP("workspace launcher exit failure produces a notice")
    {
        pico_host_pump(f.host);
        if (WorkspaceActionsToastIs(f.host, "Could not open workspace in VS Code: the launcher reported an error.")) break;
    }
    phase = "missing launchers remain actionable and report failure";
    for (int i = 0; i < 4; i++) unlink(executables[i]);
    for (int i = 0; i < 3; i++)
        if (!WorkspaceActionsChoose(f.host, i, false) || !WorkspaceActionsToastIs(f.host, start_errors[i])) goto done;
    good = true;
done:
    if (old_path) setenv("PATH", old_path, 1);
    free(old_path);
    unsetenv("PICO_ACTION_TEST_MARKER"); unsetenv("PICO_ACTION_TEST_STATUS");
    InteractionFixtureFree(&f);
    RmRf(launchers);
    if (!good && !g_failed) { fprintf(stderr, "workspace actions phase: %s\n", phase); Fail("footer workspace actions must dismiss cleanly and launch the selected checkout safely"); }
    return good ? 0 : 1;
}
