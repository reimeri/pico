#ifndef PICO_COMPLETE_INTERNAL_H
#define PICO_COMPLETE_INTERNAL_H

#include <stdint.h>
#include <stdbool.h>
#include "pico/host.h"

/* Builtin-only, nonselectable popup status; not an extension async API. */
void PicoComplete_SetStatus(const char *status);
void PicoComplete_SetScope(const PicoHost *host, int start, const char *prefix);
bool PicoComplete_TokenRange(const PicoHost *host, int *start, int *end);
void PicoComplete_BeforeEdit(int from, int to);
uint64_t PicoComplete_TokenId(void);

#endif
