#include "pico/plugin.h"
#include "background_model.h"
#include "workspace_internal.h"
#include "host_internal.h"
#include "json.h"
#include "scrollbar.h"
#include "agent.h"

#include "clay/clay.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define BG_PILL_HEIGHT 42.0f
#define BG_GAP 8.0f
#define BG_LIST_NAME "background-list"
#define BG_LOG_NAME "background-log"

typedef struct BackgroundState {
    PicoWorkspace *workspace;
    char pill_label[32];
    char selected_id[PICO_BG_ID_MAX];
    char log_copy[PICO_BG_LOG_MAX + 1];
    size_t log_len;
    char log_caption[96];
    bool list_overflow;
    bool log_overflow;
    PicoScrollbar list_scrollbar;
    PicoScrollbar log_scrollbar;
} BackgroundState;

static const char *kRunParams =
    "{\"type\":\"object\",\"properties\":{"
    "\"description\":{\"type\":\"string\",\"description\":\"Succinct 2-10 word summary of what the "
    "process is\"},"
    "\"command\":{\"type\":\"string\",\"description\":\"Shell command to start in the workspace and "
    "leave running. Output is only the latest 64 KiB; oldest lines are dropped.\"}},"
    "\"required\":[\"description\",\"command\"]}";

static const char *kIdParams =
    "{\"type\":\"object\",\"properties\":{"
    "\"id\":{\"type\":\"string\",\"description\":\"Background job id such as bg_1\"}},"
    "\"required\":[\"id\"]}";

static const char *kEmptyParams = "{\"type\":\"object\",\"properties\":{}}";

static Clay_String CStr(const char *s)
{
    if (!s)
    {
        s = "";
    }
    return (Clay_String){.length = (int32_t)strlen(s), .chars = s};
}

static PicoBgTable *TableForWorkspace(PicoWorkspace *workspace)
{
    return PicoWorkspace_Background(workspace);
}

static PicoBgTable *TableForCtx(PicoAgentContext *ctx)
{
    return TableForWorkspace(PicoAgentContext_Workspace(ctx));
}

static char *ExtractStr(const char *args_json, const char *key)
{
    JsonDoc doc;
    char *value;
    if (!args_json || !key || JsonParse(&doc, args_json, strlen(args_json)) != 0)
    {
        return NULL;
    }
    value = JsonObjStr(&doc, 0, key);
    JsonFree(&doc);
    return value;
}

static void SetToolResult(PicoToolResult *out, char *text, bool is_error)
{
    if (!out)
    {
        free(text);
        return;
    }
    memset(out, 0, sizeof(*out));
    out->output = text;
    out->is_error = is_error;
}

static void RunBackground(PicoAgentContext *ctx, const char *args_json, PicoToolResult *out, void *state)
{
    PicoBgTable *table = TableForCtx(ctx);
    char *description = ExtractStr(args_json, "description");
    char *command = ExtractStr(args_json, "command");
    char *error = NULL;
    char *json;
    (void)state;
    json = PicoBgTable_Spawn(table, pico_agent_context_id(ctx), pico_agent_context_workspace(ctx),
                             description, command, &error);
    free(description);
    free(command);
    if (!json)
    {
        SetToolResult(out, error ? error : JsonDup("run_background failed"), true);
        return;
    }
    SetToolResult(out, json, false);
}

static void KillBackground(PicoAgentContext *ctx, const char *args_json, PicoToolResult *out, void *state)
{
    PicoBgTable *table = TableForCtx(ctx);
    char *id = ExtractStr(args_json, "id");
    char *error = NULL;
    char *json;
    (void)state;
    json = PicoBgTable_Kill(table, pico_agent_context_id(ctx), id, &error);
    free(id);
    if (!json)
    {
        SetToolResult(out, error ? error : JsonDup("kill_background failed"), true);
        return;
    }
    SetToolResult(out, json, false);
}

static void ListBackground(PicoAgentContext *ctx, const char *args_json, PicoToolResult *out, void *state)
{
    PicoBgTable *table = TableForCtx(ctx);
    char *json;
    (void)args_json;
    (void)state;
    json = PicoBgTable_ListJson(table, pico_agent_context_id(ctx));
    if (!json)
    {
        SetToolResult(out, JsonDup("[]"), false);
        return;
    }
    SetToolResult(out, json, false);
}

static void LogBackground(PicoAgentContext *ctx, const char *args_json, PicoToolResult *out, void *state)
{
    PicoBgTable *table = TableForCtx(ctx);
    char *id = ExtractStr(args_json, "id");
    char *error = NULL;
    char *log;
    (void)state;
    log = PicoBgTable_Log(table, pico_agent_context_id(ctx), id, &error);
    free(id);
    if (!log)
    {
        SetToolResult(out, error ? error : JsonDup("log_background failed"), true);
        return;
    }
    SetToolResult(out, log, false);
}

static void BackgroundReset(PicoWorkspace *workspace, const PicoHookEvent *event, void *state)
{
    PicoBgTable *table = TableForWorkspace(workspace);
    (void)state;
    /* Reload announces live sessions with ON_SESSION_RESET while still RELOADING. */
    if (event && event->hook == PICO_HOOK_ON_SESSION_RESET && workspace &&
        workspace->state == PICO_WORKSPACE_RELOADING)
    {
        return;
    }
    if (event && event->agent_id)
    {
        PicoBgTable_ResetAgent(table, event->agent_id);
    }
}

static void CloseTopModal(PicoHost *host, const char *name)
{
    if (host && pico_ui_modal_is_top(host, name))
    {
        (void)pico_ui_modal_pop(host, name);
    }
}

static void OpenList(PicoHost *host)
{
    if (!host)
    {
        return;
    }
    if (!pico_ui_modal_has(host, BG_LIST_NAME))
    {
        (void)pico_ui_modal_push(host, BG_LIST_NAME);
    }
}

static void OpenLog(PicoHost *host, BackgroundState *s, const char *id)
{
    if (!host || !s || !id || !id[0])
    {
        return;
    }
    snprintf(s->selected_id, sizeof(s->selected_id), "%s", id);
    if (!pico_ui_modal_has(host, BG_LIST_NAME))
    {
        (void)pico_ui_modal_push(host, BG_LIST_NAME);
    }
    if (!pico_ui_modal_has(host, BG_LOG_NAME))
    {
        (void)pico_ui_modal_push(host, BG_LOG_NAME);
    }
}

static BackgroundState *ActiveState(PicoHost *app)
{
    PicoWorkspace *workspace = PicoHost_SelectedWorkspace(app);
    return (BackgroundState *)PicoPlugins_WorkspaceState(workspace, "background");
}

static void BackgroundRender(PicoWorkspace *workspace, PicoAgentId selected_agent_id, void *state)
{
    BackgroundState *s = (BackgroundState *)state;
    PicoHost *app = workspace ? workspace->host : NULL;
    PicoBgTable *table = TableForWorkspace(workspace);
    int running;
    PicoBgJobInfo jobs[PICO_BG_MAX_RECORDS];
    int job_n;
    float sw;
    float sh;
    if (!s)
    {
        s = (BackgroundState *)PicoPlugins_WorkspaceState(workspace, "background");
    }
    if (!s || !app || selected_agent_id == 0)
    {
        return;
    }

    running = PicoBgTable_RunningCount(table, selected_agent_id);
    snprintf(s->pill_label, sizeof(s->pill_label), "bg %d", running);

    if (running > 0)
    {
        CLAY(CLAY_ID("BackgroundPill"),
             {.floating = {.offset = {.y = -BG_GAP},
                           .parentId = CLAY_ID("Composer").id,
                           .zIndex = 10,
                           .attachPoints = {.element = CLAY_ATTACH_POINT_RIGHT_BOTTOM,
                                            .parent = CLAY_ATTACH_POINT_RIGHT_TOP},
                           .pointerCaptureMode = CLAY_POINTER_CAPTURE_MODE_CAPTURE,
                           .attachTo = CLAY_ATTACH_TO_ELEMENT_WITH_ID},
              .layout = {.layoutDirection = CLAY_LEFT_TO_RIGHT,
                         .padding = {14, 14, 8, 8},
                         .childAlignment = {.x = CLAY_ALIGN_X_CENTER, .y = CLAY_ALIGN_Y_CENTER},
                         .sizing = {.width = CLAY_SIZING_FIT(0), .height = CLAY_SIZING_FIXED(BG_PILL_HEIGHT)}},
              .backgroundColor = COLOR_CONTENT_BG,
              .cornerRadius = CLAY_CORNER_RADIUS(18)})
        {
            CLAY_TEXT(CStr(s->pill_label),
                      CLAY_TEXT_CONFIG({.fontId = FONT_BOLD,
                                        .fontSize = PICO_FONT_UI,
                                        .textColor = COLOR_TEXT,
                                        .wrapMode = CLAY_TEXT_WRAP_NONE}));
        }
    }

    if (!pico_ui_modal_has(app, BG_LIST_NAME) && !pico_ui_modal_has(app, BG_LOG_NAME))
    {
        return;
    }

    sw = (float)GetScreenWidth();
    sh = (float)GetScreenHeight();
    job_n = PicoBgTable_CopyJobs(table, selected_agent_id, jobs, PICO_BG_MAX_RECORDS);

    if (pico_ui_modal_has(app, BG_LIST_NAME))
    {
        CLAY(CLAY_ID("BackgroundListDim"),
             {.floating = {.attachTo = CLAY_ATTACH_TO_ROOT,
                           .zIndex = 50,
                           .attachPoints = {.element = CLAY_ATTACH_POINT_LEFT_TOP,
                                            .parent = CLAY_ATTACH_POINT_LEFT_TOP}},
              .layout = {.layoutDirection = CLAY_TOP_TO_BOTTOM,
                         .childAlignment = {.x = CLAY_ALIGN_X_CENTER, .y = CLAY_ALIGN_Y_CENTER},
                         .sizing = {.width = CLAY_SIZING_FIXED(sw), .height = CLAY_SIZING_FIXED(sh)}},
              .backgroundColor = {0, 0, 0, 140}})
        {
            CLAY(CLAY_ID("BackgroundListCard"),
                 {.layout = {.layoutDirection = CLAY_TOP_TO_BOTTOM,
                             .padding = {20, 20, 16, 16},
                             .childGap = 10,
                             .sizing = {.width = CLAY_SIZING_FIXED(520), .height = CLAY_SIZING_FIT(0, sh - 80)}},
                  .backgroundColor = COLOR_CONTENT_BG,
                  .cornerRadius = CLAY_CORNER_RADIUS(8)})
            {
                CLAY_TEXT(CLAY_STRING("Background processes"),
                          CLAY_TEXT_CONFIG({.fontId = FONT_BOLD, .fontSize = PICO_FONT_TITLE, .textColor = COLOR_TEXT}));
                CLAY_TEXT(CLAY_STRING("Logs keep only the latest 64 KiB; oldest lines are dropped."),
                          CLAY_TEXT_CONFIG({.fontId = FONT_REGULAR,
                                            .fontSize = PICO_FONT_CAPTION,
                                            .textColor = COLOR_MUTED,
                                            .wrapMode = CLAY_TEXT_WRAP_WORDS}));
                if (job_n == 0)
                {
                    CLAY_TEXT(CLAY_STRING("No background processes in this session."),
                              CLAY_TEXT_CONFIG({.fontId = FONT_REGULAR,
                                                .fontSize = PICO_FONT_UI,
                                                .textColor = COLOR_MUTED,
                                                .wrapMode = CLAY_TEXT_WRAP_WORDS}));
                }
                else
                {
                    CLAY(CLAY_ID("BackgroundListScrollRow"),
                         {.layout = {.layoutDirection = CLAY_LEFT_TO_RIGHT,
                                     .childGap = SCROLLBAR_GAP,
                                     .sizing = {.width = CLAY_SIZING_GROW(0), .height = CLAY_SIZING_FIT(0, sh - 220)}}})
                    {
                        CLAY(CLAY_ID("BackgroundListScroll"),
                             {.layout = {.layoutDirection = CLAY_TOP_TO_BOTTOM,
                                         .childGap = 8,
                                         .sizing = {.width = CLAY_SIZING_GROW(0), .height = CLAY_SIZING_FIT(0)}},
                              .clip = {.vertical = true, .horizontal = false, .childOffset = Clay_GetScrollOffset()}})
                        {
                            int i;
                            for (i = 0; i < job_n; i++)
                            {
                                PicoBgJobInfo *job = &jobs[i];
                                CLAY(CLAY_IDI("BackgroundRow", i),
                                     {.layout = {.layoutDirection = CLAY_LEFT_TO_RIGHT,
                                                 .padding = {10, 10, 8, 8},
                                                 .childGap = 8,
                                                 .childAlignment = {.y = CLAY_ALIGN_Y_CENTER},
                                                 .sizing = {.width = CLAY_SIZING_GROW(0)}},
                                      .backgroundColor = COLOR_CODE_BG,
                                      .cornerRadius = CLAY_CORNER_RADIUS(6)})
                                {
                                    CLAY(CLAY_IDI("BackgroundRowMain", i),
                                         {.layout = {.layoutDirection = CLAY_TOP_TO_BOTTOM,
                                                     .childGap = 2,
                                                     .sizing = {.width = CLAY_SIZING_GROW(0)}}})
                                    {
                                        CLAY_TEXT(CStr(job->description[0] ? job->description : job->id),
                                                  CLAY_TEXT_CONFIG({.fontId = FONT_BOLD,
                                                                    .fontSize = PICO_FONT_UI,
                                                                    .textColor = COLOR_TEXT,
                                                                    .wrapMode = CLAY_TEXT_WRAP_WORDS}));
                                        CLAY_TEXT(CStr(job->id),
                                                  CLAY_TEXT_CONFIG({.fontId = FONT_MONO,
                                                                    .fontSize = PICO_FONT_CAPTION,
                                                                    .textColor = COLOR_LINK}));
                                        CLAY_TEXT(CStr(PicoBgStatus_Name(job->status)),
                                                  CLAY_TEXT_CONFIG({.fontId = FONT_REGULAR,
                                                                    .fontSize = PICO_FONT_CAPTION,
                                                                    .textColor = job->status == PICO_BG_RUNNING
                                                                                     ? COLOR_TEXT
                                                                                     : COLOR_MUTED}));
                                    }
                                    if (job->status == PICO_BG_RUNNING)
                                    {
                                        CLAY(CLAY_IDI("BackgroundKill", i),
                                             {.layout = {.padding = {10, 10, 6, 6},
                                                         .childAlignment = {.x = CLAY_ALIGN_X_CENTER,
                                                                            .y = CLAY_ALIGN_Y_CENTER}},
                                              .backgroundColor = COLOR_CODE_BG,
                                              .cornerRadius = CLAY_CORNER_RADIUS(6)})
                                        {
                                            CLAY_TEXT(CLAY_STRING("Stop"),
                                                      CLAY_TEXT_CONFIG({.fontId = FONT_BOLD,
                                                                        .fontSize = PICO_FONT_CAPTION,
                                                                        .textColor = COLOR_TEXT}));
                                        }
                                    }
                                }
                            }
                        }
                        if (s->list_overflow)
                        {
                            PicoScrollbar_Render(CLAY_STRING("BackgroundListScroll"),
                                                 CLAY_STRING("BackgroundListScrollTrack"),
                                                 CLAY_STRING("BackgroundListScrollHandle"));
                        }
                    }
                }
            }
        }
    }

    if (pico_ui_modal_has(app, BG_LOG_NAME))
    {
        CLAY(CLAY_ID("BackgroundLogDim"),
             {.floating = {.attachTo = CLAY_ATTACH_TO_ROOT,
                           .zIndex = 51,
                           .attachPoints = {.element = CLAY_ATTACH_POINT_LEFT_TOP,
                                            .parent = CLAY_ATTACH_POINT_LEFT_TOP}},
              .layout = {.layoutDirection = CLAY_TOP_TO_BOTTOM,
                         .childAlignment = {.x = CLAY_ALIGN_X_CENTER, .y = CLAY_ALIGN_Y_CENTER},
                         .sizing = {.width = CLAY_SIZING_FIXED(sw), .height = CLAY_SIZING_FIXED(sh)}},
              .backgroundColor = {0, 0, 0, 160}})
        {
            CLAY(CLAY_ID("BackgroundLogCard"),
                 {.layout = {.layoutDirection = CLAY_TOP_TO_BOTTOM,
                             .padding = {20, 20, 16, 16},
                             .childGap = 10,
                             .sizing = {.width = CLAY_SIZING_FIXED(640), .height = CLAY_SIZING_FIT(0, sh - 80)}},
                  .backgroundColor = COLOR_CONTENT_BG,
                  .cornerRadius = CLAY_CORNER_RADIUS(8)})
            {
                CLAY(CLAY_ID("BackgroundLogHeader"),
                     {.layout = {.layoutDirection = CLAY_LEFT_TO_RIGHT,
                                 .childGap = 8,
                                 .childAlignment = {.y = CLAY_ALIGN_Y_CENTER},
                                 .sizing = {.width = CLAY_SIZING_GROW(0)}}})
                {
                    CLAY_TEXT(CStr(s->selected_id[0] ? s->selected_id : "Log"),
                              CLAY_TEXT_CONFIG({.fontId = FONT_BOLD, .fontSize = PICO_FONT_TITLE, .textColor = COLOR_TEXT}));
                    CLAY(CLAY_ID("BackgroundLogStop"),
                         {.layout = {.padding = {10, 10, 6, 6}},
                          .backgroundColor = COLOR_CODE_BG,
                          .cornerRadius = CLAY_CORNER_RADIUS(6)})
                    {
                        CLAY_TEXT(CLAY_STRING("Stop"),
                                  CLAY_TEXT_CONFIG({.fontId = FONT_BOLD, .fontSize = PICO_FONT_CAPTION, .textColor = COLOR_TEXT}));
                    }
                }
                CLAY_TEXT(CStr(s->log_caption),
                          CLAY_TEXT_CONFIG({.fontId = FONT_REGULAR,
                                            .fontSize = PICO_FONT_CAPTION,
                                            .textColor = COLOR_MUTED,
                                            .wrapMode = CLAY_TEXT_WRAP_WORDS}));
                CLAY(CLAY_ID("BackgroundLogScrollRow"),
                     {.layout = {.layoutDirection = CLAY_LEFT_TO_RIGHT,
                                 .childGap = SCROLLBAR_GAP,
                                 .sizing = {.width = CLAY_SIZING_GROW(0), .height = CLAY_SIZING_FIT(0, sh - 240)}}})
                {
                    CLAY(CLAY_ID("BackgroundLogScroll"),
                         {.layout = {.layoutDirection = CLAY_TOP_TO_BOTTOM,
                                     .sizing = {.width = CLAY_SIZING_GROW(0), .height = CLAY_SIZING_FIT(0)}},
                          .clip = {.vertical = true, .horizontal = false, .childOffset = Clay_GetScrollOffset()}})
                    {
                        CLAY_TEXT(CStr(s->log_copy[0] ? s->log_copy : "(no output)"),
                                  CLAY_TEXT_CONFIG({.fontId = FONT_MONO,
                                                    .fontSize = PICO_FONT_CAPTION,
                                                    .textColor = COLOR_TEXT,
                                                    .lineHeight = Pico_FontPxU16(PICO_FONT_CAPTION_LINE),
                                                    .wrapMode = CLAY_TEXT_WRAP_WORDS}));
                    }
                    if (s->log_overflow)
                    {
                        PicoScrollbar_Render(CLAY_STRING("BackgroundLogScroll"),
                                             CLAY_STRING("BackgroundLogScrollTrack"),
                                             CLAY_STRING("BackgroundLogScrollHandle"));
                    }
                }
            }
        }
    }
}

static void BackgroundAfterLayout(PicoHost *app, const PicoHookEvent *event, void *state)
{
    BackgroundState *s = ActiveState(app);
    PicoWorkspace *workspace = PicoHost_SelectedWorkspace(app);
    PicoBgTable *table = TableForWorkspace(workspace);
    PicoAgentId agent_id = event && event->agent_id ? event->agent_id : pico_agent_active(app);
    PicoBgJobInfo jobs[PICO_BG_MAX_RECORDS];
    int job_n;
    int i;
    (void)state;
    if (!app || !s)
    {
        return;
    }
    if (pico_ui_modal_has(app, BG_LIST_NAME))
    {
        s->list_overflow = PicoScrollbar_Overflows(CLAY_STRING("BackgroundListScroll"));
    }
    else
    {
        s->list_overflow = false;
    }
    if (pico_ui_modal_has(app, BG_LOG_NAME))
    {
        s->log_overflow = PicoScrollbar_Overflows(CLAY_STRING("BackgroundLogScroll"));
    }
    else
    {
        s->log_overflow = false;
    }

    if (Clay_PointerOver(CLAY_ID("BackgroundPill")) || Clay_PointerOver(CLAY_ID("BackgroundKill")) ||
        Clay_PointerOver(CLAY_ID("BackgroundLogStop")) || Clay_PointerOver(CLAY_ID("BackgroundRow")))
    {
        pico_host_set_hovered_clickable(app);
    }
    for (i = 0; i < PICO_BG_MAX_RECORDS; i++)
    {
        if (Clay_PointerOver(CLAY_IDI("BackgroundRow", i)) || Clay_PointerOver(CLAY_IDI("BackgroundKill", i)))
        {
            pico_host_set_hovered_clickable(app);
        }
    }

    if (!IsMouseButtonPressed(MOUSE_BUTTON_LEFT))
    {
        return;
    }

    if (pico_ui_modal_is_top(app, BG_LOG_NAME))
    {
        if (Clay_PointerOver(CLAY_ID("BackgroundLogStop")))
        {
            char *error = NULL;
            char *json = PicoBgTable_Kill(table, agent_id, s->selected_id, &error);
            free(json);
            free(error);
            return;
        }
        if (Clay_PointerOver(CLAY_ID("BackgroundLogCard")))
        {
            return;
        }
        if (Clay_PointerOver(CLAY_ID("BackgroundLogDim")))
        {
            CloseTopModal(app, BG_LOG_NAME);
        }
        return;
    }

    if (pico_ui_modal_is_top(app, BG_LIST_NAME))
    {
        job_n = PicoBgTable_CopyJobs(table, agent_id, jobs, PICO_BG_MAX_RECORDS);
        for (i = 0; i < job_n; i++)
        {
            if (Clay_PointerOver(CLAY_IDI("BackgroundKill", i)))
            {
                char *error = NULL;
                char *json = PicoBgTable_Kill(table, agent_id, jobs[i].id, &error);
                free(json);
                free(error);
                return;
            }
        }
        for (i = 0; i < job_n; i++)
        {
            if (Clay_PointerOver(CLAY_IDI("BackgroundRow", i)))
            {
                OpenLog(app, s, jobs[i].id);
                return;
            }
        }
        if (Clay_PointerOver(CLAY_ID("BackgroundListCard")))
        {
            return;
        }
        if (Clay_PointerOver(CLAY_ID("BackgroundListDim")))
        {
            CloseTopModal(app, BG_LIST_NAME);
        }
        return;
    }

    if (Clay_PointerOver(CLAY_ID("BackgroundPill")))
    {
        OpenList(app);
    }
}

static void BackgroundHostFrame(PicoHost *app, void *state, float dt)
{
    (void)state;
    (void)dt;
    if (!app)
    {
        return;
    }
    if (!IsKeyPressed(KEY_ESCAPE))
    {
        return;
    }
    if (pico_ui_modal_is_top(app, BG_LOG_NAME))
    {
        CloseTopModal(app, BG_LOG_NAME);
        return;
    }
    if (pico_ui_modal_is_top(app, BG_LIST_NAME))
    {
        CloseTopModal(app, BG_LIST_NAME);
    }
}

static void BackgroundWorkspaceFrame(PicoWorkspace *workspace, void *state, float dt)
{
    BackgroundState *s = (BackgroundState *)state;
    PicoHost *app = workspace ? workspace->host : NULL;
    PicoAgentId agent_id = app ? pico_agent_active(app) : 0;
    (void)dt;
    if (!s)
    {
        return;
    }
    if (app && pico_ui_modal_has(app, BG_LOG_NAME) && s->selected_id[0] && agent_id)
    {
        size_t n = 0;
        if (!PicoBgTable_CopyLog(TableForWorkspace(workspace), agent_id, s->selected_id, s->log_copy,
                                 sizeof(s->log_copy), &n))
        {
            s->log_copy[0] = '\0';
            n = 0;
        }
        s->log_len = n;
        snprintf(s->log_caption, sizeof(s->log_caption), "Latest 64 KiB of output; oldest lines are dropped.");
        PicoScrollbar_UpdateDrag(&s->log_scrollbar, CLAY_STRING("BackgroundLogScroll"),
                                 CLAY_STRING("BackgroundLogScrollHandle"));
    }
    if (app && pico_ui_modal_has(app, BG_LIST_NAME))
    {
        PicoScrollbar_UpdateDrag(&s->list_scrollbar, CLAY_STRING("BackgroundListScroll"),
                                 CLAY_STRING("BackgroundListScrollHandle"));
    }
}

static void BackgroundCommand(PicoWorkspace *workspace, PicoAgentId agent_id, const char *args, void *state)
{
    BackgroundState *s = (BackgroundState *)state;
    PicoHost *host = workspace ? workspace->host : NULL;
    (void)args;
    (void)agent_id;
    OpenList(host);
    if (s)
    {
        s->selected_id[0] = '\0';
    }
    PicoComposer_SetText(host, "");
    PicoHost_RequestSubmitCancel(host);
}

static int BackgroundHostInit(PicoHost *app, void **state_out)
{
    (void)state_out;
    pico_host_add_hook(app, PICO_HOOK_AFTER_LAYOUT, BackgroundAfterLayout);
    return 0;
}

static int BackgroundWorkspaceInit(PicoWorkspace *workspace, void **state_out)
{
    BackgroundState *s = (BackgroundState *)calloc(1, sizeof(BackgroundState));
    if (!s)
    {
        return 1;
    }
    s->workspace = workspace;
    if (state_out)
    {
        *state_out = s;
    }
    pico_add_tool(workspace, "run_background",
                  "Start a shell command in the workspace and leave it running. Returns an opaque job id. "
                  "Output is only the latest 64 KiB; oldest lines are dropped. Use list_background, "
                  "log_background, and kill_background to inspect or stop it.",
                  kRunParams, RunBackground, NULL);
    pico_add_tool(workspace, "kill_background", "Stop a background job started with run_background.", kIdParams,
                  KillBackground, NULL);
    pico_add_tool(workspace, "list_background",
                  "List this session's background jobs (running and recently exited).", kEmptyParams,
                  ListBackground, NULL);
    pico_add_tool(workspace, "log_background",
                  "Read captured output for a background job. Only the latest 64 KiB is kept; oldest lines "
                  "are dropped.",
                  kIdParams, LogBackground, NULL);
    pico_workspace_add_command(workspace, "background", "Show background processes", BackgroundCommand);
    pico_workspace_add_hook(workspace, PICO_HOOK_ON_SESSION_RESET, BackgroundReset);
    pico_workspace_add_hook(workspace, PICO_HOOK_ON_AGENT_DESTROY, BackgroundReset);
    pico_workspace_add_view(workspace, PICO_SLOT_OVERLAY, 7, BackgroundRender);
    return 0;
}

static void BackgroundWorkspaceShutdown(PicoWorkspace *workspace, void *state)
{
    BackgroundState *s = (BackgroundState *)state;
    PicoHost *host = workspace ? workspace->host : NULL;
    if (host)
    {
        CloseTopModal(host, BG_LOG_NAME);
        CloseTopModal(host, BG_LIST_NAME);
    }
    free(s);
}

PicoExt pico_ext_background(void)
{
    return (PicoExt){
        .abi = PICO_EXT_ABI,
        .name = "background",
        .description = "Session-local background processes",
        .host_init = BackgroundHostInit,
        .host_on_frame = BackgroundHostFrame,
        .workspace_init = BackgroundWorkspaceInit,
        .workspace_shutdown = BackgroundWorkspaceShutdown,
        .workspace_on_frame = BackgroundWorkspaceFrame,
    };
}
