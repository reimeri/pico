#ifndef PICO_TRACE_GROUP_H
#define PICO_TRACE_GROUP_H

#include <stdbool.h>
#include <stddef.h>

typedef struct PicoTraceLine PicoTraceLine;

typedef enum PicoTraceGroupKind {
    PICO_TRACE_GROUP_NONE = 0,
    PICO_TRACE_GROUP_THINK,
    PICO_TRACE_GROUP_TOOL,
    PICO_TRACE_GROUP_SPAWN,
    PICO_TRACE_GROUP_SUBAGENT,
} PicoTraceGroupKind;

bool pico_trace_line_visible(const PicoTraceLine *line);
PicoTraceGroupKind pico_trace_line_group_kind(const PicoTraceLine *line);
bool pico_trace_line_open(const PicoTraceLine *line, bool tool_pending, bool tool_fallback_live,
                          bool think_live);
int pico_trace_group_format_title(char *buf, size_t cap, int tool_calls, int spawn_processes,
                                  int subagents, bool has_thinking, int think_ms);

#endif
