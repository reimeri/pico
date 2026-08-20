#ifndef PICO_SETTINGS_H
#define PICO_SETTINGS_H

#include "pico/app.h"

void Pico_ConfigDir(char *out, size_t cap);
void Pico_MkdirP(const char *path);
void Pico_RandomHex(char *out, size_t cap);
void Pico_IsoTime(char *out, size_t cap, bool filename);
void PicoSettings_Load(PicoApp *app);
char *PicoSettings_LoadSystemPrompt(const PicoApp *app);
int PicoSettings_LoadedContext(const PicoApp *app, const char **labels, int max);
PicoModel *PicoSettings_ActiveModel(PicoApp *app);
const PicoModel *PicoSettings_ActiveModelConst(const PicoApp *app);
const char *PicoSettings_ActiveEffort(const PicoApp *app);
void PicoSettings_SyncActive(PicoApp *app);
bool PicoSettings_EffortAllowed(const PicoModel *model, const char *effort);
bool PicoSettings_SetModel(PicoApp *app, const char *id_or_name);
bool PicoSettings_SetEffort(PicoApp *app, const char *level);
bool PicoSettings_SaveSelection(PicoApp *app, bool save_model, bool save_effort);

#endif
