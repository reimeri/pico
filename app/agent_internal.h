#ifndef PICO_AGENT_INTERNAL_H
#define PICO_AGENT_INTERNAL_H

#include "pico/app.h"

struct PicoAgentRt;
typedef struct PicoAgentRt PicoAgentRt;

typedef enum PicoSessionPersistence {
    PICO_SESSION_EPHEMERAL = 0,
    PICO_SESSION_DURABLE,
    PICO_SESSION_FAILED,
} PicoSessionPersistence;

struct PicoAgent {
    PicoAgentId id;
    PicoAgentId parent_id;
    PicoAgentKind kind;
    int depth;
    char profile[65];
    char purpose[1025];

    PicoMessage *messages;
    int message_count;
    int message_capacity;

    PicoAgentState state;
    PicoAgentRt *runtime;
    char *error;
    char activity[256];
    char *compact_summary;

    char session_id[40];
    char session_path[4096];
    PicoSessionPersistence persistence;
    uint64_t session_input_tokens;
    uint64_t session_cached_tokens;
    int tokens_used;
    int tokens_cached;

    char model[128];
    char model_name[128];
    char effort[PICO_EFFORT_LEN];
    int context_limit;
    double compact_ratio;
    bool compact_enabled;

    /* NULL means all registered tools. A non-NULL snapshot is agent-owned. */
    char **allowed_tools;
    int allowed_tool_count;
};

static inline PicoAgent *PicoApp_ActiveAgent(PicoApp *app)
{
    return app ? app->agent : NULL;
}

static inline const PicoAgent *PicoApp_ActiveAgentConst(const PicoApp *app)
{
    return app ? app->agent : NULL;
}

#endif
