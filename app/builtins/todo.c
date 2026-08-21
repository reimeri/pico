#include "pico/plugin.h"
#include "todo.h"
#include "todo_model.h"
#include "json.h"

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

static PicoTodoList g_todos;
static bool g_expanded;
static float g_composer_width = TODO_EXPANDED_WIDTH;
static float g_space_above = TODO_EXPANDED_HEIGHT;
static char g_header[64];

static const char *kTodoParams =
    "{\"type\":\"object\",\"properties\":{\"todos\":{\"type\":\"array\",\"maxItems\":30,"
    "\"description\":\"The complete canonical TODO list. This replaces the previous list atomically.\","
    "\"items\":{\"type\":\"object\",\"properties\":{"
    "\"id\":{\"type\":\"string\",\"minLength\":1,\"maxLength\":64,"
    "\"pattern\":\"^[A-Za-z0-9][A-Za-z0-9._-]{0,63}$\"},"
    "\"text\":{\"type\":\"string\",\"minLength\":1,\"maxLength\":300},"
    "\"status\":{\"type\":\"string\",\"enum\":[\"pending\",\"in_progress\",\"completed\"]}},"
    "\"required\":[\"id\",\"text\",\"status\"]}}},"
    "\"explanation\":{\"type\":\"string\",\"maxLength\":300}},\"required\":[\"todos\"]}";

static Clay_String CStr(const char *s)
{
    if (!s)
    {
        s = "";
    }
    return (Clay_String){.length = (int32_t)strlen(s), .chars = s};
}

bool PicoTodo_IsExpanded(void)
{
    return g_expanded && g_todos.count > 0;
}

static void TodoReset(PicoApp *app)
{
    (void)app;
    PicoTodoList_Free(&g_todos);
    g_expanded = false;
}

static void TodoRun(PicoApp *app, const char *args_json, PicoToolResult *out)
{
    (void)app;
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

static bool TodoApply(PicoApp *app, const char *details_json, bool replay)
{
    (void)app;
    (void)replay;
    PicoTodoList parsed;
    char *error = NULL;
    if (!PicoTodoList_ParseDetails(details_json, &parsed, &error))
    {
        free(error);
        return false;
    }
    PicoTodoList_Swap(&g_todos, &parsed);
    PicoTodoList_Free(&parsed);
    if (g_todos.count == 0)
    {
        g_expanded = false;
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

static void TodoContext(PicoApp *app, PicoContextEvent *ev)
{
    (void)app;
    if (!ev || ev->compact || g_todos.count == 0 || PicoTodoList_AllCompleted(&g_todos) ||
        LastInputIsSuccessfulTodo(ev))
    {
        return;
    }
    ev->extra_context = PicoTodoList_FormatReminder(&g_todos);
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

static void RenderTodoRows(void)
{
    for (int i = 0; i < g_todos.count; i++)
    {
        PicoTodoItem *todo = &g_todos.items[i];
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
                                        .lineHeight = 19,
                                        .textColor = todo->status == PICO_TODO_COMPLETED ? COLOR_MUTED : COLOR_TEXT,
                                        .wrapMode = CLAY_TEXT_WRAP_WORDS}));
        }
    }
}

static void TodoRender(PicoApp *app)
{
    (void)app;
    if (g_todos.count == 0)
    {
        return;
    }

    int completed = PicoTodoList_Completed(&g_todos);
    snprintf(g_header, sizeof(g_header), "Todo %d/%d", completed, g_todos.count);

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
    float width = g_expanded ? expanded_w : TODO_COLLAPSED_WIDTH;
    float height = g_expanded ? expanded_h : TODO_COLLAPSED_HEIGHT;

    CLAY(CLAY_ID("TodoPanel"),
         {.floating = {.offset = {.y = -TODO_GAP},
                       .parentId = CLAY_ID("Composer").id,
                       .zIndex = 10,
                       .attachPoints = {.element = CLAY_ATTACH_POINT_CENTER_BOTTOM,
                                        .parent = CLAY_ATTACH_POINT_CENTER_TOP},
                       .pointerCaptureMode = CLAY_POINTER_CAPTURE_MODE_CAPTURE,
                       .attachTo = CLAY_ATTACH_TO_ELEMENT_WITH_ID},
          .layout = {.layoutDirection = CLAY_TOP_TO_BOTTOM,
                     .padding = g_expanded ? (Clay_Padding){14, 14, 12, 12} : (Clay_Padding){12, 12, 8, 8},
                     .childGap = 10,
                     .childAlignment = {.x = CLAY_ALIGN_X_CENTER, .y = CLAY_ALIGN_Y_CENTER},
                     .sizing = {.width = CLAY_SIZING_FIXED(width), .height = CLAY_SIZING_FIXED(height)}},
          .backgroundColor = COLOR_CONTENT_BG,
          .cornerRadius = g_expanded ? CLAY_CORNER_RADIUS(10) : CLAY_CORNER_RADIUS(18),
          .border = {.color = COLOR_HR, .width = {1, 1, 1, 1}},
          .transition = {.handler = Clay_EaseOut,
                         .duration = 0.18f,
                         .properties = CLAY_TRANSITION_PROPERTY_DIMENSIONS |
                                       CLAY_TRANSITION_PROPERTY_CORNER_RADIUS}})
    {
        CLAY(CLAY_ID("TodoPanelHeader"),
             {.layout = {.childAlignment = {.x = CLAY_ALIGN_X_CENTER, .y = CLAY_ALIGN_Y_CENTER},
                         .sizing = {.width = CLAY_SIZING_GROW(0)}}})
        {
            CLAY_TEXT(CStr(g_header),
                      CLAY_TEXT_CONFIG({.fontId = FONT_BOLD, .fontSize = 14, .textColor = COLOR_TEXT}));
        }
        if (g_expanded)
        {
            CLAY(CLAY_ID("TodoListScroll"),
                 {.layout = {.layoutDirection = CLAY_TOP_TO_BOTTOM,
                             .padding = {0, 6, 0, 0},
                             .childGap = 8,
                             .sizing = {.width = CLAY_SIZING_GROW(0), .height = CLAY_SIZING_GROW(0)}},
                  .clip = {.vertical = true, .horizontal = false, .childOffset = Clay_GetScrollOffset()}})
            {
                RenderTodoRows();
            }
        }
    }
}

static void TodoAfterLayout(PicoApp *app)
{
    Clay_ElementData composer = Clay_GetElementData(CLAY_ID("Composer"));
    if (composer.found)
    {
        g_composer_width = composer.boundingBox.width;
        g_space_above = composer.boundingBox.y - 20.0f;
    }
    if (g_todos.count == 0)
    {
        return;
    }
    bool over_panel = Clay_PointerOver(CLAY_ID("TodoPanel"));
    bool over_header = Clay_PointerOver(CLAY_ID("TodoPanelHeader"));
    if ((!g_expanded && over_panel) || (g_expanded && over_header))
    {
        app->hovered_clickable = true;
    }
    if (!IsMouseButtonPressed(MOUSE_BUTTON_LEFT))
    {
        return;
    }
    if (!g_expanded && over_panel)
    {
        g_expanded = true;
    }
    else if (g_expanded && over_header)
    {
        g_expanded = false;
    }
    else if (g_expanded && !over_panel)
    {
        g_expanded = false;
    }
}

static void TodoFrame(PicoApp *app, float dt)
{
    (void)app;
    (void)dt;
    if (g_expanded && IsKeyPressed(KEY_ESCAPE))
    {
        g_expanded = false;
    }
}

static void TodoInit(PicoApp *app)
{
    pico_add_tool(app, "todo_update",
                  "Replace the complete canonical TODO list. Include every current item, use stable IDs and statuses "
                  "pending, in_progress, or completed, and keep at most one item in_progress.",
                  kTodoParams, TodoRun, TodoApply);
    pico_add_context_hook(app, TodoContext);
    pico_add_hook(app, PICO_HOOK_ON_SESSION_RESET, TodoReset);
    pico_add_hook(app, PICO_HOOK_AFTER_LAYOUT, TodoAfterLayout);
    pico_add_view(app, PICO_SLOT_OVERLAY, 5, TodoRender);
}

static void TodoShutdown(PicoApp *app)
{
    TodoReset(app);
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
