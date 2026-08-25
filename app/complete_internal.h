#ifndef PICO_COMPLETE_INTERNAL_H
#define PICO_COMPLETE_INTERNAL_H

#include <stdint.h>

void PicoComplete_BeforeEdit(int from, int to);
uint64_t PicoComplete_TokenId(void);

#endif
