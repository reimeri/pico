#ifndef PICO_AGENT_H
#define PICO_AGENT_H

#include "pico/app.h"

void PicoAgent_Init(PicoApp *app);
/* False when a worker was still running and had to be detached, meaning anything
 * it can still reach must not be freed. */
bool PicoAgent_Shutdown(PicoApp *app);
void PicoAgent_StartTurn(PicoApp *app, const char *user_text);
void PicoAgent_Cancel(PicoApp *app);
void PicoAgent_ForceCancel(PicoApp *app);
bool PicoAgent_IsBusy(const PicoApp *app);
bool PicoAgent_CancelRequested(const PicoApp *app);
bool PicoAgent_AskUiOpen(const PicoApp *app);
void PicoAgent_DismissError(PicoApp *app);
void PicoAgent_Pump(PicoApp *app);
bool PicoAgent_BlocksReload(const PicoApp *app);
void PicoAgent_Compact(PicoApp *app);
/* Malloc'd instructions for the next normal turn: SYSTEM.md / AGENTS.md plus
 * pico_add_llm_hook extras. Caller frees. */
char *PicoAgent_BuildInstructions(PicoApp *app);

const char *PicoAgent_CacheKey(const PicoApp *app);
void PicoAgent_SetCacheKey(PicoApp *app, const char *key);
void PicoAgent_RotateCacheKey(PicoApp *app);
void PicoAgent_ClearInput(PicoApp *app);
void PicoAgent_PushHistoryUser(PicoApp *app, const char *text);
void PicoAgent_PushHistoryAssistant(PicoApp *app, const char *text);
void PicoAgent_PushHistoryFunctionCall(PicoApp *app, const char *call_id, const char *name, const char *args);
void PicoAgent_PushHistoryFunctionOutput(PicoApp *app, const char *call_id, const char *name,
                                         const char *output, bool is_error);

#endif
