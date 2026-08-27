#ifndef PICO_BUILTIN_CHAT_H
#define PICO_BUILTIN_CHAT_H

#include <stdbool.h>

struct PicoApp;

void PicoChat_InspectClose(void);
void PicoChat_HarvestVirtualHeights(struct PicoApp *app);
bool PicoChat_TakeVirtualRelayout(void);

#endif
