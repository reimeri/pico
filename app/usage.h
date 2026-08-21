#ifndef PICO_USAGE_H
#define PICO_USAGE_H

#include "pico/app.h"

bool PicoUsage_Apply(PicoApp *app, int input_tokens, int cached_tokens, int *normalized_cached);
bool PicoUsage_SessionPercent(const PicoApp *app, int *percent);

#endif
