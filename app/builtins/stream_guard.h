#ifndef PICO_STREAM_GUARD_H
#define PICO_STREAM_GUARD_H

#include <stddef.h>

/* Internal builtin-provider policy, reset for every request attempt. Only new,
 * decoded output belongs here, never SSE metadata or cumulative snapshots. */
#define PICO_STREAM_TOOL_ARGUMENT_LIMIT (1024u * 1024u)

typedef enum PicoStreamKind {
    PICO_STREAM_TEXT,
    PICO_STREAM_THINKING,
    PICO_STREAM_SUMMARY,
    PICO_STREAM_ARGUMENTS,
} PicoStreamKind;

typedef struct PicoStreamPart PicoStreamPart;
typedef struct PicoStreamGuard {
    PicoStreamPart *parts;
    const char *error; /* sticky, borrowed; never capacity-retry a guard failure */
} PicoStreamGuard;

/* NULL on success, otherwise a borrowed failure explanation. index separates
 * interleaved tool calls; non-tool streams use -1. */
const char *pico_stream_guard_delta(PicoStreamGuard *guard, PicoStreamKind kind, int index,
                                   const char *text, size_t len);
/* Full tool-argument snapshots are size-checked, not added to streamed bytes. */
const char *pico_stream_guard_tool_size(PicoStreamGuard *guard, size_t len);
void pico_stream_guard_free(PicoStreamGuard *guard);

#endif
