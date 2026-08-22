#ifndef PICO_PUBLIC_AGENT_H
#define PICO_PUBLIC_AGENT_H

#include <stdbool.h>
#include <stdint.h>

#ifndef PICO_EFFORT_LEN
#define PICO_EFFORT_LEN 16
#endif

#define PICO_MAX_AGENTS 16
#define PICO_MAX_SUBAGENT_PROFILES 32
#define PICO_MAX_DELEGATION_DEPTH 4
#define PICO_MAX_RETIRED_RUNTIMES 16

typedef uint64_t PicoAgentId;

typedef struct PicoAgent PicoAgent;
typedef struct PicoAgentContext PicoAgentContext;

typedef enum PicoAgentKind {
    PICO_AGENT_NORMAL = 0,
    PICO_AGENT_SUBAGENT,
} PicoAgentKind;

typedef enum PicoAgentState {
    PICO_AGENT_IDLE = 0,
    PICO_AGENT_LLM_WAIT,
    PICO_AGENT_TOOL_WAIT,
    PICO_AGENT_COMPACT_WAIT,
    PICO_AGENT_ERROR,
} PicoAgentState;

typedef struct PicoAgentInfo {
    PicoAgentId id;
    PicoAgentId parent_id;
    PicoAgentKind kind;
    PicoAgentState state;
    int depth;

    char session_id[40];
    char profile[65];
    char purpose[1025];
    char model[128];
    char effort[PICO_EFFORT_LEN];
    char activity[256];

    bool busy;
    bool cancelling;
    bool resumable;
} PicoAgentInfo;

PicoAgentId pico_agent_id(const PicoAgent *agent);
bool pico_agent_info_snapshot(const PicoAgent *agent, PicoAgentInfo *out);

/* Worker callback context. All returned strings are read-only and valid only
 * for the duration of the callback that received ctx. */
PicoAgentId pico_agent_context_id(const PicoAgentContext *ctx);
uint64_t pico_agent_context_generation(const PicoAgentContext *ctx);
const char *pico_agent_context_workspace(const PicoAgentContext *ctx);
const char *pico_agent_context_session_id(const PicoAgentContext *ctx);
const char *pico_agent_context_profile(const PicoAgentContext *ctx);
const char *pico_agent_context_purpose(const PicoAgentContext *ctx);
bool pico_agent_context_safe_mode(const PicoAgentContext *ctx);
bool pico_agent_context_cancelled(const PicoAgentContext *ctx);

#endif
