/* Included after InteractionFixture: assertions observe the real host's first
 * presentation, never an extra layout that could conceal missing invalidation. */
static bool LayoutClickAt(PicoHost *host, float x, float y)
{
    g_find_pointer = (Vector2){x, y};
    Clay_SetPointerState((Clay_Vector2){x, y}, false);
    g_find_press = true;
    PicoHost_Frame(host);
    g_find_press = false;
    return host->frame_presented;
}

static int TestModalAndWarningDismissal(void)
{
    InteractionFixture f = {0};
    bool good = false;
    if (!InteractionFixtureInit(&f)) goto done;
    char agents_path[4096], instructions[4096] = {0};
    snprintf(agents_path, sizeof(agents_path), "%s/AGENTS.md", f.dir);
    for (int i = 0; i < 80; i++) strcat(instructions, "Prompt overflow fixture line\n");
    if (WriteFile(agents_path, instructions)) goto done;
    PicoComposer_SetText(f.host, "/show-prompt");
    PicoHost_Submit(f.host);
    PicoHost_Frame(f.host);
    if (!Clay_GetElementData(CLAY_ID("PromptModalCard")).found) goto done;
    bool overflow = PicoScrollbar_Overflows(CLAY_STRING("PromptModalScroll"));
    if (!overflow || !Clay_GetElementData(CLAY_ID("PromptModalScrollHandle")).found) goto done;
    if (!LayoutClickAt(f.host, 2, 2) || Clay_GetElementData(CLAY_ID("PromptModalCard")).found)
        goto done;
    for (int agent_error = 0; agent_error < 2; agent_error++)
    {
        PicoAgent *agent = PicoHost_FindAgent(f.host, f.agent);
        if (agent_error) { agent->error = strdup("Dismissed agent error"); agent->state = PICO_AGENT_ERROR; }
        else f.host->status_warn = strdup("Dismissed host warning");
        pico_host_request_redraw(f.host);
        PicoHost_Frame(f.host);
        if (!SidebarFrameClick(f.host, Clay_GetElementData(CLAY_ID("ExtWarn"))) ||
            Clay_GetElementData(CLAY_ID("ExtWarn")).found) goto done;
    }
    good = true;
done:
    InteractionFixtureFree(&f);
    if (!good) Fail("prompt and warning dismissal must discard borrowed text commands before presentation");
    return !good;
}

static int TestAttachmentPresentation(void)
{
    InteractionFixture f = {0};
    bool good = false;
    const char *phase = "setup";
    if (!InteractionFixtureInit(&f)) goto done;
    g_chat_image_test = true;
    char photo[4096]; snprintf(photo, sizeof(photo), "%s/photo.png", f.dir);
    Image image = GenImageColor(80, 40, RED);
    bool saved = ExportImage(image, photo); UnloadImage(image);
    if (!saved || !pico_composer_attach_path(photo, false)) goto done;
    pico_host_request_redraw(f.host); PicoHost_Frame(f.host);
    phase = "open preview";
    if (!SidebarFrameClick(f.host, Clay_GetElementData(CLAY_IDI("CompAttach", 0))) ||
        !pico_ui_modal_is_top(f.host, "preview") ||
        !Clay_GetElementData(CLAY_ID("ComposerPreviewDim")).found) goto done;
    phase = "close preview";
    if (!LayoutClickAt(f.host, 2, 2) || pico_ui_modal_has(f.host, "preview") ||
        Clay_GetElementData(CLAY_ID("ComposerPreviewDim")).found) goto done;
    phase = "remove attachment";
    Clay_ElementData attachment = Clay_GetElementData(CLAY_IDI("CompAttach", 0));
    g_find_pointer = (Vector2){attachment.boundingBox.x + 5, attachment.boundingBox.y + 5};
    Clay_SetPointerState((Clay_Vector2){g_find_pointer.x, g_find_pointer.y}, false);
    pico_host_request_redraw(f.host); PicoHost_Frame(f.host);
    if (!SidebarFrameClick(f.host, Clay_GetElementData(CLAY_IDI("CompAttachRemove", 0))) ||
        Clay_GetElementData(CLAY_IDI("CompAttach", 0)).found ||
        ChatImageCommand(&g_last_presented_commands, 0)) goto done;
    good = true;
done:
    InteractionFixtureFree(&f);
    g_chat_image_test = false;
    if (!good) fprintf(stderr, "attachment phase: %s\n", phase);
    if (!good) Fail("preview and attachment removal must rebuild commands before consuming changed textures");
    return !good;
}

static int LayoutCompletionQuery(PicoHost *host, const char *prefix, PicoCompleteItem *out, int max, void *state)
{
    (void)host; (void)prefix; (void)state;
    if (max < 1) return 0;
    memset(out, 0, sizeof(*out));
    snprintf(out->label, sizeof(out->label), "Insert long completion");
    /* Longer than a row and the initial small composer allocation. */
    memset(out->insert, 'x', sizeof(out->insert) - 2);
    out->insert[sizeof(out->insert) - 2] = ' ';
    return 1;
}

static int TestComposerPointerCompletion(void)
{
    InteractionFixture f = {0};
    bool good = false;
    if (!InteractionFixtureInit(&f)) goto done;
    f.host->completers[f.host->completer_count++] = (PicoCompleter){
        .trigger = '%', .host_query = LayoutCompletionQuery};
    PicoComposer_SetText(f.host, "%");
    pico_host_request_redraw(f.host); PicoHost_Frame(f.host);
    if (!SidebarFrameClick(f.host, Clay_GetElementData(CLAY_IDI("CompleteItem", 0))) ||
        Clay_GetElementData(CLAY_ID("CompletePopup")).found ||
        !Clay_GetElementData(CLAY_IDI("CompLine", 1)).found) goto done;
    PicoComposer_SetText(f.host, "%");
    pico_host_request_redraw(f.host); PicoHost_Frame(f.host);
    if (!Clay_GetElementData(CLAY_ID("CompletePopup")).found ||
        !LayoutClickAt(f.host, 2, 2) || Clay_GetElementData(CLAY_ID("CompletePopup")).found) goto done;
    good = true;
done:
    InteractionFixtureFree(&f);
    if (!good) Fail("pointer completion must replace composer text and dismiss popup in its first presentation");
    return !good;
}

static int TestComposerScrollPresentation(void)
{
    InteractionFixture f = {0};
    bool good = false;
    if (!InteractionFixtureInit(&f)) goto done;
    char text[4096] = {0};
    for (int i = 0; i < 50; i++) strcat(text, "Composer line\n");
    strcat(text, "Caret target");
    PicoComposer_SetText(f.host, text);
    pico_host_request_redraw(f.host); PicoHost_Frame(f.host);
    Clay_ElementData thumb = Clay_GetElementData(CLAY_ID("CompScrollBarHandle"));
    Clay_RenderCommand *target = FindCardText(&g_last_presented_commands, "Caret target");
    Clay_ElementData viewport = Clay_GetElementData(CLAY_ID("ComposerScroll"));
    if (!thumb.found || !target || !viewport.found ||
        !ShellVerticallyContains(viewport.boundingBox, target->boundingBox)) goto done;
    pico_host_request_redraw(f.host); PicoHost_Frame(f.host);
    Clay_ElementData next = Clay_GetElementData(CLAY_ID("CompScrollBarHandle"));
    if (!next.found || !ShellBoxStable(thumb.boundingBox, next.boundingBox)) goto done;
    PicoComposer_SetText(f.host, "Short");
    pico_host_request_redraw(f.host); PicoHost_Frame(f.host);
    if (Clay_GetElementData(CLAY_ID("CompScrollBarHandle")).found ||
        !FindCardText(&g_last_presented_commands, "Short")) goto done;
    good = true;
done:
    InteractionFixtureFree(&f);
    if (!good) Fail("composer reveal and scrollbar must agree with text on first presentation");
    return !good;
}

static int TestLayoutInvalidation(void)
{
    return PICO_TEST_RUN(TestModalAndWarningDismissal()) || PICO_TEST_RUN(TestAttachmentPresentation()) ||
           PICO_TEST_RUN(TestComposerPointerCompletion()) || PICO_TEST_RUN(TestComposerScrollPresentation());
}
