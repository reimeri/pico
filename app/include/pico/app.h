#ifndef PICO_APP_H
#define PICO_APP_H

#include <stdbool.h>
#include <stddef.h>

#include "raylib.h"

#include "markdown.h"
#include "pico/theme.h"

#define PICO_MAX_SLOT_VIEWS 8
#define PICO_MAX_HOOKS 32
#define PICO_MAX_TOOLS 32

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
    PICO_HOOK_ON_SUBMIT,
    PICO_HOOK_ON_MESSAGE,
    PICO_HOOK_COUNT,
} PicoHook;

typedef struct PicoMessage {
    PicoRole role;
    char *source;
    MdDocument doc;
} PicoMessage;

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

typedef struct PicoSettings {
    char api_key[512];
    char base_url[512];
    char model[128];
    int context_limit;
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
    int tokens_limit;
    char *agent_error;
    Font *fonts;
    PicoSlotView views[PICO_SLOT_COUNT][PICO_MAX_SLOT_VIEWS];
    int view_count[PICO_SLOT_COUNT];
    PicoHookEntry hooks[PICO_MAX_HOOKS];
    int hook_count;
    PicoTool tools[PICO_MAX_TOOLS];
    int tool_count;
    PicoScrollbar chat_scrollbar;
    PicoScrollbar composer_scrollbar;
    bool chat_follow_bottom;
    bool chat_overflow;
    bool composer_overflow;
    int selected_message;
    bool reinitialize_clay;
    bool debug_enabled;
    bool safe_mode;
    bool reload_queued;
    const char *hovered_link;
    char footer_text[256];
    char workspace[4096];
    char *status_warn;
} PicoApp;

void pico_add_view(PicoApp *app, PicoUiSlot slot, int z, PicoViewFn render);
void pico_add_hook(PicoApp *app, PicoHook hook, PicoHookFn fn);
void pico_add_tool(PicoApp *app, const char *name, const char *description, const char *params_json,
                   PicoToolFn run);
void pico_clear_registrations(PicoApp *app);
void pico_run_hooks(PicoApp *app, PicoHook hook);

void PicoApp_Init(PicoApp *app, Font *fonts, const char *workspace, bool safe_mode);
void PicoApp_Free(PicoApp *app);
void PicoApp_AddMessage(PicoApp *app, PicoRole role, const char *markdown);
void PicoApp_Submit(PicoApp *app);
void PicoApp_Cancel(PicoApp *app);
void PicoApp_Frame(PicoApp *app);
void PicoApp_RequestReload(PicoApp *app);

void PicoPlugins_Load(PicoApp *app);
void PicoPlugins_Reload(PicoApp *app);
void PicoPlugins_Poll(PicoApp *app);
void PicoPlugins_OnFrame(PicoApp *app, float dt);
void PicoPlugins_UnloadUser(PicoApp *app);
void PicoPlugins_Shutdown(PicoApp *app);

void PicoChat_Render(PicoApp *app);
void PicoChat_HandlePointer(PicoApp *app);
void PicoComposer_HandleInput(PicoApp *app);
void PicoComposer_HandlePointer(PicoApp *app);
void PicoComposer_Render(PicoApp *app);
void PicoComposer_DrawOverlay(PicoApp *app);
bool PicoComposer_HasSelection(const PicoApp *app);
void PicoComposer_Copy(PicoApp *app);
void PicoFooter_Render(PicoApp *app);
void PicoOverlay_Render(PicoApp *app);
void PicoOverlay_OnFrame(PicoApp *app, float dt);

#endif
