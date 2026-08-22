// Pico extension: structured, multi-step clarifying questions.
// The ask_user tool accepts all questions in one call and presents a custom
// modal with required single-select and free-form answers.

#include "pico/plugin.h"
#include "pico/theme.h"
#include "builtins/ask_user.h"
#include "json.h"

#include "clay/clay.h"

#include <ctype.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define ASK_USER_MAX_QUESTIONS 24
#define ASK_USER_MAX_OPTIONS 20
#define ASK_USER_MAX_TEXT 16384

static const char *kAskUserParams =
    "{\"type\":\"object\",\"additionalProperties\":false,\"properties\":{"
    "\"questions\":{\"type\":\"array\",\"minItems\":1,\"maxItems\":24,"
    "\"description\":\"All clarifying questions to ask in this questionnaire.\","
    "\"items\":{\"type\":\"object\",\"additionalProperties\":false,\"properties\":{"
    "\"id\":{\"type\":\"string\",\"description\":\"Short unique identifier used to associate the answer.\"},"
    "\"question\":{\"type\":\"string\",\"description\":\"The question shown to the user.\"},"
    "\"kind\":{\"type\":\"string\",\"enum\":[\"select\",\"text\"],"
    "\"description\":\"select requires options; text accepts a free-form answer.\"},"
    "\"options\":{\"type\":\"array\",\"minItems\":1,\"maxItems\":20,"
    "\"items\":{\"type\":\"string\"},"
    "\"description\":\"Required for select questions and ignored for text questions.\"},"
    "\"allow_other\":{\"type\":\"boolean\","
    "\"description\":\"For select questions, add an Other option that requires a free-form answer; ignored for text questions.\"}"
    "},\"required\":[\"id\",\"question\",\"kind\"]}}},"
    "\"required\":[\"questions\"]}";

typedef enum AskQuestionKind {
    ASK_QUESTION_SELECT = 0,
    ASK_QUESTION_TEXT,
} AskQuestionKind;

typedef struct AskQuestion {
    char *id;
    char *prompt;
    AskQuestionKind kind;
    char **options;
    int option_count;
    bool allow_other;
    int selected;
    int focus;
    char *text;
    int text_len;
    int text_cap;
    int cursor;
} AskQuestion;

typedef struct AskUiState {
    uint64_t id;
    uint64_t answered_id;
    AskQuestion *questions;
    int question_count;
    int current;
    bool show;
    char validation[160];
    char progress[64];
    char *caret_text;
} AskUiState;

static AskUiState g_ui;

static Clay_String CStr(const char *s)
{
    if (!s)
    {
        s = "";
    }
    return (Clay_String){.length = (int32_t)strlen(s), .chars = s};
}

static bool HasNonSpace(const char *s)
{
    if (!s)
    {
        return false;
    }
    while (*s)
    {
        if (!isspace((unsigned char)*s))
        {
            return true;
        }
        s++;
    }
    return false;
}

static void FreeQuestion(AskQuestion *q)
{
    if (!q)
    {
        return;
    }
    free(q->id);
    free(q->prompt);
    for (int i = 0; i < q->option_count; i++)
    {
        free(q->options[i]);
    }
    free(q->options);
    free(q->text);
    memset(q, 0, sizeof(*q));
}

static void ClearQuestions(void)
{
    for (int i = 0; i < g_ui.question_count; i++)
    {
        FreeQuestion(&g_ui.questions[i]);
    }
    free(g_ui.questions);
    g_ui.questions = NULL;
    g_ui.question_count = 0;
    g_ui.current = 0;
    g_ui.id = 0;
    g_ui.show = false;
    g_ui.validation[0] = '\0';
    free(g_ui.caret_text);
    g_ui.caret_text = NULL;
}

static void SetToolError(PicoToolResult *out, const char *message)
{
    if (!out)
    {
        return;
    }
    JsonBuf b;
    JsonBuf_Init(&b);
    JsonBuf_Puts(&b, "{\"error\":");
    JsonBuf_String(&b, message ? message : "ask_user failed");
    JsonBuf_Putc(&b, '}');
    out->output = JsonBuf_Steal(&b);
    if (!out->output)
    {
        out->output = JsonDup("{\"error\":\"ask_user failed\"}");
    }
    out->is_error = true;
}

static bool SeenId(char **ids, int count, const char *id)
{
    for (int i = 0; i < count; i++)
    {
        if (strcmp(ids[i], id) == 0)
        {
            return true;
        }
    }
    return false;
}

/* Validate the agent-facing arguments and produce the private custom-UI ask. */
char *PicoAskUser_BuildRequest(const char *args_json, char *error, size_t error_cap)
{
    JsonDoc doc;
    const char *src = args_json ? args_json : "";
    if (JsonParse(&doc, src, strlen(src)) != 0)
    {
        snprintf(error, error_cap, "arguments must be valid JSON");
        return NULL;
    }
    if (!JsonIsObject(&doc, 0))
    {
        snprintf(error, error_cap, "arguments must be a JSON object");
        JsonFree(&doc);
        return NULL;
    }

    int questions_tok = JsonObjGet(&doc, 0, "questions");
    int count = JsonArrayLen(&doc, questions_tok);
    if (!JsonIsArray(&doc, questions_tok) || count < 1 || count > ASK_USER_MAX_QUESTIONS)
    {
        snprintf(error, error_cap, "questions must contain between 1 and %d items", ASK_USER_MAX_QUESTIONS);
        JsonFree(&doc);
        return NULL;
    }

    char **ids = (char **)calloc((size_t)count, sizeof(char *));
    if (!ids)
    {
        snprintf(error, error_cap, "out of memory");
        JsonFree(&doc);
        return NULL;
    }

    JsonBuf request;
    JsonBuf_Init(&request);
    JsonBuf_Puts(&request, "{\"type\":\"questionnaire\",\"ui\":\"custom\",\"questions\":[");
    bool ok = true;

    for (int i = 0; i < count && ok; i++)
    {
        int qtok = JsonArrayAt(&doc, questions_tok, i);
        if (!JsonIsObject(&doc, qtok))
        {
            snprintf(error, error_cap, "question %d must be an object", i + 1);
            ok = false;
            break;
        }

        char *id = JsonObjStr(&doc, qtok, "id");
        char *prompt = JsonObjStr(&doc, qtok, "question");
        char *kind = JsonObjStr(&doc, qtok, "kind");
        if (!HasNonSpace(id) || strlen(id ? id : "") > 128)
        {
            snprintf(error, error_cap, "question %d needs a non-empty id of at most 128 characters", i + 1);
            ok = false;
        }
        else if (SeenId(ids, i, id))
        {
            snprintf(error, error_cap, "question id '%s' is duplicated", id);
            ok = false;
        }
        else if (!HasNonSpace(prompt))
        {
            snprintf(error, error_cap, "question %d needs non-empty question text", i + 1);
            ok = false;
        }
        else if (!kind || (strcmp(kind, "select") != 0 && strcmp(kind, "text") != 0))
        {
            snprintf(error, error_cap, "question %d kind must be 'select' or 'text'", i + 1);
            ok = false;
        }

        bool is_select = kind && strcmp(kind, "select") == 0;
        int options_tok = JsonObjGet(&doc, qtok, "options");
        int option_count = is_select ? JsonArrayLen(&doc, options_tok) : 0;
        int allow_other_tok = JsonObjGet(&doc, qtok, "allow_other");
        bool allow_other = is_select && JsonEq(&doc, allow_other_tok, "true");
        if (ok && is_select && allow_other_tok >= 0 && !allow_other &&
            !JsonEq(&doc, allow_other_tok, "false"))
        {
            snprintf(error, error_cap, "question %d allow_other must be a boolean", i + 1);
            ok = false;
        }
        if (ok && is_select &&
            (!JsonIsArray(&doc, options_tok) || option_count < 1 || option_count > ASK_USER_MAX_OPTIONS))
        {
            snprintf(error, error_cap, "select question %d needs between 1 and %d options", i + 1,
                     ASK_USER_MAX_OPTIONS);
            ok = false;
        }

        char **options = NULL;
        if (ok && option_count > 0)
        {
            options = (char **)calloc((size_t)option_count, sizeof(char *));
            if (!options)
            {
                snprintf(error, error_cap, "out of memory");
                ok = false;
            }
        }
        for (int j = 0; j < option_count && ok; j++)
        {
            options[j] = JsonStrDup(&doc, JsonArrayAt(&doc, options_tok, j));
            if (!HasNonSpace(options[j]))
            {
                snprintf(error, error_cap, "option %d of question %d must be non-empty", j + 1, i + 1);
                ok = false;
            }
        }

        if (ok)
        {
            ids[i] = id;
            id = NULL;
            if (i > 0)
            {
                JsonBuf_Putc(&request, ',');
            }
            JsonBuf_Puts(&request, "{\"id\":");
            JsonBuf_String(&request, ids[i]);
            JsonBuf_Puts(&request, ",\"question\":");
            JsonBuf_String(&request, prompt);
            JsonBuf_Puts(&request, ",\"kind\":");
            JsonBuf_String(&request, kind);
            if (option_count > 0)
            {
                JsonBuf_Puts(&request, ",\"options\":[");
                for (int j = 0; j < option_count; j++)
                {
                    if (j > 0)
                    {
                        JsonBuf_Putc(&request, ',');
                    }
                    JsonBuf_String(&request, options[j]);
                }
                JsonBuf_Putc(&request, ']');
            }
            if (allow_other)
            {
                JsonBuf_Puts(&request, ",\"allow_other\":true");
            }
            JsonBuf_Putc(&request, '}');
        }

        free(id);
        free(prompt);
        free(kind);
        for (int j = 0; j < option_count; j++)
        {
            free(options ? options[j] : NULL);
        }
        free(options);
    }

    JsonBuf_Puts(&request, "]}");
    for (int i = 0; i < count; i++)
    {
        free(ids[i]);
    }
    free(ids);
    JsonFree(&doc);

    if (!ok)
    {
        JsonBuf_Free(&request);
        return NULL;
    }

    char *result = JsonBuf_Steal(&request);
    if (!result)
    {
        snprintf(error, error_cap, "out of memory");
        return NULL;
    }
    if (strlen(result) > PICO_TOOL_ASK_MAX_REQUEST)
    {
        free(result);
        snprintf(error, error_cap, "questionnaire is too large");
        return NULL;
    }
    return result;
}

static void AskUserRun(PicoAgentContext *ctx, const char *args_json, PicoToolResult *out)
{
    if (!out)
    {
        return;
    }
    memset(out, 0, sizeof(*out));

    char error[256];
    char *request = PicoAskUser_BuildRequest(args_json, error, sizeof(error));
    if (!request)
    {
        SetToolError(out, error);
        return;
    }

    char *answer = NULL;
    int rc = pico_tool_ask(ctx, request, &answer);
    free(request);
    if (rc != PICO_ASK_OK)
    {
        free(answer);
        SetToolError(out, rc == PICO_ASK_CANCEL ? "questionnaire cancelled" : "could not open questionnaire");
        return;
    }

    out->output = answer ? answer : JsonDup("{\"answers\":[]}");
    if (!out->output)
    {
        SetToolError(out, "out of memory");
    }
}

static bool ParseUiQuestion(const JsonDoc *doc, int qtok, AskQuestion *q, AskQuestion *prior, int prior_count,
                            char *error, size_t error_cap)
{
    if (!JsonIsObject(doc, qtok))
    {
        snprintf(error, error_cap, "question is not an object");
        return false;
    }
    q->id = JsonObjStr(doc, qtok, "id");
    q->prompt = JsonObjStr(doc, qtok, "question");
    char *kind = JsonObjStr(doc, qtok, "kind");
    if (!HasNonSpace(q->id) || !HasNonSpace(q->prompt) || !kind)
    {
        snprintf(error, error_cap, "question fields are missing");
        free(kind);
        return false;
    }
    for (int i = 0; i < prior_count; i++)
    {
        if (strcmp(prior[i].id, q->id) == 0)
        {
            snprintf(error, error_cap, "question ids must be unique");
            free(kind);
            return false;
        }
    }

    if (strcmp(kind, "select") == 0)
    {
        q->kind = ASK_QUESTION_SELECT;
        q->allow_other = JsonEq(doc, JsonObjGet(doc, qtok, "allow_other"), "true");
        int options_tok = JsonObjGet(doc, qtok, "options");
        q->option_count = JsonArrayLen(doc, options_tok);
        if (!JsonIsArray(doc, options_tok) || q->option_count < 1 || q->option_count > ASK_USER_MAX_OPTIONS)
        {
            snprintf(error, error_cap, "select question has invalid options");
            free(kind);
            return false;
        }
        q->options = (char **)calloc((size_t)q->option_count, sizeof(char *));
        if (!q->options)
        {
            snprintf(error, error_cap, "out of memory");
            free(kind);
            return false;
        }
        for (int i = 0; i < q->option_count; i++)
        {
            q->options[i] = JsonStrDup(doc, JsonArrayAt(doc, options_tok, i));
            if (!HasNonSpace(q->options[i]))
            {
                snprintf(error, error_cap, "select question has an empty option");
                free(kind);
                return false;
            }
        }
        q->selected = -1;
        q->focus = 0;
        if (q->allow_other)
        {
            q->text = JsonDup("");
            if (!q->text)
            {
                snprintf(error, error_cap, "out of memory");
                free(kind);
                return false;
            }
            q->text_cap = 1;
        }
    }
    else if (strcmp(kind, "text") == 0)
    {
        q->kind = ASK_QUESTION_TEXT;
        q->text = JsonDup("");
        if (!q->text)
        {
            snprintf(error, error_cap, "out of memory");
            free(kind);
            return false;
        }
        q->text_cap = 1;
    }
    else
    {
        snprintf(error, error_cap, "question has an invalid kind");
        free(kind);
        return false;
    }
    free(kind);
    return true;
}

/* 1 = this extension's valid ask, 0 = another ask type, -1 = ours but invalid. */
static int LoadUiRequest(const char *request_json, char *error, size_t error_cap)
{
    JsonDoc doc;
    if (JsonParse(&doc, request_json, strlen(request_json)) != 0 || !JsonIsObject(&doc, 0))
    {
        return 0;
    }
    bool ours = JsonEq(&doc, JsonObjGet(&doc, 0, "type"), "questionnaire") &&
                JsonEq(&doc, JsonObjGet(&doc, 0, "ui"), "custom");
    if (!ours)
    {
        JsonFree(&doc);
        return 0;
    }

    int questions_tok = JsonObjGet(&doc, 0, "questions");
    int count = JsonArrayLen(&doc, questions_tok);
    if (!JsonIsArray(&doc, questions_tok) || count < 1 || count > ASK_USER_MAX_QUESTIONS)
    {
        snprintf(error, error_cap, "questionnaire has an invalid question count");
        JsonFree(&doc);
        return -1;
    }

    AskQuestion *questions = (AskQuestion *)calloc((size_t)count, sizeof(AskQuestion));
    if (!questions)
    {
        snprintf(error, error_cap, "out of memory");
        JsonFree(&doc);
        return -1;
    }

    bool ok = true;
    int loaded = 0;
    for (int i = 0; i < count; i++)
    {
        if (!ParseUiQuestion(&doc, JsonArrayAt(&doc, questions_tok, i), &questions[i], questions, i, error,
                             error_cap))
        {
            ok = false;
            break;
        }
        loaded++;
    }
    JsonFree(&doc);

    if (!ok)
    {
        /* Include the partially initialized failing item in cleanup. */
        int cleanup_count = loaded < count ? loaded + 1 : loaded;
        for (int i = 0; i < cleanup_count; i++)
        {
            FreeQuestion(&questions[i]);
        }
        free(questions);
        return -1;
    }

    g_ui.questions = questions;
    g_ui.question_count = count;
    g_ui.current = 0;
    g_ui.show = true;
    return 1;
}

static void AnswerUiError(PicoApp *app, uint64_t id, const char *message)
{
    JsonBuf b;
    JsonBuf_Init(&b);
    JsonBuf_Puts(&b, "{\"error\":");
    JsonBuf_String(&b, message);
    JsonBuf_Putc(&b, '}');
    char *answer = JsonBuf_Steal(&b);
    if (!answer)
    {
        answer = JsonDup("{\"error\":\"invalid questionnaire payload\"}");
    }
    if (answer && pico_tool_answer(app, id, answer))
    {
        g_ui.answered_id = id;
    }
    free(answer);
}

static void SyncPendingAsk(PicoApp *app)
{
    PicoToolAsk ask;
    if (!pico_tool_pending_ask(app, &ask) || !ask.request_json)
    {
        ClearQuestions();
        g_ui.answered_id = 0;
        return;
    }
    if ((g_ui.show && g_ui.id == ask.id) || g_ui.answered_id == ask.id)
    {
        return;
    }

    ClearQuestions();
    char error[192] = "invalid questionnaire payload";
    int rc = LoadUiRequest(ask.request_json, error, sizeof(error));
    if (rc > 0)
    {
        g_ui.id = ask.id;
    }
    else if (rc < 0)
    {
        AnswerUiError(app, ask.id, error);
    }
}

static bool QuestionAnswered(const AskQuestion *q)
{
    if (q->kind == ASK_QUESTION_TEXT)
    {
        return HasNonSpace(q->text);
    }
    if (q->selected < 0)
    {
        return false;
    }
    return q->allow_other && q->selected == q->option_count ? HasNonSpace(q->text) : true;
}

static void SetValidation(const char *message)
{
    snprintf(g_ui.validation, sizeof(g_ui.validation), "%s", message ? message : "");
}

static int Utf8Prev(const char *s, int pos)
{
    if (pos <= 0)
    {
        return 0;
    }
    pos--;
    while (pos > 0 && ((unsigned char)s[pos] & 0xC0) == 0x80)
    {
        pos--;
    }
    return pos;
}

static int Utf8Next(const char *s, int len, int pos)
{
    if (pos >= len)
    {
        return len;
    }
    pos++;
    while (pos < len && ((unsigned char)s[pos] & 0xC0) == 0x80)
    {
        pos++;
    }
    return pos;
}

static int Utf8Encode(int cp, char out[4])
{
    if (cp <= 0x7F)
    {
        out[0] = (char)cp;
        return 1;
    }
    if (cp <= 0x7FF)
    {
        out[0] = (char)(0xC0 | (cp >> 6));
        out[1] = (char)(0x80 | (cp & 0x3F));
        return 2;
    }
    if (cp <= 0xFFFF)
    {
        out[0] = (char)(0xE0 | (cp >> 12));
        out[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
        out[2] = (char)(0x80 | (cp & 0x3F));
        return 3;
    }
    out[0] = (char)(0xF0 | (cp >> 18));
    out[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
    out[2] = (char)(0x80 | ((cp >> 6) & 0x3F));
    out[3] = (char)(0x80 | (cp & 0x3F));
    return 4;
}

static bool EnsureTextCapacity(AskQuestion *q, int needed)
{
    if (needed + 1 <= q->text_cap)
    {
        return true;
    }
    int cap = q->text_cap > 0 ? q->text_cap : 32;
    while (cap < needed + 1)
    {
        cap *= 2;
    }
    char *next = (char *)realloc(q->text, (size_t)cap);
    if (!next)
    {
        return false;
    }
    q->text = next;
    q->text_cap = cap;
    return true;
}

static void AskTextInsert(AskQuestion *q, const char *s, int n)
{
    if (!q || !s || n <= 0 || q->text_len >= ASK_USER_MAX_TEXT)
    {
        return;
    }
    if (n > ASK_USER_MAX_TEXT - q->text_len)
    {
        n = ASK_USER_MAX_TEXT - q->text_len;
        while (n > 0 && ((unsigned char)s[n] & 0xC0) == 0x80)
        {
            n--;
        }
    }
    if (n <= 0 || !EnsureTextCapacity(q, q->text_len + n))
    {
        return;
    }
    memmove(q->text + q->cursor + n, q->text + q->cursor, (size_t)(q->text_len - q->cursor + 1));
    memcpy(q->text + q->cursor, s, (size_t)n);
    q->cursor += n;
    q->text_len += n;
    g_ui.validation[0] = '\0';
}

static void AskTextDeleteRange(AskQuestion *q, int from, int to)
{
    if (!q || from < 0 || to < from || to > q->text_len || from == to)
    {
        return;
    }
    memmove(q->text + from, q->text + to, (size_t)(q->text_len - to + 1));
    q->text_len -= to - from;
    q->cursor = from;
}

static char *BuildAnswer(void)
{
    JsonBuf b;
    JsonBuf_Init(&b);
    JsonBuf_Puts(&b, "{\"answers\":[");
    for (int i = 0; i < g_ui.question_count; i++)
    {
        AskQuestion *q = &g_ui.questions[i];
        if (i > 0)
        {
            JsonBuf_Putc(&b, ',');
        }
        JsonBuf_Puts(&b, "{\"id\":");
        JsonBuf_String(&b, q->id);
        JsonBuf_Puts(&b, ",\"answer\":");
        const char *value = q->text;
        if (q->kind == ASK_QUESTION_SELECT && q->selected < q->option_count)
        {
            value = q->options[q->selected];
        }
        JsonBuf_String(&b, value);
        JsonBuf_Putc(&b, '}');
    }
    JsonBuf_Puts(&b, "]}");
    return JsonBuf_Steal(&b);
}

static void SubmitAnswers(PicoApp *app)
{
    for (int i = 0; i < g_ui.question_count; i++)
    {
        if (!QuestionAnswered(&g_ui.questions[i]))
        {
            g_ui.current = i;
            SetValidation("An answer is required before continuing.");
            return;
        }
    }

    char *answer = BuildAnswer();
    if (!answer)
    {
        SetValidation("Could not build the answer. Please try again.");
        return;
    }
    if (strlen(answer) > PICO_TOOL_ASK_MAX_ANSWER)
    {
        free(answer);
        SetValidation("The answers are too long. Shorten one or more text answers.");
        return;
    }
    uint64_t id = g_ui.id;
    if (pico_tool_answer(app, id, answer))
    {
        free(answer);
        ClearQuestions();
        g_ui.answered_id = id;
        return;
    }
    free(answer);
    SetValidation("The questionnaire is no longer active.");
}

static void GoBack(void)
{
    if (g_ui.current > 0)
    {
        g_ui.current--;
        g_ui.validation[0] = '\0';
    }
}

static void GoForward(PicoApp *app)
{
    AskQuestion *q = &g_ui.questions[g_ui.current];
    if (!QuestionAnswered(q))
    {
        SetValidation("An answer is required before continuing.");
        return;
    }
    g_ui.validation[0] = '\0';
    if (g_ui.current + 1 < g_ui.question_count)
    {
        g_ui.current++;
    }
    else
    {
        SubmitAnswers(app);
    }
}

static bool CtrlDown(void)
{
    return IsKeyDown(KEY_LEFT_CONTROL) || IsKeyDown(KEY_RIGHT_CONTROL);
}

static bool ShiftDown(void)
{
    return IsKeyDown(KEY_LEFT_SHIFT) || IsKeyDown(KEY_RIGHT_SHIFT);
}

static void HandleTextKeys(PicoApp *app, AskQuestion *q);

static void HandleSelectKeys(PicoApp *app, AskQuestion *q)
{
    bool up = IsKeyPressed(KEY_UP) || IsKeyPressedRepeat(KEY_UP);
    bool down = IsKeyPressed(KEY_DOWN) || IsKeyPressedRepeat(KEY_DOWN);
    int choice_count = q->option_count + (q->allow_other ? 1 : 0);
    if (up)
    {
        q->focus = q->selected < 0 ? 0 : q->selected;
        if (q->selected >= 0 && q->focus > 0)
        {
            q->focus--;
        }
        q->selected = q->focus;
        g_ui.validation[0] = '\0';
    }
    if (down)
    {
        q->focus = q->selected < 0 ? 0 : q->selected;
        if (q->selected >= 0 && q->focus + 1 < choice_count)
        {
            q->focus++;
        }
        q->selected = q->focus;
        g_ui.validation[0] = '\0';
    }
    if (IsKeyPressed(KEY_SPACE) && q->selected != q->option_count)
    {
        q->selected = q->focus;
        g_ui.validation[0] = '\0';
    }
    if (q->allow_other && q->selected == q->option_count)
    {
        HandleTextKeys(app, q);
        return;
    }
    if (IsKeyPressed(KEY_LEFT))
    {
        GoBack();
        return;
    }
    if (IsKeyPressed(KEY_ENTER) || IsKeyPressed(KEY_KP_ENTER))
    {
        if (q->selected < 0)
        {
            q->selected = q->focus;
        }
        GoForward(app);
    }
}

static void PasteText(AskQuestion *q)
{
    const char *clip = GetClipboardText();
    if (clip && clip[0])
    {
        AskTextInsert(q, clip, (int)strlen(clip));
    }
}

static void HandleTextKeys(PicoApp *app, AskQuestion *q)
{
    bool ctrl = CtrlDown();
    bool shift = ShiftDown();
    bool left = IsKeyPressed(KEY_LEFT) || IsKeyPressedRepeat(KEY_LEFT);
    bool right = IsKeyPressed(KEY_RIGHT) || IsKeyPressedRepeat(KEY_RIGHT);
    bool backspace = IsKeyPressed(KEY_BACKSPACE) || IsKeyPressedRepeat(KEY_BACKSPACE);
    bool del = IsKeyPressed(KEY_DELETE) || IsKeyPressedRepeat(KEY_DELETE);

    if (ctrl && Pico_ShortcutPressed('v'))
    {
        PasteText(q);
    }
    if (IsKeyPressed(KEY_HOME))
    {
        q->cursor = 0;
    }
    if (IsKeyPressed(KEY_END))
    {
        q->cursor = q->text_len;
    }
    if (left)
    {
        if (q->cursor > 0)
        {
            q->cursor = Utf8Prev(q->text, q->cursor);
        }
        else if (IsKeyPressed(KEY_LEFT))
        {
            GoBack();
            return;
        }
    }
    if (right)
    {
        q->cursor = Utf8Next(q->text, q->text_len, q->cursor);
    }
    if (backspace && q->cursor > 0)
    {
        AskTextDeleteRange(q, Utf8Prev(q->text, q->cursor), q->cursor);
    }
    if (del && q->cursor < q->text_len)
    {
        AskTextDeleteRange(q, q->cursor, Utf8Next(q->text, q->text_len, q->cursor));
    }
    if (IsKeyPressed(KEY_ENTER) || IsKeyPressed(KEY_KP_ENTER))
    {
        if (shift)
        {
            AskTextInsert(q, "\n", 1);
        }
        else
        {
            GoForward(app);
            return;
        }
    }

    if (!ctrl)
    {
        int cp;
        while ((cp = GetCharPressed()) != 0)
        {
            if (cp < 32)
            {
                continue;
            }
            char bytes[4];
            int n = Utf8Encode(cp, bytes);
            AskTextInsert(q, bytes, n);
        }
    }
}

static void AskUserOnFrame(PicoApp *app, float dt)
{
    (void)dt;
    SyncPendingAsk(app);
    if (!g_ui.show || g_ui.current < 0 || g_ui.current >= g_ui.question_count)
    {
        return;
    }
    /* Esc is deliberately left to Pico, which cancels the ask and turn. */
    if (IsKeyPressed(KEY_ESCAPE))
    {
        return;
    }

    bool tab = IsKeyPressed(KEY_TAB);
    if (tab)
    {
        if (ShiftDown())
        {
            GoBack();
        }
        else
        {
            GoForward(app);
        }
        return;
    }

    AskQuestion *q = &g_ui.questions[g_ui.current];
    if (q->kind == ASK_QUESTION_SELECT)
    {
        HandleSelectKeys(app, q);
    }
    else
    {
        HandleTextKeys(app, q);
    }
}

static void RenderButton(Clay_String id, const char *label, bool enabled, bool primary)
{
    Clay_ElementId eid = CLAY_SID(id);
    bool hover = enabled && Clay_PointerOver(eid);
    Clay_Color bg = primary ? (Clay_Color){74, 104, 180, 255} : COLOR_FOOTER_BG;
    if (!enabled)
    {
        bg = (Clay_Color){38, 38, 44, 255};
    }
    else if (hover)
    {
        bg = primary ? (Clay_Color){92, 126, 210, 255} : COLOR_CODE_BG;
    }
    Clay_Color text = enabled ? COLOR_TEXT : COLOR_MUTED;
    CLAY(eid, {.layout = {.padding = {14, 14, 9, 9}}, .backgroundColor = bg,
               .cornerRadius = CLAY_CORNER_RADIUS(6)})
    {
        CLAY_TEXT(CStr(label), CLAY_TEXT_CONFIG({.fontId = FONT_BOLD, .fontSize = 14, .textColor = text}));
    }
}

static void RenderTextQuestion(const AskQuestion *q);

static void RenderSelectQuestion(const AskQuestion *q)
{
    int choice_count = q->option_count + (q->allow_other ? 1 : 0);
    for (int i = 0; i < choice_count; i++)
    {
        Clay_ElementId eid = CLAY_IDI("AskUserOption", i);
        bool hover = Clay_PointerOver(eid);
        bool selected = q->selected == i;
        bool focused = q->focus == i;
        const char *label = i < q->option_count ? q->options[i] : "Other…";
        Clay_Color bg = selected ? (Clay_Color){62, 78, 124, 255}
                                 : (hover || focused ? COLOR_CODE_BG : COLOR_FOOTER_BG);
        CLAY(eid,
             {.layout = {.layoutDirection = CLAY_LEFT_TO_RIGHT,
                         .childGap = 10,
                         .childAlignment = {.y = CLAY_ALIGN_Y_CENTER},
                         .padding = {12, 12, 10, 10},
                         .sizing = {.width = CLAY_SIZING_GROW(0)}},
              .backgroundColor = bg,
              .cornerRadius = CLAY_CORNER_RADIUS(6)})
        {
            CLAY_TEXT(selected ? CLAY_STRING("●") : CLAY_STRING("○"),
                      CLAY_TEXT_CONFIG({.fontId = FONT_REGULAR,
                                        .fontSize = 16,
                                        .textColor = selected ? COLOR_LINK : COLOR_MUTED}));
            CLAY_TEXT(CStr(label), CLAY_TEXT_CONFIG({.fontId = FONT_REGULAR,
                                                     .fontSize = 14,
                                                     .textColor = COLOR_TEXT,
                                                     .wrapMode = CLAY_TEXT_WRAP_WORDS}));
        }
    }
    if (q->allow_other && q->selected == q->option_count)
    {
        RenderTextQuestion(q);
    }
    else
    {
        CLAY_TEXT(CLAY_STRING("↑/↓ select  •  Space choose  •  Enter next"),
                  CLAY_TEXT_CONFIG({.fontId = FONT_REGULAR, .fontSize = 12, .textColor = COLOR_MUTED}));
    }
}

static void BuildCaretText(const AskQuestion *q)
{
    free(g_ui.caret_text);
    g_ui.caret_text = (char *)malloc((size_t)q->text_len + 2);
    if (!g_ui.caret_text)
    {
        return;
    }
    memcpy(g_ui.caret_text, q->text, (size_t)q->cursor);
    g_ui.caret_text[q->cursor] = '|';
    memcpy(g_ui.caret_text + q->cursor + 1, q->text + q->cursor, (size_t)(q->text_len - q->cursor));
    g_ui.caret_text[q->text_len + 1] = '\0';
}

static void RenderTextQuestion(const AskQuestion *q)
{
    BuildCaretText(q);
    CLAY(CLAY_ID("AskUserTextBox"),
         {.layout = {.layoutDirection = CLAY_TOP_TO_BOTTOM,
                     .padding = {12, 12, 10, 10},
                     .sizing = {.width = CLAY_SIZING_GROW(0), .height = CLAY_SIZING_FIT(120, 260)}},
          .backgroundColor = COLOR_COMPOSER_BG,
          .cornerRadius = CLAY_CORNER_RADIUS(6)})
    {
        CLAY_TEXT(CStr(g_ui.caret_text ? g_ui.caret_text : q->text),
                  CLAY_TEXT_CONFIG({.fontId = FONT_REGULAR,
                                    .fontSize = 15,
                                    .textColor = COLOR_TEXT,
                                    .wrapMode = CLAY_TEXT_WRAP_WORDS}));
    }
    if (q->text_len == 0)
    {
        CLAY_TEXT(CLAY_STRING("Type your answer…"),
                  CLAY_TEXT_CONFIG({.fontId = FONT_REGULAR, .fontSize = 12, .textColor = COLOR_MUTED}));
    }
    CLAY_TEXT(CLAY_STRING("Enter next  •  Shift+Enter newline  •  Shift+Tab back"),
              CLAY_TEXT_CONFIG({.fontId = FONT_REGULAR, .fontSize = 12, .textColor = COLOR_MUTED}));
}

static void AskUserRender(PicoApp *app)
{
    (void)app;
    if (!g_ui.show || g_ui.current < 0 || g_ui.current >= g_ui.question_count)
    {
        return;
    }

    AskQuestion *q = &g_ui.questions[g_ui.current];
    snprintf(g_ui.progress, sizeof(g_ui.progress), "Question %d of %d", g_ui.current + 1, g_ui.question_count);

    float sw = (float)GetScreenWidth();
    float sh = (float)GetScreenHeight();
    float card_w = sw < 760.0f ? sw - 40.0f : 680.0f;
    if (card_w < 280.0f)
    {
        card_w = 280.0f;
    }
    float card_h = sh * 0.76f;
    if (card_h < 320.0f)
    {
        card_h = 320.0f;
    }
    if (card_h > 700.0f)
    {
        card_h = 700.0f;
    }

    CLAY(CLAY_ID("AskUserModalDim"),
         {.floating = {.attachTo = CLAY_ATTACH_TO_ROOT,
                       .zIndex = 55,
                       .attachPoints = {.element = CLAY_ATTACH_POINT_LEFT_TOP,
                                        .parent = CLAY_ATTACH_POINT_LEFT_TOP}},
          .layout = {.layoutDirection = CLAY_TOP_TO_BOTTOM,
                     .childAlignment = {.x = CLAY_ALIGN_X_CENTER, .y = CLAY_ALIGN_Y_CENTER},
                     .sizing = {.width = CLAY_SIZING_FIXED(sw), .height = CLAY_SIZING_FIXED(sh)}},
          .backgroundColor = {0, 0, 0, 160}})
    {
        CLAY(CLAY_ID("AskUserModalCard"),
             {.layout = {.layoutDirection = CLAY_TOP_TO_BOTTOM,
                         .padding = {22, 22, 18, 18},
                         .childGap = 12,
                         .sizing = {.width = CLAY_SIZING_FIXED(card_w), .height = CLAY_SIZING_FIXED(card_h)}},
              .backgroundColor = COLOR_CONTENT_BG,
              .cornerRadius = CLAY_CORNER_RADIUS(9)})
        {
            CLAY(CLAY_ID("AskUserHeader"),
                 {.layout = {.layoutDirection = CLAY_LEFT_TO_RIGHT,
                             .childAlignment = {.y = CLAY_ALIGN_Y_CENTER},
                             .sizing = {.width = CLAY_SIZING_GROW(0)}}})
            {
                CLAY_TEXT(CLAY_STRING("Clarifying questions"),
                          CLAY_TEXT_CONFIG({.fontId = FONT_BOLD, .fontSize = 18, .textColor = COLOR_TEXT}));
                CLAY_AUTO_ID({.layout = {.sizing = {.width = CLAY_SIZING_GROW(0)}}}) {}
                CLAY_TEXT(CStr(g_ui.progress),
                          CLAY_TEXT_CONFIG({.fontId = FONT_REGULAR, .fontSize = 13, .textColor = COLOR_MUTED}));
            }

            CLAY(CLAY_ID("AskUserBody"),
                 {.layout = {.layoutDirection = CLAY_TOP_TO_BOTTOM,
                             .childGap = 12,
                             .padding = {0, 8, 0, 0},
                             .sizing = {.width = CLAY_SIZING_GROW(0), .height = CLAY_SIZING_GROW(0)}},
                  .clip = {.vertical = true, .horizontal = false, .childOffset = Clay_GetScrollOffset()}})
            {
                CLAY_TEXT(CStr(q->prompt), CLAY_TEXT_CONFIG({.fontId = FONT_BOLD,
                                                             .fontSize = 16,
                                                             .textColor = COLOR_TEXT,
                                                             .wrapMode = CLAY_TEXT_WRAP_WORDS}));
                if (q->kind == ASK_QUESTION_SELECT)
                {
                    RenderSelectQuestion(q);
                }
                else
                {
                    RenderTextQuestion(q);
                }
            }

            if (g_ui.validation[0])
            {
                CLAY_TEXT(CStr(g_ui.validation),
                          CLAY_TEXT_CONFIG({.fontId = FONT_REGULAR, .fontSize = 13,
                                            .textColor = (Clay_Color){235, 140, 140, 255},
                                            .wrapMode = CLAY_TEXT_WRAP_WORDS}));
            }

            CLAY(CLAY_ID("AskUserButtons"),
                 {.layout = {.layoutDirection = CLAY_LEFT_TO_RIGHT,
                             .childGap = 8,
                             .childAlignment = {.y = CLAY_ALIGN_Y_CENTER},
                             .sizing = {.width = CLAY_SIZING_GROW(0)}}})
            {
                RenderButton(CLAY_STRING("AskUserBack"), "Back", g_ui.current > 0, false);
                CLAY_AUTO_ID({.layout = {.sizing = {.width = CLAY_SIZING_GROW(0)}}}) {}
                bool answered = QuestionAnswered(q);
                RenderButton(CLAY_STRING("AskUserNext"),
                             g_ui.current + 1 == g_ui.question_count ? "Submit" : "Next", answered, true);
            }
        }
    }
}

static bool PointerOver(Clay_String id)
{
    return Clay_PointerOver(CLAY_SID(id));
}

static void AskUserAfterLayout(PicoApp *app, const PicoHookEvent *event)
{
    (void)event;
    if (!g_ui.show || g_ui.current < 0 || g_ui.current >= g_ui.question_count)
    {
        return;
    }
    AskQuestion *q = &g_ui.questions[g_ui.current];

    bool over_back = PointerOver(CLAY_STRING("AskUserBack"));
    bool over_next = PointerOver(CLAY_STRING("AskUserNext"));
    if ((over_back && g_ui.current > 0) || (over_next && QuestionAnswered(q)))
    {
        app->hovered_clickable = true;
    }
    if (q->kind == ASK_QUESTION_SELECT)
    {
        int choice_count = q->option_count + (q->allow_other ? 1 : 0);
        for (int i = 0; i < choice_count; i++)
        {
            if (Clay_PointerOver(CLAY_IDI("AskUserOption", i)))
            {
                app->hovered_clickable = true;
            }
        }
    }

    if (!IsMouseButtonPressed(MOUSE_BUTTON_LEFT))
    {
        return;
    }
    if (q->kind == ASK_QUESTION_SELECT)
    {
        int choice_count = q->option_count + (q->allow_other ? 1 : 0);
        for (int i = 0; i < choice_count; i++)
        {
            if (Clay_PointerOver(CLAY_IDI("AskUserOption", i)))
            {
                q->selected = i;
                q->focus = i;
                g_ui.validation[0] = '\0';
                return;
            }
        }
    }
    if ((q->kind == ASK_QUESTION_TEXT ||
         (q->kind == ASK_QUESTION_SELECT && q->allow_other && q->selected == q->option_count)) &&
        PointerOver(CLAY_STRING("AskUserTextBox")))
    {
        q->cursor = q->text_len;
        return;
    }

    if (over_back && g_ui.current > 0)
    {
        GoBack();
    }
    else if (over_next)
    {
        GoForward(app);
    }
}

static void AskUserLlm(PicoApp *app, PicoAgentId agent_id, PicoLlmEvent *ev)
{
    (void)app;
    (void)agent_id;
    bool offered = false;
    for (int i = 0; ev && i < ev->tool_count; i++)
    {
        if (ev->tools[i].name && strcmp(ev->tools[i].name, "ask_user") == 0 &&
            (!ev->exclude || !ev->exclude[i]))
        {
            offered = true;
            break;
        }
    }
    if (ev && !ev->compact && offered)
    {
        ev->extra_instructions = JsonDup(
            "If implementation details are ambiguous, always use ask_user to resolve every open question, and do "
            "not begin implementation until all questions have been answered.");
    }
}

static void AskUserInit(PicoApp *app)
{
    pico_add_tool(app, "ask_user",
                  "Ask the user one required clarifying question or a multi-step questionnaire. Provide all questions "
                  "in one call. Use kind 'select' with options for a single choice, optionally setting "
                  "allow_other=true for a required free-form Other choice; use kind 'text' for a free-form answer. "
                  "Results are returned as an ordered answers array keyed by question id.",
                  kAskUserParams, AskUserRun, NULL);
    pico_add_llm_hook(app, AskUserLlm);
    pico_add_view(app, PICO_SLOT_OVERLAY, 30, AskUserRender);
    pico_add_hook(app, PICO_HOOK_AFTER_LAYOUT, AskUserAfterLayout);
}

static void AskUserShutdown(PicoApp *app)
{
    (void)app;
    ClearQuestions();
    g_ui.answered_id = 0;
}

PicoExt pico_ext_ask_user(void)
{
    return (PicoExt){
        .abi = PICO_EXT_ABI,
        .name = "ask-user",
        .description = "Multi-step required select and free-form clarifying questions",
        .init = AskUserInit,
        .shutdown = AskUserShutdown,
        .on_frame = AskUserOnFrame,
    };
}
