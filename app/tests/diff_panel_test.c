/* Production frames exercise public shell controls, not review internals. */
static bool DiffTestWrite(const char *dir, int extra)
{
    char path[4096];
    snprintf(path, sizeof(path), "%s/review.txt", dir);
    FILE *file = fopen(path, "w");
    if (!file) return false;
    for (int i = 0; i < extra; i++) fprintf(file, "inserted before review %d\n", i);
    for (int i = 0; i < 160; i++) fprintf(file, "review line %03d\n", i);
    return fclose(file) == 0;
}

static int TestDiffPanelInteraction(void)
{
    InteractionFixture f = {0};
    bool good = false;
    const char *phase = "open empty panel";
    if (!InteractionFixtureInit(&f)) goto done;
    PicoHost *host = f.host;
    if (!SidebarFrameClick(host, Clay_GetElementData(CLAY_ID("ShellChanges"))) ||
        !PicoDiff_IsOpen(host) || PicoUi_ModalOpen(host) ||
        !Clay_GetElementData(CLAY_ID("ChangesPane")).found) goto done;
    phase = "focus isolates chat draft and Escape";
    PicoComposer_SetText(host, "chat draft");
    g_find_character = 'x'; PicoHost_Frame(host);
    g_find_key = KEY_ESCAPE; PicoHost_Frame(host); g_find_key = -1;
    if (strcmp(host->composer.text, "chat draft") || !PicoDiff_IsOpen(host)) goto done;
    phase = "settings replaces Changes";
    if (!SidebarFrameClick(host, Clay_GetElementData(CLAY_ID("ShellSettings"))) ||
        PicoDiff_IsOpen(host) || !PicoSettingsUi_IsOpen(host)) goto done;
    if (!SidebarFrameClick(host, Clay_GetElementData(CLAY_ID("ShellChanges"))) ||
        !PicoDiff_IsOpen(host) || PicoSettingsUi_IsOpen(host)) goto done;
    phase = "capture untracked content";
    char command[8192], path[4096];
    snprintf(command, sizeof(command), "git -C '%s' init -q", f.dir);
    if (system(command) || !DiffTestWrite(f.dir, 0)) goto done;
    snprintf(path, sizeof(path), "%s/.gitignore", f.dir);
    if (WriteFile(path, ".pico/\n.gitignore\n")) goto done;
    PicoDiffUi_Close(host); PicoDiffUi_Open(host);
    PICO_TEST_WAIT_LOOP("Changes captures review file") {
        PicoHost_Frame(host);
        Clay_RenderCommandArray commands = PicoHost_LayoutShell(host, 800, 0);
        if (FindCardText(&commands, "review line 000")) break;
    }
    pico_host_request_redraw(host); PicoHost_Frame(host);
    phase = "footer toggles panel and preserves scroll position";
    Clay_ScrollContainerData scroll = Clay_GetScrollContainerData(CLAY_ID("DiffScroll"));
    if (!scroll.found || !scroll.scrollPosition) goto done;
    scroll.scrollPosition->y = -600;
    pico_host_request_redraw(host); PicoHost_Frame(host);
    float before = Clay_GetScrollContainerData(CLAY_ID("DiffScroll")).scrollPosition->y;
    if (!SidebarFrameClick(host, Clay_GetElementData(CLAY_ID("FooterDiff"))) || PicoDiff_IsOpen(host) ||
        Clay_GetElementData(CLAY_ID("ChangesPane")).found ||
        !SidebarFrameClick(host, Clay_GetElementData(CLAY_ID("FooterDiff"))) || !PicoDiff_IsOpen(host) ||
        !Clay_GetElementData(CLAY_ID("ChangesPane")).found) goto done;
    PicoHost_Frame(host);
    scroll = Clay_GetScrollContainerData(CLAY_ID("DiffScroll"));
    if (!scroll.found || fabsf(scroll.scrollPosition->y - before) > 0.01f) { fprintf(stderr, "diff retention before=%f after=%f found=%d\n", before, scroll.found ? scroll.scrollPosition->y : 0, scroll.found); goto done; }
    phase = "refresh anchors source after insertion above viewport";
    if (!DiffTestWrite(f.dir, 20)) goto done;
    /* Reopening requests immediate refresh without resetting review state. */
    PicoDiffUi_Open(host);
    PICO_TEST_WAIT_LOOP("Changes refresh anchors source") {
        PicoHost_Frame(host);
        scroll = Clay_GetScrollContainerData(CLAY_ID("DiffScroll"));
        if (scroll.found && scroll.scrollPosition->y < before - 100) break;
    }
    phase = "file collapse, context-free navigation and reopen";
    if (!SidebarFrameClick(host, Clay_GetElementData(CLAY_ID("DiffSticky")))) goto done;
    PicoHost_Frame(host);
    scroll = Clay_GetScrollContainerData(CLAY_ID("DiffScroll"));
    if (scroll.contentDimensions.height > scroll.scrollContainerDimensions.height) goto done;
    if (!SidebarFrameClick(host, Clay_GetElementData(CLAY_ID("DiffNext")))) goto done;
    PicoHost_Frame(host);
    scroll = Clay_GetScrollContainerData(CLAY_ID("DiffScroll"));
    if (scroll.contentDimensions.height <= scroll.scrollContainerDimensions.height) goto done;
    phase = "resize keeps chat visible";
    Clay_BoundingBox pane = Clay_GetElementData(CLAY_ID("ChangesPane")).boundingBox;
    g_find_pointer = (Vector2){pane.x, pane.y + pane.height / 2};
    g_find_press = g_find_down = true; PicoHost_Frame(host); g_find_press = false;
    g_find_pointer.x += 40; PicoHost_Frame(host);
    g_find_down = false; g_find_released = true; PicoHost_Frame(host); g_find_released = false;
    if (Clay_GetElementData(CLAY_ID("ChangesPane")).boundingBox.width >= pane.width ||
        Clay_GetElementData(CLAY_ID("MainColumn")).boundingBox.width <= 0) goto done;
    good = true;
done:
    if (!good) fprintf(stderr, "Changes panel: %s\n", phase);
    InteractionFixtureFree(&f);
    if (!good) Fail("Changes must be non-modal, preserve review state, navigate collapsed files, and isolate focus");
    return !good;
}

static Clay_RenderCommand *DiffTextIn(Clay_RenderCommandArray *commands, const char *text, Clay_BoundingBox box)
{
    for (int i = 0; i < commands->length; i++) {
        Clay_RenderCommand *c = Clay_RenderCommandArray_Get(commands, i);
        if (c->commandType != CLAY_RENDER_COMMAND_TYPE_TEXT) continue;
        Clay_StringSlice t = c->renderData.text.stringContents;
        if (t.length == (int)strlen(text) && !memcmp(t.chars, text, (size_t)t.length) &&
            c->boundingBox.x >= box.x && c->boundingBox.y >= box.y &&
            c->boundingBox.y + c->boundingBox.height <= box.y + box.height) return c;
    }
    return NULL;
}

static int TestDiffPanelContext(void)
{
    InteractionFixture f = {0};
    bool good = false;
    const char *phase = "capture";
    if (!InteractionFixtureInit(&f)) goto done;
    char command[8192], path[4096];
    snprintf(command, sizeof(command), "git -C '%s' init -q", f.dir);
    if (system(command)) goto done;
    snprintf(path, sizeof(path), "%s/src", f.dir); Pico_MkdirP(path);
    snprintf(path, sizeof(path), "%s/src/review.txt", f.dir);
    FILE *file = fopen(path, "w");
    if (!file) goto done;
    for (int i = 0; i < 4000; i++) fprintf(file, "review line %04d\n", i);
    fclose(file);
    snprintf(command, sizeof(command), "git -C '%s' add src/review.txt && git -C '%s' -c user.name=Test -c user.email=test@example.invalid -c commit.gpgsign=false -c core.hooksPath=/dev/null commit -qm initial", f.dir, f.dir);
    if (system(command)) goto done;
    file = fopen(path, "w");
    if (!file) goto done;
    for (int i = 0; i < 4000; i++) {
        if (i == 80 || i == 3080) fprintf(file, "changed review line %04d\n", i);
        else fprintf(file, "review line %04d\n", i);
    }
    fclose(file);
    snprintf(path, sizeof(path), "%s/.gitignore", f.dir);
    if (WriteFile(path, ".pico/\n.gitignore\n")) goto done;
    PicoDiffUi_Open(f.host);
    PICO_TEST_WAIT_LOOP("Changes displays focused tracked hunks") {
        PicoHost_Frame(f.host);
        Clay_RenderCommandArray commands = PicoHost_LayoutShell(f.host, 800, 0);
        if (FindCardText(&commands, "changed review line 3080")) break;
    }
    phase = "only adjacent context is shown";
    Clay_RenderCommandArray commands = PicoHost_LayoutShell(f.host, 800, 0);
    if (!FindCardText(&commands, "review line 0077") || !FindCardText(&commands, "review line 0083") ||
        FindCardText(&commands, "review line 0076") || FindCardText(&commands, "review line 0084")) goto done;
    Clay_ScrollContainerData scroll = Clay_GetScrollContainerData(CLAY_ID("DiffScroll"));
    float compact_height = scroll.contentDimensions.height;
    if (compact_height > scroll.scrollContainerDimensions.height) goto done;
    phase = "picker name directory and colored totals share a row";
    if (!SidebarFrameClick(f.host, Clay_GetElementData(CLAY_ID("DiffFilesToggle")))) goto done;
    commands = PicoHost_LayoutShell(f.host, 800, 0);
    Clay_BoundingBox row = Clay_GetElementData(CLAY_IDI("DiffFile", 0)).boundingBox;
    Clay_BoundingBox footer = Clay_GetElementData(CLAY_ID("FooterDiff")).boundingBox;
    Clay_RenderCommand *name = DiffTextIn(&commands, "review.txt", row);
    Clay_RenderCommand *dir = DiffTextIn(&commands, "src", row);
    Clay_RenderCommand *adds = DiffTextIn(&commands, "+2", row), *dels = DiffTextIn(&commands, "−2", row);
    Clay_RenderCommand *footer_adds = DiffTextIn(&commands, "+2", footer), *footer_dels = DiffTextIn(&commands, "-2", footer);
    if (!name || !dir || !adds || !dels || !footer_adds || !footer_dels ||
        name->boundingBox.x >= dir->boundingBox.x || dir->boundingBox.x >= adds->boundingBox.x ||
        adds->boundingBox.x >= dels->boundingBox.x || dels->boundingBox.x + dels->boundingBox.width > row.x + row.width ||
        fabsf(name->boundingBox.y - adds->boundingBox.y) > 0.01f ||
        memcmp(&adds->renderData.text.textColor, &footer_adds->renderData.text.textColor, sizeof(Clay_Color)) ||
        memcmp(&dels->renderData.text.textColor, &footer_dels->renderData.text.textColor, sizeof(Clay_Color))) goto done;
    if (!SidebarFrameClick(f.host, Clay_GetElementData(CLAY_IDI("DiffFile", 0))) ||
        Clay_GetElementData(CLAY_ID("DiffFilesMenu")).found) goto done;
    phase = "expanding one gap leaves other gaps collapsed";
    commands = PicoHost_LayoutShell(f.host, 800, 0);
    Clay_RenderCommand *gap = FindCardText(&commands, "… 77 unchanged lines · expand context");
    if (!gap || !SidebarFrameClick(f.host, (Clay_ElementData){.found=true, .boundingBox=gap->boundingBox})) goto done;
    scroll = Clay_GetScrollContainerData(CLAY_ID("DiffScroll"));
    /* The leading gap is much smaller than the untouched middle gap. Its
     * expansion must not turn this 4000-line source into a full-file view. */
    if (scroll.contentDimensions.height <= compact_height ||
        scroll.contentDimensions.height >= compact_height * 10) goto done;
    commands = PicoHost_LayoutShell(f.host, 800, 0);
    if (!FindCardText(&commands, "review line 0000")) goto done;
    good = true;
done:
    if (!good) fprintf(stderr, "Changes context/picker: %s\n", phase);
    InteractionFixtureFree(&f);
    if (!good) Fail("Changes must trim context, expand only the chosen gap, and align filename/path/footer-colored counts");
    return !good;
}

static int TestDiffFileBands(void)
{
    InteractionFixture f = {0};
    bool good = false;
    if (!InteractionFixtureInit(&f)) goto done;
    char command[8192], path[4096];
    snprintf(command, sizeof(command), "git -C '%s' init -q", f.dir);
    if (system(command)) goto done;
    snprintf(path, sizeof(path), "%s/.gitignore", f.dir);
    if (WriteFile(path, ".pico/\n.gitignore\n")) goto done;
    const char *names[] = {"first.txt", "second.txt"};
    for (int i = 0; i < 2; i++) {
        snprintf(path, sizeof(path), "%s/%s", f.dir, names[i]);
        FILE *file = fopen(path, "w");
        if (!file) goto done;
        for (int line = 0; line < 60; line++) fprintf(file, "source line %d\n", line);
        fclose(file);
    }
    PicoDiffUi_Open(f.host);
    PICO_TEST_WAIT_LOOP("file bands capture") {
        PicoHost_Frame(f.host);
        Clay_RenderCommandArray commands = PicoHost_LayoutShell(f.host, 800, 0);
        if (FindCardText(&commands, "source line 0")) break;
    }
    Clay_RenderCommandArray commands = PicoHost_LayoutShell(f.host, 800, 0);
    Clay_BoundingBox viewport = Clay_GetElementData(CLAY_ID("DiffScroll")).boundingBox;
    Clay_RenderCommand *name = DiffTextIn(&commands, "first.txt", viewport);
    if (!name) goto done;
    bool band = false, border = false;
    for (int i = 0; i < commands.length; i++) {
        Clay_RenderCommand *c = Clay_RenderCommandArray_Get(&commands, i);
        Clay_BoundingBox box = c->boundingBox;
        if (box.y > name->boundingBox.y || box.y + box.height < name->boundingBox.y + name->boundingBox.height ||
            box.width < viewport.width || box.height <= name->boundingBox.height) continue;
        /* The document header, rather than an ancestor pane, supplies both
         * the contrasting band and its visible boundary. */
        if (box.y < viewport.y || box.height >= viewport.height) continue;
        if (c->commandType == CLAY_RENDER_COMMAND_TYPE_RECTANGLE &&
            c->renderData.rectangle.backgroundColor.a > 0) band = true;
        if (c->commandType == CLAY_RENDER_COMMAND_TYPE_BORDER && c->renderData.border.width.top > 0) border = true;
    }
    if (!band || !border) goto done;
    Clay_ScrollContainerData scroll = Clay_GetScrollContainerData(CLAY_ID("DiffScroll"));
    float height = scroll.contentDimensions.height;
    /* Crossing taller file headers must not change the virtual document's
     * measured extent or introduce spacer drift. */
    for (float y = 0; y < height - viewport.height; y += viewport.height / 3) {
        scroll = Clay_GetScrollContainerData(CLAY_ID("DiffScroll"));
        scroll.scrollPosition->y = -y;
        (void)PicoHost_LayoutShell(f.host, 800, 0);
        scroll = Clay_GetScrollContainerData(CLAY_ID("DiffScroll"));
        if (fabsf(scroll.contentDimensions.height - height) > 0.01f) goto done;
    }
    if (!SidebarFrameClick(f.host, Clay_GetElementData(CLAY_ID("DiffFilesToggle"))) ||
        !SidebarFrameClick(f.host, Clay_GetElementData(CLAY_IDI("DiffFile", 1)))) goto done;
    commands = PicoHost_LayoutShell(f.host, 800, 0);
    Clay_BoundingBox sticky = Clay_GetElementData(CLAY_ID("DiffSticky")).boundingBox;
    if (!DiffTextIn(&commands, "second.txt", sticky)) goto done;
    good = true;
done:
    InteractionFixtureFree(&f);
    if (!good) Fail("file boundaries need padded full-width bands and stable height-aware scrolling/navigation");
    return !good;
}
