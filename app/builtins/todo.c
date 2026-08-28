#include "pico/plugin.h"
#include "todo.h"
#include "todo_model.h"
#include "agent_manager.h"
#include "session.h"
#include "json.h"
#include "scrollbar.h"

#include "clay/clay.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define TODO_COLLAPSED_WIDTH 112.0f
#define TODO_COLLAPSED_HEIGHT 36.0f
#define TODO_EXPANDED_WIDTH 520.0f
#define TODO_EXPANDED_HEIGHT 420.0f
#define TODO_GAP 8.0f

typedef struct TodoAgentState {
    PicoAgentId agent_id;
    PicoTodoList todos;
    bool expanded;
    bool title_pending;
} TodoAgentState;

static TodoAgentState g_states[PICO_MAX_AGENTS];
static float g_composer_width = TODO_EXPANDED_WIDTH;
static float g_space_above = TODO_EXPANDED_HEIGHT;
static char g_header[64];
static bool g_overflow;
static PicoScrollbar g_scrollbar;

static const char *kTodoParams =
    "{\"type\":\"object\",\"properties\":{\"task\":{\"type\":\"string\",\"minLength\":1,\"maxLength\":72,"
    "\"description\":\"Succinct session name for the current work (a few words, not a paragraph). "
    "Keep it stable unless the goal changes.\"},"
    "\"todos\":{\"type\":\"array\",\"maxItems\":30,"
    "\"description\":\"The complete canonical TODO list. This replaces the previous list atomically.\","
    "\"items\":{\"type\":\"object\",\"properties\":{"
    "\"id\":{\"type\":\"string\",\"minLength\":1,\"maxLength\":64,"
    "\"pattern\":\"^[A-Za-z0-9][A-Za-z0-9._-]{0,63}$\"},"
    "\"text\":{\"type\":\"string\",\"minLength\":1,\"maxLength\":300},"
    "\"status\":{\"type\":\"string\",\"enum\":[\"pending\",\"in_progress\",\"completed\"]}},"
    "\"required\":[\"id\",\"text\",\"status\"]}},"
    "\"explanation\":{\"type\":\"string\",\"maxLength\":300}},\"required\":[\"todos\",\"task\"]}";

static Clay_String CStr(const char *s)
{
    if (!s)
    {
        s = "";
    }
    return (Clay_String){.length = (int32_t)strlen(s), .chars = s};
}

static TodoAgentState *FindState(PicoAgentId agent_id, bool create)
{
    if (!agent_id)
    {
        return NULL;
    }
    TodoAgentState *empty = NULL;
    for (int i = 0; i < PICO_MAX_AGENTS; i++)
    {
        if (g_states[i].agent_id == agent_id)
        {
            return &g_states[i];
        }
        if (!g_states[i].agent_id && !empty)
        {
            empty = &g_states[i];
        }
    }
    if (create && empty)
    {
        empty->agent_id = agent_id;
        return empty;
    }
    return NULL;
}

static TodoAgentState *ActiveState(const PicoApp *app)
{
    return FindState(pico_agent_active(app), false);
}

bool PicoTodo_IsExpanded(const PicoApp *app)
{
    TodoAgentState *state = ActiveState(app);
    return state && state->expanded && state->todos.count > 0;
}

static void ClearState(PicoAgentId agent_id)
{
    TodoAgentState *state = FindState(agent_id, false);
    if (!state)
    {
        return;
    }
    PicoTodoList_Free(&state->todos);
    memset(state, 0, sizeof(*state));
}

static void TodoReset(PicoApp *app, const PicoHookEvent *event)
{
    (void)app;
    if (event)
    {
        ClearState(event->agent_id);
    }
}

static void TodoRun(PicoAgentContext *ctx, const char *args_json, PicoToolResult *out)
{
    (void)ctx;
    if (!out)
    {
        return;
    }
    memset(out, 0, sizeof(*out));
    PicoTodoList parsed;
    char *error = NULL;
    if (!PicoTodoList_ParseArgs(args_json, &parsed, &error))
    {
        out->output = error ? error : JsonDup("Invalid TODO update");
        out->is_error = true;
        return;
    }
    out->output = PicoTodoList_FormatAgent(&parsed);
    out->details_json = PicoTodoList_DetailsJson(&parsed);
    if (!out->output || !out->details_json)
    {
        free(out->output);
        free(out->details_json);
        out->output = JsonDup("Could not build TODO update");
        out->details_json = NULL;
        out->is_error = true;
    }
    PicoTodoList_Free(&parsed);
}

static bool TodoApply(PicoApp *app, PicoAgentId agent_id, const char *details_json, bool replay)
{
    PicoTodoList parsed;
    char *error = NULL;
    if (!PicoTodoList_ParseDetails(details_json, &parsed, &error))
    {
        free(error);
        return false;
    }
    TodoAgentState *state = FindState(agent_id, true);
    if (!state)
    {
        PicoTodoList_Free(&parsed);
        return false;
    }
    const char *old_task = state->todos.task;
    const char *new_task = parsed.task;
    bool changed = !old_task || !new_task || strcmp(old_task, new_task) != 0;
    PicoTodoList_Swap(&state->todos, &parsed);
    PicoTodoList_Free(&parsed);
    if (state->todos.count == 0)
    {
        state->expanded = false;
    }
    if (replay)
    {
        state->title_pending = state->todos.task && state->todos.task[0];
    }
    else if (changed && state->todos.task && state->todos.task[0])
    {
        state->title_pending = true;
    }
    return true;
}

static bool LastInputIsSuccessfulTodo(const PicoContextEvent *ev)
{
    if (!ev || ev->history_count <= 0 || !ev->history_json)
    {
        return false;
    }
    const char *last = ev->history_json[ev->history_count - 1];
    JsonDoc doc;
    memset(&doc, 0, sizeof(doc));
    if (!last || JsonParse(&doc, last, strlen(last)) != 0)
    {
        return false;
    }
    bool match = JsonEq(&doc, JsonObjGet(&doc, 0, "type"), "tool_result") &&
                 JsonEq(&doc, JsonObjGet(&doc, 0, "name"), "todo_update") &&
                 !JsonEq(&doc, JsonObjGet(&doc, 0, "is_error"), "true");
    JsonFree(&doc);
    return match;
}

static void TodoContext(PicoApp *app, PicoAgentId agent_id, PicoContextEvent *ev)
{
    (void)app;
    bool offered = false;
    for (int i = 0; ev && i < ev->tool_count; i++)
    {
        if (ev->tools[i].name && strcmp(ev->tools[i].name, "todo_update") == 0)
        {
            offered = true;
            break;
        }
    }
    TodoAgentState *state = FindState(agent_id, false);
    if (!ev || !offered || !state || ev->compact || state->todos.count == 0 ||
        PicoTodoList_AllCompleted(&state->todos) || LastInputIsSuccessfulTodo(ev))
    {
        return;
    }
    ev->extra_context = PicoTodoList_FormatReminder(&state->todos);
}

static Clay_Color StatusColor(PicoTodoStatus status)
{
    if (status == PICO_TODO_COMPLETED)
    {
        return COLOR_STATUS_ON;
    }
    if (status == PICO_TODO_IN_PROGRESS)
    {
        return COLOR_STATUS_RUN;
    }
    return COLOR_MUTED;
}

static const char *StatusLabel(PicoTodoStatus status)
{
    if (status == PICO_TODO_COMPLETED)
    {
        return "Completed";
    }
    if (status == PICO_TODO_IN_PROGRESS)
    {
        return "In progress";
    }
    return "Pending";
}

static void RenderTodoRows(TodoAgentState *state)
{
    for (int i = 0; i < state->todos.count; i++)
    {
        PicoTodoItem *todo = &state->todos.items[i];
        CLAY(CLAY_IDI("TodoRow", i),
             {.layout = {.layoutDirection = CLAY_TOP_TO_BOTTOM,
                         .padding = {10, 10, 8, 8},
                         .childGap = 4,
                         .sizing = {.width = CLAY_SIZING_GROW(0)}},
              .backgroundColor = COLOR_CODE_BG,
              .cornerRadius = CLAY_CORNER_RADIUS(6)})
        {
            CLAY(CLAY_IDI("TodoRowHead", i),
                 {.layout = {.layoutDirection = CLAY_LEFT_TO_RIGHT,
                             .childGap = 8,
                             .sizing = {.width = CLAY_SIZING_GROW(0)}}})
            {
                CLAY_TEXT(CStr(StatusLabel(todo->status)),
                          CLAY_TEXT_CONFIG({.fontId = FONT_BOLD,
                                            .fontSize = 12,
                                            .textColor = StatusColor(todo->status)}));
                CLAY_TEXT(CStr(todo->id),
                          CLAY_TEXT_CONFIG({.fontId = FONT_MONO, .fontSize = 12, .textColor = COLOR_LINK}));
            }
            CLAY_TEXT(CStr(todo->text),
                      CLAY_TEXT_CONFIG({.fontId = FONT_REGULAR,
                                        .fontSize = 14,
                                        .lineHeight = Pico_FontPxU16(19),
                                        .textColor = todo->status == PICO_TODO_COMPLETED ? COLOR_MUTED : COLOR_TEXT,
                                        .wrapMode = CLAY_TEXT_WRAP_WORDS}));
        }
    }
}

static void TodoRender(PicoApp *app)
{
    TodoAgentState *state = ActiveState(app);
    if (!state || state->todos.count == 0)
    {
        return;
    }

    int completed = PicoTodoList_Completed(&state->todos);
    snprintf(g_header, sizeof(g_header), "Todo %d/%d", completed, state->todos.count);

    float screen_w = (float)GetScreenWidth();
    float expanded_w = g_composer_width > 0 ? g_composer_width : TODO_EXPANDED_WIDTH;
    if (expanded_w > TODO_EXPANDED_WIDTH)
    {
        expanded_w = TODO_EXPANDED_WIDTH;
    }
    if (expanded_w > screen_w - 24.0f)
    {
        expanded_w = screen_w - 24.0f;
    }
    if (expanded_w < 220.0f)
    {
        expanded_w = 220.0f;
    }
    float expanded_h = g_space_above;
    if (expanded_h > TODO_EXPANDED_HEIGHT)
    {
        expanded_h = TODO_EXPANDED_HEIGHT;
    }
    if (expanded_h < 150.0f)
    {
        expanded_h = 150.0f;
    }
    float width = state->expanded ? expanded_w : TODO_COLLAPSED_WIDTH;
    Clay_SizingAxis height = state->expanded ? CLAY_SIZING_FIT(0, expanded_h)
                                             : CLAY_SIZING_FIXED(TODO_COLLAPSED_HEIGHT);

    CLAY(CLAY_ID("TodoPanel"),
         {.floating = {.offset = {.y = -TODO_GAP},
                       .parentId = CLAY_ID("Composer").id,
                       .zIndex = 10,
                       .attachPoints = {.element = CLAY_ATTACH_POINT_CENTER_BOTTOM,
                                        .parent = CLAY_ATTACH_POINT_CENTER_TOP},
                       .pointerCaptureMode = CLAY_POINTER_CAPTURE_MODE_CAPTURE,
                       .attachTo = CLAY_ATTACH_TO_ELEMENT_WITH_ID},
          .layout = {.layoutDirection = CLAY_TOP_TO_BOTTOM,
                     .padding = state->expanded ? (Clay_Padding){14, 14, 12, 12} : (Clay_Padding){12, 12, 8, 8},
                     .childGap = 10,
                     .childAlignment = {.x = CLAY_ALIGN_X_CENTER, .y = CLAY_ALIGN_Y_CENTER},
                     .sizing = {.width = CLAY_SIZING_FIXED(width), .height = height}},
          .backgroundColor = COLOR_CONTENT_BG,
          .cornerRadius = state->expanded ? CLAY_CORNER_RADIUS(10) : CLAY_CORNER_RADIUS(18),
          .transition = {.handler = Clay_EaseOut,
                         .duration = 0.18f,
                         .properties = CLAY_TRANSITION_PROPERTY_DIMENSIONS |
                                       CLAY_TRANSITION_PROPERTY_CORNER_RADIUS}})
    {
        CLAY(CLAY_ID("TodoPanelHeader"),
             {.layout = {.layoutDirection = state->expanded ? CLAY_TOP_TO_BOTTOM : CLAY_LEFT_TO_RIGHT,
                         .childGap = state->expanded ? 4 : 0,
                         .childAlignment = {.x = CLAY_ALIGN_X_CENTER, .y = CLAY_ALIGN_Y_CENTER},
                         .sizing = {.width = CLAY_SIZING_GROW(0)}}})
        {
            if (state->expanded && state->todos.task && state->todos.task[0])
            {
                CLAY_TEXT(CStr(state->todos.task),
                          CLAY_TEXT_CONFIG({.fontId = FONT_BOLD,
                                            .fontSize = 14,
                                            .textColor = COLOR_TEXT,
                                            .wrapMode = CLAY_TEXT_WRAP_WORDS}));
                CLAY_TEXT(CStr(g_header),
                          CLAY_TEXT_CONFIG({.fontId = FONT_REGULAR, .fontSize = 12, .textColor = COLOR_MUTED}));
            }
            else
            {
                CLAY_TEXT(CStr(g_header),
                          CLAY_TEXT_CONFIG({.fontId = FONT_BOLD, .fontSize = 14, .textColor = COLOR_TEXT}));
            }
        }
        if (state->expanded)
        {
            CLAY(CLAY_ID("TodoListScrollRow"),
                 {.layout = {.layoutDirection = CLAY_LEFT_TO_RIGHT,
                             .childGap = SCROLLBAR_GAP,
                             .sizing = {.width = CLAY_SIZING_GROW(0), .height = CLAY_SIZING_FIT(0)}}})
            {
                CLAY(CLAY_ID("TodoListScroll"),
                     {.layout = {.layoutDirection = CLAY_TOP_TO_BOTTOM,
                                 .childGap = 8,
                                 .sizing = {.width = CLAY_SIZING_GROW(0), .height = CLAY_SIZING_FIT(0)}},
                      .clip = {.vertical = true, .horizontal = false, .childOffset = Clay_GetScrollOffset()}})
                {
                    RenderTodoRows(state);
                }
                if (g_overflow)
                {
                    PicoScrollbar_Render(CLAY_STRING("TodoListScroll"), CLAY_STRING("TodoListScrollTrack"),
                                         CLAY_STRING("TodoListScrollHandle"));
                }
            }
        }
    }
}

static void TodoAfterLayout(PicoApp *app, const PicoHookEvent *event)
{
    TodoAgentState *state = FindState(event ? event->agent_id : 0, false);
    Clay_ElementData composer = Clay_GetElementData(CLAY_ID("Composer"));
    if (composer.found)
    {
        g_composer_width = composer.boundingBox.width;
        g_space_above = composer.boundingBox.y - 20.0f;
    }
    if (!state || state->todos.count == 0)
    {
        g_overflow = false;
        return;
    }
    if (state->expanded)
    {
        g_overflow = PicoScrollbar_Overflows(CLAY_STRING("TodoListScroll"));
    }
    else
    {
        g_overflow = false;
    }
    bool over_panel = Clay_PointerOver(CLAY_ID("TodoPanel"));
    bool over_header = Clay_PointerOver(CLAY_ID("TodoPanelHeader"));
    if ((!state->expanded && over_panel) || (state->expanded && over_header))
    {
        app->hovered_clickable = true;
    }
    if (!IsMouseButtonPressed(MOUSE_BUTTON_LEFT))
    {
        return;
    }
    if (!state->expanded && over_panel)
    {
        state->expanded = true;
    }
    else if (state->expanded && over_header)
    {
        state->expanded = false;
    }
    else if (state->expanded && !over_panel)
    {
        state->expanded = false;
    }
}

static void TodoFrame(PicoApp *app, float dt)
{
    (void)dt;
    for (int i = 0; app && i < PICO_MAX_AGENTS; i++)
    {
        TodoAgentState *pending = &g_states[i];
        if (!pending->agent_id || !pending->title_pending)
        {
            continue;
        }
        PicoAgent *agent = app->agents ? PicoAgentManager_Find(app->agents, pending->agent_id) : NULL;
        if (agent && pending->todos.task && pending->todos.task[0])
        {
            (void)PicoSession_LogTitle(app, agent, pending->todos.task);
        }
        pending->title_pending = false;
    }

    TodoAgentState *state = ActiveState(app);
    if (state && state->expanded)
    {
        PicoScrollbar_UpdateDrag(&g_scrollbar, CLAY_STRING("TodoListScroll"),
                                 CLAY_STRING("TodoListScrollHandle"));
        if (IsKeyPressed(KEY_ESCAPE))
        {
            state->expanded = false;
        }
    }
}

static void TodoInit(PicoApp *app)
{
    pico_add_tool(app, "todo_update",
                  "Replace the complete canonical TODO list. Set task to a succinct session title for the current "
                  "work and keep it stable unless the goal changes. Include every current item, use stable IDs and "
                  "statuses pending, in_progress, or completed, and keep at most one item in_progress.",
                  kTodoParams, TodoRun, TodoApply);
    pico_add_context_hook(app, TodoContext);
    pico_add_hook(app, PICO_HOOK_ON_SESSION_RESET, TodoReset);
    pico_add_hook(app, PICO_HOOK_ON_AGENT_DESTROY, TodoReset);
    pico_add_hook(app, PICO_HOOK_AFTER_LAYOUT, TodoAfterLayout);
    pico_add_view(app, PICO_SLOT_OVERLAY, 5, TodoRender);
}

static void TodoShutdown(PicoApp *app)
{
    (void)app;
    for (int i = 0; i < PICO_MAX_AGENTS; i++)
    {
        if (g_states[i].agent_id)
        {
            ClearState(g_states[i].agent_id);
        }
    }
}

PicoExt pico_ext_todo(void)
{
    return (PicoExt){
        .abi = PICO_EXT_ABI,
        .name = "todos",
        .description = "Agent TODO tracking",
        .init = TodoInit,
        .shutdown = TodoShutdown,
        .on_frame = TodoFrame,
    };
}
