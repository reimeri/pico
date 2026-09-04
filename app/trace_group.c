#include "trace_group.h"

#include "pico/app.h"

#include <stdio.h>
#include <string.h>

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
    if (line->tool_name && strcmp(line->tool_name, "run_background") == 0)
    {
        return PICO_TRACE_GROUP_SPAWN;
    }
    if (line->tool_name && strcmp(line->tool_name, "subagent") == 0)
    {
        return PICO_TRACE_GROUP_SUBAGENT;
    }
    return PICO_TRACE_GROUP_TOOL;
}

bool pico_trace_line_open(const PicoTraceLine *line, bool tool_pending, bool tool_fallback_live,
                          bool think_live)
{
    if (!pico_trace_line_visible(line))
    {
        return false;
    }
    if (line->is_tool)
    {
        return tool_pending || (!line->tool_output && tool_fallback_live);
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

int pico_trace_group_format_title(char *buf, size_t cap, int tool_calls, int spawn_processes,
                                  int subagents, bool has_thinking, int think_ms)
{
    size_t used = 0;
    char part[64];

    if (!buf || cap == 0)
    {
        return 0;
    }
    buf[0] = '\0';
    if (tool_calls < 0)
    {
        tool_calls = 0;
    }
    if (spawn_processes < 0)
    {
        spawn_processes = 0;
    }
    if (subagents < 0)
    {
        subagents = 0;
    }
    if (think_ms < 0)
    {
        think_ms = 0;
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
    if (spawn_processes == 1)
    {
        TitleAppend(buf, cap, &used, "1 x spawn process");
    }
    else if (spawn_processes > 1)
    {
        snprintf(part, sizeof(part), "%d x spawn processes", spawn_processes);
        TitleAppend(buf, cap, &used, part);
    }
    if (subagents == 1)
    {
        TitleAppend(buf, cap, &used, "1 x subagent");
    }
    else if (subagents > 1)
    {
        snprintf(part, sizeof(part), "%d x subagents", subagents);
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
