#ifndef PICO_SETTINGS_H
#define PICO_SETTINGS_H

#include "agent_internal.h"
#include "config.h"
void Pico_MkdirP(const char *path);
void Pico_RandomHex(char *out, size_t cap);
void Pico_IsoTime(char *out, size_t cap, bool filename);
void PicoSettings_Load(PicoApp *app);
char *PicoSettings_LoadSystemPrompt(const PicoApp *app);
int PicoSettings_LoadedContext(const PicoApp *app, const char **labels, int max);
PicoModel *PicoSettings_FindModel(PicoApp *app, const char *id);
const PicoModel *PicoSettings_FindModelConst(const PicoApp *app, const char *id);
PicoModel *PicoSettings_ActiveModel(PicoApp *app, const PicoAgent *agent);
const PicoModel *PicoSettings_ActiveModelConst(const PicoApp *app, const PicoAgent *agent);
const char *PicoSettings_ActiveEffort(const PicoAgent *agent);
void PicoSettings_InitAgent(const PicoApp *app, PicoAgent *agent);
void PicoSettings_SyncAgent(const PicoApp *app, PicoAgent *agent);
bool PicoSettings_EffortAllowed(const PicoModel *model, const char *effort);
bool PicoSettings_SetModel(PicoApp *app, PicoAgent *agent, const char *id_or_name);
bool PicoSettings_SetEffort(PicoApp *app, PicoAgent *agent, const char *level);
bool PicoSettings_SaveSelection(PicoApp *app, const PicoAgent *agent, bool save_model, bool save_effort);

#endif
