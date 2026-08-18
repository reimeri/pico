#ifndef PICO_AGENT_H
#define PICO_AGENT_H

#include "pico/app.h"

void PicoAgent_Init(PicoApp *app);
void PicoAgent_Shutdown(PicoApp *app);
void PicoAgent_StartTurn(PicoApp *app, const char *user_text);
void PicoAgent_Cancel(PicoApp *app);
void PicoAgent_DismissError(PicoApp *app);
void PicoAgent_Pump(PicoApp *app);
bool PicoAgent_BlocksReload(const PicoApp *app);

#endif
