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
#include "scrollbar.h"
#include "markdown.h"
#include "transcript_virtual.h"

#include "clay/clay.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define TOOL_OUTPUT_MAX_LINES 100
#define TOOL_WRAP_MAX_LINES 48
#define THINK_SHEEN_PERIOD 1.4f
#define THINK_SHEEN_BAND 56.0f
#define INSPECT_CARD_PAD_X 16.0f
#define INSPECT_CARD_PAD_Y 14.0f
#define INSPECT_CARD_GAP 10.0f
#define INSPECT_MSG_PAD_X 16.0f
#define TOOL_ROW_FIXED_CHROME 46.0f
#define TRANSCRIPT_MESSAGE_GAP 8.0f
#define TRANSCRIPT_OVERSCAN_VIEWPORTS 0.75f

static const float kThinkBriefMaxSec = 10.0f;
static const float kThinkDeepMaxSec = 60.0f;

typedef struct ThinkLabelBlock {
    struct ThinkLabelBlock *next;
    size_t len;
    size_t cap;
    char data[];
} ThinkLabelBlock;

static ThinkLabelBlock *g_think_label_blocks;
static ThinkLabelBlock *g_think_label_block;
static MdDocument *g_think_docs;
static int g_think_doc_count;
static int g_think_doc_cap;

typedef struct TranscriptView {
    PicoApp *app;
    const PicoMessage *messages;
    int message_count;
    PicoAgentState state;
    const char *activity;
    PicoAgent *owner;
    PicoTranscriptVirtual *virtual_cache;
    Clay_String scroll_id;
    uint64_t virtual_identity;
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

static PicoApp *g_app;
static InspectFrame g_inspect[PICO_MAX_DELEGATION_DEPTH + 1];
static int g_inspect_n;
static bool g_inspect_follow = true;
static bool g_inspect_overflow;
static PicoScrollbar g_inspect_bar;
static bool g_inspect_pressed_dim;
static bool g_inspect_pressed_back;
static bool g_inspect_pressed_tool;
static int g_inspect_tool_msg;
static int g_inspect_tool_idx;
static PicoTranscriptVirtual g_main_virtual;
static PicoTranscriptVirtual g_inspect_virtual;
static int g_inspect_virtual_ns;
static bool g_virtual_relayout;

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

static int Utf8Step(const char *s, int len, int pos)
{
    if (pos >= len)
    {
        return len;
    }
    unsigned char c = (unsigned char)s[pos];
    int step = 1;
    if ((c & 0xE0) == 0xC0)
    {
        step = 2;
    }
    else if ((c & 0xF0) == 0xE0)
    {
        step = 3;
    }
    else if ((c & 0xF8) == 0xF0)
    {
        step = 4;
    }
    pos += step;
    return pos > len ? len : pos;
}

static float MeasureCfg(Clay_TextElementConfig *config, const char *s, int n)
{
    Clay_StringSlice slice = {.length = n, .chars = s, .baseChars = s};
    return Pico_MeasureTextUtf8(slice, config, NULL).width;
}

/* Pre-wrap so long unspaced tokens (JSON args, paths) stay inside `width`. Clay
 * WRAP_WORDS will not break a token, which is what overflowed the inspect card. */
static void ViewWrappedText(const TranscriptView *view, Clay_String text, Clay_TextElementConfig config,
                            float width)
{
    int i;
    int lines;
    if (text.length <= 0 || !text.chars)
    {
        return;
    }
    if (width < 20.0f)
    {
        width = 20.0f;
    }
    config.wrapMode = CLAY_TEXT_WRAP_NONE;
    i = 0;
    lines = 0;
    while (i < text.length && lines < TOOL_WRAP_MAX_LINES)
    {
        int line_start = i;
        float line_w = 0.0f;
        int break_at = -1;
        int break_resume = -1;
        int wrapped = 0;
        if (text.chars[i] == '\n')
        {
            ViewText(view, (Clay_String){.length = 1, .chars = " "}, config);
            ViewBreak(view);
            i++;
            lines++;
            continue;
        }
        while (i < text.length && text.chars[i] != '\n')
        {
            int next = Utf8Step(text.chars, text.length, i);
            float ch_w = MeasureCfg(&config, text.chars + i, next - i);
            if (line_w + ch_w > width && i > line_start)
            {
                int end = break_at > line_start ? break_at : i;
                ViewText(view, (Clay_String){.length = end - line_start, .chars = text.chars + line_start},
                         config);
                ViewBreak(view);
                i = break_at > line_start ? break_resume : i;
                wrapped = 1;
                lines++;
                break;
            }
            line_w += ch_w;
            if (text.chars[i] == ' ' || text.chars[i] == '\t')
            {
                break_at = i;
                break_resume = next;
            }
            i = next;
        }
        if (!wrapped)
        {
            int len = i - line_start;
            ViewText(view, (Clay_String){.length = len > 0 ? len : 1, .chars = len > 0 ? text.chars + line_start : " "},
                     config);
            ViewBreak(view);
            lines++;
            if (i < text.length && text.chars[i] == '\n')
            {
                i++;
            }
        }
    }
    if (i < text.length)
    {
        CLAY_TEXT(CLAY_STRING("…"), CLAY_TEXT_CONFIG({.fontId = config.fontId,
                                                     .fontSize = config.fontSize,
                                                     .textColor = COLOR_MUTED,
                                                     .wrapMode = CLAY_TEXT_WRAP_NONE}));
        ViewBreak(view);
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

static Clay_ElementId ThinkRowId(const TranscriptView *view, int message_index, int trace_index)
{
    return ToolElementId(view, message_index, trace_index, CLAY_STRING("ThinkRow"));
}

static Clay_ElementId ThinkLabelId(const TranscriptView *view, int message_index, int trace_index)
{
    return ToolElementId(view, message_index, trace_index, CLAY_STRING("ThinkLabel"));
}

static Clay_ElementId ThinkChevronId(const TranscriptView *view, int message_index, int trace_index)
{
    return ToolElementId(view, message_index, trace_index, CLAY_STRING("ThinkChevron"));
}

static Clay_ElementId ThinkSynthId(const TranscriptView *view, int message_index)
{
    return ToolElementId(view, message_index, 0, CLAY_STRING("ThinkSynth"));
}

static void ThinkFrameReset(void)
{
    for (ThinkLabelBlock *block = g_think_label_blocks; block; block = block->next)
    {
        block->len = 0;
    }
    g_think_label_block = g_think_label_blocks;
    for (int i = 0; i < g_think_doc_count; i++)
    {
        MdDocument_Free(&g_think_docs[i]);
    }
    g_think_doc_count = 0;
}

static void ThinkFrameFree(void)
{
    ThinkFrameReset();
    while (g_think_label_blocks)
    {
        ThinkLabelBlock *next = g_think_label_blocks->next;
        free(g_think_label_blocks);
        g_think_label_blocks = next;
    }
    g_think_label_block = NULL;
    free(g_think_docs);
    g_think_docs = NULL;
    g_think_doc_cap = 0;
}

static const char *ThinkLabelDup(const char *s)
{
    size_t n = s ? strlen(s) : 0;
    size_t need = n + 1;
    ThinkLabelBlock *block = g_think_label_block;
    while (block && block->cap - block->len < need)
    {
        block = block->next;
    }
    if (!block)
    {
        size_t cap = 2048;
        while (cap < need)
        {
            cap *= 2;
        }
        block = (ThinkLabelBlock *)malloc(sizeof(*block) + cap);
        if (!block)
        {
            return "";
        }
        block->next = NULL;
        block->len = 0;
        block->cap = cap;
        if (!g_think_label_blocks)
        {
            g_think_label_blocks = block;
        }
        else
        {
            ThinkLabelBlock *tail = g_think_label_blocks;
            while (tail->next)
            {
                tail = tail->next;
            }
            tail->next = block;
        }
    }
    g_think_label_block = block;
    char *out = block->data + block->len;
    if (s)
    {
        memcpy(out, s, n);
    }
    out[n] = '\0';
    block->len += need;
    return out;
}

static MdDocument *ThinkDocumentPush(const char *text)
{
    if (g_think_doc_count >= g_think_doc_cap)
    {
        int cap = g_think_doc_cap == 0 ? 8 : g_think_doc_cap * 2;
        MdDocument *next = (MdDocument *)realloc(g_think_docs, (size_t)cap * sizeof(MdDocument));
        if (!next)
        {
            return NULL;
        }
        g_think_docs = next;
        g_think_doc_cap = cap;
    }
    MdDocument *doc = &g_think_docs[g_think_doc_count++];
    *doc = MdDocument_ParseEx(text, strlen(text), MD_PARSE_DEFAULT);
    return doc;
}

static const char *ThoughtLabel(int think_ms)
{
    if (think_ms <= 0)
    {
        return "Thought";
    }
    float sec = (float)think_ms / 1000.0f;
    if (sec < kThinkBriefMaxSec)
    {
        return "Thought briefly";
    }
    if (sec < kThinkDeepMaxSec)
    {
        return "Thought deeply";
    }
    return "Thought very deeply";
}

static void FlattenSummary(const char *md, char *out, size_t cap)
{
    if (!out || cap == 0)
    {
        return;
    }
    out[0] = '\0';
    if (!md || !md[0])
    {
        return;
    }
    MdDocument doc = MdDocument_ParseEx(md, strlen(md), MD_PARSE_DEFAULT);
    size_t n = 0;
    for (int b = 0; b < doc.block_count && n + 1 < cap; b++)
    {
        MdBlock *block = &doc.blocks[b];
        for (int c = 0; c < block->chunk_count && n + 1 < cap; c++)
        {
            const char *text = block->chunks[c].text;
            int length = block->chunks[c].length;
            if (!text || length <= 0)
            {
                continue;
            }
            if (n > 0 && n + 1 < cap)
            {
                out[n++] = ' ';
            }
            size_t room = cap - 1 - n;
            size_t take = (size_t)length < room ? (size_t)length : room;
            memcpy(out + n, text, take);
            n += take;
        }
    }
    out[n] = '\0';
    MdDocument_Free(&doc);
    if (n == 0)
    {
        snprintf(out, cap, "%s", md);
    }
}

static const char *LatestSummary(const PicoTraceLine *line)
{
    if (line && line->think_part_count > 0 && line->think_parts &&
        line->think_parts[line->think_part_count - 1] &&
        line->think_parts[line->think_part_count - 1][0])
    {
        return line->think_parts[line->think_part_count - 1];
    }
    return line && line->text ? line->text : "";
}

static bool ThinkHasSummary(const PicoTraceLine *line)
{
    return line && (line->think_steps >= 1 || line->think_part_count > 0);
}

static bool ThinkHasBody(const PicoTraceLine *line)
{
    if (!line || line->is_tool)
    {
        return false;
    }
    if (line->think_part_count > 0)
    {
        return true;
    }
    return line->text && line->text[0];
}

static const char *ThinkHeaderText(const PicoTraceLine *line, bool live)
{
    char flat[512];
    if (ThinkHasSummary(line))
    {
        FlattenSummary(LatestSummary(line), flat, sizeof(flat));
        if (flat[0])
        {
            return ThinkLabelDup(flat);
        }
    }
    if (live)
    {
        return "Thinking…";
    }
    return ThoughtLabel(line ? line->think_ms : 0);
}

static void RenderToolOutput(const TranscriptView *view, const char *output, float available_width)
{
    const char *text = (output && output[0]) ? output : "(empty)";
    float inner_w = available_width - 24.0f;
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
            ViewWrappedText(view, s, (Clay_TextElementConfig){.fontId = FONT_MONO,
                                                              .fontSize = 14,
                                                              .lineHeight = Pico_FontPxU16(18),
                                                              .textColor = COLOR_CODE_TEXT},
                            inner_w);
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

static void RenderThinkMarkdown(const TranscriptView *view, const char *text, float available_width)
{
    if (!text || !text[0])
    {
        return;
    }
    MdDocument *doc = ThinkDocumentPush(text);
    if (!doc)
    {
        return;
    }
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
    for (int b = 0; b < doc->block_count; b++)
    {
        MdBlock *block = &doc->blocks[b];
        if (!block->chunks || block->chunk_count <= 0)
        {
            continue;
        }
        RichText_RenderParagraph(block, &doc->arena, available_width, &style, &emit);
    }
    ViewBreak(view);
}

static void RenderThinkBody(const TranscriptView *view, PicoTraceLine *line, float available_width)
{
    CLAY_AUTO_ID({.layout = {.layoutDirection = CLAY_TOP_TO_BOTTOM,
                             .padding = {12, 12, 10, 10},
                             .childGap = 8,
                             .sizing = {.width = CLAY_SIZING_GROW(0)}},
                  .backgroundColor = COLOR_CODE_BG,
                  .cornerRadius = CLAY_CORNER_RADIUS(6)})
    {
        if (ThinkHasSummary(line) && line->think_part_count > 0)
        {
            for (int i = 0; i < line->think_part_count; i++)
            {
                RenderThinkMarkdown(view, line->think_parts[i], available_width - 24.0f);
            }
        }
        else if (line->text && line->text[0])
        {
            ViewText(view, ViewCStr(line->text),
                     (Clay_TextElementConfig){.fontId = FONT_ITALIC,
                                              .fontSize = 15,
                                              .textColor = COLOR_MUTED,
                                              .wrapMode = CLAY_TEXT_WRAP_WORDS});
            ViewBreak(view);
        }
    }
}

static bool ThinkBurstLive(const TranscriptView *view, int message_index, int trace_index)
{
    if (!view || message_index != view->message_count - 1 || view->state != PICO_AGENT_LLM_WAIT)
    {
        return false;
    }
    const PicoMessage *msg = &view->messages[message_index];
    if (msg->source && msg->source[0])
    {
        return false;
    }
    return trace_index == msg->trace_count - 1;
}

static void RenderThinkLine(const TranscriptView *view, PicoTraceLine *line, int message_index,
                            int trace_index, float available_width)
{
    if (!ThinkHasBody(line))
    {
        return;
    }
    Clay_ElementId row_id = ThinkRowId(view, message_index, trace_index);
    Clay_ElementId label_id = ThinkLabelId(view, message_index, trace_index);
    bool hovered = Clay_PointerOver(row_id);
    if (hovered)
    {
        view->app->hovered_tool = true;
    }
    bool live = ThinkBurstLive(view, message_index, trace_index);
    const char *label = ThinkHeaderText(line, live);
    Clay_Color color = hovered ? COLOR_TOOL_NAME_HOVER : COLOR_MUTED;

    CLAY_AUTO_ID({.layout = {.layoutDirection = CLAY_TOP_TO_BOTTOM,
                             .childGap = 6,
                             .sizing = {.width = CLAY_SIZING_GROW(0)}}})
    {
        CLAY(row_id, {.layout = {.layoutDirection = CLAY_LEFT_TO_RIGHT,
                                 .childGap = 8,
                                 .childAlignment = {.y = CLAY_ALIGN_Y_CENTER},
                                 .sizing = {.width = CLAY_SIZING_GROW(0)}}})
        {
            CLAY(label_id, {.layout = {.sizing = {.width = CLAY_SIZING_GROW(0)}},
                            .clip = {.horizontal = true, .vertical = true}})
            {
                ViewText(view, ViewCStr(label),
                         (Clay_TextElementConfig){.fontId = FONT_ITALIC,
                                                  .fontSize = 15,
                                                  .textColor = color,
                                                  .wrapMode = CLAY_TEXT_WRAP_NONE});
            }
            CLAY(ThinkChevronId(view, message_index, trace_index),
                 {.layout = {.sizing = {.width = CLAY_SIZING_FIXED(14), .height = CLAY_SIZING_GROW(0)}}})
            {
            }
        }
        if (line->expanded)
        {
            RenderThinkBody(view, line, available_width);
        }
    }
    ViewBreak(view);
}

static void RenderSyntheticThink(const TranscriptView *view, int message_index)
{
    Clay_ElementId label_id = ThinkSynthId(view, message_index);
    CLAY(label_id, {.layout = {.sizing = {.width = CLAY_SIZING_GROW(0)}},
                    .clip = {.horizontal = true, .vertical = true}})
    {
        CLAY_TEXT(CLAY_STRING("Thinking…"),
                  CLAY_TEXT_CONFIG({.fontId = FONT_ITALIC,
                                    .fontSize = 15,
                                    .textColor = COLOR_MUTED,
                                    .wrapMode = CLAY_TEXT_WRAP_NONE}));
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
                           int trace_index, float available_width)
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
    Clay_TextElementConfig name_cfg = {.fontId = FONT_REGULAR,
                                       .fontSize = 15,
                                       .textColor = name_color,
                                       .wrapMode = CLAY_TEXT_WRAP_NONE};
    Clay_TextElementConfig args_cfg = {.fontId = FONT_REGULAR, .fontSize = 15, .textColor = args_color};
    float args_w = available_width - TOOL_ROW_FIXED_CHROME - MeasureCfg(&name_cfg, name.chars, name.length);
    if (args_w < 40.0f)
    {
        args_w = 40.0f;
    }

    CLAY_AUTO_ID({.layout = {.layoutDirection = CLAY_TOP_TO_BOTTOM,
                             .childGap = 6,
                             .sizing = {.width = CLAY_SIZING_GROW(0)}}})
    {
        CLAY(row_id, {.layout = {.layoutDirection = CLAY_LEFT_TO_RIGHT,
                                 .childGap = 8,
                                 .childAlignment = {.y = CLAY_ALIGN_Y_TOP},
                                 .sizing = {.width = CLAY_SIZING_GROW(0)}}})
        {
            CLAY(ToolStatusId(view, message_index, trace_index),
                 {.layout = {.sizing = {.width = CLAY_SIZING_FIXED(8), .height = CLAY_SIZING_FIXED(8)}},
                  .backgroundColor = ToolStatusColor(line),
                  .cornerRadius = CLAY_CORNER_RADIUS(4)})
            {
            }
            ViewText(view, name, name_cfg);
            if (args.length > 0)
            {
                ViewGlue(view, " ");
                CLAY_AUTO_ID({.layout = {.layoutDirection = CLAY_TOP_TO_BOTTOM,
                                         .sizing = {.width = CLAY_SIZING_GROW(0, args_w)}}})
                {
                    ViewWrappedText(view, args, args_cfg, args_w);
                }
            }
            else
            {
                CLAY_AUTO_ID({.layout = {.sizing = {.width = CLAY_SIZING_GROW(0)}}}) {}
            }
            CLAY(ToolChevronId(view, message_index, trace_index),
                 {.layout = {.sizing = {.width = CLAY_SIZING_FIXED(14), .height = CLAY_SIZING_FIXED(18)}}})
            {
            }
        }
        if (subagent && !line->tool_output)
        {
            const char *activity = SubagentActivity(view->app, line);
            ViewWrappedText(view, ViewCStr(activity),
                            (Clay_TextElementConfig){.fontId = FONT_ITALIC,
                                                     .fontSize = 14,
                                                     .textColor = COLOR_MUTED},
                            available_width);
        }
        else if (!subagent && line->expanded)
        {
            const char *output = line->tool_output;
            if (!output && OwnerWaiting(view))
            {
                output = OwnerHasAsk(view) ? "Waiting for you…" : "Running…";
            }
            RenderToolOutput(view, output, available_width);
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

static uint64_t RevisionMix(uint64_t hash, uint64_t value)
{
    hash ^= value + UINT64_C(0x9e3779b97f4a7c15) + (hash << 6) + (hash >> 2);
    return hash;
}

static uint64_t RevisionPointer(uint64_t hash, const void *pointer)
{
    return RevisionMix(hash, (uint64_t)(uintptr_t)pointer);
}

static uint64_t RevisionText(uint64_t hash, const char *text)
{
    if (!text)
    {
        return RevisionMix(hash, 0);
    }
    for (const unsigned char *p = (const unsigned char *)text; *p; p++)
    {
        hash = RevisionMix(hash, *p);
    }
    return hash;
}

static uint64_t MessageRevision(const TranscriptView *view, int message_index)
{
    const PicoMessage *msg = &view->messages[message_index];
    uint64_t hash = UINT64_C(0xcbf29ce484222325);
    hash = RevisionMix(hash, (uint64_t)msg->role);
    hash = RevisionPointer(hash, msg->source);
    hash = RevisionPointer(hash, msg->trace);
    hash = RevisionMix(hash, (uint64_t)msg->trace_count);
    for (int t = 0; t < msg->trace_count; t++)
    {
        const PicoTraceLine *line = &msg->trace[t];
        hash = RevisionPointer(hash, line->text);
        hash = RevisionPointer(hash, line->tool_name);
        hash = RevisionPointer(hash, line->tool_args);
        hash = RevisionPointer(hash, line->tool_output);
        hash = RevisionPointer(hash, line->think_parts);
        hash = RevisionMix(hash, (uint64_t)line->think_part_count);
        hash = RevisionMix(hash, (uint64_t)line->think_steps);
        hash = RevisionMix(hash, (uint64_t)line->think_ms);
        hash = RevisionMix(hash, (uint64_t)line->child_id);
        hash = RevisionMix(hash, line->is_tool ? 1 : 0);
        hash = RevisionMix(hash, line->tool_error ? 1 : 0);
        hash = RevisionMix(hash, line->expanded ? 1 : 0);
        for (int p = 0; p < line->think_part_count; p++)
        {
            hash = RevisionPointer(hash, line->think_parts ? line->think_parts[p] : NULL);
        }
    }
    if (message_index == view->message_count - 1)
    {
        /* Streaming buffers often grow in place, so pointer identity alone is
         * insufficient for the one message that can still be changing. */
        hash = RevisionText(hash, msg->source);
        for (int t = 0; t < msg->trace_count; t++)
        {
            const PicoTraceLine *line = &msg->trace[t];
            hash = RevisionText(hash, line->text);
            if (line->think_part_count > 0 && line->think_parts)
            {
                hash = RevisionText(hash, line->think_parts[line->think_part_count - 1]);
            }
        }
        hash = RevisionMix(hash, (uint64_t)view->state);
        hash = RevisionText(hash, view->activity);
    }
    return hash;
}

static Clay_ElementId TranscriptSpacerId(const TranscriptView *view, int begin)
{
    switch (view ? view->id_ns : 0)
    {
    case 0: return CLAY_IDI("TranscriptSpacerMain", begin);
    case 1: return CLAY_IDI("TranscriptSpacerInspect1", begin);
    case 2: return CLAY_IDI("TranscriptSpacerInspect2", begin);
    case 3: return CLAY_IDI("TranscriptSpacerInspect3", begin);
    case 4: return CLAY_IDI("TranscriptSpacerInspect4", begin);
    default: return CLAY_IDI("TranscriptSpacerInspect5", begin);
    }
}

static Clay_ElementId TranscriptGapId(const TranscriptView *view, int message_index)
{
    switch (view ? view->id_ns : 0)
    {
    case 0: return CLAY_IDI("TranscriptGapMain", message_index);
    case 1: return CLAY_IDI("TranscriptGapInspect1", message_index);
    case 2: return CLAY_IDI("TranscriptGapInspect2", message_index);
    case 3: return CLAY_IDI("TranscriptGapInspect3", message_index);
    case 4: return CLAY_IDI("TranscriptGapInspect4", message_index);
    default: return CLAY_IDI("TranscriptGapInspect5", message_index);
    }
}

static void RenderTranscriptGap(const TranscriptView *view, int message_index)
{
    if (message_index + 1 >= view->message_count)
    {
        return;
    }
    CLAY(TranscriptGapId(view, message_index),
         {.layout = {.sizing = {.width = CLAY_SIZING_GROW(0),
                                .height = CLAY_SIZING_FIXED(TRANSCRIPT_MESSAGE_GAP)}}})
    {
    }
}

static void RenderTranscriptMessage(const TranscriptView *view, int i, float available_width)
{
    PicoMessage *msg = (PicoMessage *)&view->messages[i];
    bool user = msg->role == PICO_ROLE_USER;
    Clay_Color bg = user ? COLOR_USER_BG : COLOR_ASSISTANT_BG;
    Clay_Padding pad = user ? (Clay_Padding){16, 16, 12, 12} : (Clay_Padding){16, 16, 0, 0};
    float msg_max = available_width + (float)(pad.left + pad.right);
    if (msg_max < 50.0f)
    {
        msg_max = 50.0f;
    }
    CLAY(MessageId(view, i),
         {.layout = {.layoutDirection = CLAY_TOP_TO_BOTTOM,
                     .padding = pad,
                     .childGap = 8,
                     .sizing = {.width = CLAY_SIZING_GROW(0, msg_max)}},
          .backgroundColor = bg,
          .cornerRadius = user ? CLAY_CORNER_RADIUS(8) : CLAY_CORNER_RADIUS(0)})
    {
        if (view->selectable)
        {
            PicoChatSel_SetMessage(i);
        }
        bool has_source = msg->source && msg->source[0];
        bool live = !user && i == view->message_count - 1 && OwnerWaiting(view);
        bool live_llm = live && view->state == PICO_AGENT_LLM_WAIT;
        for (int t = 0; t < msg->trace_count; t++)
        {
            PicoTraceLine *line = &msg->trace[t];
            if (line->is_tool)
            {
                RenderToolLine(view, line, i, t, available_width);
                continue;
            }
            RenderThinkLine(view, line, i, t, available_width);
        }
        bool trailing_think = msg->trace_count > 0 && !msg->trace[msg->trace_count - 1].is_tool &&
                              ThinkHasBody(&msg->trace[msg->trace_count - 1]);
        if (live_llm && !has_source && !trailing_think)
        {
            RenderSyntheticThink(view, i);
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
    RenderTranscriptGap(view, i);
}

static void RenderTranscriptSpacer(const TranscriptView *view, int begin, int end)
{
    float height = PicoTranscriptVirtual_SpanHeight(view->virtual_cache, begin, end,
                                                    TRANSCRIPT_MESSAGE_GAP);
    if (height <= 0.5f)
    {
        return;
    }
    CLAY(TranscriptSpacerId(view, begin),
         {.layout = {.sizing = {.width = CLAY_SIZING_GROW(0),
                                .height = CLAY_SIZING_FIXED(height)}}})
    {
    }
}

static void RenderTranscript(const TranscriptView *view, float available_width)
{
    PicoTranscriptVirtual *cache = view->virtual_cache;
    PicoTranscriptVirtual_Begin(cache, view->virtual_identity, view->message_count,
                                available_width, Pico_FontScale());
    if (!cache || cache->count != view->message_count)
    {
        for (int i = 0; i < view->message_count; i++)
        {
            RenderTranscriptMessage(view, i, available_width);
        }
        return;
    }
    for (int i = 0; i < view->message_count; i++)
    {
        PicoTranscriptVirtual_SetRevision(cache, i, MessageRevision(view, i));
    }

    Clay_ScrollContainerData scroll =
        Clay_GetScrollContainerData(Clay_GetElementId(view->scroll_id));
    float scroll_top = scroll.found && scroll.scrollPosition ? -scroll.scrollPosition->y : 0.0f;
    float viewport = scroll.found ? scroll.scrollContainerDimensions.height : 0.0f;
    int force_index = -1;
    if (view->selectable &&
        (view->app->chat_sel.mouse_selecting || PicoChatSel_HasSelection(view->app)))
    {
        force_index = view->app->chat_sel.msg;
    }
    PicoTranscriptVirtual_Plan(cache, scroll_top, viewport,
                               viewport * TRANSCRIPT_OVERSCAN_VIEWPORTS,
                               force_index, TRANSCRIPT_MESSAGE_GAP);

    int skipped = -1;
    for (int i = 0; i < view->message_count; i++)
    {
        if (!PicoTranscriptVirtual_Mounted(cache, i))
        {
            if (skipped < 0)
            {
                skipped = i;
            }
            continue;
        }
        if (skipped >= 0)
        {
            RenderTranscriptSpacer(view, skipped, i);
            skipped = -1;
        }
        RenderTranscriptMessage(view, i, available_width);
    }
    if (skipped >= 0)
    {
        RenderTranscriptSpacer(view, skipped, view->message_count);
    }
}

static uint64_t TranscriptIdentity(const TranscriptView *view)
{
    uint64_t identity = UINT64_C(0x6a09e667f3bcc909);
    if (view->owner)
    {
        identity = RevisionMix(identity, (uint64_t)view->owner->id);
        identity = RevisionText(identity, view->owner->session_id);
    }
    else
    {
        identity = RevisionPointer(identity, view->messages);
    }
    identity = RevisionMix(identity, (uint64_t)view->id_ns);
    return identity ? identity : 1;
}

static void HarvestTranscriptHeights(PicoTranscriptVirtual *cache, int id_ns,
                                     Clay_String scroll_id, bool preserve_anchor)
{
    if (!cache || cache->count <= 0)
    {
        return;
    }
    Clay_ScrollContainerData scroll =
        Clay_GetScrollContainerData(Clay_GetElementId(scroll_id));
    float anchor_top = scroll.found && scroll.scrollPosition ? -scroll.scrollPosition->y : 0.0f;
    float anchor_delta = 0.0f;
    TranscriptView ids = {.id_ns = id_ns};
    for (int i = 0; i < cache->count; i++)
    {
        if (!PicoTranscriptVirtual_Mounted(cache, i))
        {
            continue;
        }
        Clay_ElementData element = Clay_GetElementData(MessageId(&ids, i));
        if (!element.found)
        {
            continue;
        }
        float delta = preserve_anchor
                          ? PicoTranscriptVirtual_AnchorDelta(
                                cache, i, element.boundingBox.height, anchor_top,
                                TRANSCRIPT_MESSAGE_GAP)
                          : 0.0f;
        PicoTranscriptVirtual_RecordHeight(cache, i, element.boundingBox.height);
        anchor_top += delta;
        anchor_delta += delta;
    }
    PicoTranscriptVirtual_FinishMeasure(cache);

    if (!preserve_anchor || !scroll.found || !scroll.scrollPosition ||
        fabsf(anchor_delta) <= 0.01f)
    {
        return;
    }
    float next = scroll.scrollPosition->y - anchor_delta;
    float min_y = scroll.scrollContainerDimensions.height - scroll.contentDimensions.height;
    if (min_y > 0.0f)
    {
        min_y = 0.0f;
    }
    if (next > 0.0f)
    {
        next = 0.0f;
    }
    else if (next < min_y)
    {
        next = min_y;
    }
    if (fabsf(next - scroll.scrollPosition->y) > 0.01f)
    {
        scroll.scrollPosition->y = next;
        g_virtual_relayout = true;
    }
}

void PicoChat_HarvestVirtualHeights(PicoApp *app)
{
    if (!app)
    {
        return;
    }
    HarvestTranscriptHeights(&g_main_virtual, 0, CLAY_STRING("ChatScroll"),
                             !app->chat_follow_bottom);
    HarvestTranscriptHeights(&g_inspect_virtual, g_inspect_virtual_ns,
                             CLAY_STRING("SubagentChatScroll"), !g_inspect_follow);
}

bool PicoChat_TakeVirtualRelayout(void)
{
    bool relayout = g_virtual_relayout;
    g_virtual_relayout = false;
    return relayout;
}

void PicoChat_Render(PicoApp *app)
{
    app->hovered_tool = false;
    ThinkFrameReset();
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
                    .virtual_cache = &g_main_virtual,
                    .scroll_id = CLAY_STRING("ChatScroll"),
                    .id_ns = 0,
                    .selectable = true,
                };
                view.virtual_identity = TranscriptIdentity(&view);
                RenderTranscript(&view, available_width);
            }
        }

        if (app->chat_overflow)
        {
            PicoScrollbar_Render(CLAY_STRING("ChatScroll"), CLAY_STRING("ChatScrollTrack"),
                                 CLAY_STRING("ChatScrollBarHandle"));
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
        .virtual_cache = &g_main_virtual,
        .scroll_id = CLAY_STRING("ChatScroll"),
        .id_ns = 0,
        .selectable = true,
    };
    view.virtual_identity = TranscriptIdentity(&view);
    return view;
}

bool PicoChat_InspectIsOpen(void)
{
    return g_inspect_n > 0;
}

static bool InspectIsTopModal(PicoApp *app)
{
    const char *top = pico_ui_modal_top(app);
    return g_inspect_n > 0 && top && strcmp(top, "inspect") == 0;
}

static bool InspectPop(void)
{
    if (g_inspect_n <= 0)
    {
        return true;
    }
    if (g_inspect_n == 1 && g_app && !pico_ui_modal_pop(g_app, "inspect"))
    {
        return false;
    }
    g_inspect_n--;
    free(g_inspect[g_inspect_n].tool_call_id);
    free(g_inspect[g_inspect_n].fallback);
    memset(&g_inspect[g_inspect_n], 0, sizeof(g_inspect[g_inspect_n]));
    g_inspect_follow = true;
    return true;
}

void PicoChat_InspectClose(void)
{
    while (g_inspect_n > 0 && InspectPop())
    {
        /* pop every nested frame */
    }
    g_inspect_pressed_dim = false;
    g_inspect_pressed_back = false;
    g_inspect_pressed_tool = false;
    g_inspect_overflow = false;
    memset(&g_inspect_bar, 0, sizeof(g_inspect_bar));
}

static void InspectForceReset(void)
{
    while (g_inspect_n > 0)
    {
        g_inspect_n--;
        free(g_inspect[g_inspect_n].tool_call_id);
        free(g_inspect[g_inspect_n].fallback);
        memset(&g_inspect[g_inspect_n], 0, sizeof(g_inspect[g_inspect_n]));
    }
    g_inspect_pressed_dim = false;
    g_inspect_pressed_back = false;
    g_inspect_pressed_tool = false;
    g_inspect_overflow = false;
    memset(&g_inspect_bar, 0, sizeof(g_inspect_bar));
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
    if (g_inspect_n == 0 &&
        (!g_app || !pico_ui_modal_push(g_app, "inspect")))
    {
        free(frame->tool_call_id);
        free(frame->fallback);
        memset(frame, 0, sizeof(*frame));
        return;
    }
    g_inspect_n++;
    g_inspect_follow = true;
}

static PicoTraceLine *FindToolLine(PicoApp *app, PicoAgentId agent_id, const char *call_id)
{
    PicoAgent *agent;
    int i;
    int t;
    if (!app || !call_id || !call_id[0])
    {
        return NULL;
    }
    agent = PicoAgentManager_Find(app->agents, agent_id);
    if (!agent)
    {
        return NULL;
    }
    for (i = agent->message_count - 1; i >= 0; i--)
    {
        PicoMessage *message = &agent->messages[i];
        for (t = message->trace_count - 1; t >= 0; t--)
        {
            PicoTraceLine *line = &message->trace[t];
            if (line->is_tool && line->tool_call_id && strcmp(line->tool_call_id, call_id) == 0)
            {
                return line;
            }
        }
    }
    return NULL;
}

static void SubagentToolRow(PicoApp *app, PicoToolRowEvent *ev)
{
    PicoTraceLine scratch;
    PicoTraceLine *line;
    if (!ev || !ev->name || strcmp(ev->name, "subagent") != 0)
    {
        return;
    }
    line = FindToolLine(app, ev->agent_id, ev->call_id);
    if (!line)
    {
        memset(&scratch, 0, sizeof(scratch));
        scratch.is_tool = true;
        scratch.tool_name = (char *)(void *)ev->name;
        scratch.tool_call_id = (char *)(void *)ev->call_id;
        scratch.tool_args = (char *)(void *)ev->args_json;
        scratch.tool_args_json = (char *)(void *)ev->args_json;
        scratch.tool_output = (char *)(void *)ev->output;
        scratch.tool_error = ev->is_error;
        scratch.child_id = ev->child_id;
        if (ev->child_session_id)
        {
            snprintf(scratch.child_session_id, sizeof(scratch.child_session_id), "%s",
                     ev->child_session_id);
        }
        line = &scratch;
    }
    InspectPushLine(line, ev->agent_id);
    ev->handled = true;
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
                              CLAY_TEXT_CONFIG({.fontId = FONT_BOLD,
                                                .fontSize = 16,
                                                .textColor = COLOR_TEXT,
                                                .wrapMode = CLAY_TEXT_WRAP_WORDS}));
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
            float scroll_w = card_w - INSPECT_CARD_PAD_X * 2.0f;
            if (g_inspect_overflow)
            {
                scroll_w -= (float)(SCROLLBAR_WIDTH + SCROLLBAR_GAP);
            }
            if (scroll_w < 50.0f)
            {
                scroll_w = 50.0f;
            }
            float header_h = Pico_FontPx(22);
            if (meta[0])
            {
                header_h += Pico_FontPx(16) + 2.0f;
            }
            if (g_inspect_n > 1)
            {
                float back_h = Pico_FontPx(13) + 12.0f;
                if (back_h > header_h)
                {
                    header_h = back_h;
                }
            }
            float scroll_h = card_h - INSPECT_CARD_PAD_Y * 2.0f - INSPECT_CARD_GAP - header_h;
            if (scroll_h < 80.0f)
            {
                scroll_h = 80.0f;
            }
            float transcript_w = scroll_w - INSPECT_MSG_PAD_X * 2.0f;
            if (transcript_w < 10.0f)
            {
                transcript_w = 10.0f;
            }
            CLAY(CLAY_ID("SubagentChatRow"),
                 {.layout = {.layoutDirection = CLAY_LEFT_TO_RIGHT,
                             .childGap = SCROLLBAR_GAP,
                             .sizing = {.width = CLAY_SIZING_GROW(0),
                                        .height = CLAY_SIZING_FIXED(scroll_h)}}})
            {
                CLAY(CLAY_ID("SubagentChatScroll"),
                     {.layout = {.layoutDirection = CLAY_TOP_TO_BOTTOM,
                                 .padding = {0, 0, 0, 12},
                                 .sizing = {.width = CLAY_SIZING_FIXED(scroll_w),
                                            .height = CLAY_SIZING_FIXED(scroll_h)}},
                      .clip = {.vertical = true,
                               .horizontal = true,
                               .childOffset = Clay_GetScrollOffset()}})
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
                            .virtual_cache = &g_inspect_virtual,
                            .scroll_id = CLAY_STRING("SubagentChatScroll"),
                            .id_ns = g_inspect_n,
                            .selectable = false,
                        };
                        view.virtual_identity = RevisionText(TranscriptIdentity(&view),
                                                             inspect.session_id);
                        g_inspect_virtual_ns = view.id_ns;
                        RenderTranscript(&view, transcript_w);
                    }
                    else if (fallback && fallback[0])
                    {
                        RenderToolOutput(NULL, fallback, transcript_w);
                    }
                    else
                    {
                        CLAY_TEXT(CLAY_STRING("No transcript"),
                                  CLAY_TEXT_CONFIG({.fontId = FONT_ITALIC, .fontSize = 14, .textColor = COLOR_MUTED}));
                    }
                }
                if (g_inspect_overflow)
                {
                    PicoScrollbar_Render(CLAY_STRING("SubagentChatScroll"), CLAY_STRING("SubagentChatScrollTrack"),
                                         CLAY_STRING("SubagentChatScrollHandle"));
                }
            }
        }
    }
}

static Clay_ElementId TraceRowId(const TranscriptView *view, const PicoTraceLine *line,
                                 int message_index, int trace_index)
{
    return line && line->is_tool ? ToolRowId(view, message_index, trace_index)
                                 : ThinkRowId(view, message_index, trace_index);
}

static bool HitTraceRow(const TranscriptView *view, const PicoTraceLine *line, int message_index,
                        int trace_index)
{
    if (!line)
    {
        return false;
    }
    return Clay_PointerOver(TraceRowId(view, line, message_index, trace_index)) &&
           (line->is_tool || ThinkHasBody(line));
}

static void InspectHandlePointer(PicoApp *app)
{
    if (!InspectIsTopModal(app))
    {
        return;
    }
    g_inspect_overflow = PicoScrollbar_Overflows(CLAY_STRING("SubagentChatScroll"));
    if (Clay_PointerOver(Clay_GetElementId(CLAY_STRING("SubagentChatRow"))) &&
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
                if ((msg->trace[t].is_tool || ThinkHasBody(&msg->trace[t])) &&
                    HitTraceRow(&view, &msg->trace[t], i, t))
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
        PicoTraceLine *line = (PicoTraceLine *)&inspect.messages[tool_msg].trace[tool_idx];
        if (line->is_tool && pico_tool_row_activate(app, inspect.live_id, line))
        {
            /* hook handled the row */
        }
        else
        {
            line->expanded = !line->expanded;
        }
    }
    g_inspect_pressed_dim = false;
    g_inspect_pressed_back = false;
    g_inspect_pressed_tool = false;
}

void PicoChat_HandleToolRelease(PicoApp *app)
{
    if (!app || IsMouseButtonDown(MOUSE_BUTTON_LEFT) || app->status_warn || PicoUi_ModalOpen(app) ||
        !app->chat_sel.mouse_selecting || app->chat_sel.dragging || !app->chat_sel.pressed_tool ||
        app->chat_sel.tool_msg < 0 || app->chat_sel.tool_msg >= PicoApp_ActiveAgent(app)->message_count)
    {
        return;
    }

    PicoMessage *msg = &PicoApp_ActiveAgent(app)->messages[app->chat_sel.tool_msg];
    int t = app->chat_sel.tool_idx;
    TranscriptView main;
    if (t < 0 || t >= msg->trace_count)
    {
        return;
    }
    main = MainTranscriptView(app);
    if (!HitTraceRow(&main, &msg->trace[t], app->chat_sel.tool_msg, t))
    {
        return;
    }
    if (msg->trace[t].is_tool && pico_tool_row_activate(app, PicoApp_ActiveAgent(app)->id, &msg->trace[t]))
    {
        app->chat_sel.pressed_tool = false;
        return;
    }
    if (IsSubagentTool(&msg->trace[t]))
    {
        return;
    }
    msg->trace[t].expanded = !msg->trace[t].expanded;
    app->chat_sel.pressed_tool = false;
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
            PicoClickSeq_Reset(&app->chat_sel.click_seq);
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
                if ((msg->trace[t].is_tool || ThinkHasBody(&msg->trace[t])) &&
                    HitTraceRow(&main, &msg->trace[t], i, t))
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

        app->chat_sel.mouse_selecting = true;
        app->chat_sel.dragging = false;
        app->chat_sel.press_x = mouse.x;
        app->chat_sel.press_y = mouse.y;
        app->chat_sel.pressed_tool = tool_idx >= 0;
        app->chat_sel.tool_msg = tool_msg;
        app->chat_sel.tool_idx = tool_idx;
        app->composer.sel_anchor = app->composer.cursor;
        if (tool_idx >= 0)
        {
            PicoClickSeq_Reset(&app->chat_sel.click_seq);
            app->chat_sel.granularity = 1;
            app->chat_sel.msg = -1;
            app->chat_sel.anchor = 0;
            app->chat_sel.cursor = 0;
            app->chat_sel.unit_from = 0;
            app->chat_sel.unit_to = 0;
        }
        else
        {
            int msg = -1;
            int pos = PicoChatSel_OffsetAtPoint(app, mouse.x, mouse.y, -1, &msg);
            int count = PicoClickSeq_Press(&app->chat_sel.click_seq, GetTime(), mouse.x, mouse.y);
            PicoChatSel_SelectUnitAt(app, msg, pos, count);
        }
    }

    if (!IsMouseButtonDown(MOUSE_BUTTON_LEFT))
    {
        if (app->chat_sel.mouse_selecting && !app->chat_sel.dragging && app->chat_sel.pressed_tool &&
            app->chat_sel.tool_msg >= 0 && app->chat_sel.tool_msg < PicoApp_ActiveAgent(app)->message_count)
        {
            PicoMessage *msg = &PicoApp_ActiveAgent(app)->messages[app->chat_sel.tool_msg];
            int t = app->chat_sel.tool_idx;
            if (t >= 0 && t < msg->trace_count &&
                HitTraceRow(&main, &msg->trace[t], app->chat_sel.tool_msg, t))
            {
                if (msg->trace[t].is_tool &&
                    pico_tool_row_activate(app, PicoApp_ActiveAgent(app)->id, &msg->trace[t]))
                {
                    /* hook handled the row */
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
            if (app->chat_sel.pressed_tool || app->chat_sel.granularity <= 1)
            {
                if (app->chat_sel.msg < 0)
                {
                    app->chat_sel.msg = msg;
                    app->chat_sel.anchor = pos;
                }
                app->chat_sel.cursor = pos;
            }
            else
            {
                PicoChatSel_ExtendUnitTo(app, pos);
            }
        }
    }
}

static Color ClayToRay(Clay_Color c)
{
    return (Color){(unsigned char)c.r, (unsigned char)c.g, (unsigned char)c.b, (unsigned char)c.a};
}

static void IntersectScissor(Clay_BoundingBox a, Clay_BoundingBox b, Rectangle *out)
{
    float x1 = a.x > b.x ? a.x : b.x;
    float y1 = a.y > b.y ? a.y : b.y;
    float x2 = (a.x + a.width) < (b.x + b.width) ? (a.x + a.width) : (b.x + b.width);
    float y2 = (a.y + a.height) < (b.y + b.height) ? (a.y + a.height) : (b.y + b.height);
    out->x = x1;
    out->y = y1;
    out->width = x2 > x1 ? x2 - x1 : 0;
    out->height = y2 > y1 ? y2 - y1 : 0;
}

static bool InspectScrollClip(Clay_BoundingBox *out)
{
    Clay_ElementData sub = Clay_GetElementData(Clay_GetElementId(CLAY_STRING("SubagentChatScroll")));
    Clay_ElementData card = Clay_GetElementData(Clay_GetElementId(CLAY_STRING("SubagentModalCard")));
    Rectangle vis;
    if (!sub.found)
    {
        return false;
    }
    if (!card.found)
    {
        *out = sub.boundingBox;
        return true;
    }
    IntersectScissor(sub.boundingBox, card.boundingBox, &vis);
    if (vis.width <= 0.5f || vis.height <= 0.5f)
    {
        return false;
    }
    *out = (Clay_BoundingBox){vis.x, vis.y, vis.width, vis.height};
    return true;
}

static void DrawTraceChevrons(const TranscriptView *view, Clay_BoundingBox clip)
{
    if (!view || !view->messages)
    {
        return;
    }
    BeginScissorMode((int)clip.x, (int)clip.y, (int)clip.width, (int)clip.height);
    Font font = Pico_FontAt(FONT_REGULAR, 15);
    const char *glyph = "\xE2\x80\xBA";
    float glyph_px = Pico_FontPx(15);
    Vector2 size = MeasureTextEx(font, glyph, glyph_px, 0.0f);
    for (int i = 0; i < view->message_count; i++)
    {
        PicoMessage *msg = (PicoMessage *)&view->messages[i];
        for (int t = 0; t < msg->trace_count; t++)
        {
            PicoTraceLine *line = &msg->trace[t];
            bool think = !line->is_tool && ThinkHasBody(line);
            if (!line->is_tool && !think)
            {
                continue;
            }
            Clay_ElementId row_id = TraceRowId(view, line, i, t);
            Clay_ElementId chevron_id =
                line->is_tool ? ToolChevronId(view, i, t) : ThinkChevronId(view, i, t);
            bool hovered = Clay_PointerOver(row_id);
            if (!hovered && !line->expanded)
            {
                continue;
            }
            Clay_ElementData el = Clay_GetElementData(chevron_id);
            if (!el.found)
            {
                continue;
            }
            Clay_BoundingBox box = el.boundingBox;
            Vector2 center = {roundf(box.x + box.width * 0.5f), roundf(box.y + box.height * 0.5f)};
            Color color = ClayToRay(hovered ? COLOR_TOOL_NAME_HOVER : COLOR_TOOL_CHEVRON);
            DrawTextPro(font, glyph, center, (Vector2){size.x * 0.5f, size.y * 0.5f},
                        line->expanded ? 90.0f : 0.0f, glyph_px, 0.0f, color);
        }
    }
    EndScissorMode();
}

static void PicoChat_DrawChevrons(PicoApp *app)
{
    if (!app->fonts)
    {
        return;
    }
    if (!PicoUi_ModalOpen(app))
    {
        Clay_ElementData scroll = Clay_GetElementData(Clay_GetElementId(CLAY_STRING("ChatScroll")));
        if (scroll.found)
        {
            TranscriptView main = MainTranscriptView(app);
            DrawTraceChevrons(&main, scroll.boundingBox);
        }
    }
    if (InspectIsTopModal(app))
    {
        PicoSubagentInspect inspect;
        memset(&inspect, 0, sizeof(inspect));
        const char *fallback = NULL;
        if (InspectCurrent(app, &inspect, &fallback) && inspect.message_count > 0)
        {
            Clay_BoundingBox clip;
            if (InspectScrollClip(&clip))
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
                DrawTraceChevrons(&view, clip);
            }
        }
    }
}

static void DrawThinkSheenLabel(Clay_ElementId label_id, Clay_BoundingBox clip, const char *text)
{
    Clay_ElementData el = Clay_GetElementData(label_id);
    if (!el.found || !text || !text[0])
    {
        return;
    }
    Clay_BoundingBox box = el.boundingBox;
    Rectangle vis;
    IntersectScissor(clip, box, &vis);
    if (vis.width <= 0.5f || vis.height <= 0.5f)
    {
        return;
    }
    float period = THINK_SHEEN_PERIOD;
    float t = fmodf((float)GetTime(), period) / period;
    if (t < 0.0f)
    {
        t += 1.0f;
    }
    float band = THINK_SHEEN_BAND * Pico_FontScale();
    if (band < 24.0f)
    {
        band = 24.0f;
    }
    float x = box.x - band + t * (box.width + band * 2.0f);
    Rectangle band_rect = {x, box.y, band, box.height};
    Rectangle sheen;
    IntersectScissor((Clay_BoundingBox){band_rect.x, band_rect.y, band_rect.width, band_rect.height},
                     (Clay_BoundingBox){vis.x, vis.y, vis.width, vis.height}, &sheen);
    if (sheen.width <= 0.5f || sheen.height <= 0.5f)
    {
        return;
    }
    BeginBlendMode(BLEND_ADDITIVE);
    DrawRectangleGradientH((int)sheen.x, (int)sheen.y, (int)sheen.width, (int)sheen.height,
                           (Color){255, 255, 255, 0}, (Color){255, 255, 255, 40});
    EndBlendMode();
    BeginScissorMode((int)sheen.x, (int)sheen.y, (int)(sheen.width + 0.5f), (int)(sheen.height + 0.5f));
    Font font = Pico_FontAt(FONT_ITALIC, 15);
    float px = Pico_FontPx(15);
    DrawTextEx(font, text, (Vector2){roundf(box.x), roundf(box.y)}, px, 0.0f, ClayToRay(COLOR_TEXT));
    EndScissorMode();
}

static void PicoChat_DrawThinkSheen(PicoApp *app)
{
    if (!app || !app->fonts)
    {
        return;
    }
    Clay_ElementData scroll = Clay_GetElementData(Clay_GetElementId(CLAY_STRING("ChatScroll")));
    if (!scroll.found)
    {
        return;
    }
    TranscriptView main = MainTranscriptView(app);
    int last = main.message_count - 1;
    if (last < 0 || main.state != PICO_AGENT_LLM_WAIT)
    {
        return;
    }
    PicoMessage *msg = &PicoApp_ActiveAgent(app)->messages[last];
    if (msg->role != PICO_ROLE_ASSISTANT || (msg->source && msg->source[0]))
    {
        return;
    }
    if (msg->trace_count > 0 && !msg->trace[msg->trace_count - 1].is_tool &&
        ThinkHasBody(&msg->trace[msg->trace_count - 1]))
    {
        int t = msg->trace_count - 1;
        PicoTraceLine *line = &msg->trace[t];
        DrawThinkSheenLabel(ThinkLabelId(&main, last, t), scroll.boundingBox,
                            ThinkHeaderText(line, true));
        return;
    }
    DrawThinkSheenLabel(ThinkSynthId(&main, last), scroll.boundingBox, "Thinking…");
}

static void PicoChat_DrawInspectSheen(PicoApp *app)
{
    if (g_inspect_n <= 0)
    {
        return;
    }
    PicoSubagentInspect inspect;
    memset(&inspect, 0, sizeof(inspect));
    const char *fallback = NULL;
    if (!InspectCurrent(app, &inspect, &fallback) || inspect.state != PICO_AGENT_LLM_WAIT ||
        inspect.message_count <= 0)
    {
        return;
    }
    Clay_BoundingBox clip;
    if (!InspectScrollClip(&clip))
    {
        return;
    }
    PicoAgent *owner = inspect.live_id ? PicoAgentManager_Find(app->agents, inspect.live_id) : NULL;
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
    int last = inspect.message_count - 1;
    const PicoMessage *msg = &inspect.messages[last];
    if (msg->role != PICO_ROLE_ASSISTANT || (msg->source && msg->source[0]))
    {
        return;
    }
    if (msg->trace_count > 0 && !msg->trace[msg->trace_count - 1].is_tool &&
        ThinkHasBody(&msg->trace[msg->trace_count - 1]))
    {
        int t = msg->trace_count - 1;
        DrawThinkSheenLabel(ThinkLabelId(&view, last, t), clip,
                            ThinkHeaderText(&msg->trace[t], true));
        return;
    }
    DrawThinkSheenLabel(ThinkSynthId(&view, last), clip, "Thinking…");
}

void PicoChat_DrawOverlay(PicoApp *app, const PicoHookEvent *event)
{
    (void)event;
    if (!PicoUi_ModalOpen(app))
    {
        PicoChatSel_DrawOverlay(app);
        PicoChat_DrawThinkSheen(app);
    }
    PicoChat_DrawChevrons(app);
    if (InspectIsTopModal(app))
    {
        PicoChat_DrawInspectSheen(app);
    }
}

static void ChatOnFrame(PicoApp *app, float dt)
{
    (void)dt;
    if (g_inspect_n <= 0)
    {
        return;
    }
    PicoScrollbar_UpdateDrag(&g_inspect_bar, CLAY_STRING("SubagentChatScroll"),
                             CLAY_STRING("SubagentChatScrollHandle"));
    if (g_inspect_bar.mouse_down)
    {
        g_inspect_follow = false;
    }
    if (!InspectIsTopModal(app))
    {
        return;
    }
    if (IsKeyPressed(KEY_ESCAPE))
    {
        InspectPop();
    }
}

static void ChatSessionReset(PicoApp *app, const PicoHookEvent *event)
{
    PicoAgent *active = PicoApp_ActiveAgent(app);
    if (!event || !active || event->agent_id == active->id)
    {
        PicoTranscriptVirtual_Free(&g_main_virtual);
    }
    PicoTranscriptVirtual_Free(&g_inspect_virtual);
    g_virtual_relayout = false;
}

static void ChatShutdown(PicoApp *app)
{
    (void)app;
    ThinkFrameFree();
    PicoTranscriptVirtual_Free(&g_main_virtual);
    PicoTranscriptVirtual_Free(&g_inspect_virtual);
    g_virtual_relayout = false;
    PicoChat_InspectClose();
    InspectForceReset();
    g_app = NULL;
}

static void ChatInit(PicoApp *app)
{
    g_app = app;
    if (g_inspect_n > 0 && !pico_ui_modal_has(app, "inspect"))
    {
        InspectForceReset();
    }
    pico_add_view(app, PICO_SLOT_MAIN, 0, PicoChat_Render);
    pico_add_view(app, PICO_SLOT_OVERLAY, 20, InspectRender);
    pico_add_hook(app, PICO_HOOK_AFTER_LAYOUT, PicoChat_HandlePointer);
    pico_add_hook(app, PICO_HOOK_AFTER_RENDER, PicoChat_DrawOverlay);
    pico_add_hook(app, PICO_HOOK_ON_SESSION_RESET, ChatSessionReset);
    pico_add_tool_row_hook(app, SubagentToolRow);
}

PicoExt pico_ext_chat(void)
{
    return (PicoExt){
        .abi = PICO_EXT_ABI,
        .name = "chat",
        .description = "Chat transcript",
        .init = ChatInit,
        .shutdown = ChatShutdown,
        .on_frame = ChatOnFrame,
    };
}
