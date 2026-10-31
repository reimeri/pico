#ifndef PICO_SUBAGENT_SETTINGS_H
#define PICO_SUBAGENT_SETTINGS_H

#include "pico/app.h"

typedef struct PicoSubagentSettings PicoSubagentSettings;
typedef struct PicoSubagentSettingsSave PicoSubagentSettingsSave;
PicoSubagentSettings *PicoSubagentSettings_Create(PicoHost *host);
void PicoSubagentSettings_Free(PicoSubagentSettings *s);
void PicoSubagentSettings_Open(PicoSubagentSettings *s);
void PicoSubagentSettings_Discard(PicoSubagentSettings *s);
void PicoSubagentSettings_Blur(PicoSubagentSettings *s);
void PicoSubagentSettings_Render(PicoSubagentSettings *s);
void PicoSubagentSettings_RenderConfirmation(PicoSubagentSettings *s);
void PicoSubagentSettings_AfterLayout(PicoSubagentSettings *s);
bool PicoSubagentSettings_Frame(PicoSubagentSettings *s);
void PicoSubagentSettings_Draw(PicoSubagentSettings *s);
bool PicoSubagentSettings_RequestLeave(PicoSubagentSettings *s);
/* 1 discard approved, -1 stay, 0 no decision yet. */
int PicoSubagentSettings_TakeLeaveDecision(PicoSubagentSettings *s);
bool PicoSubagentSettings_TakeApplyRequest(PicoSubagentSettings *s);
/* Prepare/complete on the main thread, run on the persistence worker. NULL
 * snapshots are valid when no profiles changed. Completion rebases only the
 * successful operations, preserving remaining drafts on partial failure. */
bool PicoSubagentSettings_PrepareSave(PicoSubagentSettings *s, PicoSubagentSettingsSave **out);
bool PicoSubagentSettings_RunSave(PicoSubagentSettingsSave *save);
void PicoSubagentSettings_CompleteSave(PicoSubagentSettings *s, PicoSubagentSettingsSave *save);
void PicoSubagentSettings_FreeSave(PicoSubagentSettingsSave *save);
#endif
