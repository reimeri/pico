#ifndef PICO_APP_H
#define PICO_APP_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

#include "raylib.h"

#include "markdown.h"
#include "pico/theme.h"

#define PICO_MAX_SLOT_VIEWS 16
#define PICO_MAX_HOOKS 64
#define PICO_MAX_TOOLS 64
#define PICO_TOOL_ASK_MAX_REQUEST (64 * 1024)
#define PICO_TOOL_ASK_MAX_ANSWER (64 * 1024)
#define PICO_MAX_COMMANDS 64
#define PICO_MAX_COMPLETERS 16
#define PICO_MAX_COMPLETE_ITEMS 24
#define PICO_MAX_PROVIDERS 16
#define PICO_MAX_AUTH 16
#define PICO_MAX_EFFORTS 16
#define PICO_EFFORT_LEN 16

typedef enum PicoRole {
    PICO_ROLE_USER = 0,
    PICO_ROLE_ASSISTANT,
} PicoRole;

typedef enum PicoAgentState {
    PICO_AGENT_IDLE = 0,
    PICO_AGENT_LLM_WAIT,
    PICO_AGENT_TOOL_WAIT,
    PICO_AGENT_COMPACT_WAIT,
    PICO_AGENT_ERROR,
} PicoAgentState;

typedef enum PicoUiSlot {
    PICO_SLOT_SIDEBAR = 0,
    PICO_SLOT_MAIN,
    PICO_SLOT_COMPOSER,
    PICO_SLOT_FOOTER,
    PICO_SLOT_OVERLAY,
    PICO_SLOT_COUNT,
} PicoUiSlot;

typedef enum PicoHook {
    PICO_HOOK_AFTER_LAYOUT = 0,
    PICO_HOOK_AFTER_RENDER,
    PICO_HOOK_BEFORE_SUBMIT, /* set submit_cancel and/or agent_input */
    PICO_HOOK_ON_SUBMIT,
    PICO_HOOK_ON_MESSAGE,
    PICO_HOOK_ON_COMPACT, /* set app->compact_summary to replace the default briefing */
    PICO_HOOK_COUNT,
} PicoHook;

typedef enum PicoSessionStart {
    PICO_SESSION_NEW = 0,
    PICO_SESSION_RESUME,
    PICO_SESSION_NONE,
} PicoSessionStart;

typedef struct PicoModel {
    char id[128];
    char name[128];
    char provider[64];
    char base_url[512];
    int context_limit;
    bool vision;
    char effort[PICO_MAX_EFFORTS][PICO_EFFORT_LEN];
    int effort_count;
    char selected_effort[PICO_EFFORT_LEN];
} PicoModel;

typedef struct PicoTraceLine {
    char *text;
    bool is_tool;
    char *tool_name;
    char *tool_args;
    char *tool_output;
    bool expanded;
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
} PicoChatSelect;

typedef struct PicoComposer {
    char *text;
    int length;
    int capacity;
    int cursor;
    int sel_anchor;
    bool mouse_selecting;
} PicoComposer;

typedef struct PicoScrollbar {
    Clay_Vector2 click_origin;
    Clay_Vector2 position_origin;
    bool mouse_down;
} PicoScrollbar;

struct PicoApp;

typedef void (*PicoViewFn)(struct PicoApp *app);
typedef void (*PicoHookFn)(struct PicoApp *app);
typedef void (*PicoToolFn)(struct PicoApp *app, const char *args_json, char **out);
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

typedef struct PicoHookEntry {
    PicoHook hook;
    PicoHookFn fn;
} PicoHookEntry;

typedef struct PicoTool {
    const char *name;
    const char *description;
    const char *params_json;
    PicoToolFn run;
} PicoTool;

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
    const char *const *input_json;
    int input_count;
    const PicoTool *tools;
    int tool_count;
} PicoLlmTurn;

typedef struct PicoLlmToolCall {
    char *call_id;
    char *name;
    char *arguments;
} PicoLlmToolCall;

typedef struct PicoLlmResult {
    char *error;
    int input_tokens;
    int cached_tokens;
    char *assistant_text;
    char *think_text;
    PicoLlmToolCall *calls;
    int call_count;
    char **raw_items;
    int raw_count;
} PicoLlmResult;

typedef int (*PicoProviderStreamFn)(struct PicoApp *app, const PicoLlmTurn *turn, PicoLlmCancelFn cancel,
                                    PicoLlmDeltaFn on_delta, void *user, PicoLlmResult *out);

typedef struct PicoProvider {
    const char *name;
    PicoProviderStreamFn stream;
} PicoProvider;

typedef struct PicoAuth {
    const char *provider;
    const char *help;
    /* Space-separated sub-verbs `login` accepts, e.g. "key cancel". Offered as
     * completions and forwarded verbatim; the provider parses them. */
    const char *verbs;
    PicoCmdFn login;
    PicoHookFn logout;
} PicoAuth;

typedef struct PicoSettings {
    char model[128];
    int context_limit;
    double compact_ratio;
    bool compact_enabled;
    bool context_limit_set;
    bool resume_last;
} PicoSettings;

struct PicoAgentRt;
typedef struct PicoAgentRt PicoAgentRt;

typedef struct PicoApp {
    PicoMessage *messages;
    int message_count;
    int message_capacity;
    PicoComposer composer;
    PicoAgentState agent_state;
    PicoAgentRt *agent;
    PicoSettings settings;
    const char *model_name;
    int tokens_used;
    int tokens_cached;
    int tokens_limit;
    char *agent_error;
    Font *fonts;
    PicoSlotView views[PICO_SLOT_COUNT][PICO_MAX_SLOT_VIEWS];
    int view_count[PICO_SLOT_COUNT];
    PicoHookEntry hooks[PICO_MAX_HOOKS];
    int hook_count;
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
    PicoScrollbar chat_scrollbar;
    PicoScrollbar composer_scrollbar;
    PicoChatSelect chat_sel;
    bool chat_follow_bottom;
    bool chat_overflow;
    bool composer_overflow;
    bool reinitialize_clay;
    bool debug_enabled;
    bool safe_mode;
    bool reload_queued;
    const char *hovered_link;
    bool hovered_tool;
    bool hovered_clickable;
    char workspace[4096];
    char session_id[40];
    char session_path[4096];
    bool session_ephemeral;
    char *status_warn;
    char agent_activity[256];
    char *compact_summary;
    PicoModel *models;
    int model_count;
} PicoApp;

void pico_add_view(PicoApp *app, PicoUiSlot slot, int z, PicoViewFn render);
void pico_add_hook(PicoApp *app, PicoHook hook, PicoHookFn fn);
void pico_add_tool(PicoApp *app, const char *name, const char *description, const char *params_json,
                   PicoToolFn run);
/* Bind a child pid to the in-flight tool so force-cancel can kill its process
 * group. Call from the tool (worker thread) after fork; 0 clears. */
void pico_tool_set_child(PicoApp *app, pid_t pid);
/* Worker thread, inside PicoToolFn only. Validates and copies request_json.
 * Invalid JSON/confirm schema returns an immediate OK error answer.
 * On OK, *answer_json is malloc'd and the tool frees it.
 * On CANCEL/FAIL, *answer_json is always set to NULL. */
int pico_tool_ask(PicoApp *app, const char *request_json, char **answer_json);
/* Main thread. False when no live ask exists. request_json is valid until
 * the next PicoAgent_Pump; do not retain it across frames. */
bool pico_tool_pending_ask(const PicoApp *app, PicoToolAsk *out);
/* Main thread. False if id is stale, cancelled, or no longer pending. */
bool pico_tool_answer(PicoApp *app, uint64_t id, const char *answer_json);
bool PicoUi_ModalOpen(const PicoApp *app);
void pico_add_command(PicoApp *app, const char *name, const char *help, PicoCmdFn run);
void pico_add_completer(PicoApp *app, char trigger, bool bol_only, PicoCompleteQueryFn query,
                        PicoCompleteAcceptFn accept);
void pico_add_provider(PicoApp *app, const PicoProvider *p);
const PicoProvider *pico_find_provider(const PicoApp *app, const char *name);
void pico_add_auth(PicoApp *app, const PicoAuth *a);
const PicoAuth *pico_find_auth(const PicoApp *app, const char *provider);
void pico_llm_result_free(PicoLlmResult *r);
void pico_clear_registrations(PicoApp *app);
void pico_run_hooks(PicoApp *app, PicoHook hook);
void pico_session_log_custom(PicoApp *app, const char *ext, const char *data_json);

void PicoApp_Init(PicoApp *app, Font *fonts, const char *workspace, bool safe_mode,
                 PicoSessionStart session_start, const char *session_file);
void PicoApp_Free(PicoApp *app);
void PicoApp_ClearMessages(PicoApp *app);
void PicoApp_AddMessage(PicoApp *app, PicoRole role, const char *markdown);
void PicoApp_AddToolCall(PicoApp *app, const char *name, const char *args);
void PicoApp_SetLastToolOutput(PicoApp *app, const char *output);
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
void PicoChat_HandlePointer(PicoApp *app);
void PicoChat_DrawOverlay(PicoApp *app);
bool PicoChatSel_HasSelection(const PicoApp *app);
void PicoChatSel_Clear(PicoApp *app);
void PicoChatSel_Copy(PicoApp *app);
bool PicoChatSel_PointerOverText(void);
void PicoComposer_HandleInput(PicoApp *app);
void PicoComposer_HandlePointer(PicoApp *app);
void PicoComposer_Render(PicoApp *app);
void PicoComposer_DrawOverlay(PicoApp *app);
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
void PicoOverlay_Render(PicoApp *app);
void PicoOverlay_OnFrame(PicoApp *app, float dt);
void PicoExts_Close(void);
bool PicoExts_IsOpen(void);

#endif
