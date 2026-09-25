#ifndef PICO_THEME_INTERNAL_H
#define PICO_THEME_INTERNAL_H

#include "pico/theme.h"

void Pico_SetFontScale(float scale);
/* Invalidate cached widths when fonts are unloaded (also used by tests). */
void Pico_MeasureCacheReset(void);
uint64_t Pico_FontGeneration(void);

/* Main-thread, process-owned arena. Replace only between completed layouts;
 * all Clay-owned pointers become invalid after successful replacement. */
bool Pico_InitClay(Clay_Dimensions dimensions);
void Pico_FreeClay(void);
void Pico_HandleClayErrors(Clay_ErrorData error_data);
bool Pico_NeedsClayReinit(void);
void Pico_ClearClayReinit(void);
bool Pico_ReinitClay(Font *fonts, bool debug_enabled);
void Pico_CaptureClayScroll(void);
void Pico_RememberClayScroll(void);
bool Pico_RestoreClayScroll(void);

#endif
