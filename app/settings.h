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
void PicoSettings_Load(PicoHost *app);
float Pico_ClampChatWidth(float available, float text_max);
float Pico_ChatTextMaxPx(const PicoHost *app);
float Pico_ChatColumnMaxPx(const PicoHost *app);
char *PicoSettings_LoadSystemPrompt(const PicoHost *app);
char *PicoSettings_LoadSystemPromptSpans(const PicoHost *app, PicoPromptSpan *spans, int *span_count);
int PicoSettings_LoadedContext(const PicoHost *app, const char **labels, int max);
PicoModel *PicoSettings_FindModel(PicoHost *app, const char *id);
const PicoModel *PicoSettings_FindModelConst(const PicoHost *app, const char *id);
PicoModel *PicoSettings_ActiveModel(PicoHost *app, const PicoAgent *agent);
const PicoModel *PicoSettings_ActiveModelConst(const PicoHost *app, const PicoAgent *agent);
const char *PicoSettings_ActiveEffort(const PicoAgent *agent);
void PicoSettings_InitAgent(const PicoHost *app, PicoAgent *agent);
void PicoSettings_SyncAgent(const PicoHost *app, PicoAgent *agent);
bool PicoSettings_EffortAllowed(const PicoModel *model, const char *effort);
bool PicoSettings_SetModel(PicoHost *app, PicoAgent *agent, const char *id_or_name);
bool PicoSettings_SetEffort(PicoHost *app, PicoAgent *agent, const char *level);
bool PicoSettings_SaveSelection(PicoHost *app, const PicoAgent *agent, bool save_model, bool save_effort);
bool PicoSettings_SetExtensionDisabled(PicoHost *app, const char *name, bool disabled);

#endif
