#ifndef PICO_AGENT_H
#define PICO_AGENT_H

#include "agent_internal.h"

#include <time.h>

PicoAgent *PicoAgent_Create(PicoApp *app);
/* Rebind an unpublished idle agent after a staged workspace replacement. */
void PicoAgent_RebindHost(PicoApp *app, PicoAgent *agent, PicoAgentManager *manager);
/* False when a worker was still running and had to be detached. */
bool PicoAgent_Destroy(PicoAgent *agent);
bool PicoAgent_DestroyBefore(PicoAgent *agent, const struct timespec *deadline);
void PicoAgent_ReapRetired(PicoAgentManager *manager);
bool PicoAgent_ShutdownRetired(PicoAgentManager *manager, const struct timespec *deadline);
bool PicoAgent_RetiredReferences(const PicoAgentManager *manager, PicoAgentId id);
void PicoAgent_StartTurn(PicoApp *app, PicoAgent *agent, const char *user_text);
void PicoAgent_Cancel(PicoAgent *agent);
void PicoAgent_ForceCancel(PicoApp *app, PicoAgent *agent);
bool PicoAgent_IsBusy(const PicoAgent *agent);
bool PicoAgent_CancelRequested(const PicoAgent *agent);
bool PicoAgent_AskUiOpen(const PicoAgent *agent);
void PicoAgent_DismissError(PicoAgent *agent);
void PicoAgent_Pump(PicoApp *app, PicoAgent *agent);
bool PicoAgent_PendingAsk(const PicoAgent *agent, PicoToolAsk *out);
bool PicoAgent_AnswerAsk(PicoAgent *agent, uint64_t id, const char *answer_json);
bool PicoAgent_BlocksReload(const PicoAgent *agent);
void PicoAgent_PrepareReload(PicoAgent *agent);
bool PicoAgent_RevalidateToolPolicy(const PicoApp *app, PicoAgent *agent);
void PicoAgent_Compact(PicoApp *app, PicoAgent *agent);
/* Malloc'd instructions for the next normal turn. Caller frees. */
char *PicoAgent_BuildInstructions(PicoApp *app, PicoAgent *agent);
struct PicoAuthStore *PicoAgentContext_AuthStore(const PicoAgentContext *ctx);
bool PicoAgentContext_LockIfLive(const PicoAgentContext *ctx);
void PicoAgentContext_UnlockLive(const PicoAgentContext *ctx);

const char *PicoAgent_CacheKey(const PicoAgent *agent);
void PicoAgent_SetCacheKey(PicoAgent *agent, const char *key);
void PicoAgent_RotateCacheKey(PicoAgent *agent);
void PicoAgent_ClearInput(PicoAgent *agent);
void PicoAgent_PushHistoryUser(PicoAgent *agent, const char *text);
void PicoAgent_PushHistoryUserParts(PicoAgent *agent, const char *text, const char *parts_json);
void PicoAgent_PushHistoryAssistant(PicoAgent *agent, const char *text, const char *thinking,
                                    const char *signature);
void PicoAgent_PushHistoryAssistantParts(PicoAgent *agent, const char *text, const char *thinking,
                                         const char *signature, const char *parts_json);
void PicoAgent_PushHistoryFunctionCall(PicoAgent *agent, const char *call_id, const char *name,
                                       const char *args, const char *item_id);
void PicoAgent_PushHistoryFunctionOutput(PicoAgent *agent, const char *call_id, const char *name,
                                         const char *output, bool is_error);

void PicoAgent_AddMessage(PicoApp *app, PicoAgent *agent, PicoRole role, const char *markdown);
void PicoAgent_AppendAssistant(PicoApp *app, PicoAgent *agent, const char *text);
void PicoAgent_AppendThink(PicoApp *app, PicoAgent *agent, const char *text);
void PicoAgent_AddToolCall(PicoApp *app, PicoAgent *agent, const char *name, const char *args);
void PicoAgent_AddToolCallWithId(PicoApp *app, PicoAgent *agent, const char *call_id,
                                const char *name, const char *args);
void PicoAgent_SetLastToolOutput(PicoAgent *agent, const char *output, bool is_error);
void PicoAgent_SetToolOutputByCallId(PicoAgent *agent, const char *call_id,
                                     const char *output, bool is_error);
void PicoAgent_ClearMessages(PicoAgent *agent);
void PicoMessages_Free(PicoMessage *messages, int count);
bool PicoMessages_Copy(const PicoMessage *src, int count, PicoMessage **dst, int *dst_count);
void PicoMessages_PrepareDocs(PicoMessage *messages, int count);
void PicoAgent_CopyInfo(const PicoAgent *agent, PicoAgentInfo *out);
PicoAgentManager *PicoAgentContext_Manager(const PicoAgentContext *ctx);
const char *PicoAgentContext_ToolCallId(const PicoAgentContext *ctx);

#endif
