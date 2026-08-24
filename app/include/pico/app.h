#ifndef PICO_APP_H
#define PICO_APP_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

#include "raylib.h"

#include "markdown.h"
#include "pico/agent.h"
#include "pico/theme.h"
#include "text_range.h"

#define PICO_MAX_SLOT_VIEWS 16
#define PICO_MAX_EMPTY_VIEWS 16
#define PICO_MAX_HOOKS 64
#define PICO_MAX_TOOL_HOOKS 64
#define PICO_MAX_LLM_HOOKS 64
#define PICO_MAX_CONTEXT_HOOKS 64
#define PICO_TOOL_DETAILS_MAX (64 * 1024)
#define PICO_TOOL_ASK_MAX_REQUEST (64 * 1024)
#define PICO_TOOL_ASK_MAX_ANSWER (64 * 1024)
#define PICO_MAX_COMMANDS 64
#define PICO_MAX_COMPLETERS 16
#define PICO_MAX_COMPLETE_ITEMS 24
#define PICO_MAX_PROVIDERS 16
#define PICO_MAX_AUTH 16
#define PICO_MAX_EFFORTS 16

typedef enum PicoRole {
    PICO_ROLE_USER = 0,
    PICO_ROLE_ASSISTANT,
} PicoRole;

typedef enum PicoAppShutdownResult {
    PICO_APP_SHUTDOWN_CLEAN = 0,
    PICO_APP_SHUTDOWN_RETAINED,
} PicoAppShutdownResult;

typedef enum PicoUiSlot {
    PICO_SLOT_SIDEBAR = 0,
    PICO_SLOT_MAIN,
    PICO_SLOT_COMPOSER,
    PICO_SLOT_FOOTER,
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
    PICO_HOOK_BEFORE_SUBMIT, /* set submit_cancel and/or agent_input */
    PICO_HOOK_ON_SUBMIT,
    PICO_HOOK_ON_MESSAGE,
    PICO_HOOK_ON_COMPACT, /* pico_agent_set_compact_summary can replace the default briefing */
    PICO_HOOK_AFTER_COMPACT,
    PICO_HOOK_ON_TURN_END, /* idle after a finished turn (not cancel/error) */
    PICO_HOOK_ON_CANCEL,
    PICO_HOOK_ON_ERROR,
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
    char *tool_args;
    char *tool_output;
    bool tool_error;
    bool expanded;
    int think_steps;
    PicoAgentId child_id;
    char child_session_id[40];
    MdDocument doc;
} PicoTraceLine;

typedef struct PicoMessage {
    PicoRole role;
    char *source;
    PicoTraceLine *trace;
    int trace_count;
    MdDocument doc;
} PicoMessage;

typedef struct PicoChatSelect {
    int msg;
    int anchor;
    int cursor;
    bool mouse_selecting;
    bool dragging;
    bool pressed_tool;
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

struct PicoApp;

typedef void (*PicoViewFn)(struct PicoApp *app);

typedef struct PicoHookEvent {
    PicoHook hook;
    PicoAgentId agent_id; /* zero only for app-global hooks without an agent target */
} PicoHookEvent;

typedef void (*PicoHookFn)(struct PicoApp *app, const PicoHookEvent *event);

typedef struct PicoToolResult {
    char *output;       /* malloc; Pico frees */
    char *details_json; /* optional malloc'd JSON object; Pico validates and frees */
    bool is_error;
} PicoToolResult;

typedef void (*PicoToolFn)(PicoAgentContext *ctx, const char *args_json, PicoToolResult *out);
typedef bool (*PicoToolApplyFn)(struct PicoApp *app, PicoAgentId agent_id,
                                const char *details_json, bool replay);
typedef void (*PicoCmdFn)(struct PicoApp *app, const char *args);

typedef struct PicoCompleteItem {
    char label[256];
    char detail[128];
    char insert[512];
} PicoCompleteItem;

typedef int (*PicoCompleteQueryFn)(struct PicoApp *app, const char *prefix, PicoCompleteItem *out, int max);
typedef bool (*PicoCompleteAcceptFn)(struct PicoApp *app, const PicoCompleteItem *item);

typedef struct PicoSlotView {
    PicoViewFn render;
    int z;
} PicoSlotView;

typedef struct PicoEmptyView {
    PicoViewFn render;
    PicoEmptyKind kind;
    int z;
} PicoEmptyView;

typedef struct PicoHookEntry {
    PicoHook hook;
    PicoHookFn fn;
} PicoHookEntry;

typedef struct PicoTool {
    const char *name;
    const char *description;
    const char *params_json;
    PicoToolFn run;
    PicoToolApplyFn apply; /* optional; main thread after success and during replay */
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

typedef void (*PicoToolBeforeFn)(PicoAgentContext *ctx, PicoToolEvent *event);
typedef void (*PicoToolAfterFn)(struct PicoApp *app, PicoAgentId agent_id,
                                PicoToolEvent *event);

typedef struct PicoLlmEvent {
    bool compact;
    bool include_tools; /* read-only; false => catalog omitted, exclude ignored */
    const PicoTool *tools;
    int tool_count;
    bool *exclude; /* include_tools ? tool_count flags : NULL */
    const char *instructions;
    char *extra_instructions; /* malloc; core appends under "## Additional instructions" after this hook and frees */
} PicoLlmEvent;

typedef void (*PicoLlmHookFn)(struct PicoApp *app, PicoAgentId agent_id,
                              PicoLlmEvent *event);

typedef struct PicoContextEvent {
    bool compact;
    const char *const *history_json; /* immutable base history for this request */
    int history_count;
    const PicoTool *tools; /* final effective catalog for this request */
    int tool_count;
    char *extra_context; /* optional malloc'd text; Pico appends request-only user context */
} PicoContextEvent;

typedef void (*PicoContextHookFn)(struct PicoApp *app, PicoAgentId agent_id,
                                  PicoContextEvent *event);

typedef struct PicoCommand {
    const char *name;
    const char *help;
    PicoCmdFn run;
} PicoCommand;

typedef struct PicoCompleter {
    char trigger;
    bool bol_only;
    PicoCompleteQueryFn query;
    PicoCompleteAcceptFn accept;
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
                                    void *user, PicoLlmResult *out);

typedef struct PicoProvider {
    const char *name;
    PicoProviderStreamFn stream;
} PicoProvider;

typedef void (*PicoAuthLogoutFn)(struct PicoApp *app);

typedef struct PicoAuth {
    const char *provider;
    const char *help;
    /* Space-separated sub-verbs `login` accepts, e.g. "key cancel". Offered as
     * completions and forwarded verbatim; the provider parses them. */
    const char *verbs;
    PicoCmdFn login;
    PicoAuthLogoutFn logout;
} PicoAuth;

typedef struct PicoSettings {
    char model[128];
    int context_limit;
    double compact_ratio;
    bool compact_enabled;
    bool context_limit_set;
    bool resume_last;
    double font_scale;
} PicoSettings;

typedef struct PicoApp {
    PicoAgentManager *agents;
    PicoComposer composer;
    PicoSettings settings; /* defaults for newly created agents */
    Font *fonts;
    PicoSlotView views[PICO_SLOT_COUNT][PICO_MAX_SLOT_VIEWS];
    int view_count[PICO_SLOT_COUNT];
    PicoEmptyView empty_views[PICO_MAX_EMPTY_VIEWS];
    int empty_view_count;
    PicoHookEntry hooks[PICO_MAX_HOOKS];
    int hook_count;
    PicoToolBeforeFn tool_before_hooks[PICO_MAX_TOOL_HOOKS];
    int tool_before_hook_count;
    PicoToolAfterFn tool_after_hooks[PICO_MAX_TOOL_HOOKS];
    int tool_after_hook_count;
    PicoLlmHookFn llm_hooks[PICO_MAX_LLM_HOOKS];
    int llm_hook_count;
    PicoContextHookFn context_hooks[PICO_MAX_CONTEXT_HOOKS];
    int context_hook_count;
    PicoTool tools[PICO_MAX_TOOLS];
    int tool_count;
    PicoCommand commands[PICO_MAX_COMMANDS];
    int command_count;
    PicoCompleter completers[PICO_MAX_COMPLETERS];
    int completer_count;
    PicoProvider providers[PICO_MAX_PROVIDERS];
    int provider_count;
    PicoAuth auths[PICO_MAX_AUTH];
    int auth_count;
    struct PicoAuthStore *auth_store;
    bool submit_cancel;
    char *agent_input;
    char *agent_parts; /* optional malloc'd JSON array of canonical user parts */
    PicoScrollbar chat_scrollbar;
    PicoScrollbar composer_scrollbar;
    PicoChatSelect chat_sel;
    bool chat_follow_bottom; /* sticky: pin to bottom until the user scrolls away */
    bool chat_overflow;
    bool composer_overflow;
    bool reinitialize_clay;
    bool debug_enabled;
    bool safe_mode;
    bool reload_queued;
    bool workspace_change_queued;
    bool terminal_shutdown;
    const char *hovered_link;
    bool hovered_tool;
    bool hovered_clickable;
    char workspace[4096];
    char pending_workspace[4096];
    char *status_warn; /* overlay; compile/load and failed pico_add_tool */
    PicoModel *models; /* immutable capabilities and configured defaults */
    int model_count;
} PicoApp;

void pico_add_view(PicoApp *app, PicoUiSlot slot, int z, PicoViewFn render);
void pico_add_empty_view(PicoApp *app, PicoEmptyKind kind, int z, PicoViewFn render);
void pico_add_hook(PicoApp *app, PicoHook hook, PicoHookFn fn);
void pico_add_tool_before_hook(PicoApp *app, PicoToolBeforeFn fn);
void pico_add_tool_after_hook(PicoApp *app, PicoToolAfterFn fn);
void pico_add_llm_hook(PicoApp *app, PicoLlmHookFn fn);
void pico_add_context_hook(PicoApp *app, PicoContextHookFn fn);
/* Append a line to status_warn (extension-error overlay). */
void pico_status_warn(PicoApp *app, const char *msg);
/* False and a status_warn line on invalid args/schema, duplicate name, or limit. */
bool pico_add_tool(PicoApp *app, const char *name, const char *description, const char *params_json,
                   PicoToolFn run, PicoToolApplyFn apply);
/* Bind a child pid to the in-flight tool so force-cancel can kill its process
 * group. Call from the tool (worker thread) after fork; 0 clears. */
void pico_tool_set_child(PicoAgentContext *ctx, pid_t pid);
/* Worker thread, inside PicoToolFn or a before-tool hook. Validates and copies
 * request_json. Invalid JSON/confirm schema returns an immediate OK error
 * answer. On OK, *answer_json is malloc'd and the caller frees it.
 * On CANCEL/FAIL, *answer_json is always set to NULL. */
int pico_tool_ask(PicoAgentContext *ctx, const char *request_json, char **answer_json);
/* Main thread. False when no live ask exists. request_json is valid until
 * the next PicoAgent_Pump; do not retain it across frames. */
bool pico_tool_pending_ask(const PicoApp *app, PicoToolAsk *out);
/* Main thread. False if id is stale, cancelled, or no longer pending. */
bool pico_tool_answer(PicoApp *app, uint64_t id, const char *answer_json);
/* Main-thread-only borrowed transcript access. The pointer is invalidated by
 * the next manager pump, transcript mutation, close, or workspace change. */
int pico_agent_message_count(const PicoApp *app, PicoAgentId id);
const PicoMessage *pico_agent_message(const PicoApp *app, PicoAgentId id, int index);
bool PicoUi_ModalOpen(const PicoApp *app);
void pico_add_command(PicoApp *app, const char *name, const char *help, PicoCmdFn run);
void pico_add_completer(PicoApp *app, char trigger, bool bol_only, PicoCompleteQueryFn query,
                        PicoCompleteAcceptFn accept);
void pico_add_provider(PicoApp *app, const PicoProvider *p);
const PicoProvider *pico_find_provider(const PicoApp *app, const char *name);
void pico_add_auth(PicoApp *app, const PicoAuth *a);
const PicoAuth *pico_find_auth(const PicoApp *app, const char *provider);
void pico_llm_result_free(PicoLlmResult *r);
PicoLlmItem *pico_llm_result_add_item(PicoLlmResult *r, PicoLlmItemKind kind);
bool pico_llm_item_add_part(PicoLlmItem *item, PicoLlmPartKind kind, const char *text, const char *path,
                            const char *url, const char *mime);
bool pico_llm_result_add_text(PicoLlmResult *r, const char *text);
bool pico_llm_result_add_refusal(PicoLlmResult *r, const char *text);
bool pico_llm_result_add_tool_call(PicoLlmResult *r, const char *call_id, const char *name,
                                   const char *arguments, const char *item_id);
bool pico_llm_result_has_output(const PicoLlmResult *r);
void pico_clear_registrations(PicoApp *app);
void pico_run_hooks(PicoApp *app, PicoHook hook, PicoAgentId agent_id);
/* Main thread. Logs to the explicit agent's session and reports durability. */
PicoSessionWriteResult pico_session_log_custom(PicoApp *app, PicoAgentId agent_id,
                                                const char *ext, const char *data_json);
/* ON_COMPACT only. Takes ownership of malloc'd summary; NULL keeps default compaction. */
void pico_agent_set_compact_summary(PicoApp *app, PicoAgentId agent_id, char *summary);

void PicoApp_Init(PicoApp *app, Font *fonts, const char *workspace, bool safe_mode,
                 PicoSessionStart session_start, const char *session_file);
/* RETAINED means a detached callback still owns execution services. Pico is
 * permanently retired in this process and the caller must proceed to exit. */
PicoAppShutdownResult PicoApp_Free(PicoApp *app);
/* Main-thread lifecycle pump for embedders that do not call PicoApp_Frame. */
void PicoApp_PumpLifecycle(PicoApp *app);
/* True after a retained shutdown; plugin and app initialization are rejected. */
bool PicoApp_ProcessRetired(void);
void PicoApp_ClearMessages(PicoApp *app);
void PicoApp_AddMessage(PicoApp *app, PicoRole role, const char *markdown);
void PicoApp_AddToolCall(PicoApp *app, const char *name, const char *args);
void PicoApp_SetLastToolOutput(PicoApp *app, const char *output, bool is_error);
void PicoApp_AppendAssistant(PicoApp *app, const char *text);
void PicoApp_Submit(PicoApp *app);
void PicoApp_Cancel(PicoApp *app);
void PicoApp_Frame(PicoApp *app);
void PicoApp_RequestReload(PicoApp *app);
bool PicoApp_ChangeWorkspace(PicoApp *app, const char *path);

typedef struct PicoExtInfo {
    const char *name;        /* NULL if unnamed / failed stub */
    const char *description; /* NULL if omitted */
    const char *source;      /* user .c path; NULL for builtins */
    bool builtin;
    bool loaded;  /* false for compile/dlopen stubs */
    bool enabled; /* currently same as loaded; later independently togglable */
} PicoExtInfo;

void PicoPlugins_Load(PicoApp *app);
void PicoPlugins_Reload(PicoApp *app);
void PicoPlugins_Poll(PicoApp *app);
void PicoPlugins_OnFrame(PicoApp *app, float dt);
void PicoPlugins_UnloadUser(PicoApp *app);
void PicoPlugins_Shutdown(PicoApp *app);
int PicoPlugins_Count(void);
bool PicoPlugins_Get(int index, PicoExtInfo *out);

bool Pico_ShortcutPressed(char letter);
bool Pico_ShortcutRepeat(char letter);

void PicoChat_Render(PicoApp *app);
void PicoChat_HandlePointer(PicoApp *app, const PicoHookEvent *event);
void PicoChat_DrawOverlay(PicoApp *app, const PicoHookEvent *event);
bool PicoChat_InspectIsOpen(void);
bool PicoChatSel_HasSelection(const PicoApp *app);
void PicoChatSel_Clear(PicoApp *app);
void PicoChatSel_Copy(PicoApp *app);
bool PicoChatSel_PointerOverText(void);
void PicoComposer_HandleInput(PicoApp *app);
void PicoComposer_HandlePointer(PicoApp *app);
void PicoComposer_Render(PicoApp *app);
void PicoComposer_DrawOverlay(PicoApp *app, const PicoHookEvent *event);
bool PicoComposer_HasSelection(const PicoApp *app);
void PicoComposer_Copy(PicoApp *app);
void PicoComposer_SetText(PicoApp *app, const char *text);
void PicoComposer_ReplaceRange(PicoApp *app, int from, int to, const char *text);
bool PicoComplete_HandleKeys(PicoApp *app);
bool PicoComplete_HandlePointer(PicoApp *app);
void PicoComplete_Refresh(PicoApp *app);
void PicoComplete_Render(PicoApp *app);
void PicoComplete_Close(void);
bool PicoComplete_IsOpen(void);
void PicoFooter_Render(PicoApp *app);
bool PicoFooter_MenuOpen(void);
bool PicoDiff_IsOpen(void);
void PicoDiff_RenderChip(PicoApp *app);
void PicoOverlay_Render(PicoApp *app);
void PicoOverlay_OnFrame(PicoApp *app, float dt);
void PicoExts_Close(void);
bool PicoExts_IsOpen(void);
void PicoPrompt_Close(void);
bool PicoPrompt_IsOpen(void);

#endif
