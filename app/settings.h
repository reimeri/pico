#ifndef PICO_SETTINGS_H
#define PICO_SETTINGS_H

#include "pico/app.h"

void Pico_ConfigDir(char *out, size_t cap);
void PicoSettings_Load(PicoApp *app);
char *PicoSettings_LoadSystemPrompt(const PicoApp *app);

#endif
