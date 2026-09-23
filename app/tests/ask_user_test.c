#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Force buffer growth to relocate, so stale-pointer regressions do not depend
 * on the allocator extending an allocation in place. */
static void *MovingRealloc(void *ptr, size_t size)
{
    void *grown = realloc(ptr, size);
    if (!grown)
    {
        return NULL;
    }
    void *moved = malloc(size);
    if (!moved)
    {
        return grown;
    }
    memcpy(moved, grown, size);
    free(grown);
    return moved;
}

#define realloc MovingRealloc
/* Include the builtin to exercise input without exposing test-only API. */
#include "builtins/ask_user.c"
#undef realloc

bool PicoUi_ModalOpen(const PicoHost *app) { (void)app; return false; }
bool PicoChatFind_PointerOver(const PicoHost *app) { (void)app; return false; }
void PicoChatSel_Clear(PicoHost *app) { (void)app; }

static bool mouse_pressed;
static Vector2 mouse_position;
static bool find_focused;
static PicoToolAsk pending_asks[2];
static int visible_ask;
static char *submitted_answer;
static uint64_t submitted_id;
static int typed_character;
static int pressed_key;
static bool control_down;
static bool shift_down;
static bool paste_pressed;
static const char *clipboard_text;

bool IsKeyDown(int key)
{
    return (key == KEY_LEFT_CONTROL && control_down) ||
           (key == KEY_LEFT_SHIFT && shift_down);
}
bool IsKeyPressed(int key) { return key == pressed_key; }
bool IsKeyPressedRepeat(int key) { (void)key; return false; }
int GetCharPressed(void) { int cp = typed_character; typed_character = 0; return cp; }
double GetTime(void) { return 0; }
const char *GetClipboardText(void) { return clipboard_text; }
void SetClipboardText(const char *text) { (void)text; }
bool Pico_ShortcutPressed(char letter) { return letter == 'v' && paste_pressed; }
bool Pico_ShortcutRepeat(char letter) { (void)letter; return false; }
float Pico_FontPx(uint16_t design) { return (float)design; }
Font Pico_FontAt(uint16_t id, uint16_t size)
{
    (void)id;
    (void)size;
    return (Font){0};
}
static float test_glyph_width = 1.0f;

Vector2 MeasureTextEx(Font font, const char *text, float size, float spacing)
{
    (void)font;
    (void)spacing;
    return (Vector2){(float)strlen(text) * test_glyph_width, size};
}
bool pico_tool_answer(PicoHost *host, uint64_t id, const char *answer)
{
    (void)host;
    if (!id) return false;
    free(submitted_answer);
    submitted_answer = JsonDup(answer);
    submitted_id = id;
    return true;
}

bool IsMouseButtonPressed(int button) { (void)button; return mouse_pressed; }
bool IsMouseButtonDown(int button) { (void)button; return false; }
Vector2 GetMousePosition(void) { return mouse_position; }
bool PicoScrollbar_Overflows(Clay_String container_id)
{
    (void)container_id;
    return false;
}
void *PicoPlugins_HostState(const PicoHost *host, const char *name)
{
    (void)host;
    (void)name;
    return NULL;
}

void pico_host_add_view(PicoHost *app, PicoUiSlot slot, int z, PicoHostViewFn fn)
{ (void)app; (void)slot; (void)z; (void)fn; }
void pico_host_add_hook(PicoHost *app, PicoHook kind, PicoHostHookFn fn)
{ (void)app; (void)kind; (void)fn; }
void BeginScissorMode(int x, int y, int w, int h) { (void)x; (void)y; (void)w; (void)h; }
void EndScissorMode(void) {}
void DrawRectangle(int x, int y, int w, int h, Color c) { (void)x; (void)y; (void)w; (void)h; (void)c; }

bool PicoChatFind_BlocksInput(const PicoHost *app) { (void)app; return find_focused; }
bool PicoChatSel_HasSelection(const PicoHost *app) { (void)app; return false; }
void PicoChatSel_Copy(PicoHost *app) { (void)app; }
float PicoHost_MainColumnWidth(const PicoHost *host) { (void)host; return Clay_GetLayoutDimensions().width; }
float Pico_ChatColumnMaxPx(const PicoHost *host) { (void)host; return 0; }
bool pico_tool_pending_ask(const PicoHost *app, PicoToolAsk *out)
{
    (void)app;
    if (!pending_asks[visible_ask].id) return false;
    *out = pending_asks[visible_ask];
    return true;
}
bool PicoAgent_PendingAsk(const PicoAgent *agent, PicoToolAsk *out)
{
    /* The fixture uses two distinct opaque agent identities. */
    int i = agent == (const PicoAgent *)&pending_asks[0] ? 0 : 1;
    *out = pending_asks[i];
    return out->id != 0;
}
void PicoScrollbar_UpdateDrag(PicoScrollbar *bar, Clay_String container, Clay_String handle)
{ (void)bar; (void)container; (void)handle; }
void PicoScrollbar_UpdateDragOverlay(PicoScrollbar *bar, Clay_String container, Clay_String handle)
{ (void)bar; (void)container; (void)handle; }
void PicoScrollbar_Render(Clay_String container, Clay_String track, Clay_String handle)
{ (void)container; (void)track; (void)handle; }
void PicoScrollbar_RenderOverlay(Clay_String container, Clay_String track, Clay_String handle)
{ (void)container; (void)track; (void)handle; }

static Clay_Dimensions ClayMeasureStub(Clay_StringSlice text, Clay_TextElementConfig *config, void *userData)
{
    (void)userData;
    return (Clay_Dimensions){(float)text.length * test_glyph_width, (float)config->fontSize};
}

static int TestTextInput(void)
{
    AskQuestion questions[2] = {0};
    AskUiState ui = {.questions = questions, .question_count = 2, .current = 1};
    s_active_ask_state = &ui;
    AskQuestion *q = &questions[1];
    q->kind = ASK_QUESTION_TEXT;
    q->text = JsonDup("");
    q->text_cap = 1;
    AskTextInsert(q, "a", 1);

    /* Reaching the start, and pressing Left there, must stay on this question. */
    pressed_key = KEY_LEFT;
    HandleTextKeys(NULL, q);
    HandleTextKeys(NULL, q);
    int failed = ui.current != 1 || q->cursor != 0;
    if (failed)
    {
        fprintf(stderr, "left: question=%d cursor=%d\n", ui.current, q->cursor);
    }

    AskMoveCursor(q, q->text_len, false);
    control_down = shift_down = true;
    HandleTextKeys(NULL, q);
    if (ui.current != 1 || AskSelFrom(q) != 0 || AskSelTo(q) != q->text_len)
    {
        fprintf(stderr, "selection: question=%d range=%d..%d length=%d\n",
                ui.current, AskSelFrom(q), AskSelTo(q), q->text_len);
        failed = 1;
    }
    control_down = shift_down = false;

    /* Left is not question navigation when an ordinary option is selected either. */
    q->kind = ASK_QUESTION_SELECT;
    q->option_count = 1;
    q->selected = 0;
    HandleSelectKeys(NULL, q);
    failed |= ui.current != 1 || q->selected != 0;
    q->option_count = 0;
    FreeQuestion(q);

    /* Paste and Ctrl+Left can arrive together, while growing the answer buffer.
     * Deleting at the resulting caret makes the editing result observable. */
    q->kind = ASK_QUESTION_TEXT;
    q->text = JsonDup("");
    q->text_cap = 1;
    control_down = paste_pressed = true;
    clipboard_text = "alpha beta";
    HandleTextKeys(NULL, q);
    control_down = paste_pressed = false;
    pressed_key = KEY_DELETE;
    HandleTextKeys(NULL, q);
    if (!q->text || strcmp(q->text, "alpha eta") != 0)
    {
        fprintf(stderr, "paste editing: got %s\n", q->text ? q->text : "NULL");
        failed = 1;
    }
    FreeQuestion(q);
    pressed_key = 0;
    s_active_ask_state = NULL;
    if (failed)
    {
        fprintf(stderr, "ask input: arrow navigation, selection, or paste editing regressed\n");
    }
    return failed;
}

/* The panel's text field is an editing target: hovering it must request the
 * I-beam cursor (hovered_text) rather than the link pointer
 * (hovered_clickable); app.c maps those flags to cursor shapes. */
static int TestHoverCursor(void)
{
    int failed = 0;
    uint32_t arena_size = Clay_MinMemorySize();
    void *memory = malloc(arena_size);
    PicoHost *app = (PicoHost *)calloc(1, sizeof(PicoHost));
    if (!memory || !app)
    {
        free(memory);
        free(app);
        fprintf(stderr, "hover cursor: allocation failed\n");
        return 1;
    }
    Clay_Arena arena = Clay_CreateArenaWithCapacityAndMemory(arena_size, memory);
    Clay_Initialize(arena, (Clay_Dimensions){800, 600}, (Clay_ErrorHandler){0});
    Clay_SetMeasureTextFunction(ClayMeasureStub, NULL);

    AskQuestion question = {0};
    question.kind = ASK_QUESTION_TEXT;
    AskUiState ui = {0};
    ui.questions = &question;
    ui.question_count = 1;
    ui.current = 0;
    ui.show = true;
    AskHostState host_ui = {.active = &ui};
    s_active_ask_state = &ui;

    /* Text box at (0,0)-(400,200) with the scrollbar handle along its left
     * edge; the Next button sits right of the box at (400,0)-(500,40). */
    Clay_BeginLayout();
    CLAY(CLAY_ID("AskUserTextBox"),
         {.layout = {.sizing = {.width = CLAY_SIZING_FIXED(400), .height = CLAY_SIZING_FIXED(200)}}})
    {
        CLAY(CLAY_ID("AskUserTextScrollHandle"),
             {.layout = {.sizing = {.width = CLAY_SIZING_FIXED(16), .height = CLAY_SIZING_FIXED(100)}}})
        {
        }
    }
    CLAY(CLAY_ID("AskUserNext"),
         {.layout = {.sizing = {.width = CLAY_SIZING_FIXED(100), .height = CLAY_SIZING_FIXED(40)}}})
    {
    }
    Clay_EndLayout(0);

    /* Hovering the text field asks for the I-beam, not the link pointer. */
    Clay_SetPointerState((Clay_Vector2){100, 100}, false);
    AskUserAfterLayout(app, NULL, &host_ui);
    if (!app->hovered_text || app->hovered_clickable)
    {
        fprintf(stderr, "hover cursor: text field must set hovered_text only\n");
        failed = 1;
    }

    /* The scrollbar strip inside the field keeps the default cursor. */
    app->hovered_text = false;
    app->hovered_clickable = false;
    Clay_SetPointerState((Clay_Vector2){8, 50}, false);
    AskUserAfterLayout(app, NULL, &host_ui);
    if (app->hovered_text || app->hovered_clickable)
    {
        fprintf(stderr, "hover cursor: scrollbar must keep the default cursor\n");
        failed = 1;
    }

    /* Buttons keep the pointer cursor. */
    app->hovered_text = false;
    app->hovered_clickable = false;
    char answer[] = "a";
    question.text = answer;
    question.text_len = 1;
    Clay_SetPointerState((Clay_Vector2){450, 20}, false);
    AskUserAfterLayout(app, NULL, &host_ui);
    if (!app->hovered_clickable || app->hovered_text)
    {
        fprintf(stderr, "hover cursor: answered Next button must set hovered_clickable only\n");
        failed = 1;
    }

    s_active_ask_state = NULL;
    free(app);
    Clay_SetCurrentContext(NULL);
    free(memory);
    return failed;
}

static Clay_RenderCommandArray PanelLayout(PicoHost *app, AskHostState *ui)
{
    Clay_BeginLayout();
    AskUserRender(app, ui);
    return Clay_EndLayout(0);
}

static bool PanelHasText(Clay_RenderCommandArray commands, const char *text)
{
    for (int i = 0; i < commands.length; i++)
    {
        Clay_RenderCommand *cmd = Clay_RenderCommandArray_Get(&commands, i);
        if (cmd->commandType != CLAY_RENDER_COMMAND_TYPE_TEXT) continue;
        Clay_StringSlice label = cmd->renderData.text.stringContents;
        if (label.length == (int)strlen(text) && !memcmp(label.chars, text, (size_t)label.length)) return true;
    }
    return false;
}

static void PanelClick(PicoHost *app, AskHostState *ui, Clay_ElementId id)
{
    Clay_BoundingBox box = Clay_GetElementData(id).boundingBox;
    mouse_position = (Vector2){box.x + box.width / 2, box.y + box.height / 2};
    mouse_pressed = true;
    Clay_SetPointerState((Clay_Vector2){mouse_position.x, mouse_position.y}, true);
    AskUserAfterLayout(app, NULL, ui);
    mouse_pressed = false;
    Clay_SetPointerState((Clay_Vector2){mouse_position.x, mouse_position.y}, false);
}

static int TestQuestionPanel(void)
{
    const char *request = "{\"type\":\"questionnaire\",\"ui\":\"custom\",\"questions\":["
        "{\"id\":\"target\",\"question\":\"Choose target\",\"kind\":\"select\",\"options\":[\"CLI\",\"GUI\"]},"
        "{\"id\":\"notes\",\"question\":\"Any notes?\",\"kind\":\"text\"}]}";
    void *memory = malloc(Clay_MinMemorySize());
    PicoHost *app = calloc(1, sizeof(*app));
    PicoWorkspace *ws = calloc(1, sizeof(*ws));
    AskHostState *ui = calloc(1, sizeof(*ui));
    if (!memory || !app || !ws || !ui) abort();
    Clay_Initialize(Clay_CreateArenaWithCapacityAndMemory(Clay_MinMemorySize(), memory),
                    (Clay_Dimensions){800, 800}, (Clay_ErrorHandler){0});
    Clay_SetMeasureTextFunction(ClayMeasureStub, NULL);
    app->workspaces[0] = ws;
    app->workspace_count = 1;
    ws->count = 2;
    for (int i = 0; i < 2; i++)
    {
        pending_asks[i] = (PicoToolAsk){.id = (uint64_t)i + 1, .request_json = request};
        ws->agents[i] = (PicoAgent *)&pending_asks[i];
    }
    visible_ask = 0;
    AskUserOnFrame(app, ui, 0);
    Clay_RenderCommandArray commands = PanelLayout(app, ui);
    int failed = !PanelHasText(commands, "Choose target") || PanelHasText(commands, "Any notes?");
    float expanded = Clay_GetElementData(CLAY_ID("Composer")).boundingBox.height;
    pressed_key = KEY_TWO;
    AskUserOnFrame(app, ui, 0);
    pressed_key = KEY_ENTER;
    AskUserOnFrame(app, ui, 0);
    pressed_key = 0;
    commands = PanelLayout(app, ui);
    failed |= !PanelHasText(commands, "Any notes?") || PanelHasText(commands, "Choose target");
    typed_character = 'A';
    AskUserOnFrame(app, ui, 0);
    PanelLayout(app, ui);
    PanelClick(app, ui, CLAY_ID("AskUserToggle"));
    commands = PanelLayout(app, ui);
    failed |= !PanelHasText(commands, "Resume") || PanelHasText(commands, "Any notes?") ||
              Clay_GetElementData(CLAY_ID("Composer")).boundingBox.height >= expanded || submitted_id != 0;
    pressed_key = KEY_ENTER;
    AskUserOnFrame(app, ui, 0);
    failed |= submitted_id != 0; /* Collapse is not submit, nor is Enter while collapsed. */
    pressed_key = 0;
    visible_ask = 1;
    AskUserOnFrame(app, ui, 0);
    commands = PanelLayout(app, ui);
    failed |= !PanelHasText(commands, "Choose target");
    visible_ask = 0;
    AskUserOnFrame(app, ui, 0);
    commands = PanelLayout(app, ui);
    failed |= !PanelHasText(commands, "Resume");
    PanelClick(app, ui, CLAY_ID("AskUserToggle"));
    commands = PanelLayout(app, ui);
    failed |= !PanelHasText(commands, "Any notes?") || !PanelHasText(commands, "A");
    /* Search takes keyboard ownership, and does not return it implicitly. */
    find_focused = true;
    pressed_key = KEY_ENTER;
    AskUserOnFrame(app, ui, 0);
    find_focused = false;
    pressed_key = 0;
    visible_ask = 1;
    AskUserOnFrame(app, ui, 0);
    visible_ask = 0;
    AskUserOnFrame(app, ui, 0);
    pressed_key = KEY_ENTER;
    AskUserOnFrame(app, ui, 0);
    failed |= submitted_id != 0;
    pressed_key = 0;
    PanelClick(app, ui, CLAY_ID("AskUserTextBox"));
    pressed_key = KEY_ENTER;
    AskUserOnFrame(app, ui, 0);
    pressed_key = 0;
    failed |= submitted_id != 1 || !submitted_answer || strcmp(submitted_answer,
        "{\"answers\":[{\"id\":\"target\",\"answer\":\"GUI\"},{\"id\":\"notes\",\"answer\":\"A\"}]}") != 0;
    /* A still-visible snapshot after submit must not reopen the questionnaire. */
    AskUserOnFrame(app, ui, 0);
    commands = PanelLayout(app, ui);
    failed |= PanelHasText(commands, "Choose target");
    memset(pending_asks, 0, sizeof(pending_asks));
    AskUserOnFrame(app, ui, 0);
    failed |= PanelHasText(PanelLayout(app, ui), "Any notes?");
    AskUserHostShutdown(app, ui);
    free(submitted_answer);
    submitted_answer = NULL;
    submitted_id = 0;
    free(ws);
    free(app);
    Clay_SetCurrentContext(NULL);
    free(memory);
    if (failed) fprintf(stderr, "question panel: navigation, collapse, draft retention, focus, or submission failed\n");
    return failed;
}

static int TestLongQuestionScroll(void)
{
    const char *request = "{\"type\":\"questionnaire\",\"ui\":\"custom\",\"questions\":["
        "{\"id\":\"target\",\"question\":\"Which target?\",\"kind\":\"select\","
        "\"options\":[\"one\",\"two\",\"three\",\"four\",\"five\",\"six\",\"seven\",\"eight\"]},"
        "{\"id\":\"next\",\"question\":\"Next question\",\"kind\":\"text\"}]}";
    void *memory = malloc(Clay_MinMemorySize());
    PicoHost *app = calloc(1, sizeof(*app));
    AskUiState *ui = calloc(1, sizeof(*ui));
    AskHostState *host_ui = calloc(1, sizeof(*host_ui));
    if (!memory || !app || !ui || !host_ui) abort();
    Clay_Initialize(Clay_CreateArenaWithCapacityAndMemory(Clay_MinMemorySize(), memory),
                    (Clay_Dimensions){500, 700}, (Clay_ErrorHandler){0});
    Clay_SetMeasureTextFunction(ClayMeasureStub, NULL);
    s_active_ask_state = ui;
    host_ui->requests = host_ui->active = ui;
    char error[192];
    int failed = LoadUiRequest(request, error, sizeof(error)) != 1;
    PanelLayout(app, host_ui);
    AskUserAfterLayout(app, NULL, host_ui);
    /* Keyboard-select Other below the fold; its free-form editor must become
     * visible without hiding the fixed Next/Back controls. */
    pressed_key = KEY_NINE;
    HandleSelectKeys(app, &ui->questions[0]);
    pressed_key = 0;
    PanelLayout(app, host_ui);
    AskUserAfterLayout(app, NULL, host_ui);
    PanelLayout(app, host_ui);
    Clay_BoundingBox body = Clay_GetElementData(CLAY_ID("AskUserBody")).boundingBox;
    Clay_BoundingBox editor = Clay_GetElementData(CLAY_ID("AskUserTextBox")).boundingBox;
    failed |= editor.y < body.y || editor.y + editor.height > body.y + body.height + .01f;
    AskTextInsert(&ui->questions[0], "Custom target", 13);
    pressed_key = KEY_ENTER;
    HandleTextKeys(app, &ui->questions[0]);
    pressed_key = 0;
    Clay_RenderCommandArray commands = PanelLayout(app, host_ui);
    Clay_ScrollContainerData scroll = Clay_GetScrollContainerData(CLAY_ID("AskUserBody"));
    failed |= !PanelHasText(commands, "Next question") || !scroll.found || scroll.scrollPosition->y != 0;
    AskTextInsert(&ui->questions[1], "Notes", 5);
    char *answer = BuildAnswer();
    failed |= !answer || strcmp(answer,
        "{\"answers\":[{\"id\":\"target\",\"answer\":\"Custom target\"},{\"id\":\"next\",\"answer\":\"Notes\"}]}");
    free(answer);
    AskUserHostShutdown(app, host_ui);
    free(app);
    Clay_SetCurrentContext(NULL);
    free(memory);
    if (failed) fprintf(stderr, "long question: Other editor visibility or next-question scroll reset failed\n");
    return failed;
}

/* Long prompts and choices must remain inside the panel, with later rows
 * pushed down rather than overlapping a wrapped label. */
static int TestWrappedQuestionPanel(void)
{
    const char *request = "{\"type\":\"questionnaire\",\"ui\":\"custom\",\"questions\":["
        "{\"id\":\"power\",\"question\":\"What base command power generation rate per second should apply without any portal? For reference: max-command-power-stays-below-the-maximum-at-all-times-even-without-any-portals\","
        "\"kind\":\"select\",\"options\":[\"A long option describing the rate and the amount of power gained each second without a portal: commandpowergenerationratewithnoportalactiveatall\",\"Second option\"]}]}";
    void *memory = malloc(Clay_MinMemorySize());
    PicoHost *app = calloc(1, sizeof(*app));
    AskUiState *ui = calloc(1, sizeof(*ui));
    AskHostState *host_ui = calloc(1, sizeof(*host_ui));
    if (!memory || !app || !ui || !host_ui) abort();
    test_glyph_width = 7.0f;
    Clay_Initialize(Clay_CreateArenaWithCapacityAndMemory(Clay_MinMemorySize(), memory),
                    (Clay_Dimensions){500, 600}, (Clay_ErrorHandler){0});
    Clay_SetMeasureTextFunction(ClayMeasureStub, NULL);
    s_active_ask_state = ui;
    host_ui->requests = host_ui->active = ui;
    char error[192];
    int failed = LoadUiRequest(request, error, sizeof(error)) != 1;
    Clay_RenderCommandArray commands = PanelLayout(app, host_ui);
    Clay_BoundingBox body = Clay_GetElementData(CLAY_ID("AskUserBody")).boundingBox;
    Clay_BoundingBox first = Clay_GetElementData(CLAY_IDI("AskUserOption", 0)).boundingBox;
    Clay_BoundingBox second = Clay_GetElementData(CLAY_IDI("AskUserOption", 1)).boundingBox;
    int prompt_lines = 0, option_lines = 0;
    for (int i = 0; i < commands.length; i++)
    {
        Clay_RenderCommand *cmd = Clay_RenderCommandArray_Get(&commands, i);
        if (cmd->commandType != CLAY_RENDER_COMMAND_TYPE_TEXT) continue;
        Clay_StringSlice label = cmd->renderData.text.stringContents;
        const char *prompt = ui->questions[0].prompt;
        const char *choice = ui->questions[0].options[0];
        bool is_prompt = (uintptr_t)label.chars >= (uintptr_t)prompt &&
                         (uintptr_t)label.chars < (uintptr_t)(prompt + strlen(prompt));
        bool is_choice = (uintptr_t)label.chars >= (uintptr_t)choice &&
                         (uintptr_t)label.chars < (uintptr_t)(choice + strlen(choice));
        if (is_prompt) prompt_lines++;
        if (is_choice) option_lines++;
        if ((is_prompt || is_choice) &&
            cmd->boundingBox.x + cmd->boundingBox.width > body.x + body.width + .01f) failed = 1;
    }
    failed |= prompt_lines < 2 || option_lines < 2 || first.y + first.height > second.y;
    if (failed)
        fprintf(stderr, "wrapped question: prompt=%d option=%d body width=%.1f first=%.1f second=%.1f\n",
                prompt_lines, option_lines, body.width, first.height, second.y - first.y);
    AskUserHostShutdown(app, host_ui);
    test_glyph_width = 1.0f;
    free(app);
    Clay_SetCurrentContext(NULL);
    free(memory);
    return failed;
}

static int ExpectRequest(const char *name, const char *args, const char *expected)
{
    char error[256] = {0};
    char *request = PicoAskUser_BuildRequest(args, error, sizeof(error));
    if (!request || strcmp(request, expected) != 0)
    {
        fprintf(stderr, "%s: expected %s, got %s (%s)\n", name, expected,
                request ? request : "NULL", error);
        free(request);
        return 1;
    }
    free(request);
    return 0;
}

static int ExpectError(const char *name, const char *args, const char *message)
{
    char error[256] = {0};
    char *request = PicoAskUser_BuildRequest(args, error, sizeof(error));
    if (request || !strstr(error, message))
    {
        fprintf(stderr, "%s: expected error containing '%s', got %s (%s)\n", name, message,
                request ? request : "NULL", error);
        free(request);
        return 1;
    }
    return 0;
}

static char *BuildQuestionList(int count)
{
    JsonBuf b;
    JsonBuf_Init(&b);
    JsonBuf_Puts(&b, "{\"questions\":[");
    for (int i = 0; i < count; i++)
    {
        if (i > 0)
        {
            JsonBuf_Putc(&b, ',');
        }
        JsonBuf_Puts(&b, "{\"id\":");
        char id[24];
        snprintf(id, sizeof(id), "q%d", i);
        JsonBuf_String(&b, id);
        JsonBuf_Puts(&b, ",\"question\":\"Question?\",\"kind\":\"text\"}");
    }
    JsonBuf_Puts(&b, "]}");
    return JsonBuf_Steal(&b);
}

int main(void)
{
    int failed = TestTextInput();
    failed |= TestHoverCursor();
    failed |= TestQuestionPanel();
    failed |= TestLongQuestionScroll();
    failed |= TestWrappedQuestionPanel();
    failed |= ExpectRequest(
        "mixed questionnaire",
        "{\"questions\":[{\"id\":\"target\",\"question\":\"Which?\",\"kind\":\"select\","
        "\"options\":[\"CLI\",\"GUI\"]},{\"id\":\"notes\","
        "\"question\":\"Notes?\",\"kind\":\"text\",\"options\":\"ignored\"}]}",
        "{\"type\":\"questionnaire\",\"ui\":\"custom\",\"questions\":[{\"id\":\"target\","
        "\"question\":\"Which?\",\"kind\":\"select\",\"options\":[\"CLI\",\"GUI\"]},"
        "{\"id\":\"notes\",\"question\":\"Notes?\",\"kind\":\"text\"}]}");

    failed |= ExpectError(
        "duplicate ids",
        "{\"questions\":[{\"id\":\"same\",\"question\":\"One?\",\"kind\":\"text\"},"
        "{\"id\":\"same\",\"question\":\"Two?\",\"kind\":\"text\"}]}",
        "duplicated");
    failed |= ExpectError(
        "select requires options",
        "{\"questions\":[{\"id\":\"choice\",\"question\":\"Choose?\",\"kind\":\"select\"}]}",
        "options");
    char error[256] = {0};
    char *empty = PicoAskUser_BuildRequest("{\"questions\":[]}", error, sizeof(error));
    int maximum = 0;
    if (empty || sscanf(error, "questions must contain between 1 and %d items", &maximum) != 1 ||
        maximum < 1)
    {
        fprintf(stderr, "empty questionnaire must report its accepted count range\n");
        free(empty);
        return 1;
    }

    char *at_limit = BuildQuestionList(maximum);
    char *too_many = BuildQuestionList(maximum + 1);
    if (!at_limit || !too_many)
    {
        fprintf(stderr, "question limit: allocation failed\n");
        failed = 1;
    }
    else
    {
        char *accepted = PicoAskUser_BuildRequest(at_limit, error, sizeof(error));
        if (!accepted)
        {
            fprintf(stderr, "question count at advertised limit must be accepted: %s\n", error);
            failed = 1;
        }
        free(accepted);
        failed |= ExpectError("question limit", too_many, "questions must contain between");
    }
    free(at_limit);
    free(too_many);
    return failed;
}
