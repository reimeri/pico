#ifndef PICO_APP_H
#define PICO_APP_H

#include <stdbool.h>
#include <stddef.h>

#include "raylib.h"

#include "markdown.h"
#include "pico/theme.h"

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

typedef struct PicoMessage {
    PicoRole role;
    char *source;
    MdDocument doc;
} PicoMessage;

typedef struct PicoComposer {
    char *text;
    int length;
    int capacity;
    int cursor; // byte offset
} PicoComposer;

typedef struct PicoScrollbar {
    Clay_Vector2 click_origin;
    Clay_Vector2 position_origin;
    bool mouse_down;
} PicoScrollbar;

struct PicoApp;

typedef void (*PicoViewFn)(struct PicoApp *app);

typedef struct PicoApp {
    PicoMessage *messages;
    int message_count;
    int message_capacity;
    PicoComposer composer;
    PicoAgentState agent_state;
    const char *model_name;
    int tokens_used;
    int tokens_limit;
    Font *fonts;
    PicoViewFn views[PICO_SLOT_COUNT];
    PicoScrollbar chat_scrollbar;
    bool chat_follow_bottom;
    bool reinitialize_clay;
    bool debug_enabled;
    const char *hovered_link;
} PicoApp;

void pico_add_view(PicoApp *app, PicoUiSlot slot, PicoViewFn render);

void PicoApp_Init(PicoApp *app, Font *fonts);
void PicoApp_Free(PicoApp *app);
void PicoApp_AddMessage(PicoApp *app, PicoRole role, const char *markdown);
void PicoApp_Submit(PicoApp *app);
void PicoApp_Frame(PicoApp *app);

void PicoChat_Render(PicoApp *app);
void PicoComposer_HandleInput(PicoApp *app);
void PicoComposer_Render(PicoApp *app);
void PicoFooter_Render(PicoApp *app);

#endif
