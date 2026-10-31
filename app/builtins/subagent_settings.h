#ifndef PICO_SUBAGENT_SETTINGS_H
#define PICO_SUBAGENT_SETTINGS_H

#include "pico/app.h"

typedef struct PicoSubagentSettings PicoSubagentSettings;
PicoSubagentSettings *PicoSettingsUi_SubagentEditor(PicoHost *host);
PicoSubagentSettings *PicoSubagentSettings_Create(PicoHost *host);
void PicoSubagentSettings_Free(PicoSubagentSettings *s);
void PicoSubagentSettings_Open(PicoSubagentSettings *s);
/* Width is bounded by the Settings card, not a previous content layout. */
void PicoSubagentSettings_Render(PicoSubagentSettings *s, float width);
void PicoSubagentSettings_AfterLayout(PicoSubagentSettings *s);
bool PicoSubagentSettings_Frame(PicoSubagentSettings *s);
void PicoSubagentSettings_Draw(PicoSubagentSettings *s);
bool PicoSubagentSettings_RequestLeave(PicoSubagentSettings *s);
/* 1 discard approved, -1 stay, 0 no decision yet. */
int PicoSubagentSettings_TakeLeaveDecision(PicoSubagentSettings *s);
#endif
