#ifndef PICO_SETTINGS_H
#define PICO_SETTINGS_H

#include "agent_internal.h"
#include "config.h"

#include <stddef.h>

typedef enum PicoPromptSource {
    PICO_PROMPT_SOURCE_BASE = 0,
    PICO_PROMPT_SOURCE_WORKSPACE_SYSTEM,
    PICO_PROMPT_SOURCE_AGENTS,
    PICO_PROMPT_SOURCE_LLM_HOOK,
} PicoPromptSource;

typedef struct PicoPromptSpan {
    PicoPromptSource source;
    size_t start;
    size_t length;
} PicoPromptSpan;

#define PICO_PROMPT_SPAN_MAX 8

void Pico_MkdirP(const char *path);
void Pico_RandomHex(char *out, size_t cap);
void Pico_IsoTime(char *out, size_t cap, bool filename);
void PicoHostPreferences_Load(PicoHost *host);
void PicoWorkspaceSettings_Load(PicoWorkspace *workspace);
float Pico_ClampChatWidth(float available, float text_max);
float Pico_ChatTextMaxPx(const PicoHost *app);
float Pico_ChatColumnMaxPx(const PicoHost *app);
char *PicoSettings_LoadSystemPrompt(const PicoWorkspace *workspace);
char *PicoSettings_LoadSystemPromptSpans(const PicoWorkspace *workspace, PicoPromptSpan *spans,
                                         int *span_count);
int PicoSettings_LoadedContext(const PicoWorkspace *workspace, const char **labels, int max);
PicoModel *PicoSettings_FindModel(PicoWorkspace *workspace, const char *id);
const PicoModel *PicoSettings_FindModelConst(const PicoWorkspace *workspace, const char *id);
PicoModel *PicoSettings_ActiveModel(const PicoAgent *agent);
const PicoModel *PicoSettings_ActiveModelConst(const PicoAgent *agent);
const char *PicoSettings_ActiveEffort(const PicoAgent *agent);
void PicoSettings_InitAgent(PicoAgent *agent);
void PicoSettings_SyncAgent(PicoAgent *agent);
bool PicoSettings_EffortAllowed(const PicoModel *model, const char *effort);
bool PicoSettings_SetModel(PicoAgent *agent, const char *id_or_name);
bool PicoSettings_SetEffort(PicoAgent *agent, const char *level);
bool PicoSettings_SaveSelection(const PicoAgent *agent, bool save_model, bool save_effort);
bool PicoHost_SetExtensionDisabled(PicoHost *host, const char *name, bool disabled);
bool PicoWorkspace_SetExtensionDisabled(PicoWorkspace *workspace, const char *name, bool disabled);

#endif
