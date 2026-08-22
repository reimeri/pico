#ifndef PICO_USAGE_H
#define PICO_USAGE_H

#include "agent_internal.h"

bool PicoUsage_Apply(PicoAgent *agent, int input_tokens, int cached_tokens, int *normalized_cached);
bool PicoUsage_SessionPercent(const PicoAgent *agent, int *percent);

#endif
