#include "trace_group.h"
#include "pico/app.h"

#include <stdio.h>
#include <string.h>

static int Fail(const char *message)
{
    fprintf(stderr, "trace group: %s\n", message);
    return 1;
}

static int TestTitle(void)
{
    char buf[128];

    if (pico_trace_group_format_title(buf, sizeof(buf), 3, 1, 1, true, 53000) <= 0 ||
        strcmp(buf, "3 x tool calls, 1 x spawn process, 1 x subagent, thought 53s") != 0)
    {
        return Fail("mixed title did not join specials and thought in order");
    }
    if (pico_trace_group_format_title(buf, sizeof(buf), 1, 0, 0, false, 0) <= 0 ||
        strcmp(buf, "1 x tool call") != 0)
    {
        return Fail("single generic tool did not use singular label");
    }
    if (pico_trace_group_format_title(buf, sizeof(buf), 0, 2, 2, false, 0) <= 0 ||
        strcmp(buf, "2 x spawn processes, 2 x subagents") != 0)
    {
        return Fail("plural spawn and subagent labels were wrong");
    }
    if (pico_trace_group_format_title(buf, sizeof(buf), 0, 0, 0, true, 12500) <= 0 ||
        strcmp(buf, "thought 13s") != 0)
    {
        return Fail("thought-only title did not round milliseconds to seconds");
    }
    if (pico_trace_group_format_title(buf, sizeof(buf), 1, 0, 0, true, 0) <= 0 ||
        strcmp(buf, "1 x tool call, thought") != 0)
    {
        return Fail("mixed title omitted untimed thinking");
    }
    if (pico_trace_group_format_title(buf, sizeof(buf), 0, 0, 0, false, 0) != 0 ||
        buf[0] != '\0')
    {
        return Fail("empty counts should produce an empty title");
    }
    return 0;
}

static int TestOpenAndKind(void)
{
    PicoTraceLine think;
    PicoTraceLine sh;
    PicoTraceLine spawn;
    PicoTraceLine subagent;
    PicoTraceLine empty;

    memset(&think, 0, sizeof(think));
    memset(&sh, 0, sizeof(sh));
    memset(&spawn, 0, sizeof(spawn));
    memset(&subagent, 0, sizeof(subagent));
    memset(&empty, 0, sizeof(empty));
    think.text = "reason";
    sh.is_tool = true;
    sh.tool_name = "sh";
    spawn.is_tool = true;
    spawn.tool_name = "run_background";
    subagent.is_tool = true;
    subagent.tool_name = "subagent";

    if (pico_trace_line_group_kind(&think) != PICO_TRACE_GROUP_THINK ||
        pico_trace_line_group_kind(&sh) != PICO_TRACE_GROUP_TOOL ||
        pico_trace_line_group_kind(&spawn) != PICO_TRACE_GROUP_SPAWN ||
        pico_trace_line_group_kind(&subagent) != PICO_TRACE_GROUP_SUBAGENT ||
        pico_trace_line_group_kind(&empty) != PICO_TRACE_GROUP_NONE)
    {
        return Fail("finished-row kinds did not split think, tools, spawn, and subagent");
    }
    if (!pico_trace_line_open(&sh, true, false, false) ||
        pico_trace_line_open(&sh, false, false, true))
    {
        return Fail("pending tools stay open; idle tools join the group");
    }
    if (!pico_trace_line_open(&sh, false, true, false))
    {
        return Fail("an output-less tool stays open when it is the fallback live row");
    }
    sh.tool_output = "done";
    if (pico_trace_line_open(&sh, false, true, false))
    {
        return Fail("a tool with output joins the group despite fallback live state");
    }
    if (!pico_trace_line_open(&think, false, false, true) ||
        pico_trace_line_open(&think, true, true, false))
    {
        return Fail("live think stays open; frozen think joins the group");
    }
    return 0;
}

int main(void)
{
    int failed = 0;
    failed |= TestTitle();
    failed |= TestOpenAndKind();
    return failed;
}
