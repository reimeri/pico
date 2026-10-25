/* Included by host_workspace_test.c. Exercise file-backed notice navigation and
 * system-opener outcomes through real frame/input and persistence boundaries. */
static bool NavigationClickFile(PicoHost *host, const char *label, const char *expected)
{
    PicoChatSel_Clear(host);
    Clay_RenderCommandArray commands = PicoHost_LayoutShell(host, 800, 0);
    Clay_RenderCommand *text = FindCardText(&commands, label);
    if (!text) return false;
    g_find_pointer = (Vector2){text->boundingBox.x + text->boundingBox.width / 2,
                               text->boundingBox.y + text->boundingBox.height / 2};
    g_chat_link_launches = 0;
    g_find_released = true;
    PicoHost_Frame(host);
    g_find_released = false;
    char actual[4096], wanted[4096];
    return g_chat_link_launches == 1 && realpath(g_chat_link_target, actual) &&
           realpath(expected, wanted) && !strcmp(actual, wanted);
}

static int TestDocsLinkOrigins(void)
{
    InteractionFixture f = {0};
    char installation[] = "/tmp/pico-docs-Alice's project-XXXXXX";
    char directory[4096], path[4096], topic[4096], example[4096], guide[4096], workspace_file[4096];
    char session_id[40], session_path[4096];
    char *old_docs = strdup(Pico_DocsAppDir());
    PicoMessage *snapshot = NULL, *loaded = NULL;
    int snapshot_count = 0, loaded_count = 0;
    bool good = false;
    const char *phase = "setup";
    if (!old_docs || !InteractionFixtureInit(&f) || !mkdtemp(installation)) goto done;
    snprintf(directory, sizeof(directory), "%s/docs/extend", installation);
    Pico_MkdirP(directory);
    snprintf(path, sizeof(path), "%s/docs/extend/README.md", installation);
    if (WriteFile(path, "[Topic](views.md) [Example](../../examples/hello.c)\n")) goto done;
    snprintf(topic, sizeof(topic), "%s/docs/extend/views.md", installation);
    if (WriteFile(topic, "topic\n")) goto done;
    snprintf(directory, sizeof(directory), "%s/examples", installation);
    Pico_MkdirP(directory);
    snprintf(example, sizeof(example), "%s/examples/hello.c", installation);
    if (WriteFile(example, "example\n")) goto done;
    snprintf(path, sizeof(path), "%s/docs/subagents.md", installation);
    if (WriteFile(path, "[Guide](guide.md)\n")) goto done;
    snprintf(guide, sizeof(guide), "%s/docs/guide.md", installation);
    if (WriteFile(guide, "guide\n")) goto done;
    snprintf(workspace_file, sizeof(workspace_file), "%s/mountain.png", f.dir);
    if (WriteFile(workspace_file, "file link fixture\n")) goto done;

    PicoAgentCreateOptions options = {.kind = PICO_AGENT_MAIN, .select = true,
                                      .session_start = PICO_SESSION_NEW};
    if (pico_main_agent_create(f.host, f.workspace, &options, &f.agent) != PICO_OK) goto done;
    PicoAgent *agent = PicoHost_FindAgent(f.host, f.agent);
    if (PicoSession_LogUser(f.host, agent, "seed", "seed", NULL) != PICO_SESSION_WRITE_OK) goto done;
    Pico_DocsSetAppDir(installation);
    PicoComposer_SetText(f.host, "/docs");
    PicoHost_Submit(f.host);
    PicoComposer_SetText(f.host, "/docs subagents");
    PicoHost_Submit(f.host);
    PicoHost_AddNotice(f.host, f.agent, PICO_NOTICE_INFO, "[Workspace](mountain.png)");
    if (!DrainSessionForAssertion(f.host, agent) || !WaitHostTasks(f.host)) goto done;
    snprintf(session_id, sizeof(session_id), "%s", agent->session_id);
    snprintf(session_path, sizeof(session_path), "%s", agent->session_path);
    f.host->view_count[PICO_SLOT_SIDEBAR] = 0;
    InteractionOmitDrawHooks(f.host);
    g_chat_link_test = true;
    phase = "live documentation and following workspace notice";
    if (!NavigationClickFile(f.host, "Topic", topic) ||
        !NavigationClickFile(f.host, "Example", example) ||
        !NavigationClickFile(f.host, "Guide", guide) ||
        !NavigationClickFile(f.host, "Workspace", workspace_file)) goto done;

    /* A snapshot owns the original origin, independently of its source agent. */
    if (!PicoMessages_Copy(agent->messages, agent->message_count, &snapshot, &snapshot_count)) goto done;
    PicoAgent_ClearMessages(agent);
    phase = "copied notice origin after source teardown";
    bool copied_origin = false;
    snprintf(directory, sizeof(directory), "%s/docs/extend", installation);
    for (int i = 0; i < snapshot_count; i++)
        if (snapshot[i].markdown_base_dir && !strcmp(snapshot[i].markdown_base_dir, directory))
            copied_origin = true;
    if (!copied_origin) goto done;

    /* Updating the installation cannot retarget saved text to a newer manual. */
    Pico_DocsSetAppDir(old_docs);
    phase = "saved inspection origin";
    if (PicoSession_LoadTranscript(PicoHost_FindWorkspace(f.host, f.workspace), session_id,
                                   &loaded, &loaded_count)) goto done;
    bool loaded_origin = false;
    for (int i = 0; i < loaded_count; i++)
        if (loaded[i].markdown_base_dir && !strcmp(loaded[i].markdown_base_dir, directory))
            loaded_origin = true;
    if (!loaded_origin) goto done;
    phase = "resumed documentation after installation change";
    if (PicoSession_Replay(f.host, agent, session_path, false) ||
        !NavigationClickFile(f.host, "Topic", topic) ||
        !NavigationClickFile(f.host, "Example", example) ||
        !NavigationClickFile(f.host, "Guide", guide) ||
        !NavigationClickFile(f.host, "Workspace", workspace_file)) goto done;
    good = true;
done:
    g_chat_link_test = false;
    PicoMessages_Free(snapshot, snapshot_count);
    PicoMessages_Free(loaded, loaded_count);
    Pico_DocsSetAppDir(old_docs);
    free(old_docs);
    InteractionFixtureFree(&f);
    RmRf(installation);
    if (!good && !g_failed) { fprintf(stderr, "docs origin phase: %s\n", phase); Fail("documentation links must retain their source directory through copy, inspection and resume"); }
    return good ? 0 : 1;
}

static bool NavigationReleaseLink(PicoHost *host, const char *label)
{
    PicoChatSel_Clear(host);
    Clay_RenderCommandArray commands = PicoHost_LayoutShell(host, 800, 0);
    Clay_RenderCommand *text = FindCardText(&commands, label);
    if (!text) return false;
    g_find_pointer = (Vector2){text->boundingBox.x + text->boundingBox.width / 2,
                               text->boundingBox.y + text->boundingBox.height / 2};
    g_find_released = true;
    PicoHost_Frame(host);
    g_find_released = false;
    return true;
}

static bool NavigationToastIs(PicoHost *host, const char *text)
{
    Clay_RenderCommandArray commands = PicoHost_LayoutShell(host, 800, 0);
    return FindCardText(&commands, text) != NULL;
}

static pid_t NavigationOpenerEntered(const char *marker, const char *target)
{
    char *text = Pico_ReadFile(marker, NULL);
    const char *argument = text ? strchr(text, '\n') : NULL;
    pid_t pid = argument ? (pid_t)strtol(text, NULL, 10) : 0;
    bool exact = argument && !strncmp(argument + 1, target, strlen(target)) &&
                 argument[1 + strlen(target)] == '\n' && !argument[2 + strlen(target)];
    free(text);
    return exact ? pid : 0;
}

static int TestExternalLinkOpener(void)
{
    InteractionFixture f = {0};
    char opener[] = "/tmp/pico-link-opener-XXXXXX";
    char executable[4096] = {0}, marker[4096] = {0}, release[4096] = {0};
    char path[8192], target[4096], markdown[8192];
    char *old_path = strdup(getenv("PATH") ? getenv("PATH") : "");
    pid_t pid = 0;
    bool good = false;
    const char *phase = "setup";
    const char *start_error = "Could not open link: the system opener could not be started.";
    const char *exit_error = "Could not open link: the system opener reported an error.";
    if (!old_path || !InteractionFixtureInit(&f) || !mkdtemp(opener)) goto done;
    snprintf(executable, sizeof(executable), "%s/xdg-open", opener);
    snprintf(marker, sizeof(marker), "%s/entered", opener);
    snprintf(release, sizeof(release), "%s/release", opener);
    const char *script = "#!/bin/sh\n"
        "printf '%s\\n%s\\n' \"$$\" \"$1\" > \"$PICO_LINK_TEST_MARKER\"\n"
        "while [ ! -f \"$PICO_LINK_TEST_RELEASE\" ]; do sleep 0.01; done\n"
        "case \"$PICO_LINK_TEST_STATUS\" in signal) kill -TERM \"$$\";; *) exit \"$PICO_LINK_TEST_STATUS\";; esac\n";
    if (WriteFile(executable, script) || chmod(executable, 0755)) goto done;
    snprintf(path, sizeof(path), "%s:%s", opener, old_path);
    setenv("PICO_LINK_TEST_MARKER", marker, 1);
    setenv("PICO_LINK_TEST_RELEASE", release, 1);
    setenv("PICO_LINK_TEST_STATUS", "7", 1);
    if (!PicoPath_Format(target, sizeof(target), "%s/Alice's $(printf NOT_LITERAL) & icon.png", f.dir) ||
        WriteFile(target, "image file link fixture\n")) goto done;
    snprintf(markdown, sizeof(markdown), "[Literal](<%s>)", target);
    PicoAgent_AddMessage(f.host, PicoHost_FindAgent(f.host, f.agent), PICO_ROLE_ASSISTANT, markdown);
    f.host->view_count[PICO_SLOT_SIDEBAR] = 0;

    phase = "failure to start";
    setenv("PATH", "/nonexistent-pico-link-opener", 1);
    if (!NavigationReleaseLink(f.host, "Literal") || !NavigationToastIs(f.host, start_error)) goto done;

    phase = "literal argv and nonblocking launcher";
    setenv("PATH", path, 1);
    if (!NavigationReleaseLink(f.host, "Literal")) goto done;
    PICO_TEST_WAIT(!(pid = NavigationOpenerEntered(marker, target))) {}
    /* The frame returned while the opener is still waiting for release. */
    if (kill(pid, 0) || WriteFile(release, "release\n")) goto done;
    phase = "asynchronous exit failure";
    PICO_TEST_WAIT_LOOP("failed system opener produces a visible error")
    {
        pico_host_pump(f.host);
        if (NavigationToastIs(f.host, exit_error)) break;
    }

    phase = "successful opener must not report an error";
    if (unlink(marker) || unlink(release)) goto done;
    pid = 0;
    setenv("PICO_LINK_TEST_STATUS", "0", 1);
    PicoOverlay_Notify(f.host, "Ready");
    if (!NavigationReleaseLink(f.host, "Literal")) goto done;
    PICO_TEST_WAIT(!(pid = NavigationOpenerEntered(marker, target))) {}
    if (WriteFile(release, "release\n")) goto done;
    PICO_TEST_WAIT(kill(pid, 0) == 0) { pico_host_pump(f.host); }
    if (NavigationToastIs(f.host, exit_error)) goto done;

    phase = "signal termination reports failure";
    if (unlink(marker) || unlink(release)) goto done;
    pid = 0;
    setenv("PICO_LINK_TEST_STATUS", "signal", 1);
    PicoOverlay_Notify(f.host, "Ready");
    if (!NavigationReleaseLink(f.host, "Literal")) goto done;
    PICO_TEST_WAIT(!(pid = NavigationOpenerEntered(marker, target))) {}
    if (WriteFile(release, "release\n")) goto done;
    PICO_TEST_WAIT_LOOP("signal-terminated opener produces a visible error")
    {
        pico_host_pump(f.host);
        if (NavigationToastIs(f.host, exit_error)) break;
    }

    /* Auth's automatic browser launch keeps its own feedback policy, even
     * when the tracked child slot previously belonged to an explicit click. */
    phase = "reporting opt-out after slot reuse";
    if (unlink(marker) || unlink(release)) goto done;
    pid = 0;
    setenv("PICO_LINK_TEST_STATUS", "7", 1);
    PicoOverlay_Notify(f.host, "Ready");
    if (!PicoHost_OpenExternal(f.host, target, false)) goto done;
    PICO_TEST_WAIT(!(pid = NavigationOpenerEntered(marker, target))) {}
    if (WriteFile(release, "release\n")) goto done;
    PICO_TEST_WAIT(kill(pid, 0) == 0) { pico_host_pump(f.host); }
    if (NavigationToastIs(f.host, exit_error)) goto done;
    good = true;
done:
    if (release[0]) WriteFile(release, "release\n");
    if (old_path) setenv("PATH", old_path, 1);
    free(old_path);
    unsetenv("PICO_LINK_TEST_MARKER"); unsetenv("PICO_LINK_TEST_RELEASE"); unsetenv("PICO_LINK_TEST_STATUS");
    InteractionFixtureFree(&f);
    RmRf(opener);
    if (!good && !g_failed) { fprintf(stderr, "external opener phase: %s\n", phase); Fail("file links must use literal nonblocking launches and report both start and exit failures"); }
    return good ? 0 : 1;
}
