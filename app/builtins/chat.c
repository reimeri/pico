#include "pico/plugin.h"

#include "../agent_internal.h"
#include "agent.h"
#include "agent_manager.h"
#include "pico/md_view.h"
#include "chat_sel.h"
#include "chat.h"
#include "json.h"
#include "richtext.h"
#include "settings.h"

#include "clay/clay.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define TOOL_OUTPUT_MAX_LINES 100

typedef struct TranscriptView {
    PicoApp *app;
    const PicoMessage *messages;
    int message_count;
    PicoAgentState state;
    const char *activity;
    PicoAgent *owner;
    int id_ns;
    bool selectable;
} TranscriptView;

typedef struct InspectFrame {
    PicoAgentId parent_id;
    PicoAgentId child_id;
    char session_id[40];
    char *tool_call_id;
    char *fallback;
} InspectFrame;

static InspectFrame g_inspect[PICO_MAX_DELEGATION_DEPTH + 1];
static int g_inspect_n;
static bool g_inspect_follow = true;
static bool g_inspect_pressed_dim;
static bool g_inspect_pressed_back;
static bool g_inspect_pressed_tool;
static int g_inspect_tool_msg;
static int g_inspect_tool_idx;

static bool IsSubagentTool(const PicoTraceLine *line)
{
    return line && line->is_tool && line->tool_name && strcmp(line->tool_name, "subagent") == 0;
}

static Clay_String ViewCStr(const char *s)
{
    if (!s)
    {
        s = "";
    }
    return (Clay_String){.length = (int32_t)strlen(s), .chars = s};
}

static void ViewText(const TranscriptView *view, Clay_String text, Clay_TextElementConfig config)
{
    if (view && view->selectable)
    {
        PicoChatSel_Text(text, config);
        return;
    }
    CLAY_TEXT(text, CLAY_TEXT_CONFIG(config));
}

static void ViewBreak(const TranscriptView *view)
{
    if (view && view->selectable)
    {
        PicoChatSel_Break();
    }
}

static void ViewGlue(const TranscriptView *view, const char *s)
{
    if (view && view->selectable)
    {
        PicoChatSel_Glue(s);
    }
}

static float ChatWidth(PicoApp *app)
{
    float width = (float)GetScreenWidth() - CONTENT_PADDING - 12;
    if (app->chat_overflow)
    {
        width -= (float)(SCROLLBAR_WIDTH + SCROLLBAR_GAP);
    }
    width -= 36; // message bubble padding
    if (width < 50)
    {
        width = 50;
    }
    return width;
}

static Clay_ElementId MessageId(const TranscriptView *view, int message_index)
{
    switch (view ? view->id_ns : 0)
    {
    case 0: return CLAY_IDI("MsgMain", message_index);
    case 1: return CLAY_IDI("MsgInspect1", message_index);
    case 2: return CLAY_IDI("MsgInspect2", message_index);
    case 3: return CLAY_IDI("MsgInspect3", message_index);
    case 4: return CLAY_IDI("MsgInspect4", message_index);
    default: return CLAY_IDI("MsgInspect5", message_index);
    }
}

static Clay_ElementId ToolElementId(const TranscriptView *view, int message_index,
                                    int trace_index, Clay_String label)
{
    Clay_ElementId message = MessageId(view, message_index);
    return Clay__HashStringWithOffset(label, (uint32_t)trace_index, message.id);
}

static Clay_ElementId ToolRowId(const TranscriptView *view, int message_index, int trace_index)
{
    return ToolElementId(view, message_index, trace_index, CLAY_STRING("ToolRow"));
}

static Clay_ElementId ToolStatusId(const TranscriptView *view, int message_index, int trace_index)
{
    return ToolElementId(view, message_index, trace_index, CLAY_STRING("ToolStatus"));
}

static Clay_ElementId ToolChevronId(const TranscriptView *view, int message_index, int trace_index)
{
    return ToolElementId(view, message_index, trace_index, CLAY_STRING("ToolChevron"));
}

static void RenderToolOutput(const TranscriptView *view, const char *output)
{
    const char *text = (output && output[0]) ? output : "(empty)";
    CLAY_AUTO_ID({.layout = {.layoutDirection = CLAY_TOP_TO_BOTTOM,
                             .padding = {12, 12, 10, 10},
                             .childGap = 1,
                             .sizing = {.width = CLAY_SIZING_GROW(0)}},
                  .backgroundColor = COLOR_CODE_BG,
                  .cornerRadius = CLAY_CORNER_RADIUS(6)})
    {
        const char *line = text;
        int shown = 0;
        while (line && shown < TOOL_OUTPUT_MAX_LINES)
        {
            const char *newline = strchr(line, '\n');
            int length = newline ? (int)(newline - line) : (int)strlen(line);
            Clay_String s = {.length = length > 0 ? length : 1, .chars = length > 0 ? line : " "};
            ViewText(view, s, (Clay_TextElementConfig){.fontId = FONT_MONO,
                                                       .fontSize = 14,
                                                       .lineHeight = 18,
                                                       .textColor = COLOR_CODE_TEXT,
                                                       .wrapMode = CLAY_TEXT_WRAP_WORDS});
            ViewBreak(view);
            line = newline ? newline + 1 : NULL;
            shown++;
        }
        if (line)
        {
            CLAY_TEXT(CLAY_STRING("…"), CLAY_TEXT_CONFIG({.fontId = FONT_MONO,
                                                         .fontSize = 14,
                                                         .textColor = COLOR_MUTED,
                                                         .wrapMode = CLAY_TEXT_WRAP_NONE}));
        }
    }
}

static void RenderThinkSummary(const TranscriptView *view, PicoTraceLine *line, float available_width)
{
    RichTextStyle style = {
        .font_regular = FONT_ITALIC,
        .font_bold = FONT_BOLD_ITALIC,
        .font_italic = FONT_ITALIC,
        .font_bold_italic = FONT_BOLD_ITALIC,
        .font_mono = FONT_MONO,
        .font_size = 15,
        .line_height = 18,
        .text_color = COLOR_MUTED,
        .code_text_color = COLOR_MUTED,
        .code_bg_color = COLOR_CODE_BG,
        .link_color = COLOR_MUTED,
        .link_hover_color = COLOR_MUTED,
    };
    RichTextEmitState emit = {0};
    for (int b = 0; b < line->doc.block_count; b++)
    {
        MdBlock *block = &line->doc.blocks[b];
        if (!block->chunks || block->chunk_count <= 0)
        {
            continue;
        }
        RichText_RenderParagraph(block, &line->doc.arena, available_width, &style, &emit);
    }
    ViewBreak(view);
}

static Clay_Color ToolStatusColor(const PicoTraceLine *line)
{
    if (!line->tool_output)
    {
        return COLOR_STATUS_RUN;
    }
    if (line->tool_error)
    {
        return COLOR_STATUS_ERR;
    }
    return COLOR_STATUS_ON;
}

static const char *SubagentActivity(PicoApp *app, const PicoTraceLine *line)
{
    PicoAgent *child = line->child_id ? PicoAgentManager_Find(app->agents, line->child_id) : NULL;
    if (child)
    {
        return child->activity[0] ? child->activity : "Thinking…";
    }
    return "Running…";
}

static bool OwnerWaiting(const TranscriptView *view)
{
    if (view->owner)
    {
        return view->owner->state == PICO_AGENT_TOOL_WAIT || view->owner->state == PICO_AGENT_LLM_WAIT;
    }
    return view->state == PICO_AGENT_TOOL_WAIT || view->state == PICO_AGENT_LLM_WAIT;
}

static bool OwnerHasAsk(const TranscriptView *view)
{
    PicoToolAsk ask;
    if (view->owner)
    {
        return PicoAgent_PendingAsk(view->owner, &ask);
    }
    return pico_tool_pending_ask(view->app, &ask);
}

static void RenderToolLine(const TranscriptView *view, PicoTraceLine *line, int message_index,
                           int trace_index)
{
    if (!line->tool_name || !line->tool_name[0])
    {
        return;
    }
    Clay_ElementId row_id = ToolRowId(view, message_index, trace_index);
    bool hovered = Clay_PointerOver(row_id);
    if (hovered)
    {
        view->app->hovered_tool = true;
    }
    Clay_Color name_color = hovered ? COLOR_TOOL_NAME_HOVER : COLOR_TOOL_NAME;
    Clay_Color args_color = hovered ? COLOR_TOOL_ARGS_HOVER : COLOR_TOOL_ARGS;
    Clay_String name = ViewCStr(line->tool_name);
    Clay_String args = ViewCStr(line->tool_args);
    bool subagent = IsSubagentTool(line);

    CLAY_AUTO_ID({.layout = {.layoutDirection = CLAY_TOP_TO_BOTTOM,
                             .childGap = 6,
                             .sizing = {.width = CLAY_SIZING_GROW(0)}}})
    {
        CLAY(row_id, {.layout = {.layoutDirection = CLAY_LEFT_TO_RIGHT,
                                 .childGap = 8,
                                 .childAlignment = {.y = CLAY_ALIGN_Y_CENTER},
                                 .sizing = {.width = CLAY_SIZING_GROW(0)}}})
        {
            CLAY(ToolStatusId(view, message_index, trace_index),
                 {.layout = {.sizing = {.width = CLAY_SIZING_FIXED(8), .height = CLAY_SIZING_FIXED(8)}},
                  .backgroundColor = ToolStatusColor(line),
                  .cornerRadius = CLAY_CORNER_RADIUS(4)})
            {
            }
            ViewText(view, name, (Clay_TextElementConfig){.fontId = FONT_REGULAR,
                                                          .fontSize = 15,
                                                          .textColor = name_color,
                                                          .wrapMode = CLAY_TEXT_WRAP_NONE});
            if (args.length > 0)
            {
                ViewGlue(view, " ");
                CLAY_AUTO_ID({.layout = {.sizing = {.width = CLAY_SIZING_GROW(0)}}})
                {
                    ViewText(view, args, (Clay_TextElementConfig){.fontId = FONT_REGULAR,
                                                                  .fontSize = 15,
                                                                  .textColor = args_color,
                                                                  .wrapMode = CLAY_TEXT_WRAP_WORDS});
                }
            }
            else
            {
                CLAY_AUTO_ID({.layout = {.sizing = {.width = CLAY_SIZING_GROW(0)}}}) {}
            }
            CLAY(ToolChevronId(view, message_index, trace_index),
                 {.layout = {.sizing = {.width = CLAY_SIZING_FIXED(14), .height = CLAY_SIZING_GROW(0)}}})
            {
            }
        }
        if (subagent && !line->tool_output)
        {
            const char *activity = SubagentActivity(view->app, line);
            ViewText(view, ViewCStr(activity),
                     (Clay_TextElementConfig){.fontId = FONT_ITALIC,
                                              .fontSize = 14,
                                              .textColor = COLOR_MUTED,
                                              .wrapMode = CLAY_TEXT_WRAP_WORDS});
            ViewBreak(view);
        }
        else if (!subagent && line->expanded)
        {
            const char *output = line->tool_output;
            if (!output && OwnerWaiting(view))
            {
                output = OwnerHasAsk(view) ? "Waiting for you…" : "Running…";
            }
            RenderToolOutput(view, output);
        }
    }
    ViewBreak(view);
}

static Clay_String EmptyCStr(const char *s)
{
    if (!s)
    {
        s = "";
    }
    return (Clay_String){.length = (int32_t)strlen(s), .chars = s};
}

static void RenderEmptyCard(int id, Clay_String title, const char **items, int n)
{
    CLAY(CLAY_IDI("EmptyCard", id),
         {.layout = {.layoutDirection = CLAY_TOP_TO_BOTTOM,
                     .padding = {16, 16, 16, 16},
                     .childGap = 8,
                     .sizing = {.width = CLAY_SIZING_GROW(0), .height = CLAY_SIZING_GROW(0)}},
          .backgroundColor = COLOR_CONTENT_BG,
          .cornerRadius = CLAY_CORNER_RADIUS(8)})
    {
        CLAY_TEXT(title, CLAY_TEXT_CONFIG({.fontId = FONT_BOLD, .fontSize = 15, .textColor = COLOR_TEXT}));
        if (n <= 0)
        {
            CLAY_TEXT(CLAY_STRING("None"), CLAY_TEXT_CONFIG({.fontId = FONT_REGULAR,
                                                             .fontSize = 13,
                                                             .textColor = COLOR_MUTED,
                                                             .wrapMode = CLAY_TEXT_WRAP_NONE}));
        }
        else
        {
            for (int i = 0; i < n; i++)
            {
                CLAY_TEXT(EmptyCStr(items[i]), CLAY_TEXT_CONFIG({.fontId = FONT_REGULAR,
                                                                 .fontSize = 13,
                                                                 .textColor = COLOR_TEXT,
                                                                 .wrapMode = CLAY_TEXT_WRAP_WORDS}));
            }
        }
    }
}

static void RenderEmptyCards(PicoApp *app)
{
    const char *tools[PICO_MAX_TOOLS];
    int tool_n = 0;
    for (int i = 0; i < app->tool_count && tool_n < PICO_MAX_TOOLS; i++)
    {
        if (app->tools[i].name && app->tools[i].name[0])
        {
            tools[tool_n++] = app->tools[i].name;
        }
    }
    const char *ctx[8];
    int ctx_n = PicoSettings_LoadedContext(app, ctx, 8);
    bool narrow = GetScreenWidth() < 720;
    CLAY(CLAY_ID("EmptyCards"),
         {.layout = {.layoutDirection = narrow ? CLAY_TOP_TO_BOTTOM : CLAY_LEFT_TO_RIGHT,
                     .childGap = 12,
                     .sizing = {.width = CLAY_SIZING_GROW(0)}}})
    {
        RenderEmptyCard(0, CLAY_STRING("Tools"), tools, tool_n);
        RenderEmptyCard(1, CLAY_STRING("Context"), ctx, ctx_n);
        RenderEmptyCard(2, CLAY_STRING("Skills"), NULL, 0);
    }
}

static bool EmptyReplaced(PicoApp *app)
{
    for (int i = 0; i < app->empty_view_count; i++)
    {
        if (app->empty_views[i].kind == PICO_EMPTY_REPLACE)
        {
            return true;
        }
    }
    return false;
}

static void RunEmpty(PicoApp *app, PicoEmptyKind kind)
{
    for (int i = 0; i < app->empty_view_count; i++)
    {
        if (app->empty_views[i].kind == kind && app->empty_views[i].render)
        {
            app->empty_views[i].render(app);
        }
    }
}

static void RenderEmptyState(PicoApp *app)
{
    if (EmptyReplaced(app))
    {
        RunEmpty(app, PICO_EMPTY_REPLACE);
        return;
    }
    CLAY(CLAY_ID("EmptyStack"),
         {.layout = {.layoutDirection = CLAY_TOP_TO_BOTTOM,
                     .childGap = 12,
                     .sizing = {.width = CLAY_SIZING_GROW(0, 900)}}})
    {
        RunEmpty(app, PICO_EMPTY_ABOVE);
        RenderEmptyCards(app);
        RunEmpty(app, PICO_EMPTY_BELOW);
    }
}

static void RenderTranscript(const TranscriptView *view, float available_width)
{
    for (int i = 0; i < view->message_count; i++)
    {
        PicoMessage *msg = (PicoMessage *)&view->messages[i];
        bool user = msg->role == PICO_ROLE_USER;
        Clay_Color bg = user ? COLOR_USER_BG : COLOR_ASSISTANT_BG;
        CLAY(MessageId(view, i),
             {.layout = {.layoutDirection = CLAY_TOP_TO_BOTTOM,
                         .padding = {16, 16, 12, 12},
                         .childGap = 8,
                         .sizing = {.width = CLAY_SIZING_GROW(0)}},
              .backgroundColor = bg,
              .cornerRadius = user ? CLAY_CORNER_RADIUS(8) : CLAY_CORNER_RADIUS(0)})
        {
            if (view->selectable)
            {
                PicoChatSel_SetMessage(i);
            }
            bool has_trace = msg->trace_count > 0;
            bool has_source = msg->source && msg->source[0];
            bool live = !user && i == view->message_count - 1 && OwnerWaiting(view);
            for (int t = 0; t < msg->trace_count; t++)
            {
                PicoTraceLine *line = &msg->trace[t];
                if (line->is_tool)
                {
                    RenderToolLine(view, line, i, t);
                    continue;
                }
                const char *text = line->text;
                if (!text || !text[0])
                {
                    continue;
                }
                if (line->think_steps >= 1 && line->doc.block_count > 0)
                {
                    RenderThinkSummary(view, line, available_width);
                    continue;
                }
                ViewText(view, ViewCStr(text),
                         (Clay_TextElementConfig){.fontId = FONT_ITALIC,
                                                  .fontSize = 15,
                                                  .textColor = COLOR_MUTED,
                                                  .wrapMode = CLAY_TEXT_WRAP_WORDS});
                ViewBreak(view);
            }
            if (!has_trace && live && !has_source)
            {
                const char *label = view->activity && view->activity[0] ? view->activity : "Thinking…";
                ViewText(view, ViewCStr(label),
                         (Clay_TextElementConfig){.fontId = FONT_ITALIC,
                                                  .fontSize = 15,
                                                  .textColor = COLOR_MUTED,
                                                  .wrapMode = CLAY_TEXT_WRAP_WORDS});
                ViewBreak(view);
            }
            if (has_source)
            {
                MdView_RenderDocument(&msg->doc, (view->id_ns + 1) * 100000 + (i + 1) * 4096,
                                      available_width);
            }
            if (view->selectable)
            {
                PicoChatSel_SetMessage(-1);
            }
        }
    }
}

void PicoChat_Render(PicoApp *app)
{
    app->hovered_tool = false;
    PicoChatSel_BeginFrame(PicoApp_ActiveAgent(app)->message_count);
    CLAY(CLAY_ID("ChatRow"),
         {.layout = {.layoutDirection = CLAY_LEFT_TO_RIGHT,
                     .childGap = SCROLLBAR_GAP,
                     .sizing = {.width = CLAY_SIZING_GROW(0), .height = CLAY_SIZING_GROW(0)}}})
    {
        CLAY(CLAY_ID("ChatScroll"),
             {.layout = {.layoutDirection = CLAY_TOP_TO_BOTTOM,
                         .sizing = {.width = CLAY_SIZING_GROW(0), .height = CLAY_SIZING_GROW(0)}},
              .clip = {.vertical = true, .horizontal = false, .childOffset = Clay_GetScrollOffset()}})
        {
            bool empty = PicoApp_ActiveAgent(app)->message_count == 0;
            Clay_ChildAlignment align = empty ? (Clay_ChildAlignment){.x = CLAY_ALIGN_X_CENTER, .y = CLAY_ALIGN_Y_CENTER}
                                              : (Clay_ChildAlignment){0};
            Clay_Sizing content_size = {.width = CLAY_SIZING_GROW(0)};
            if (empty)
            {
                content_size.height = CLAY_SIZING_GROW(0);
            }
            CLAY(CLAY_ID("ChatContent"),
                 {.layout = {.layoutDirection = CLAY_TOP_TO_BOTTOM,
                             .childGap = 16,
                             .padding = {4, 4, 8, 8},
                             .childAlignment = align,
                             .sizing = content_size}})
            {
                if (empty)
                {
                    RenderEmptyState(app);
                }
                float available_width = ChatWidth(app);
                PicoAgent *active = PicoApp_ActiveAgent(app);
                TranscriptView view = {
                    .app = app,
                    .messages = active->messages,
                    .message_count = active->message_count,
                    .state = active->state,
                    .activity = active->activity,
                    .owner = active,
                    .id_ns = 0,
                    .selectable = true,
                };
                RenderTranscript(&view, available_width);
            }
        }

        if (app->chat_overflow)
        {
            Clay_ScrollContainerData scroll_data =
                Clay_GetScrollContainerData(Clay_GetElementId(CLAY_STRING("ChatScroll")));
            float track_h = scroll_data.found ? scroll_data.scrollContainerDimensions.height : 0;
            float content_h = scroll_data.found ? scroll_data.contentDimensions.height : 1;
            float thumb_h = content_h > 0 ? (track_h / content_h) * track_h : track_h;
            if (thumb_h < 16)
            {
                thumb_h = 16;
            }
            float thumb_y = 0;
            if (scroll_data.found && scroll_data.scrollPosition && content_h > 0)
            {
                thumb_y = -(scroll_data.scrollPosition->y / content_h) * track_h;
            }

            CLAY(CLAY_ID("ChatScrollTrack"),
                 {.layout = {.sizing = {.width = CLAY_SIZING_FIXED((float)SCROLLBAR_WIDTH),
                                        .height = CLAY_SIZING_GROW(0)}}})
            {
                CLAY(CLAY_ID("ChatScrollBarHandle"),
                     {.floating = {.attachTo = CLAY_ATTACH_TO_PARENT,
                                   .offset = {.y = thumb_y},
                                   .zIndex = 1,
                                   .attachPoints = {.element = CLAY_ATTACH_POINT_LEFT_TOP,
                                                    .parent = CLAY_ATTACH_POINT_LEFT_TOP}},
                      .layout = {.sizing = {.width = CLAY_SIZING_FIXED((float)SCROLLBAR_WIDTH),
                                            .height = CLAY_SIZING_FIXED(thumb_h)}},
                      .backgroundColor = Clay_PointerOver(Clay_GetElementId(CLAY_STRING("ChatScrollBarHandle")))
                                             ? COLOR_SCROLLBAR_HOVER
                                             : COLOR_SCROLLBAR,
                      .cornerRadius = CLAY_CORNER_RADIUS((float)SCROLLBAR_WIDTH / 2.0f)})
                {
                }
            }
        }
    }
}

static TranscriptView MainTranscriptView(PicoApp *app)
{
    PicoAgent *active = PicoApp_ActiveAgent(app);
    TranscriptView view = {
        .app = app,
        .messages = active->messages,
        .message_count = active->message_count,
        .state = active->state,
        .activity = active->activity,
        .owner = active,
        .id_ns = 0,
        .selectable = true,
    };
    return view;
}

bool PicoChat_InspectIsOpen(void)
{
    return g_inspect_n > 0;
}

static void InspectPop(void)
{
    if (g_inspect_n <= 0)
    {
        return;
    }
    g_inspect_n--;
    free(g_inspect[g_inspect_n].tool_call_id);
    free(g_inspect[g_inspect_n].fallback);
    memset(&g_inspect[g_inspect_n], 0, sizeof(g_inspect[g_inspect_n]));
    g_inspect_follow = true;
}

void PicoChat_InspectClose(void)
{
    while (g_inspect_n > 0)
    {
        InspectPop();
    }
    g_inspect_pressed_dim = false;
    g_inspect_pressed_back = false;
    g_inspect_pressed_tool = false;
}

static void InspectCaptureLine(InspectFrame *frame, const PicoTraceLine *line)
{
    if (!frame || !line)
    {
        return;
    }
    if (line->child_id)
    {
        frame->child_id = line->child_id;
    }
    if (line->child_session_id[0])
    {
        snprintf(frame->session_id, sizeof(frame->session_id), "%s", line->child_session_id);
    }
    if (!frame->session_id[0] && line->tool_output && line->tool_output[0])
    {
        JsonDoc doc;
        if (JsonParse(&doc, line->tool_output, strlen(line->tool_output)) == 0)
        {
            char *id = JsonObjStr(&doc, 0, "session_id");
            if (id && id[0])
            {
                snprintf(frame->session_id, sizeof(frame->session_id), "%s", id);
            }
            free(id);
            JsonFree(&doc);
        }
    }
    if (!frame->fallback && line->tool_output && line->tool_output[0])
    {
        frame->fallback = JsonDup(line->tool_output);
    }
}

static void InspectPushLine(const PicoTraceLine *line, PicoAgentId parent_id)
{
    if (!line || g_inspect_n >= (int)(sizeof(g_inspect) / sizeof(g_inspect[0])))
    {
        return;
    }
    InspectFrame *frame = &g_inspect[g_inspect_n];
    memset(frame, 0, sizeof(*frame));
    frame->parent_id = parent_id;
    frame->tool_call_id = line->tool_call_id ? JsonDup(line->tool_call_id) : NULL;
    InspectCaptureLine(frame, line);
    g_inspect_n++;
    g_inspect_follow = true;
}

static void InspectRefreshFrame(PicoApp *app, InspectFrame *frame)
{
    if (!app || !app->agents || !frame || frame->child_id || frame->session_id[0] ||
        frame->fallback || !frame->parent_id || !frame->tool_call_id)
    {
        return;
    }
    PicoAgent *parent = PicoAgentManager_Find(app->agents, frame->parent_id);
    for (int i = parent ? parent->message_count - 1 : -1; i >= 0; i--)
    {
        PicoMessage *message = &parent->messages[i];
        for (int t = message->trace_count - 1; t >= 0; t--)
        {
            PicoTraceLine *line = &message->trace[t];
            if (line->tool_call_id && strcmp(line->tool_call_id, frame->tool_call_id) == 0)
            {
                InspectCaptureLine(frame, line);
                return;
            }
        }
    }
}

static bool InspectCurrent(PicoApp *app, PicoSubagentInspect *out, const char **fallback)
{
    InspectFrame *frame = &g_inspect[g_inspect_n - 1];
    InspectRefreshFrame(app, frame);
    PicoTraceLine line;
    memset(&line, 0, sizeof(line));
    line.child_id = frame->child_id;
    snprintf(line.child_session_id, sizeof(line.child_session_id), "%s", frame->session_id);
    line.tool_output = frame->fallback;
    if (fallback)
    {
        *fallback = frame->fallback;
    }
    return PicoAgentManager_InspectSubagent(app, &line, out);
}

static void InspectFollowScroll(void)
{
    if (g_inspect_n <= 0 || !g_inspect_follow)
    {
        return;
    }
    Clay_ScrollContainerData data =
        Clay_GetScrollContainerData(Clay_GetElementId(CLAY_STRING("SubagentChatScroll")));
    if (data.found && data.scrollPosition &&
        data.contentDimensions.height > data.scrollContainerDimensions.height)
    {
        data.scrollPosition->y = data.scrollContainerDimensions.height - data.contentDimensions.height;
    }
}

static void InspectRender(PicoApp *app)
{
    if (g_inspect_n <= 0)
    {
        return;
    }
    PicoSubagentInspect inspect;
    memset(&inspect, 0, sizeof(inspect));
    const char *fallback = NULL;
    bool found = InspectCurrent(app, &inspect, &fallback);
    float sw = (float)GetScreenWidth();
    float sh = (float)GetScreenHeight();
    float card_w = sw * 0.72f;
    if (card_w < 320.0f)
    {
        card_w = sw - 32.0f;
    }
    if (card_w > 900.0f)
    {
        card_w = 900.0f;
    }
    float card_h = sh * 0.72f;
    if (card_h < 240.0f)
    {
        card_h = sh - 48.0f;
    }

    static char title[192];
    static char meta[256];
    const char *status = found && inspect.live ? "Running" : "Done";
    if (found && inspect.profile[0])
    {
        snprintf(title, sizeof(title), "%s · %s", inspect.profile, status);
    }
    else
    {
        snprintf(title, sizeof(title), "Subagent · %s", status);
    }
    meta[0] = '\0';
    if (found)
    {
        snprintf(meta, sizeof(meta), "%s%s%s%s%s", inspect.model,
                 inspect.model[0] && inspect.effort[0] ? " · " : "", inspect.effort,
                 inspect.session_id[0] ? " · " : "", inspect.session_id);
    }

    CLAY(CLAY_ID("SubagentModalDim"),
         {.floating = {.attachTo = CLAY_ATTACH_TO_ROOT,
                       .zIndex = 20,
                       .attachPoints = {.element = CLAY_ATTACH_POINT_LEFT_TOP,
                                        .parent = CLAY_ATTACH_POINT_LEFT_TOP}},
          .layout = {.layoutDirection = CLAY_TOP_TO_BOTTOM,
                     .childAlignment = {.x = CLAY_ALIGN_X_CENTER, .y = CLAY_ALIGN_Y_CENTER},
                     .sizing = {.width = CLAY_SIZING_FIXED(sw), .height = CLAY_SIZING_FIXED(sh)}},
          .backgroundColor = {0, 0, 0, 140}})
    {
        CLAY(CLAY_ID("SubagentModalCard"),
             {.layout = {.layoutDirection = CLAY_TOP_TO_BOTTOM,
                         .padding = {16, 16, 14, 14},
                         .childGap = 10,
                         .sizing = {.width = CLAY_SIZING_FIXED(card_w),
                                    .height = CLAY_SIZING_FIXED(card_h)}},
              .backgroundColor = COLOR_CONTENT_BG,
              .cornerRadius = CLAY_CORNER_RADIUS(8)})
        {
            CLAY_AUTO_ID({.layout = {.layoutDirection = CLAY_LEFT_TO_RIGHT,
                                     .childGap = 10,
                                     .childAlignment = {.y = CLAY_ALIGN_Y_CENTER},
                                     .sizing = {.width = CLAY_SIZING_GROW(0)}}})
            {
                if (g_inspect_n > 1)
                {
                    bool back_hover = Clay_PointerOver(Clay_GetElementId(CLAY_STRING("SubagentBack")));
                    if (back_hover)
                    {
                        app->hovered_clickable = true;
                    }
                    CLAY(CLAY_ID("SubagentBack"),
                         {.layout = {.padding = {10, 10, 6, 6}},
                          .backgroundColor = back_hover ? COLOR_CODE_BG : COLOR_FOOTER_BG,
                          .cornerRadius = CLAY_CORNER_RADIUS(6)})
                    {
                        CLAY_TEXT(CLAY_STRING("Back"),
                                  CLAY_TEXT_CONFIG({.fontId = FONT_BOLD, .fontSize = 13, .textColor = COLOR_TEXT}));
                    }
                }
                CLAY_AUTO_ID({.layout = {.layoutDirection = CLAY_TOP_TO_BOTTOM,
                                         .childGap = 2,
                                         .sizing = {.width = CLAY_SIZING_GROW(0)}}})
                {
                    CLAY_TEXT(ViewCStr(title),
                              CLAY_TEXT_CONFIG({.fontId = FONT_BOLD, .fontSize = 16, .textColor = COLOR_TEXT}));
                    if (meta[0])
                    {
                        CLAY_TEXT(ViewCStr(meta),
                                  CLAY_TEXT_CONFIG({.fontId = FONT_REGULAR,
                                                    .fontSize = 13,
                                                    .textColor = COLOR_MUTED,
                                                    .wrapMode = CLAY_TEXT_WRAP_WORDS}));
                    }
                }
            }
            CLAY(CLAY_ID("SubagentChatScroll"),
                 {.layout = {.layoutDirection = CLAY_TOP_TO_BOTTOM,
                             .childGap = 12,
                             .sizing = {.width = CLAY_SIZING_GROW(0),
                                        .height = CLAY_SIZING_GROW(0)}},
                  .clip = {.vertical = true, .horizontal = false, .childOffset = Clay_GetScrollOffset()}})
            {
                if (found && inspect.message_count > 0)
                {
                    PicoAgent *owner =
                        inspect.live_id ? PicoAgentManager_Find(app->agents, inspect.live_id) : NULL;
                    TranscriptView view = {
                        .app = app,
                        .messages = inspect.messages,
                        .message_count = inspect.message_count,
                        .state = inspect.state,
                        .activity = inspect.activity,
                        .owner = owner,
                        .id_ns = g_inspect_n,
                        .selectable = false,
                    };
                    RenderTranscript(&view, card_w - 48.0f);
                }
                else if (fallback && fallback[0])
                {
                    RenderToolOutput(NULL, fallback);
                }
                else
                {
                    CLAY_TEXT(CLAY_STRING("No transcript"),
                              CLAY_TEXT_CONFIG({.fontId = FONT_ITALIC, .fontSize = 14, .textColor = COLOR_MUTED}));
                }
            }
        }
    }
}

static void InspectHandlePointer(PicoApp *app)
{
    if (g_inspect_n <= 0)
    {
        return;
    }
    if (Clay_PointerOver(Clay_GetElementId(CLAY_STRING("SubagentChatScroll"))) &&
        GetMouseWheelMove() != 0.0f)
    {
        g_inspect_follow = false;
    }
    bool over_dim = Clay_PointerOver(Clay_GetElementId(CLAY_STRING("SubagentModalDim")));
    bool over_card = Clay_PointerOver(Clay_GetElementId(CLAY_STRING("SubagentModalCard")));
    bool over_back = Clay_PointerOver(Clay_GetElementId(CLAY_STRING("SubagentBack")));
    bool over_scroll = Clay_PointerOver(Clay_GetElementId(CLAY_STRING("SubagentChatScroll")));

    PicoSubagentInspect inspect;
    memset(&inspect, 0, sizeof(inspect));
    const char *fallback = NULL;
    bool found = InspectCurrent(app, &inspect, &fallback);
    TranscriptView view = {
        .app = app,
        .messages = found ? inspect.messages : NULL,
        .message_count = found ? inspect.message_count : 0,
        .id_ns = g_inspect_n,
        .selectable = false,
    };

    int tool_msg = -1;
    int tool_idx = -1;
    if (over_scroll && found)
    {
        for (int i = 0; i < view.message_count; i++)
        {
            const PicoMessage *msg = &view.messages[i];
            for (int t = 0; t < msg->trace_count; t++)
            {
                if (msg->trace[t].is_tool && Clay_PointerOver(ToolRowId(&view, i, t)))
                {
                    tool_msg = i;
                    tool_idx = t;
                    break;
                }
            }
            if (tool_idx >= 0)
            {
                break;
            }
        }
    }

    if (IsMouseButtonPressed(MOUSE_BUTTON_LEFT))
    {
        g_inspect_pressed_dim = over_dim && !over_card;
        g_inspect_pressed_back = over_back;
        g_inspect_pressed_tool = tool_idx >= 0;
        g_inspect_tool_msg = tool_msg;
        g_inspect_tool_idx = tool_idx;
    }
    if (!IsMouseButtonReleased(MOUSE_BUTTON_LEFT))
    {
        return;
    }
    if (g_inspect_pressed_dim && over_dim && !over_card)
    {
        PicoChat_InspectClose();
    }
    else if (g_inspect_pressed_back && over_back)
    {
        InspectPop();
    }
    else if (g_inspect_pressed_tool && tool_idx >= 0 && tool_msg == g_inspect_tool_msg &&
             tool_idx == g_inspect_tool_idx && found && tool_msg < inspect.message_count)
    {
        const PicoTraceLine *line = &inspect.messages[tool_msg].trace[tool_idx];
        if (IsSubagentTool(line))
        {
            InspectPushLine(line, inspect.live_id);
        }
    }
    g_inspect_pressed_dim = false;
    g_inspect_pressed_back = false;
    g_inspect_pressed_tool = false;
}

void PicoChat_HandlePointer(PicoApp *app, const PicoHookEvent *event)
{
    (void)event;
    InspectHandlePointer(app);
    InspectFollowScroll();
    PicoChatSel_Clamp(app);
    if (app->status_warn || PicoUi_ModalOpen(app))
    {
        return;
    }

    Vector2 mouse = GetMousePosition();
    bool over_bar = Clay_PointerOver(Clay_GetElementId(CLAY_STRING("ChatScrollBarHandle"))) ||
                    Clay_PointerOver(Clay_GetElementId(CLAY_STRING("ChatScrollTrack")));
    bool over_composer = Clay_PointerOver(Clay_GetElementId(CLAY_STRING("Composer")));
    bool over_chat = Clay_PointerOver(Clay_GetElementId(CLAY_STRING("ChatScroll")));
    TranscriptView main = MainTranscriptView(app);

    if (over_bar || over_composer)
    {
        if (IsMouseButtonPressed(MOUSE_BUTTON_LEFT))
        {
            if (over_composer)
            {
                PicoChatSel_Clear(app);
            }
            app->chat_sel.mouse_selecting = false;
            app->chat_sel.dragging = false;
            app->chat_sel.pressed_tool = false;
        }
        return;
    }

    if (IsMouseButtonPressed(MOUSE_BUTTON_LEFT) && over_chat)
    {
        int tool_msg = -1;
        int tool_idx = -1;
        for (int i = 0; i < PicoApp_ActiveAgent(app)->message_count; i++)
        {
            PicoMessage *msg = &PicoApp_ActiveAgent(app)->messages[i];
            for (int t = 0; t < msg->trace_count; t++)
            {
                if (msg->trace[t].is_tool && Clay_PointerOver(ToolRowId(&main, i, t)))
                {
                    tool_msg = i;
                    tool_idx = t;
                    break;
                }
            }
            if (tool_idx >= 0)
            {
                break;
            }
        }

        int msg = -1;
        int pos = PicoChatSel_OffsetAtPoint(app, mouse.x, mouse.y, -1, &msg);
        app->chat_sel.mouse_selecting = true;
        app->chat_sel.dragging = false;
        app->chat_sel.press_x = mouse.x;
        app->chat_sel.press_y = mouse.y;
        app->chat_sel.pressed_tool = tool_idx >= 0;
        app->chat_sel.tool_msg = tool_msg;
        app->chat_sel.tool_idx = tool_idx;
        app->chat_sel.msg = msg;
        app->chat_sel.anchor = pos;
        app->chat_sel.cursor = pos;
        app->composer.sel_anchor = app->composer.cursor;
    }

    if (!IsMouseButtonDown(MOUSE_BUTTON_LEFT))
    {
        if (app->chat_sel.mouse_selecting && !app->chat_sel.dragging && app->chat_sel.pressed_tool &&
            app->chat_sel.tool_msg >= 0 && app->chat_sel.tool_msg < PicoApp_ActiveAgent(app)->message_count)
        {
            PicoMessage *msg = &PicoApp_ActiveAgent(app)->messages[app->chat_sel.tool_msg];
            int t = app->chat_sel.tool_idx;
            if (t >= 0 && t < msg->trace_count && msg->trace[t].is_tool &&
                Clay_PointerOver(ToolRowId(&main, app->chat_sel.tool_msg, t)))
            {
                if (IsSubagentTool(&msg->trace[t]))
                {
                    InspectPushLine(&msg->trace[t], PicoApp_ActiveAgent(app)->id);
                }
                else
                {
                    msg->trace[t].expanded = !msg->trace[t].expanded;
                }
            }
            app->chat_sel.anchor = app->chat_sel.cursor;
        }
        app->chat_sel.mouse_selecting = false;
        app->chat_sel.pressed_tool = false;
        if (!app->chat_sel.dragging && app->chat_sel.anchor == app->chat_sel.cursor)
        {
            app->chat_sel.dragging = false;
        }
        return;
    }

    if (app->chat_sel.mouse_selecting)
    {
        float dx = mouse.x - app->chat_sel.press_x;
        float dy = mouse.y - app->chat_sel.press_y;
        if (dx * dx + dy * dy > 16.0f)
        {
            app->chat_sel.dragging = true;
        }
        if (app->chat_sel.dragging)
        {
            int msg = app->chat_sel.msg;
            int pos = PicoChatSel_OffsetAtPoint(app, mouse.x, mouse.y, msg, &msg);
            if (app->chat_sel.msg < 0)
            {
                app->chat_sel.msg = msg;
                app->chat_sel.anchor = pos;
            }
            app->chat_sel.cursor = pos;
        }
    }
}

static Color ClayToRay(Clay_Color c)
{
    return (Color){(unsigned char)c.r, (unsigned char)c.g, (unsigned char)c.b, (unsigned char)c.a};
}

static void PicoChat_DrawChevrons(PicoApp *app)
{
    Clay_ElementData scroll = Clay_GetElementData(Clay_GetElementId(CLAY_STRING("ChatScroll")));
    if (!scroll.found || !app->fonts)
    {
        return;
    }
    Clay_BoundingBox clip = scroll.boundingBox;
    BeginScissorMode((int)clip.x, (int)clip.y, (int)clip.width, (int)clip.height);

    Font font = Pico_FontAt(FONT_REGULAR, 15);
    const char *glyph = "\xE2\x80\xBA";
    Vector2 size = MeasureTextEx(font, glyph, 15.0f, 0.0f);
    TranscriptView main = MainTranscriptView(app);

    for (int i = 0; i < PicoApp_ActiveAgent(app)->message_count; i++)
    {
        PicoMessage *msg = &PicoApp_ActiveAgent(app)->messages[i];
        for (int t = 0; t < msg->trace_count; t++)
        {
            PicoTraceLine *line = &msg->trace[t];
            if (!line->is_tool)
            {
                continue;
            }
            bool hovered = Clay_PointerOver(ToolRowId(&main, i, t));
            if (!hovered && !line->expanded)
            {
                continue;
            }
            Clay_ElementData el = Clay_GetElementData(ToolChevronId(&main, i, t));
            if (!el.found)
            {
                continue;
            }
            Clay_BoundingBox box = el.boundingBox;
            Vector2 center = {roundf(box.x + box.width * 0.5f), roundf(box.y + box.height * 0.5f)};
            Color color = ClayToRay(hovered ? COLOR_TOOL_NAME_HOVER : COLOR_TOOL_CHEVRON);
            DrawTextPro(font, glyph, center, (Vector2){size.x * 0.5f, size.y * 0.5f},
                        line->expanded ? 90.0f : 0.0f, 15.0f, 0.0f, color);
        }
    }

    EndScissorMode();
}

void PicoChat_DrawOverlay(PicoApp *app, const PicoHookEvent *event)
{
    (void)event;
    PicoChatSel_DrawOverlay(app);
    PicoChat_DrawChevrons(app);
}

static void ChatOnFrame(PicoApp *app, float dt)
{
    (void)dt;
    if (g_inspect_n <= 0)
    {
        return;
    }
    if (PicoExts_IsOpen() || PicoPrompt_IsOpen() || PicoFooter_MenuOpen() ||
        PicoAgent_AskUiOpen(PicoApp_ActiveAgent(app)))
    {
        return;
    }
    if (IsKeyPressed(KEY_ESCAPE))
    {
        InspectPop();
    }
}

static void ChatInit(PicoApp *app)
{
    pico_add_view(app, PICO_SLOT_MAIN, 0, PicoChat_Render);
    pico_add_view(app, PICO_SLOT_OVERLAY, 20, InspectRender);
    pico_add_hook(app, PICO_HOOK_AFTER_LAYOUT, PicoChat_HandlePointer);
    pico_add_hook(app, PICO_HOOK_AFTER_RENDER, PicoChat_DrawOverlay);
}

PicoExt pico_ext_chat(void)
{
    return (PicoExt){
        .abi = PICO_EXT_ABI,
        .name = "chat",
        .description = "Chat transcript",
        .init = ChatInit,
        .on_frame = ChatOnFrame,
    };
}
