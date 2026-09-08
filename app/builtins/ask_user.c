// Pico extension: structured, multi-step clarifying questions.
// The ask_user tool accepts all questions in one call and presents a custom
// modal with required single-select and free-form answers.

#include "pico/plugin.h"
#include "pico/theme.h"
#include "builtins/ask_user.h"
#include "json.h"
#include "scrollbar.h"
#include "text_range.h"
#include "host_internal.h"

#include "clay/clay.h"

#include <ctype.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define ASK_USER_MAX_QUESTIONS 24
#define ASK_USER_MAX_OPTIONS 20
#define ASK_USER_MAX_TEXT 16384
#define ASK_USER_MAX_LINES 512
#define ASK_USER_TEXT_FONT PICO_FONT_UI
#define ASK_USER_TEXT_PAD_X 12
#define ASK_USER_TEXT_PAD_Y 10
#define ASK_USER_CARET_BLINK_HZ 2.0

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
    "\"description\":\"Required for select questions and ignored for text questions.\"}"
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
    int selected;
    int focus;
    char *text;
    int text_len;
    int text_cap;
    int cursor;
    int sel_anchor;
    int granularity;
    int unit_from;
    int unit_to;
    bool mouse_selecting;
    PicoClickSeq click_seq;
} AskQuestion;

typedef struct AskLine {
    int start;
    int length;
} AskLine;

typedef struct AskUiState {
    uint64_t id;
    uint64_t answered_id;
    AskQuestion *questions;
    int question_count;
    int current;
    bool show;
    char validation[160];
    char progress[64];
    char option_nums[ASK_USER_MAX_OPTIONS + 1][12];
    AskLine lines[ASK_USER_MAX_LINES];
    int line_count;
    float line_height;
    float wrap_width;
    double caret_blink_at;
    int seen_cursor;
    int seen_length;
    bool text_overflow;
    bool body_overflow;
    bool suppress_chars;
    float text_box_max;
    float goal_x;
    PicoScrollbar scrollbar;
    PicoScrollbar body_scrollbar;
} AskUiState;

static __thread AskUiState *s_active_ask_state = NULL;

#define g_ui (*s_active_ask_state)

static void MarkTextViewDirty(void);
static bool CtrlDown(void);
static bool ShiftDown(void);
static void AskMoveCursor(AskQuestion *q, int pos, bool extend);

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
    g_ui.line_count = 0;
    g_ui.line_height = 0;
    g_ui.wrap_width = 0;
    g_ui.seen_cursor = -1;
    g_ui.seen_length = -1;
    g_ui.text_overflow = false;
    g_ui.body_overflow = false;
    g_ui.suppress_chars = false;
    g_ui.text_box_max = 120.0f;
    memset(&g_ui.scrollbar, 0, sizeof(g_ui.scrollbar));
    memset(&g_ui.body_scrollbar, 0, sizeof(g_ui.body_scrollbar));
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

static void AskUserRun(PicoAgentContext *ctx, const char *args_json, PicoToolResult *out, void *state)
{
    (void)state;
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
        q->text = JsonDup("");
        if (!q->text)
        {
            snprintf(error, error_cap, "out of memory");
            free(kind);
            return false;
        }
        q->text_cap = 1;
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
    MarkTextViewDirty();
    return 1;
}

static void AnswerUiError(PicoHost *app, uint64_t id, const char *message)
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

static void SyncPendingAsk(PicoHost *app)
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
    return q->selected == q->option_count ? HasNonSpace(q->text) : true;
}

static void SetValidation(const char *message)
{
    snprintf(g_ui.validation, sizeof(g_ui.validation), "%s", message ? message : "");
}




static float AskTextPx(void)
{
    return Pico_FontPx(ASK_USER_TEXT_FONT);
}

static Font AskTextFont(void)
{
    return Pico_FontAt(FONT_REGULAR, ASK_USER_TEXT_FONT);
}

static void NoteCaretActivity(void)
{
    g_ui.caret_blink_at = GetTime();
}

static void MarkTextViewDirty(void)
{
    g_ui.seen_cursor = -1;
    g_ui.seen_length = -1;
    g_ui.goal_x = -1.0f;
    NoteCaretActivity();
}

static bool TextFieldOpen(const AskQuestion *q)
{
    return q && (q->kind == ASK_QUESTION_TEXT || q->selected == q->option_count);
}

static float MeasureSlice(Font font, const char *s, int start, int length, float font_size)
{
    if (length <= 0)
    {
        return 0;
    }
    char saved = ((char *)s)[start + length];
    ((char *)s)[start + length] = '\0';
    Vector2 size = MeasureTextEx(font, s + start, font_size, 0);
    ((char *)s)[start + length] = saved;
    return size.x;
}

static int WrapAskText(const AskQuestion *q, Font font, float max_width, AskLine *lines, int max_lines,
                       float *line_height)
{
    Vector2 sample = MeasureTextEx(font, "Hg", AskTextPx(), 0);
    *line_height = sample.y > 1 ? sample.y : AskTextPx();
    if (!q->text || q->text_len == 0)
    {
        lines[0].start = 0;
        lines[0].length = 0;
        return 1;
    }

    int line_count = 0;
    int i = 0;
    while (i < q->text_len && line_count < max_lines)
    {
        int line_start = i;
        if (q->text[i] == '\n')
        {
            lines[line_count].start = line_start;
            lines[line_count].length = 0;
            line_count++;
            i++;
            continue;
        }

        float width = 0;
        int break_at = -1;
        int break_resume = -1;
        int wrapped = 0;
        while (i < q->text_len && q->text[i] != '\n')
        {
            int next = PicoText_Utf8Next(q->text, q->text_len, i);
            float ch_w = MeasureSlice(font, q->text, i, next - i, AskTextPx());
            if (width + ch_w > max_width && i > line_start)
            {
                if (break_at > line_start)
                {
                    lines[line_count].start = line_start;
                    lines[line_count].length = break_at - line_start;
                    line_count++;
                    i = break_resume;
                }
                else
                {
                    lines[line_count].start = line_start;
                    lines[line_count].length = i - line_start;
                    line_count++;
                }
                wrapped = 1;
                break;
            }
            width += ch_w;
            if (q->text[i] == ' ' || q->text[i] == '\t')
            {
                break_at = i;
                break_resume = next;
            }
            i = next;
        }
        if (!wrapped)
        {
            lines[line_count].start = line_start;
            lines[line_count].length = i - line_start;
            line_count++;
            if (i < q->text_len && q->text[i] == '\n')
            {
                i++;
            }
        }
    }
    if (q->text_len > 0 && q->text[q->text_len - 1] == '\n' && line_count < max_lines)
    {
        lines[line_count].start = q->text_len;
        lines[line_count].length = 0;
        line_count++;
    }
    if (line_count == 0)
    {
        lines[0].start = 0;
        lines[0].length = q->text_len;
        return 1;
    }
    return line_count;
}

static int CaretLineIndex(int cursor)
{
    int line_i = 0;
    for (int i = 0; i < g_ui.line_count; i++)
    {
        if (cursor >= g_ui.lines[i].start)
        {
            line_i = i;
        }
    }
    return line_i;
}

static int DigitChoiceIndex(void)
{
    for (int d = 1; d <= 9; d++)
    {
        if (IsKeyPressed(KEY_ZERO + d) || IsKeyPressed(KEY_KP_0 + d))
        {
            return d - 1;
        }
    }
    if (IsKeyPressed(KEY_ZERO) || IsKeyPressed(KEY_KP_0))
    {
        return 9;
    }
    return -1;
}

static bool DigitKeyDown(void)
{
    for (int d = 0; d <= 9; d++)
    {
        if (IsKeyDown(KEY_ZERO + d) || IsKeyDown(KEY_KP_0 + d))
        {
            return true;
        }
    }
    return false;
}

static int OffsetOnLine(const AskQuestion *q, AskLine line, float target_x)
{
    if (target_x <= 0 || line.length <= 0 || !q->text)
    {
        return line.start;
    }
    Font font = AskTextFont();
    float width = 0;
    int pos = line.start;
    int end = line.start + line.length;
    while (pos < end)
    {
        int next = PicoText_Utf8Next(q->text, q->text_len, pos);
        float ch_w = MeasureSlice(font, q->text, pos, next - pos, AskTextPx());
        if (width + ch_w * 0.5f >= target_x)
        {
            return pos;
        }
        width += ch_w;
        pos = next;
    }
    return end;
}

static void MoveTextVertical(AskQuestion *q, int dir, bool extend)
{
    if (!q || g_ui.line_count <= 0)
    {
        return;
    }
    int line_i = CaretLineIndex(q->cursor);
    AskLine from = g_ui.lines[line_i];
    int take = q->cursor - from.start;
    if (take > from.length)
    {
        take = from.length;
    }
    if (take < 0)
    {
        take = 0;
    }
    float x = MeasureSlice(AskTextFont(), q->text ? q->text : "", from.start, take, AskTextPx());
    float goal = g_ui.goal_x >= 0.0f ? g_ui.goal_x : x;
    int next = line_i + dir;
    int pos;
    if (next < 0)
    {
        pos = 0;
    }
    else if (next >= g_ui.line_count)
    {
        pos = q->text_len;
    }
    else
    {
        pos = OffsetOnLine(q, g_ui.lines[next], goal);
    }
    AskMoveCursor(q, pos, extend);
    g_ui.goal_x = goal;
}

static bool AskHasSelection(const AskQuestion *q)
{
    return q->sel_anchor != q->cursor;
}

static int AskSelFrom(const AskQuestion *q)
{
    return q->sel_anchor < q->cursor ? q->sel_anchor : q->cursor;
}

static int AskSelTo(const AskQuestion *q)
{
    return q->sel_anchor > q->cursor ? q->sel_anchor : q->cursor;
}

static void AskMoveCursor(AskQuestion *q, int pos, bool extend)
{
    if (pos < 0)
    {
        pos = 0;
    }
    if (pos > q->text_len)
    {
        pos = q->text_len;
    }
    q->cursor = pos;
    if (!extend)
    {
        q->sel_anchor = pos;
    }
    g_ui.goal_x = -1.0f;
    NoteCaretActivity();
}

static void AskCopy(const AskQuestion *q)
{
    if (!AskHasSelection(q) || !q->text)
    {
        return;
    }
    int from = AskSelFrom(q);
    int n = AskSelTo(q) - from;
    char *copy = (char *)malloc((size_t)n + 1);
    if (!copy)
    {
        return;
    }
    memcpy(copy, q->text + from, (size_t)n);
    copy[n] = '\0';
    SetClipboardText(copy);
    free(copy);
}

static void AskUnitRange(const AskQuestion *q, int pos, int granularity, int *from, int *to)
{
    const char *text = q->text ? q->text : "";
    if (granularity >= 3)
    {
        PicoText_ParaRange(text, q->text_len, pos, from, to);
    }
    else
    {
        PicoText_WordRange(text, q->text_len, pos, from, to);
    }
}

static void AskSelectUnit(AskQuestion *q, int pos, int granularity)
{
    q->granularity = granularity;
    if (granularity <= 1)
    {
        q->unit_from = pos;
        q->unit_to = pos;
        AskMoveCursor(q, pos, ShiftDown());
        return;
    }
    int from = pos;
    int to = pos;
    AskUnitRange(q, pos, granularity, &from, &to);
    q->unit_from = from;
    q->unit_to = to;
    q->sel_anchor = from;
    q->cursor = to;
    g_ui.goal_x = -1.0f;
    NoteCaretActivity();
}

static void AskExtendUnit(AskQuestion *q, int pos)
{
    if (q->granularity <= 1)
    {
        AskMoveCursor(q, pos, true);
        return;
    }
    int from = pos;
    int to = pos;
    AskUnitRange(q, pos, q->granularity, &from, &to);
    int span_from = 0;
    int span_to = 0;
    PicoText_UnionRange(q->unit_from, q->unit_to, from, to, &span_from, &span_to);
    if (pos >= q->unit_from)
    {
        q->sel_anchor = q->unit_from;
        q->cursor = span_to;
    }
    else
    {
        q->sel_anchor = q->unit_to;
        q->cursor = span_from;
    }
    g_ui.goal_x = -1.0f;
    NoteCaretActivity();
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

static void AskTextDeleteRange(AskQuestion *q, int from, int to);

static void AskTextInsert(AskQuestion *q, const char *s, int n)
{
    if (!q || !s || n <= 0)
    {
        return;
    }
    if (AskHasSelection(q))
    {
        AskTextDeleteRange(q, AskSelFrom(q), AskSelTo(q));
    }
    if (q->text_len >= ASK_USER_MAX_TEXT)
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
    q->sel_anchor = q->cursor;
    g_ui.validation[0] = '\0';
    g_ui.goal_x = -1.0f;
    NoteCaretActivity();
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
    q->sel_anchor = from;
    g_ui.goal_x = -1.0f;
    NoteCaretActivity();
}

static void AskDeleteSelection(AskQuestion *q)
{
    if (AskHasSelection(q))
    {
        AskTextDeleteRange(q, AskSelFrom(q), AskSelTo(q));
    }
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

static void SubmitAnswers(PicoHost *app)
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
        MarkTextViewDirty();
    }
}

static void GoForward(PicoHost *app)
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
        MarkTextViewDirty();
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

static void HandleTextKeys(PicoHost *app, AskQuestion *q);

static void HandleSelectKeys(PicoHost *app, AskQuestion *q)
{
    if (q->selected == q->option_count)
    {
        HandleTextKeys(app, q);
        return;
    }

    bool up = IsKeyPressed(KEY_UP) || IsKeyPressedRepeat(KEY_UP);
    bool down = IsKeyPressed(KEY_DOWN) || IsKeyPressedRepeat(KEY_DOWN);
    int choice_count = q->option_count + 1;
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
    int digit = DigitChoiceIndex();
    if (digit >= 0 && digit < choice_count)
    {
        q->selected = digit;
        q->focus = digit;
        g_ui.validation[0] = '\0';
        while (GetCharPressed() != 0)
        {
        }
        if (q->selected == q->option_count)
        {
            g_ui.suppress_chars = true;
        }
        return;
    }
    while (GetCharPressed() != 0)
    {
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

static void HandleTextKeys(PicoHost *app, AskQuestion *q)
{
    bool ctrl = CtrlDown();
    bool shift = ShiftDown();
    bool left = IsKeyPressed(KEY_LEFT) || IsKeyPressedRepeat(KEY_LEFT);
    bool right = IsKeyPressed(KEY_RIGHT) || IsKeyPressedRepeat(KEY_RIGHT);
    bool up = IsKeyPressed(KEY_UP) || IsKeyPressedRepeat(KEY_UP);
    bool down = IsKeyPressed(KEY_DOWN) || IsKeyPressedRepeat(KEY_DOWN);
    bool backspace = IsKeyPressed(KEY_BACKSPACE) || IsKeyPressedRepeat(KEY_BACKSPACE);
    bool del = IsKeyPressed(KEY_DELETE) || IsKeyPressedRepeat(KEY_DELETE);

    if (ctrl && Pico_ShortcutPressed('c'))
    {
        AskCopy(q);
        return;
    }
    if (ctrl && Pico_ShortcutPressed('x'))
    {
        AskCopy(q);
        AskDeleteSelection(q);
        return;
    }
    if (ctrl && Pico_ShortcutPressed('a'))
    {
        AskMoveCursor(q, 0, false);
        AskMoveCursor(q, q->text_len, true);
    }
    if (ctrl && Pico_ShortcutPressed('v'))
    {
        PasteText(q);
    }
    /* Paste may allocate or move the answer buffer. */
    const char *text = q->text ? q->text : "";
    if (IsKeyPressed(KEY_HOME))
    {
        int from = 0;
        int to = 0;
        PicoText_ParaRange(text, q->text_len, q->cursor, &from, &to);
        AskMoveCursor(q, from, shift);
    }
    if (IsKeyPressed(KEY_END))
    {
        int from = 0;
        int to = 0;
        PicoText_ParaRange(text, q->text_len, q->cursor, &from, &to);
        AskMoveCursor(q, to, shift);
    }
    if (left)
    {
        int pos = ctrl ? PicoText_PrevWord(text, q->cursor) : PicoText_Utf8Prev(text, q->cursor);
        AskMoveCursor(q, pos, shift);
    }
    if (right)
    {
        int pos = ctrl ? PicoText_NextWord(text, q->text_len, q->cursor)
                       : PicoText_Utf8Next(text, q->text_len, q->cursor);
        AskMoveCursor(q, pos, shift);
    }
    if (up)
    {
        MoveTextVertical(q, -1, shift);
    }
    if (down)
    {
        MoveTextVertical(q, 1, shift);
    }
    if (ctrl && Pico_ShortcutRepeat('w'))
    {
        if (AskHasSelection(q))
        {
            AskDeleteSelection(q);
        }
        else
        {
            AskTextDeleteRange(q, PicoText_PrevWord(text, q->cursor), q->cursor);
        }
    }
    else if (backspace)
    {
        if (AskHasSelection(q))
        {
            AskDeleteSelection(q);
        }
        else if (ctrl)
        {
            AskTextDeleteRange(q, PicoText_PrevWord(text, q->cursor), q->cursor);
        }
        else if (q->cursor > 0)
        {
            AskTextDeleteRange(q, PicoText_Utf8Prev(text, q->cursor), q->cursor);
        }
    }
    if (del)
    {
        if (AskHasSelection(q))
        {
            AskDeleteSelection(q);
        }
        else if (q->cursor < q->text_len)
        {
            AskTextDeleteRange(q, q->cursor, PicoText_Utf8Next(text, q->text_len, q->cursor));
        }
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

    if (g_ui.suppress_chars)
    {
        while (GetCharPressed() != 0)
        {
        }
        if (!DigitKeyDown())
        {
            g_ui.suppress_chars = false;
        }
    }
    else if (!ctrl)
    {
        int cp;
        while ((cp = GetCharPressed()) != 0)
        {
            if (cp < 32)
            {
                continue;
            }
            char bytes[4];
            int n = PicoText_Utf8Encode(cp, bytes);
            AskTextInsert(q, bytes, n);
        }
    }
}

static void UpdateAskScrollbarDrag(void)
{
    PicoScrollbar_UpdateDragOverlay(&g_ui.scrollbar, CLAY_STRING("AskUserTextScroll"),
                                    CLAY_STRING("AskUserTextScrollHandle"));
    PicoScrollbar_UpdateDrag(&g_ui.body_scrollbar, CLAY_STRING("AskUserBody"),
                             CLAY_STRING("AskUserBodyHandle"));
}

static void AskUserOnFrame(PicoHost *app, void *state, float dt)
{
    (void)dt;
    s_active_ask_state = state ? (AskUiState *)state : (AskUiState *)PicoPlugins_HostState(app, "ask-user");
    if (!s_active_ask_state)
    {
        return;
    }
    SyncPendingAsk(app);
    if (!g_ui.show || g_ui.current < 0 || g_ui.current >= g_ui.question_count)
    {
        return;
    }
    UpdateAskScrollbarDrag();
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
        CLAY_TEXT(CStr(label), CLAY_TEXT_CONFIG({.fontId = FONT_BOLD, .fontSize = PICO_FONT_UI, .textColor = text,
                                                .wrapMode = CLAY_TEXT_WRAP_WORDS}));
    }
}

static void RenderTextQuestion(const AskQuestion *q);

static void RenderSelectQuestion(const AskQuestion *q)
{
    int choice_count = q->option_count + 1;
    CLAY(CLAY_ID("AskUserOptions"),
         {.layout = {.layoutDirection = CLAY_TOP_TO_BOTTOM,
                     .childGap = 8,
                     .sizing = {.width = CLAY_SIZING_GROW(0)}}})
    {
        for (int i = 0; i < choice_count; i++)
        {
            Clay_ElementId eid = CLAY_IDI("AskUserOption", i);
            bool hover = Clay_PointerOver(eid);
            bool selected = q->selected == i;
            bool focused = q->focus == i;
            const char *label = i < q->option_count ? q->options[i] : "Other…";
            snprintf(g_ui.option_nums[i], sizeof(g_ui.option_nums[i]), "%d", i + 1);
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
                CLAY_TEXT(CStr(g_ui.option_nums[i]),
                          CLAY_TEXT_CONFIG({.fontId = FONT_MONO,
                                            .fontSize = PICO_FONT_UI,
                                            .textColor = selected ? COLOR_LINK : COLOR_MUTED}));
                CLAY_TEXT(CStr(label), CLAY_TEXT_CONFIG({.fontId = FONT_REGULAR,
                                                         .fontSize = PICO_FONT_UI,
                                                         .textColor = COLOR_TEXT,
                                                         .wrapMode = CLAY_TEXT_WRAP_WORDS}));
            }
        }
    }
    if (q->selected == q->option_count)
    {
        RenderTextQuestion(q);
    }
    else
    {
        CLAY_TEXT(CLAY_STRING("Up/Down or 1-N select  •  Enter next"),
                  CLAY_TEXT_CONFIG({.fontId = FONT_REGULAR, .fontSize = PICO_FONT_CAPTION, .textColor = COLOR_MUTED,
                                    .wrapMode = CLAY_TEXT_WRAP_WORDS}));
    }
}

static void RenderTextQuestion(const AskQuestion *q)
{
    bool empty = q->text_len == 0;
    float wrap = g_ui.wrap_width > 10.0f ? g_ui.wrap_width : 400.0f;
    g_ui.line_count = WrapAskText(q, AskTextFont(), wrap, g_ui.lines, ASK_USER_MAX_LINES, &g_ui.line_height);
    if (g_ui.line_height < 1.0f)
    {
        g_ui.line_height = AskTextPx();
    }

    CLAY(CLAY_ID("AskUserTextBox"),
         {.layout = {.layoutDirection = CLAY_TOP_TO_BOTTOM,
                     .padding = {ASK_USER_TEXT_PAD_X, ASK_USER_TEXT_PAD_X, ASK_USER_TEXT_PAD_Y, ASK_USER_TEXT_PAD_Y},
                     .sizing = {.width = CLAY_SIZING_GROW(0),
                                .height = CLAY_SIZING_GROW(120, g_ui.text_box_max)}},
          .backgroundColor = COLOR_COMPOSER_BG,
          .cornerRadius = CLAY_CORNER_RADIUS(6)})
    {
        CLAY(CLAY_ID("AskUserTextScroll"),
             {.layout = {.layoutDirection = CLAY_TOP_TO_BOTTOM,
                         .sizing = {.width = CLAY_SIZING_GROW(0), .height = CLAY_SIZING_GROW(0)}},
              .clip = {.vertical = true, .horizontal = true, .childOffset = Clay_GetScrollOffset()}})
        {
            CLAY(CLAY_ID("AskUserTextContent"),
                 {.layout = {.layoutDirection = CLAY_TOP_TO_BOTTOM,
                             .sizing = {.width = CLAY_SIZING_GROW(0), .height = CLAY_SIZING_FIT(0)}}})
            {
                if (empty)
                {
                    CLAY_TEXT(CLAY_STRING("Type your answer…"),
                              CLAY_TEXT_CONFIG({.fontId = FONT_REGULAR,
                                                .fontSize = ASK_USER_TEXT_FONT,
                                                .textColor = COLOR_MUTED,
                                                .wrapMode = CLAY_TEXT_WRAP_WORDS}));
                }
                else
                {
                    for (int i = 0; i < g_ui.line_count; i++)
                    {
                        CLAY(CLAY_IDI("AskUserTextLine", i),
                             {.layout = {.sizing = {.width = CLAY_SIZING_GROW(0),
                                                    .height = CLAY_SIZING_FIXED(g_ui.line_height)}}})
                        {
                            if (g_ui.lines[i].length > 0)
                            {
                                Clay_String text = {.length = (int32_t)g_ui.lines[i].length,
                                                    .chars = q->text + g_ui.lines[i].start};
                                CLAY_TEXT(text, CLAY_TEXT_CONFIG({.fontId = FONT_REGULAR,
                                                                  .fontSize = ASK_USER_TEXT_FONT,
                                                                  .textColor = COLOR_TEXT,
                                                                  .wrapMode = CLAY_TEXT_WRAP_NONE}));
                            }
                        }
                    }
                }
            }
        }
        if (g_ui.text_overflow)
        {
            PicoScrollbar_RenderOverlay(CLAY_STRING("AskUserTextScroll"), CLAY_STRING("AskUserTextScrollTrack"),
                                        CLAY_STRING("AskUserTextScrollHandle"));
        }
    }
    CLAY_TEXT(CLAY_STRING("Enter next  •  Shift+Enter newline  •  Shift+Tab back"),
              CLAY_TEXT_CONFIG({.fontId = FONT_REGULAR, .fontSize = PICO_FONT_CAPTION, .textColor = COLOR_MUTED,
                                .wrapMode = CLAY_TEXT_WRAP_WORDS}));
}

static void AskUserRender(PicoHost *app, void *state)
{
    s_active_ask_state = state ? (AskUiState *)state : (AskUiState *)PicoPlugins_HostState(app, "ask-user");
    if (!s_active_ask_state || !g_ui.show || g_ui.current < 0 || g_ui.current >= g_ui.question_count)
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

    float body_max = card_h - 18.0f * 2.0f - 12.0f * 2.0f - 28.0f - 42.0f;
    if (g_ui.validation[0])
    {
        body_max -= 32.0f;
    }
    if (body_max < 160.0f)
    {
        body_max = 160.0f;
    }
    float text_max = body_max - 22.0f - 18.0f - 12.0f * 2.0f;
    if (q->kind == ASK_QUESTION_SELECT && q->selected == q->option_count)
    {
        int n = q->option_count + 1;
        text_max -= (float)n * 44.0f + (float)(n > 0 ? n - 1 : 0) * 8.0f + 12.0f;
    }
    if (text_max < 120.0f)
    {
        text_max = 120.0f;
    }
    g_ui.text_box_max = text_max;

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
              .clip = {.vertical = true, .horizontal = false},
              .backgroundColor = COLOR_CONTENT_BG,
              .cornerRadius = CLAY_CORNER_RADIUS(9)})
        {
            CLAY(CLAY_ID("AskUserHeader"),
                 {.layout = {.layoutDirection = CLAY_LEFT_TO_RIGHT,
                             .childAlignment = {.y = CLAY_ALIGN_Y_CENTER},
                             .sizing = {.width = CLAY_SIZING_GROW(0)}}})
            {
                CLAY_TEXT(CLAY_STRING("Clarifying questions"),
                          CLAY_TEXT_CONFIG({.fontId = FONT_BOLD, .fontSize = PICO_FONT_TITLE, .textColor = COLOR_TEXT,
                                            .wrapMode = CLAY_TEXT_WRAP_WORDS}));
                CLAY_AUTO_ID({.layout = {.sizing = {.width = CLAY_SIZING_GROW(0)}}}) {}
                CLAY_TEXT(CStr(g_ui.progress),
                          CLAY_TEXT_CONFIG({.fontId = FONT_REGULAR, .fontSize = PICO_FONT_CAPTION, .textColor = COLOR_MUTED,
                                            .wrapMode = CLAY_TEXT_WRAP_WORDS}));
            }

            CLAY(CLAY_ID("AskUserBodyRow"),
                 {.layout = {.layoutDirection = CLAY_LEFT_TO_RIGHT,
                             .childGap = SCROLLBAR_GAP,
                             .sizing = {.width = CLAY_SIZING_GROW(0), .height = CLAY_SIZING_GROW(0, body_max)}}})
            {
                CLAY(CLAY_ID("AskUserBody"),
                     {.layout = {.layoutDirection = CLAY_TOP_TO_BOTTOM,
                                 .childGap = 12,
                                 .sizing = {.width = CLAY_SIZING_GROW(0), .height = CLAY_SIZING_GROW(0)}},
                      .clip = {.vertical = true, .horizontal = false, .childOffset = Clay_GetScrollOffset()}})
                {
                    CLAY_TEXT(CStr(q->prompt), CLAY_TEXT_CONFIG({.fontId = FONT_REGULAR,
                                                                 .fontSize = PICO_FONT_BODY,
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
                if (g_ui.body_overflow)
                {
                    PicoScrollbar_Render(CLAY_STRING("AskUserBody"), CLAY_STRING("AskUserBodyTrack"),
                                         CLAY_STRING("AskUserBodyHandle"));
                }
            }

            if (g_ui.validation[0])
            {
                CLAY_TEXT(CStr(g_ui.validation),
                          CLAY_TEXT_CONFIG({.fontId = FONT_REGULAR, .fontSize = PICO_FONT_CAPTION,
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

static int OffsetAtPoint(const AskQuestion *q, float x, float y)
{
    Clay_ElementData scroll_box = Clay_GetElementData(Clay_GetElementId(CLAY_STRING("AskUserTextScroll")));
    if (!scroll_box.found)
    {
        return q->cursor;
    }
    Clay_ScrollContainerData scroll =
        Clay_GetScrollContainerData(Clay_GetElementId(CLAY_STRING("AskUserTextScroll")));
    float scroll_y = (scroll.found && scroll.scrollPosition) ? scroll.scrollPosition->y : 0;
    float local_x = x - scroll_box.boundingBox.x;
    float local_y = y - scroll_box.boundingBox.y - scroll_y;
    if (local_y < 0)
    {
        return 0;
    }
    float line_height = g_ui.line_height > 1 ? g_ui.line_height : AskTextPx();
    int line_i = (int)(local_y / line_height);
    if (line_i >= g_ui.line_count)
    {
        return q->text_len;
    }
    if (line_i < 0)
    {
        line_i = 0;
    }
    AskLine line = g_ui.lines[line_i];
    if (local_x <= 0 || line.length <= 0 || !q->text)
    {
        return line.start;
    }
    Font font = AskTextFont();
    float width = 0;
    int pos = line.start;
    int end = line.start + line.length;
    while (pos < end)
    {
        int next = PicoText_Utf8Next(q->text, q->text_len, pos);
        float ch_w = MeasureSlice(font, q->text, pos, next - pos, AskTextPx());
        if (width + ch_w * 0.5f >= local_x)
        {
            return pos;
        }
        width += ch_w;
        pos = next;
    }
    return end;
}

static void EnsureAskCaretVisible(const AskQuestion *q)
{
    if (q->cursor == g_ui.seen_cursor && q->text_len == g_ui.seen_length)
    {
        return;
    }
    g_ui.seen_cursor = q->cursor;
    g_ui.seen_length = q->text_len;

    Clay_ScrollContainerData scroll =
        Clay_GetScrollContainerData(Clay_GetElementId(CLAY_STRING("AskUserTextScroll")));
    if (!scroll.found || !scroll.scrollPosition || g_ui.line_height < 1)
    {
        return;
    }
    int line_i = CaretLineIndex(q->cursor);
    float caret_top = (float)line_i * g_ui.line_height;
    float caret_bot = caret_top + g_ui.line_height;
    float view_h = scroll.scrollContainerDimensions.height;
    float vis_top = -scroll.scrollPosition->y;
    float vis_bot = vis_top + view_h;
    if (caret_top < vis_top)
    {
        scroll.scrollPosition->y = -caret_top;
    }
    else if (caret_bot > vis_bot)
    {
        scroll.scrollPosition->y = -(caret_bot - view_h);
    }
}

static void AskUserAfterLayout(PicoHost *app, const PicoHookEvent *event, void *state)
{
    (void)event;
    s_active_ask_state = state ? (AskUiState *)state : (AskUiState *)PicoPlugins_HostState(app, "ask-user");
    if (!s_active_ask_state || !g_ui.show || g_ui.current < 0 || g_ui.current >= g_ui.question_count)
    {
        return;
    }
    AskQuestion *q = &g_ui.questions[g_ui.current];

    bool over_back = PointerOver(CLAY_STRING("AskUserBack"));
    bool over_next = PointerOver(CLAY_STRING("AskUserNext"));
    bool over_text = PointerOver(CLAY_STRING("AskUserTextBox"));
    bool over_bar = Clay_PointerOver(Clay_GetElementId(CLAY_STRING("AskUserTextScrollHandle"))) ||
                    Clay_PointerOver(Clay_GetElementId(CLAY_STRING("AskUserTextScrollTrack")));
    if ((over_back && g_ui.current > 0) || (over_next && QuestionAnswered(q)))
    {
        app->hovered_clickable = true;
    }
    /* The text field is an editing target: hovering it asks for the I-beam
     * cursor, while its scrollbar strip keeps the default cursor. */
    if (TextFieldOpen(q) && over_text && !over_bar)
    {
        app->hovered_text = true;
    }
    if (q->kind == ASK_QUESTION_SELECT)
    {
        int choice_count = q->option_count + 1;
        for (int i = 0; i < choice_count; i++)
        {
            if (Clay_PointerOver(CLAY_IDI("AskUserOption", i)))
            {
                app->hovered_clickable = true;
            }
        }
    }

    if (TextFieldOpen(q))
    {
        Clay_ElementData scroll_box = Clay_GetElementData(Clay_GetElementId(CLAY_STRING("AskUserTextScroll")));
        if (scroll_box.found && scroll_box.boundingBox.width > 10)
        {
            g_ui.wrap_width = scroll_box.boundingBox.width;
        }
        g_ui.text_overflow = PicoScrollbar_Overflows(CLAY_STRING("AskUserTextScroll"));
        EnsureAskCaretVisible(q);
    }
    else
    {
        g_ui.text_overflow = false;
    }
    g_ui.body_overflow = PicoScrollbar_Overflows(CLAY_STRING("AskUserBody"));

    bool pressed = IsMouseButtonPressed(MOUSE_BUTTON_LEFT);
    if (TextFieldOpen(q))
    {
        if (pressed && over_bar)
        {
            q->mouse_selecting = false;
            PicoClickSeq_Reset(&q->click_seq);
        }
        else if (pressed && over_text)
        {
            Vector2 mouse = GetMousePosition();
            int count = PicoClickSeq_Press(&q->click_seq, GetTime(), mouse.x, mouse.y);
            AskSelectUnit(q, OffsetAtPoint(q, mouse.x, mouse.y), count);
            q->mouse_selecting = true;
        }
        if (!IsMouseButtonDown(MOUSE_BUTTON_LEFT))
        {
            q->mouse_selecting = false;
        }
        else if (q->mouse_selecting)
        {
            Vector2 mouse = GetMousePosition();
            AskExtendUnit(q, OffsetAtPoint(q, mouse.x, mouse.y));
        }
    }

    if (!pressed)
    {
        return;
    }
    if (q->kind == ASK_QUESTION_SELECT)
    {
        int choice_count = q->option_count + 1;
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
    if (TextFieldOpen(q) && over_text && !over_bar)
    {
        /* Click handled by selection logic above. */
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

static void AskUserDrawOverlay(PicoHost *app, const PicoHookEvent *event, void *state)
{
    (void)event;
    s_active_ask_state = state ? (AskUiState *)state : (AskUiState *)PicoPlugins_HostState(app, "ask-user");
    if (!s_active_ask_state || !g_ui.show || g_ui.current < 0 || g_ui.current >= g_ui.question_count)
    {
        return;
    }
    AskQuestion *q = &g_ui.questions[g_ui.current];
    if (!TextFieldOpen(q))
    {
        return;
    }
    Clay_ElementData scroll_box = Clay_GetElementData(Clay_GetElementId(CLAY_STRING("AskUserTextScroll")));
    Clay_ElementData card = Clay_GetElementData(Clay_GetElementId(CLAY_STRING("AskUserModalCard")));
    if (!scroll_box.found)
    {
        return;
    }
    Clay_BoundingBox clip = scroll_box.boundingBox;
    if (card.found)
    {
        float x0 = clip.x > card.boundingBox.x ? clip.x : card.boundingBox.x;
        float y0 = clip.y > card.boundingBox.y ? clip.y : card.boundingBox.y;
        float x1 = clip.x + clip.width;
        float y1 = clip.y + clip.height;
        float cx1 = card.boundingBox.x + card.boundingBox.width;
        float cy1 = card.boundingBox.y + card.boundingBox.height;
        if (cx1 < x1)
        {
            x1 = cx1;
        }
        if (cy1 < y1)
        {
            y1 = cy1;
        }
        if (x1 <= x0 || y1 <= y0)
        {
            return;
        }
        clip.x = x0;
        clip.y = y0;
        clip.width = x1 - x0;
        clip.height = y1 - y0;
    }
    Clay_ScrollContainerData scroll =
        Clay_GetScrollContainerData(Clay_GetElementId(CLAY_STRING("AskUserTextScroll")));
    float scroll_y = (scroll.found && scroll.scrollPosition) ? scroll.scrollPosition->y : 0;
    float line_height = g_ui.line_height > 1 ? g_ui.line_height : AskTextPx();
    BeginScissorMode((int)clip.x, (int)clip.y, (int)clip.width, (int)clip.height);

    if (AskHasSelection(q) && q->text)
    {
        int sel_from = AskSelFrom(q);
        int sel_to = AskSelTo(q);
        Color fill = {(unsigned char)COLOR_SELECTION.r, (unsigned char)COLOR_SELECTION.g,
                      (unsigned char)COLOR_SELECTION.b, (unsigned char)COLOR_SELECTION.a};
        for (int i = 0; i < g_ui.line_count; i++)
        {
            int start = g_ui.lines[i].start;
            int end = start + g_ui.lines[i].length;
            int range_lo = start;
            int range_hi = end;
            if (g_ui.lines[i].length == 0 && start > 0)
            {
                range_lo = start - 1;
            }
            if (sel_from >= range_hi || sel_to <= range_lo)
            {
                continue;
            }
            float y = scroll_box.boundingBox.y + (float)i * line_height + scroll_y;
            if (g_ui.lines[i].length == 0)
            {
                DrawRectangle((int)scroll_box.boundingBox.x, (int)y, 6, (int)line_height, fill);
                continue;
            }
            int a = sel_from > start ? sel_from : start;
            int b = sel_to < end ? sel_to : end;
            if (a > b)
            {
                a = b;
            }
            float x0 = MeasureSlice(AskTextFont(), q->text, start, a - start, AskTextPx());
            float x1 = MeasureSlice(AskTextFont(), q->text, start, b - start, AskTextPx());
            DrawRectangle((int)(scroll_box.boundingBox.x + x0), (int)y,
                          (int)(x1 - x0 < 2 ? 2 : x1 - x0), (int)line_height, fill);
        }
    }

    double elapsed = GetTime() - g_ui.caret_blink_at;
    if (elapsed < 0)
    {
        elapsed = 0;
    }
    if (((int)(elapsed * ASK_USER_CARET_BLINK_HZ) & 1) == 0)
    {
        int line_i = CaretLineIndex(q->cursor);
        AskLine line = g_ui.line_count > 0 ? g_ui.lines[line_i] : (AskLine){0, 0};
        int take = q->cursor - line.start;
        if (take > line.length)
        {
            take = line.length;
        }
        if (take < 0)
        {
            take = 0;
        }
        float x = scroll_box.boundingBox.x +
                  MeasureSlice(AskTextFont(), q->text ? q->text : "", line.start, take, AskTextPx());
        float y = scroll_box.boundingBox.y + (float)line_i * line_height + scroll_y;
        Color caret = {(unsigned char)COLOR_CURSOR.r, (unsigned char)COLOR_CURSOR.g, (unsigned char)COLOR_CURSOR.b,
                       255};
        DrawRectangle((int)x, (int)y, 2, (int)line_height, caret);
    }
    EndScissorMode();
}

static void AskUserLlm(PicoWorkspace *workspace, PicoAgentId agent_id, PicoLlmEvent *event, void *state)
{
    PicoLlmEvent *ev = event;
    (void)workspace;
    (void)state;
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

static int AskUserHostInit(PicoHost *app, void **state_out)
{
    AskUiState *s = (AskUiState *)calloc(1, sizeof(AskUiState));
    if (!s)
    {
        return 1;
    }
    s->seen_cursor = -1;
    s->seen_length = -1;
    s->goal_x = -1.0f;
    s->text_box_max = 120.0f;
    if (state_out)
    {
        *state_out = s;
    }
    s_active_ask_state = s;
    pico_host_add_view(app, PICO_SLOT_OVERLAY, 30, AskUserRender);
    pico_host_add_hook(app, PICO_HOOK_AFTER_LAYOUT, AskUserAfterLayout);
    pico_host_add_hook(app, PICO_HOOK_AFTER_RENDER, AskUserDrawOverlay);
    return 0;
}

static void AskUserHostShutdown(PicoHost *app, void *state)
{
    (void)app;
    AskUiState *s = (AskUiState *)state;
    if (!s)
    {
        return;
    }
    s_active_ask_state = s;
    ClearQuestions();
    s->answered_id = 0;
    free(s);
    s_active_ask_state = NULL;
}

static int AskUserWorkspaceInit(PicoWorkspace *workspace, void **state_out)
{
    (void)state_out;
    pico_add_tool(workspace, "ask_user",
                  "Ask the user one required clarifying question or a multi-step questionnaire. Provide all questions "
                  "in one call. Use kind 'select' with options for a single choice; select questions always include a "
                  "required free-form Other choice. Use kind 'text' for a free-form answer. "
                  "Results are returned as an ordered answers array keyed by question id.",
                  kAskUserParams, AskUserRun, NULL, PICO_TOOL_SEQUENTIAL);
    pico_add_llm_hook(workspace, AskUserLlm);
    return 0;
}

PicoExt pico_ext_ask_user(void)
{
    return (PicoExt){
        .abi = PICO_EXT_ABI,
        .name = "ask-user",
        .description = "Multi-step required select and free-form clarifying questions",
        .host_init = AskUserHostInit,
        .host_shutdown = AskUserHostShutdown,
        .host_on_frame = AskUserOnFrame,
        .workspace_init = AskUserWorkspaceInit,
    };
}
