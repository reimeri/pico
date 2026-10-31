#ifndef PICO_SETTINGS_UI_H
#define PICO_SETTINGS_UI_H
#include "pico/app.h"
/* Private shell integration; settings remain a host-global builtin. */
void PicoSettingsUi_Layout(PicoHost *host);
float PicoSettingsUi_ShellWidth(const PicoHost *host);
void PicoSettingsUi_Input(PicoHost *host);
bool PicoSettingsUi_BlocksKeyboard(const PicoHost *host);
bool PicoSettingsUi_BlocksPointer(const PicoHost *host);
bool PicoSettingsUi_ResizeHovered(const PicoHost *host);
bool PicoSettingsUi_CloseForBrowser(PicoHost *host);
#endif
