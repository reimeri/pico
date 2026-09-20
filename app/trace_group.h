#ifndef PICO_TRACE_GROUP_H
#define PICO_TRACE_GROUP_H

#include <stdbool.h>
#include <stddef.h>

typedef struct PicoTraceLine PicoTraceLine;
typedef struct PicoTool PicoTool;

typedef enum PicoTraceGroupKind {
    PICO_TRACE_GROUP_NONE = 0,
    PICO_TRACE_GROUP_THINK,
    PICO_TRACE_GROUP_TOOL,
} PicoTraceGroupKind;

typedef struct PicoTraceGroupLabel {
    const char *singular;
    const char *plural;
    int count;
} PicoTraceGroupLabel;

bool pico_trace_line_visible(const PicoTraceLine *line);
PicoTraceGroupKind pico_trace_line_group_kind(const PicoTraceLine *line);
bool pico_trace_tool_group_label(const PicoTool *tools, int tool_count, const char *tool_name,
                                 const char **singular, const char **plural);
double pico_trace_now(void);
bool pico_trace_tool_row_dwelling(double done_t0, double now);
void pico_trace_line_stamp_tool_done(PicoTraceLine *line);
bool pico_trace_line_open(const PicoTraceLine *line, bool tool_pending, bool tool_fallback_live,
                          bool think_live, bool tool_dwell_live);
int pico_trace_group_format_title(char *buf, size_t cap, int tool_calls,
                                  const PicoTraceGroupLabel *labels, int label_count, bool has_thinking,
                                  int think_ms);

#endif
