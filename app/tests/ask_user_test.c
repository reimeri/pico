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
int GetCharPressed(void) { return 0; }
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
Vector2 MeasureTextEx(Font font, const char *text, float size, float spacing)
{
    (void)font;
    (void)spacing;
    return (Vector2){(float)strlen(text), size};
}
bool pico_tool_answer(PicoHost *host, uint64_t id, const char *answer)
{
    (void)host;
    (void)id;
    (void)answer;
    return false;
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
