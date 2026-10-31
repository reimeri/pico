#ifndef PICO_AGENT_INTERNAL_H
#define PICO_AGENT_INTERNAL_H

#include "pico/app.h"
#include "pico/host.h"

struct PicoClarification;
struct PicoAgentRt;
typedef struct PicoAgentRt PicoAgentRt;
typedef struct PicoRegistrationGeneration PicoRegistrationGeneration;

void PicoAgent_RefreshRegistration(PicoHost *app, PicoAgent *agent);
PicoRegistrationGeneration *PicoAgent_Registration(PicoAgent *agent);

/* A delayed selected-transcript reader reconciles if this ring wraps. */
#define PICO_TRANSCRIPT_CHANGE_CAP 256

typedef struct PicoSteeringMessage {
    struct PicoSteeringMessage *next;
    uint64_t id;
    char *text;
    char *display;
    char *parts_json;
    char *input_json;
    size_t bytes;
    MdDocument doc;
} PicoSteeringMessage;

typedef struct PicoMissingImageWarning {
    char *path;
    char *notice;
} PicoMissingImageWarning;

struct PicoAgent {
    PicoWorkspace *workspace;
    PicoAgentId id;
    PicoAgentId parent_id;
    uint64_t runtime_generation;
    PicoAgentKind kind;
    struct PicoClarification *clarification;
    char *turn_user_request;
    char *originating_user_request;
    bool running_fast;
    int depth;
    /* Monotonic seconds of the last user-facing activity (selection, submit,
     * session commit). Drives least-recently-active eviction at the agent caps. */
    double last_activity;
    double inbox_activity; /* monotonic conversation/status activity, not selection */
    int64_t last_user_action; /* Unix seconds; accepted messages, steering and user answers only. */
    double inbox_working_activity; /* monotonic turn start or accepted user input, never background activity */
    char profile[65];
    char purpose[1025];
    char parent_session_id[40];

    PicoMessage *messages;
    int message_count;
    int message_capacity;
    /* Main-thread transcript invalidations. Old readers detect journal overflow
     * and do a full reconciliation rather than losing an offscreen edit. */
    uint64_t transcript_reset_generation;
    uint64_t transcript_change_seq;
    int transcript_changes[PICO_TRANSCRIPT_CHANGE_CAP];

    PicoSteeringMessage *steering_head;
    PicoSteeringMessage *steering_tail;
    int steering_count;
    size_t steering_bytes;
    uint64_t last_steering_id;
    PicoMissingImageWarning *missing_image_warnings;
    int missing_image_warning_count; /* session-owned, restored from notice metadata */
    bool settling_error; /* no new turns/close while error callbacks are dispatched */
    bool steering_delivery; /* prevents recursive delivery from message hooks */

    PicoAgentState state;
    bool unseen_complete;
    bool browser_isolated; /* Durable conversation override; false shares workspace site storage. */
    bool browser_expanded; /* Session-local presentation; opening is explicit and lazy. */
    float browser_width; /* Preferred dock width in logical pixels; zero uses the default. Never persisted. */
    bool inbox_pending; /* inbox review flag; not a run state */
    uint64_t inbox_revision; /* protects newer live state from a failed offline ack */
    PicoAgentRt *runtime;
    char *error;
    char activity[256];
    char *compact_summary;

    char session_id[40];
    char session_path[4096];
    PicoSessionPersistence persistence;
    bool accepted_submit;
    bool has_llm_response; /* successful non-compaction response, restored from session usage */
    uint64_t session_input_tokens;
    uint64_t session_cached_tokens;
    int tokens_used;
    int tokens_cached;
    bool tokens_unknown; /* native replacement size awaits the next ordinary response */
    /* Main-thread display cache, never persisted; only user main agents sample. */
    double tokens_per_second;
    bool has_tokens_per_second;

    char model[128];
    char model_name[128];
    char effort[PICO_EFFORT_LEN];
    bool fast; /* selected mode for the next turn */
    char last_service_tier[32];
    int context_limit;
    /* Turn-scoped snapshot of the selected model and effort: pinned at turn
     * start and used for every request of the turn (tool follow-ups and
     * compaction included). Released when the agent goes idle. */
    PicoModel running_model;
    bool has_running_model;
    char running_effort[PICO_EFFORT_LEN];
    double compact_ratio;
    bool compact_enabled;
    int max_parallel_tools_override; /* zero uses workspace setting */

    /* NULL means all registered tools. A non-NULL snapshot is agent-owned. */
    char **allowed_tools;
    int allowed_tool_count;
    bool tool_policy_valid;
};

#endif
