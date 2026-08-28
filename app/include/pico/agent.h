#ifndef PICO_PUBLIC_AGENT_H
#define PICO_PUBLIC_AGENT_H

#include <stdbool.h>
#include <stdint.h>

#ifndef PICO_EFFORT_LEN
#define PICO_EFFORT_LEN 16
#endif

#define PICO_MAX_AGENTS 16
#define PICO_MAX_TOTAL_AGENTS 32
#define PICO_MAX_SUBAGENT_PROFILES 32
#ifndef PICO_MAX_TOOLS
#define PICO_MAX_TOOLS 64
#endif
#define PICO_MAX_DELEGATION_DEPTH 4
#define PICO_MAX_RETIRED_RUNTIMES 16

typedef uint64_t PicoAgentId;

typedef struct PicoHost PicoHost;
typedef struct PicoWorkspace PicoWorkspace;
typedef struct PicoAgent PicoAgent;
typedef struct PicoAgentContext PicoAgentContext;
typedef struct PicoAgentManager PicoAgentManager;

typedef enum PicoAgentKind {
    PICO_AGENT_MAIN = 0,
    PICO_AGENT_SUBAGENT,
} PicoAgentKind;

typedef enum PicoAgentState {
    PICO_AGENT_IDLE = 0,
    PICO_AGENT_LLM_WAIT,
    PICO_AGENT_TOOL_WAIT,
    PICO_AGENT_COMPACT_WAIT,
    PICO_AGENT_ERROR,
} PicoAgentState;

typedef enum PicoSessionStart {
    PICO_SESSION_NEW = 0,
    PICO_SESSION_RESUME,
    PICO_SESSION_NONE,
} PicoSessionStart;

typedef enum PicoSessionPersistence {
    PICO_SESSION_EPHEMERAL = 0,
    PICO_SESSION_DURABLE,
    PICO_SESSION_FAILED,
} PicoSessionPersistence;

typedef enum PicoSessionWriteResult {
    PICO_SESSION_WRITE_SKIPPED = 0,
    PICO_SESSION_WRITE_OK,
    PICO_SESSION_WRITE_FAILED,
} PicoSessionWriteResult;

typedef enum PicoAgentResult {
    PICO_AGENT_RESULT_OK = 0,
    PICO_AGENT_RESULT_INVALID,
    PICO_AGENT_RESULT_NOT_FOUND,
    PICO_AGENT_RESULT_BUSY,
    PICO_AGENT_RESULT_LIMIT,
    PICO_AGENT_RESULT_SESSION_IN_USE,
    PICO_AGENT_RESULT_SESSION_INVALID,
    PICO_AGENT_RESULT_NO_MEMORY,
} PicoAgentResult;

typedef struct PicoAgentCreateOptions {
    PicoAgentKind kind;
    PicoAgentId parent_id;
    const char *profile;
    const char *purpose;
    const char *model;
    const char *effort;
    const char *const *tools; /* NULL allows all registered tools; non-NULL is an exact-name allowlist. */
    int tool_count;
    PicoSessionStart session_start;
    const char *session_id; /* exact durable ID; used when session_start is PICO_SESSION_RESUME */
    bool select;
} PicoAgentCreateOptions;

typedef struct PicoSubagentProfileInfo {
    char name[65];
    char description[257];
    char purpose[1025];
    char model[128];
    char effort[PICO_EFFORT_LEN];
    bool has_model;
    bool has_effort;
    bool restricted_tools;
    int tool_count;
    char tools[PICO_MAX_TOOLS][128];
} PicoSubagentProfileInfo;

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

    PicoSessionPersistence persistence;
    bool busy;
    bool cancelling;
    bool resumable;
} PicoAgentInfo;

PicoAgentId pico_agent_id(const PicoAgent *agent);
bool pico_agent_info_snapshot(const PicoAgent *agent, PicoAgentInfo *out);

/* Main-thread-only manager API. All returned information is copied. */
int pico_agent_count(const PicoHost *host);
bool pico_agent_info(const PicoHost *host, int index, PicoAgentInfo *out);
bool pico_agent_find(const PicoHost *host, PicoAgentId id, PicoAgentInfo *out);
PicoAgentId pico_agent_active(const PicoHost *host);
bool pico_agent_select(PicoHost *host, PicoAgentId id);
PicoAgentResult pico_agent_create(PicoHost *host, const PicoAgentCreateOptions *options,
                                  PicoAgentId *out);
PicoAgentResult pico_agent_close(PicoHost *host, PicoAgentId id);
PicoAgentResult pico_agent_cancel(PicoHost *host, PicoAgentId id);
PicoAgentResult pico_agent_force_cancel(PicoHost *host, PicoAgentId id);
int pico_subagent_profile_count(const PicoHost *host);
bool pico_subagent_profile_info(const PicoHost *host, int index, PicoSubagentProfileInfo *out);

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
