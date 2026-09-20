#define _POSIX_C_SOURCE 200809L

#include "trace_group.h"

#include "pico/app.h"

#include <stdio.h>
#include <string.h>
#include <time.h>

#define PICO_TOOL_ROW_DWELL_SEC 2.0

bool pico_trace_line_visible(const PicoTraceLine *line)
{
    if (!line)
    {
        return false;
    }
    if (line->is_tool)
    {
        return line->tool_name && line->tool_name[0];
    }
    if (line->think_part_count > 0)
    {
        return true;
    }
    return line->text && line->text[0];
}

PicoTraceGroupKind pico_trace_line_group_kind(const PicoTraceLine *line)
{
    if (!pico_trace_line_visible(line))
    {
        return PICO_TRACE_GROUP_NONE;
    }
    if (!line->is_tool)
    {
        return PICO_TRACE_GROUP_THINK;
    }
    return PICO_TRACE_GROUP_TOOL;
}

bool pico_trace_tool_group_label(const PicoTool *tools, int tool_count, const char *tool_name,
                                 const char **singular, const char **plural)
{
    if (!tools || tool_count <= 0 || !tool_name || !tool_name[0])
    {
        return false;
    }
    for (int i = 0; i < tool_count; i++)
    {
        const PicoTool *tool = &tools[i];
        if (!tool->name || strcmp(tool->name, tool_name) != 0)
        {
            continue;
        }
        if (!tool->group_singular || !tool->group_singular[0] || !tool->group_plural ||
            !tool->group_plural[0])
        {
            return false;
        }
        if (singular)
        {
            *singular = tool->group_singular;
        }
        if (plural)
        {
            *plural = tool->group_plural;
        }
        return true;
    }
    return false;
}

double pico_trace_now(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1000000000.0;
}

bool pico_trace_tool_row_dwelling(double done_t0, double now)
{
    double elapsed;
    if (done_t0 <= 0.0)
    {
        return false;
    }
    elapsed = now - done_t0;
    return elapsed >= 0.0 && elapsed < PICO_TOOL_ROW_DWELL_SEC;
}

void pico_trace_line_stamp_tool_done(PicoTraceLine *line)
{
    if (!line || line->tool_done_t0 > 0.0)
    {
        return;
    }
    line->tool_done_t0 = pico_trace_now();
    if (line->tool_done_t0 <= 0.0)
    {
        line->tool_done_t0 = 0.000000001;
    }
}

bool pico_trace_line_open(const PicoTraceLine *line, bool tool_pending, bool tool_fallback_live,
                          bool think_live, bool tool_dwell_live)
{
    if (!pico_trace_line_visible(line))
    {
        return false;
    }
    if (line->is_tool)
    {
        return tool_pending || tool_dwell_live || (!line->tool_output && tool_fallback_live);
    }
    return think_live;
}

static void TitleAppend(char *buf, size_t cap, size_t *used, const char *part)
{
    int n;
    if (!buf || !used || !part || cap == 0 || *used >= cap)
    {
        return;
    }
    if (*used == 0)
    {
        n = snprintf(buf, cap, "%s", part);
    }
    else
    {
        n = snprintf(buf + *used, cap - *used, ", %s", part);
    }
    if (n < 0)
    {
        buf[cap - 1] = '\0';
        *used = cap - 1;
        return;
    }
    if (*used + (size_t)n >= cap)
    {
        *used = cap - 1;
        return;
    }
    *used += (size_t)n;
}

int pico_trace_group_format_title(char *buf, size_t cap, int tool_calls,
                                  const PicoTraceGroupLabel *labels, int label_count, bool has_thinking,
                                  int think_ms)
{
    size_t used = 0;
    char part[96];

    if (!buf || cap == 0)
    {
        return 0;
    }
    buf[0] = '\0';
    if (tool_calls < 0)
    {
        tool_calls = 0;
    }
    if (label_count < 0)
    {
        label_count = 0;
    }
    if (think_ms < 0)
    {
        think_ms = 0;
    }
    if (!labels)
    {
        label_count = 0;
    }

    if (tool_calls == 1)
    {
        TitleAppend(buf, cap, &used, "1 x tool call");
    }
    else if (tool_calls > 1)
    {
        snprintf(part, sizeof(part), "%d x tool calls", tool_calls);
        TitleAppend(buf, cap, &used, part);
    }
    for (int i = 0; i < label_count; i++)
    {
        const PicoTraceGroupLabel *label = &labels[i];
        int count = label->count;
        if (count <= 0 || !label->singular || !label->singular[0] || !label->plural || !label->plural[0])
        {
            continue;
        }
        if (count == 1)
        {
            snprintf(part, sizeof(part), "1 x %s", label->singular);
        }
        else
        {
            snprintf(part, sizeof(part), "%d x %s", count, label->plural);
        }
        TitleAppend(buf, cap, &used, part);
    }
    if (think_ms > 0)
    {
        snprintf(part, sizeof(part), "thought %lds", (think_ms + 500) / 1000L);
        TitleAppend(buf, cap, &used, part);
    }
    else if (has_thinking)
    {
        TitleAppend(buf, cap, &used, "thought");
    }
    return (int)used;
}
