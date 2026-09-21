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
bool PicoWorkspaceSettings_Load(PicoWorkspace *workspace);
float Pico_ClampChatWidth(float available, float text_max);
float Pico_ChatTextMaxPx(const PicoHost *app);
float Pico_ChatColumnMaxPx(const PicoHost *app);
char *PicoSettings_LoadSystemPrompt(const PicoWorkspace *workspace);
char *PicoSettings_LoadSystemPromptSpans(const PicoWorkspace *workspace, PicoPromptSpan *spans,
                                         int *span_count);
int PicoSettings_LoadedContext(const PicoWorkspace *workspace, const char **labels, int max);
PicoModel *PicoSettings_FindModel(PicoWorkspace *workspace, const char *id);
const PicoModel *PicoSettings_FindModelConst(const PicoWorkspace *workspace, const char *id);
/* The model the agent's in-flight or next request uses: the turn's pinned
 * snapshot while busy, otherwise the selected catalog model. */
PicoModel *PicoSettings_ActiveModel(const PicoAgent *agent);
const PicoModel *PicoSettings_ActiveModelConst(const PicoAgent *agent);
/* The catalog model the agent's selection resolves to: what the next turn will
 * use. Differs from PicoSettings_ActiveModel while a busy turn keeps its
 * pinned snapshot. */
PicoModel *PicoSettings_SelectedModel(PicoAgent *agent);
const PicoModel *PicoSettings_SelectedModelConst(const PicoAgent *agent);
/* The selected effort for the next turn (the running turn keeps its pinned
 * effort; see PicoSettings_PinTurnModel). */
const char *PicoSettings_ActiveEffort(const PicoAgent *agent);
/* Pin the selected model and resolved effort as the agent's running snapshot;
 * call at turn start. The pin holds for the whole turn and is released by
 * PicoSettings_ReconcileIdleAgent once the agent is idle. */
void PicoSettings_PinTurnModel(PicoAgent *agent);
void PicoSettings_InitAgent(PicoAgent *agent);
void PicoSettings_SyncAgent(PicoAgent *agent);
void PicoSettings_ReconcileIdleAgent(PicoAgent *agent);
bool PicoSettings_EffortAllowed(const PicoModel *model, const char *effort);
bool PicoSettings_SetModel(PicoAgent *agent, const char *id_or_name);
bool PicoSettings_SetEffort(PicoAgent *agent, const char *level);
bool PicoSettings_ModelSupportsFast(const PicoWorkspace *workspace, const PicoModel *model);
bool PicoSettings_FastAvailable(const PicoAgent *agent);
bool PicoSettings_SetFast(PicoAgent *agent, bool enabled);
bool PicoHost_SetExtensionDisabled(PicoHost *host, const char *name, bool disabled);
bool PicoWorkspace_SetExtensionDisabled(PicoWorkspace *workspace, const char *name, bool disabled);

#define PICO_SETTINGS_MODEL_MAX 64

typedef struct PicoUserSettingsDraft {
    char default_model[128];
    int context_limit_fallback;
    int max_parallel_tools;
    double compact_ratio;
    bool compact_enabled;
    bool resume_last;
    bool spell;
    char spell_lang[32];
    double font_scale;
    int chat_width;
    PicoModel *models;
    char (*source_model_ids)[128];
    int model_count;
} PicoUserSettingsDraft;

void PicoSettings_InitUserDraft(PicoUserSettingsDraft *draft);
void PicoSettings_FreeUserDraft(PicoUserSettingsDraft *draft);
bool PicoSettings_LoadUserDraft(PicoUserSettingsDraft *draft);
bool PicoSettings_ParseModelContextLimit(const char *text, int *out);
const char *PicoSettings_ValidateUserDraft(const PicoUserSettingsDraft *draft);
bool PicoSettings_SaveUserDraft(PicoHost *host, const PicoUserSettingsDraft *draft);
bool PicoSettings_ApplyUserDraft(PicoHost *host);

#endif
