#ifndef PICO_SETTINGS_H
#define PICO_SETTINGS_H

#include "pico/app.h"

void Pico_ConfigDir(char *out, size_t cap);
void Pico_MkdirP(const char *path);
void Pico_RandomHex(char *out, size_t cap);
void Pico_IsoTime(char *out, size_t cap, bool filename);
void PicoSettings_Load(PicoApp *app);
char *PicoSettings_LoadSystemPrompt(const PicoApp *app);

#endif
