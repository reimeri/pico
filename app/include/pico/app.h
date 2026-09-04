#ifndef PICO_APP_H
#define PICO_APP_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

#include "raylib.h"

#include "markdown.h"
#include "pico/agent.h"
#include "pico/host.h"
#include "pico/theme.h"
#include "pico/workspace.h"
#include "text_range.h"

#define PICO_MAX_SLOT_VIEWS 16
#define PICO_MAX_EMPTY_VIEWS 16
#define PICO_MAX_HOOKS 64
#define PICO_MAX_TOOL_HOOKS 64
#define PICO_MAX_LLM_HOOKS 64
#define PICO_MAX_CONTEXT_HOOKS 64
#define PICO_MAX_TOOL_ROW_HOOKS 64
#define PICO_MAX_UI_MODALS 16
#define PICO_UI_MODAL_NAME 64
#define PICO_MAX_UI_POSTS 16
#define PICO_UI_POST_TEXT_MAX (64 * 1024)
#define PICO_UI_POST_STATUS_MAX 128
#define PICO_TOOL_DETAILS_MAX (64 * 1024)
#define PICO_TOOL_ASK_MAX_REQUEST (64 * 1024)
#define PICO_TOOL_ASK_MAX_ANSWER (64 * 1024)
#define PICO_MAX_COMMANDS 64
#define PICO_MAX_COMPLETERS 16
#define PICO_MAX_COMPLETE_ITEMS 24
#define PICO_MAX_PROVIDERS 16
#define PICO_MAX_AUTH 16
#define PICO_MAX_EFFORTS 16
#define PICO_MAX_DISABLED_EXTENSIONS 48
#define PICO_DISABLED_EXT_NAME 64

typedef enum PicoRole {
    PICO_ROLE_USER = 0,
    PICO_ROLE_ASSISTANT,
} PicoRole;

typedef enum PicoUiSlot {
    PICO_SLOT_SIDEBAR = 0,
    PICO_SLOT_MAIN,
    PICO_SLOT_COMPOSER, /* not rendered without a selected agent */
    PICO_SLOT_FOOTER,   /* not rendered without a selected agent */
    PICO_SLOT_OVERLAY,
    PICO_SLOT_COUNT,
} PicoUiSlot;

typedef enum PicoEmptyKind {
    PICO_EMPTY_ABOVE = 0, /* stacked above the three cards */
    PICO_EMPTY_BELOW,     /* stacked below the three cards */
    PICO_EMPTY_REPLACE,   /* takes over the empty state */
} PicoEmptyKind;

typedef enum PicoHook {
    PICO_HOOK_AFTER_LAYOUT = 0,
    PICO_HOOK_AFTER_RENDER,
    PICO_HOOK_BEFORE_SUBMIT, /* pico_host_request_submit_cancel / pico_host_set_agent_input */
    PICO_HOOK_ON_SUBMIT,
    PICO_HOOK_ON_MESSAGE,
    PICO_HOOK_ON_COMPACT, /* pico_agent_set_compact_summary can replace the default briefing */
    PICO_HOOK_AFTER_COMPACT,
    PICO_HOOK_ON_TURN_END, /* idle after a finished turn (not cancel/error) */
    PICO_HOOK_ON_CANCEL,
    PICO_HOOK_ON_ERROR,
    PICO_HOOK_ON_ASK, /* pending ask snapshot published for this agent */
    PICO_HOOK_ON_ASK_END, /* that ask is no longer pending (answer/cancel/stop) */
    PICO_HOOK_ON_SESSION_RESET,
    PICO_HOOK_ON_AGENT_DESTROY,
    PICO_HOOK_COUNT,
} PicoHook;

typedef struct PicoModel {
    char id[128];
    char name[128];
    char provider[64];
    char base_url[512];
    int context_limit;
    bool vision;
    char effort[PICO_MAX_EFFORTS][PICO_EFFORT_LEN];
    int effort_count;
    char default_effort[PICO_EFFORT_LEN];
} PicoModel;

typedef struct PicoTraceLine {
    char *text;
    bool is_tool;
    char *tool_name;
    char *tool_call_id;
    char *tool_args;      /* formatted for transcript display */
    char *tool_args_json; /* original JSON supplied by the provider */
    char *tool_output;
    bool tool_error;
    bool expanded;
    int think_steps;
    char **think_parts;
    int think_part_count;
    double think_t0;
    int think_ms;
    PicoAgentId child_id;
    char child_session_id[40];
    MdDocument doc;
} PicoTraceLine;

typedef struct PicoMessage {
    PicoRole role;
    char *source;
    PicoTraceLine *trace;
    int trace_count;
    bool trace_group_expanded;
    MdDocument doc;
} PicoMessage;

typedef struct PicoChatSelect {
    int msg;
    int anchor;
    int cursor;
    bool mouse_selecting;
    bool dragging;
    bool pressed_tool;
    bool pressed_group;
    int tool_msg;
    int tool_idx;
    float press_x;
    float press_y;
    int granularity;
    int unit_from;
    int unit_to;
    PicoClickSeq click_seq;
} PicoChatSelect;

typedef struct PicoComposer {
    char *text;
    int length;
    int capacity;
    int cursor;
    int sel_anchor;
    bool mouse_selecting;
    int granularity;
    int unit_from;
    int unit_to;
    PicoClickSeq click_seq;
} PicoComposer;

typedef struct PicoScrollbar {
    Clay_Vector2 click_origin;
    Clay_Vector2 position_origin;
    bool mouse_down;
} PicoScrollbar;

/* Sidebar/main/overlay host views may run with zero workspaces and no selected agent. */
typedef void (*PicoHostViewFn)(PicoHost *host, void *state);
typedef void (*PicoWorkspaceViewFn)(PicoWorkspace *workspace, PicoAgentId selected_agent_id, void *state);

typedef struct PicoHookEvent {
    PicoHook hook;
    PicoAgentId agent_id; /* zero only for host-global hooks without an agent target */
} PicoHookEvent;

typedef void (*PicoHostHookFn)(PicoHost *host, const PicoHookEvent *event, void *state);
typedef void (*PicoWorkspaceHookFn)(PicoWorkspace *workspace, const PicoHookEvent *event, void *state);

typedef struct PicoToolResult {
    char *output;       /* malloc; Pico frees */
    char *details_json; /* optional malloc'd JSON object; Pico validates and frees */
    bool is_error;
} PicoToolResult;

typedef void (*PicoToolFn)(PicoAgentContext *ctx, const char *args_json, PicoToolResult *out, void *state);
typedef bool (*PicoToolApplyFn)(PicoWorkspace *workspace, PicoAgentId agent_id, const char *details_json,
                                bool replay, void *state);
typedef void (*PicoHostCmdFn)(PicoHost *host, PicoAgentId agent_id, const char *args, void *state);
typedef void (*PicoWorkspaceCmdFn)(PicoWorkspace *workspace, PicoAgentId agent_id, const char *args,
                                   void *state);

#define PICO_COMPLETE_LABEL_MAX 288

typedef struct PicoCompleteItem {
    char label[PICO_COMPLETE_LABEL_MAX + 1];
    char detail[128];
    char insert[512];
} PicoCompleteItem;

typedef int (*PicoHostCompleteQueryFn)(PicoHost *host, const char *prefix, PicoCompleteItem *out, int max,
                                       void *state);
typedef bool (*PicoHostCompleteAcceptFn)(PicoHost *host, const PicoCompleteItem *item, void *state);
typedef int (*PicoWorkspaceCompleteQueryFn)(PicoWorkspace *workspace, const char *prefix, PicoCompleteItem *out,
                                            int max, void *state);
typedef bool (*PicoWorkspaceCompleteAcceptFn)(PicoWorkspace *workspace, const PicoCompleteItem *item, void *state);

typedef struct PicoSlotView {
    PicoHostViewFn host_render;
    PicoWorkspaceViewFn workspace_render;
    PicoWorkspace *workspace;
    int z;
    void *state;
} PicoSlotView;

typedef struct PicoEmptyView {
    PicoHostViewFn host_render;
    PicoWorkspaceViewFn workspace_render;
    PicoWorkspace *workspace;
    PicoEmptyKind kind;
    int z;
    void *state;
} PicoEmptyView;

typedef struct PicoHookEntry {
    PicoHook hook;
    PicoHostHookFn host_fn;
    PicoWorkspaceHookFn workspace_fn;
    PicoWorkspace *workspace;
    void *state;
} PicoHookEntry;

typedef struct PicoTool {
    const char *name;
    const char *description;
    const char *params_json;
    PicoToolFn run;
    PicoToolApplyFn apply; /* optional; main thread after success and during replay */
    void *state;
} PicoTool;

typedef struct PicoToolEvent {
    const char *name;
    const char *call_id;
    const char *args_json; /* current args; core-owned */
    char *args_json_out;   /* BEFORE only: malloc rewrite; Pico frees */
    bool deny;             /* BEFORE only */
    const char *output;       /* AFTER only: current output; core-owned */
    const char *details_json; /* AFTER only: validated details object; core-owned */
    bool executed;            /* AFTER only: false when denied */
    bool is_error;            /* AFTER only: tool-defined/core failure */
    char *result;             /* BEFORE+deny, or AFTER rewrite; malloc, Pico frees */
} PicoToolEvent;

typedef void (*PicoToolBeforeFn)(PicoAgentContext *ctx, PicoToolEvent *event, void *state);
typedef void (*PicoToolAfterFn)(PicoWorkspace *workspace, PicoAgentId agent_id, PicoToolEvent *event, void *state);

typedef struct PicoToolRowEvent {
    PicoAgentId agent_id;
    const char *name;      /* tool name; core-owned, callback-scoped */
    const char *call_id;   /* may be NULL */
    const char *args_json;        /* original tool arguments; may be NULL */
    const char *output;           /* NULL while the call is still running */
    PicoAgentId child_id;         /* subagent child identity; zero when absent */
    const char *child_session_id; /* subagent session identity; may be NULL */
    bool is_error;
    bool handled;          /* first hook that sets this skips later hooks and expand */
} PicoToolRowEvent;

typedef void (*PicoToolRowFn)(PicoWorkspace *workspace, PicoToolRowEvent *event, void *state);

typedef struct PicoLlmEvent {
    bool compact;
    bool include_tools; /* read-only; false => catalog omitted, exclude ignored */
    const PicoTool *tools;
    int tool_count;
    bool *exclude; /* include_tools ? tool_count flags : NULL */
    const char *instructions;
    char *extra_instructions; /* malloc; core appends under "## Additional instructions" after this hook and frees */
} PicoLlmEvent;

typedef void (*PicoLlmHookFn)(PicoWorkspace *workspace, PicoAgentId agent_id, PicoLlmEvent *event, void *state);

typedef struct PicoContextEvent {
    bool compact;
    const char *const *history_json; /* immutable base history for this request */
    int history_count;
    const PicoTool *tools; /* final effective catalog for this request */
    int tool_count;
    char *extra_context; /* optional malloc'd text; Pico appends a request-only context item */
} PicoContextEvent;

typedef void (*PicoContextHookFn)(PicoWorkspace *workspace, PicoAgentId agent_id, PicoContextEvent *event,
                                  void *state);

typedef struct PicoCommand {
    const char *name;
    const char *help;
    PicoHostCmdFn host_run;
    PicoWorkspaceCmdFn workspace_run;
    PicoWorkspace *workspace;
    void *state;
} PicoCommand;

typedef struct PicoCompleter {
    char trigger;
    bool bol_only;
    PicoHostCompleteQueryFn host_query;
    PicoHostCompleteAcceptFn host_accept;
    PicoWorkspaceCompleteQueryFn workspace_query;
    PicoWorkspaceCompleteAcceptFn workspace_accept;
    PicoWorkspace *workspace;
    void *state;
} PicoCompleter;

typedef bool (*PicoLlmCancelFn)(void *user);

typedef enum PicoLlmDeltaKind {
    PICO_LLM_DELTA_TEXT = 0,
    PICO_LLM_DELTA_THINKING,
    PICO_LLM_DELTA_THINKING_SUMMARY,
    PICO_LLM_DELTA_STATUS,
} PicoLlmDeltaKind;

typedef void (*PicoLlmDeltaFn)(void *user, PicoLlmDeltaKind kind, const char *s, size_t n);

enum {
    PICO_LLM_OK = 0,
    PICO_LLM_FAIL = 1,
    PICO_LLM_CANCEL = 2,
};

enum {
    PICO_ASK_OK = 0,
    PICO_ASK_CANCEL = 1,
    PICO_ASK_FAIL = 2,
};

typedef struct PicoToolAsk {
    uint64_t id;
    PicoAgentId agent_id;
    const char *profile;
    const char *purpose;
    const char *request_json;
} PicoToolAsk;

typedef enum PicoUiPostKind {
    PICO_UI_POST_TEXT = 0,
    PICO_UI_POST_STATUS,
} PicoUiPostKind;

typedef struct PicoUiPost {
    PicoAgentId agent_id;
    uint64_t generation;
    const char *status; /* borrowed; empty if none */
    const char *text;   /* borrowed; empty if none */
} PicoUiPost;

/* Provider-facing request. input_json is canonical items, oldest first:
 *   {"type":"user","parts":[{"type":"text","text":"..."},{"type":"image","path":"...","mime":"..."}]}
 *   {"type":"assistant","parts":[{"type":"text","text":"..."}],"thinking":"...","thinking_signature":"..."}
 *   {"type":"tool_call","call_id":"...","name":"...","arguments":"...","item_id":"..."}
 *   {"type":"tool_result","call_id":"...","name":"...","output":"...","is_error":false}
 *   {"type":"context","parts":[{"type":"text","text":"..."}]}
 * `thinking`, `thinking_signature`, and `item_id` are optional. User/assistant
 * parts may also be `refusal`, `image`, or `audio` (`path`, optional `mime` / `url`).
 * `context` is request-only extra_context: text parts only, not persisted in
 * history. Map it to a non-user role. Pico refuses the turn unless map_context. */
typedef struct PicoLlmTurn {
    const char *model;
    const char *base_url;
    const char *instructions;
    const char *cache_key;
    const char *effort;
    bool compact;
    bool include_tools;
    bool vision;
    const char *const *input_json;
    int input_count;
    const PicoTool *tools;
    int tool_count;
} PicoLlmTurn;

typedef enum PicoLlmPartKind {
    PICO_LLM_PART_TEXT = 0,
    PICO_LLM_PART_REFUSAL,
    PICO_LLM_PART_IMAGE,
    PICO_LLM_PART_AUDIO,
} PicoLlmPartKind;

typedef struct PicoLlmPart {
    PicoLlmPartKind kind;
    char *text;
    char *path;
    char *url;
    char *mime;
} PicoLlmPart;

typedef enum PicoLlmItemKind {
    PICO_LLM_ITEM_ASSISTANT = 0,
    PICO_LLM_ITEM_TOOL_CALL,
} PicoLlmItemKind;

typedef struct PicoLlmItem {
    PicoLlmItemKind kind;
    PicoLlmPart *parts;
    int part_count;
    char *thinking;
    char *thinking_signature;
    char *call_id;
    char *name;
    char *arguments;
    char *item_id;
} PicoLlmItem;

typedef struct PicoLlmResult {
    char *error;
    int input_tokens;
    int cached_tokens;
    PicoLlmItem *items;
    int item_count;
} PicoLlmResult;

typedef int (*PicoProviderStreamFn)(PicoAgentContext *ctx, const PicoLlmTurn *turn,
                                    PicoLlmCancelFn cancel, PicoLlmDeltaFn on_delta,
                                    void *user, PicoLlmResult *out, void *state);

typedef struct PicoProvider {
    const char *name;
    PicoProviderStreamFn stream;
    bool map_context; /* stream maps type:context items to a non-user role */
    void *state;
} PicoProvider;

typedef void (*PicoAuthLoginFn)(PicoHost *host, PicoAgentId agent_id, const char *args, void *state);
typedef void (*PicoAuthLogoutFn)(PicoHost *host, PicoAgentId agent_id, void *state);

typedef struct PicoAuth {
    const char *provider;
    const char *help;
    /* Space-separated sub-verbs `login` accepts, e.g. "key cancel". Offered as
     * completions and forwarded verbatim; the provider parses them. */
    const char *verbs;
    PicoAuthLoginFn login;
    PicoAuthLogoutFn logout;
    void *state;
} PicoAuth;

typedef struct PicoHostPreferences {
    double font_scale;
    int chat_width;
    char disabled_host_extensions[PICO_MAX_DISABLED_EXTENSIONS][PICO_DISABLED_EXT_NAME];
    int disabled_host_extension_count;
} PicoHostPreferences;

typedef struct PicoWorkspaceSettings {
    char default_model[128];
    int context_limit_fallback;
    double compact_ratio;
    bool compact_enabled;
    bool resume_last;
    char disabled_extensions[PICO_MAX_DISABLED_EXTENSIONS][PICO_DISABLED_EXT_NAME];
    int disabled_extension_count;
} PicoWorkspaceSettings;

typedef struct PicoToolBeforeEntry {
    PicoToolBeforeFn fn;
    void *state;
} PicoToolBeforeEntry;

typedef struct PicoToolAfterEntry {
    PicoToolAfterFn fn;
    void *state;
} PicoToolAfterEntry;

typedef struct PicoLlmHookEntry {
    PicoLlmHookFn fn;
    void *state;
} PicoLlmHookEntry;

typedef struct PicoContextHookEntry {
    PicoContextHookFn fn;
    void *state;
} PicoContextHookEntry;

typedef struct PicoToolRowEntry {
    PicoToolRowFn fn;
    void *state;
} PicoToolRowEntry;

void pico_host_set_hovered_clickable(PicoHost *host);
/* BEFORE_SUBMIT. Swallows the send. */
void pico_host_request_submit_cancel(PicoHost *host);
/* BEFORE_SUBMIT. Takes ownership of malloc'd replacement text; Pico frees it. */
void pico_host_set_agent_input(PicoHost *host, char *text);
/* BEFORE_SUBMIT. Takes ownership of malloc'd canonical parts JSON; Pico frees it. */
void pico_host_set_agent_parts(PicoHost *host, char *parts_json);
void pico_host_add_view(PicoHost *host, PicoUiSlot slot, int z, PicoHostViewFn render);
void pico_workspace_add_view(PicoWorkspace *workspace, PicoUiSlot slot, int z, PicoWorkspaceViewFn render);
void pico_workspace_add_empty_view(PicoWorkspace *workspace, PicoEmptyKind kind, int z,
                                   PicoWorkspaceViewFn render);
void pico_host_add_hook(PicoHost *host, PicoHook hook, PicoHostHookFn fn);
void pico_workspace_add_hook(PicoWorkspace *workspace, PicoHook hook, PicoWorkspaceHookFn fn);
void pico_add_tool_before_hook(PicoWorkspace *workspace, PicoToolBeforeFn fn);
void pico_add_tool_after_hook(PicoWorkspace *workspace, PicoToolAfterFn fn);
void pico_add_llm_hook(PicoWorkspace *workspace, PicoLlmHookFn fn);
void pico_add_context_hook(PicoWorkspace *workspace, PicoContextHookFn fn);
void pico_add_tool_row_hook(PicoWorkspace *workspace, PicoToolRowFn fn);
/* Main thread. Named modal stack used by PicoUi_ModalOpen. `name` is copied.
 * Push fails when name is empty, already claimed, or the stack is full. Pop
 * succeeds only when `name` is the current top. */
bool pico_ui_modal_push(PicoHost *host, const char *name);
bool pico_ui_modal_pop(PicoHost *host, const char *name);
const char *pico_ui_modal_top(const PicoHost *host);
int pico_ui_modal_count(const PicoHost *host);
bool pico_ui_modal_claimed(const PicoHost *host);
bool pico_ui_modal_has(const PicoHost *host, const char *name);
bool pico_ui_modal_is_top(const PicoHost *host, const char *name);
/* Core lifecycle helper. Drops every named claim before registrations are rebuilt. */
void pico_ui_modal_reset(PicoHost *host);
/* Worker thread, inside PicoToolFn or a before-tool hook. Copies bytes into a
 * named mailbox keyed by (agent_id, runtime_generation, name) and publishes
 * them on the next main-thread pump. Drops the post when ctx is inactive, the
 * name is empty/oversized, kind is invalid, or the workspace already holds
 * PICO_MAX_UI_POSTS keys. TEXT appends up to PICO_UI_POST_TEXT_MAX (prefix
 * kept). STATUS replaces up to PICO_UI_POST_STATUS_MAX. Two agents may share
 * a name without collision; each (agent, generation, name) occupies a slot. */
void pico_ui_post(PicoAgentContext *ctx, const char *name, PicoUiPostKind kind,
                  const char *text, size_t n);
/* Main thread. Latest published snapshot for the UI-selected agent's `name`.
 * Pointers are valid until the next pump, clear of this name, force-cancel,
 * generation retirement, or workspace close. Prefer pico_agent_ui_latest. */
bool pico_ui_latest(const PicoHost *host, const char *name, PicoUiPost *out);
bool pico_agent_ui_latest(const PicoHost *host, PicoAgentId agent_id, const char *name, PicoUiPost *out);
/* Main thread. Drops the snapshot and any unpublished posts for `name`. */
void pico_ui_clear(PicoHost *host, const char *name);
void pico_agent_ui_clear(PicoHost *host, PicoAgentId agent_id, const char *name);
/* Main thread. Runs tool-row hooks in registration order. Returns true when a
 * hook set handled. Strings are borrowed from `line` for this callback. */
bool pico_tool_row_activate(PicoWorkspace *workspace, PicoAgentId agent_id, const PicoTraceLine *line);
/* Append a line to status_warn (extension-error overlay). */
void pico_status_warn(PicoHost *host, const char *msg);
void pico_workspace_status_warn(PicoWorkspace *workspace, const char *msg);
/* False and a status_warn line on invalid args/schema, duplicate name, or limit. */
bool pico_add_tool(PicoWorkspace *workspace, const char *name, const char *description,
                   const char *params_json, PicoToolFn run, PicoToolApplyFn apply);
/* Bind a child pid to the in-flight tool so force-cancel can kill its process
 * group. Call from the tool (worker thread) after fork; 0 clears. */
void pico_tool_set_child(PicoAgentContext *ctx, pid_t pid);
/* Worker thread, inside PicoToolFn or a before-tool hook. Validates and copies
 * request_json. Invalid JSON/confirm schema returns an immediate OK error
 * answer. On OK, *answer_json is malloc'd and the caller frees it.
 * On CANCEL/FAIL, *answer_json is always set to NULL. */
int pico_tool_ask(PicoAgentContext *ctx, const char *request_json, char **answer_json);
/* Main thread. Returns the oldest live ask owned by the open session: the
 * selected agent or a transitive delegated child of it. False when no such ask
 * exists. request_json is valid until the next PicoAgent_Pump; do not retain
 * it across frames. */
bool pico_tool_pending_ask(const PicoHost *host, PicoToolAsk *out);
/* Main thread. False if id is stale, cancelled, or no longer pending. */
bool pico_tool_answer(PicoHost *host, uint64_t id, const char *answer_json);
/* Main-thread-only borrowed transcript access. The pointer is invalidated by
 * the next pump, transcript mutation, agent close, or workspace close. */
int pico_agent_message_count(const PicoHost *host, PicoAgentId id);
const PicoMessage *pico_agent_message(const PicoHost *host, PicoAgentId id, int index);
/* True while the named modal stack is non-empty or a tool ask is showing. */
bool PicoUi_ModalOpen(const PicoHost *host);
void pico_host_add_command(PicoHost *host, const char *name, const char *help, PicoHostCmdFn run);
void pico_workspace_add_command(PicoWorkspace *workspace, const char *name, const char *help,
                                PicoWorkspaceCmdFn run);
void pico_host_add_completer(PicoHost *host, char trigger, bool bol_only, PicoHostCompleteQueryFn query,
                             PicoHostCompleteAcceptFn accept);
void pico_workspace_add_completer(PicoWorkspace *workspace, char trigger, bool bol_only,
                                  PicoWorkspaceCompleteQueryFn query, PicoWorkspaceCompleteAcceptFn accept);
void pico_add_provider(PicoWorkspace *workspace, const PicoProvider *p);
const PicoProvider *pico_workspace_find_provider(const PicoWorkspace *workspace, const char *name);
void pico_add_auth(PicoHost *host, const PicoAuth *a);
const PicoAuth *pico_find_auth(const PicoHost *host, const char *provider);
void pico_llm_result_free(PicoLlmResult *r);
PicoLlmItem *pico_llm_result_add_item(PicoLlmResult *r, PicoLlmItemKind kind);
bool pico_llm_item_add_part(PicoLlmItem *item, PicoLlmPartKind kind, const char *text, const char *path,
                            const char *url, const char *mime);
bool pico_llm_result_add_text(PicoLlmResult *r, const char *text);
bool pico_llm_result_add_refusal(PicoLlmResult *r, const char *text);
bool pico_llm_result_add_tool_call(PicoLlmResult *r, const char *call_id, const char *name,
                                   const char *arguments, const char *item_id);
bool pico_llm_result_has_output(const PicoLlmResult *r);
void pico_clear_registrations(PicoHost *host);
void pico_run_hooks(PicoHost *host, PicoHook hook, PicoAgentId agent_id);
/* Main thread. Queues a record to the explicit agent's session on Pico's persist thread;
 * PICO_SESSION_WRITE_OK reports acceptance; write failures surface asynchronously. */
PicoSessionWriteResult pico_session_log_custom(PicoHost *host, PicoAgentId agent_id, const char *ext,
                                               const char *data_json);
/* ON_COMPACT only. Takes ownership of malloc'd summary; NULL keeps default compaction. */
void pico_agent_set_compact_summary(PicoHost *host, PicoAgentId agent_id, char *summary);

void PicoHost_Start(PicoHost *host, Font *fonts, const char *workspace, bool safe_mode,
                    PicoSessionStart session_start, const char *session_file);
PicoHostShutdownResult PicoHost_Shutdown(PicoHost *host);
void PicoHost_ClearMessages(PicoHost *host, PicoAgentId agent_id);
void PicoHost_AddMessage(PicoHost *host, PicoAgentId agent_id, PicoRole role, const char *markdown);
void PicoHost_AddToolCall(PicoHost *host, PicoAgentId agent_id, const char *name, const char *args);
void PicoHost_SetLastToolOutput(PicoHost *host, PicoAgentId agent_id, const char *output, bool is_error);
void PicoHost_AppendAssistant(PicoHost *host, PicoAgentId agent_id, const char *text);
void PicoHost_Submit(PicoHost *host);
void PicoHost_Cancel(PicoHost *host);
void PicoHost_RequestSubmitCancel(PicoHost *host);
void PicoHost_Frame(PicoHost *host);
void PicoHost_RequestReload(PicoHost *host);
/* Open or select `path`. Relative paths resolve against `from`, never UI selection.
   `from` may be NULL; relative paths then resolve against ".". */
bool PicoHost_ChangeWorkspace(PicoHost *host, const PicoWorkspace *from, const char *path);

typedef enum PicoExtensionScope {
    PICO_EXTENSION_HOST = 0,
    PICO_EXTENSION_WORKSPACE,
} PicoExtensionScope;

typedef struct PicoExtInfo {
    const char *name;        /* NULL if unnamed / failed stub */
    const char *description; /* NULL if omitted */
    const char *source;      /* user .c path; NULL for builtins */
    PicoExtensionScope scope;
    PicoWorkspaceId workspace_id; /* 0 for host scope */
    uint64_t desired_generation;
    uint64_t active_generation;
    const char *last_error;  /* NULL if none */
    bool builtin;
    bool loaded;  /* false for compile/dlopen stubs */
    bool enabled; /* independent of loaded; false when toggled off */
} PicoExtInfo;

void PicoPlugins_Load(PicoHost *host);
void PicoPlugins_InitWorkspace(PicoHost *host, PicoWorkspace *workspace);
/* Compile and swap user-global (config) modules only. Workspace-local sources
   are refreshed by that workspace's reload. */
bool PicoPlugins_ReloadHost(PicoHost *host);
void PicoPlugins_Reload(PicoHost *host);
void PicoPlugins_Poll(PicoHost *host);
void PicoPlugins_OnFrame(PicoHost *host, float dt);
void PicoPlugins_UnloadUser(PicoHost *host);
void PicoPlugins_Shutdown(PicoHost *host);
int PicoPlugins_Count(const PicoHost *host);
bool PicoPlugins_Get(const PicoHost *host, int index, PicoExtInfo *out);
bool PicoPlugins_SetEnabled(PicoHost *host, int index, bool enabled);
void *PicoPlugins_HostState(const PicoHost *host, const char *name);
void *PicoPlugins_WorkspaceState(const PicoWorkspace *workspace, const char *name);

bool Pico_ShortcutPressed(char letter);
bool Pico_ShortcutRepeat(char letter);

void PicoChat_Render(PicoHost *app, void *state);
void PicoChat_HandleToolRelease(PicoHost *app);
void PicoChat_HandlePointer(PicoHost *app, const PicoHookEvent *event, void *state);
void PicoChat_DrawOverlay(PicoHost *app, const PicoHookEvent *event, void *state);
bool PicoChat_InspectIsOpen(void);
bool PicoChatSel_HasSelection(const PicoHost *app);
void PicoChatSel_Clear(PicoHost *app);
void PicoChatSel_Copy(PicoHost *app);
bool PicoChatSel_PointerOverText(void);
void PicoComposer_HandleInput(PicoHost *app);
void PicoComposer_HandlePointer(PicoHost *app);
void PicoComposer_Render(PicoHost *app, void *state);
void PicoComposer_DrawOverlay(PicoHost *app, const PicoHookEvent *event, void *state);
bool PicoComposer_HasSelection(const PicoHost *app);
void PicoComposer_Copy(PicoHost *app);
void PicoComposer_SetText(PicoHost *app, const char *text);
void PicoComposer_ReplaceRange(PicoHost *app, int from, int to, const char *text);
bool PicoComplete_HandleKeys(PicoHost *app);
bool PicoComplete_HandlePointer(PicoHost *app);
void PicoComplete_Refresh(PicoHost *app);
void PicoComplete_Render(PicoHost *app);
void PicoComplete_Close(void);
bool PicoComplete_IsOpen(void);
void PicoFooter_Render(PicoHost *app, void *state);
bool PicoFooter_MenuOpen(void);
bool PicoDiff_IsOpen(const PicoHost *app);
void PicoDiff_RenderChip(PicoHost *app);
void PicoOverlay_Render(PicoHost *app, void *state);
void PicoOverlay_OnFrame(PicoHost *app, void *state, float dt);
void PicoExts_Open(PicoHost *host);
void PicoExts_Close(PicoHost *host);
void PicoExts_Toggle(PicoHost *host);
bool PicoExts_IsOpen(const PicoHost *host);
void PicoSettingsUi_Open(PicoHost *host);
void PicoSettingsUi_Close(PicoHost *host);
bool PicoSettingsUi_IsOpen(const PicoHost *host);
void PicoPrompt_Close(PicoHost *host);
bool PicoPrompt_IsOpen(const PicoHost *host);

#endif
