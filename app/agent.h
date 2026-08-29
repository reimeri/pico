#ifndef PICO_AGENT_H
#define PICO_AGENT_H

#include "agent_internal.h"
#include "settings.h"

#include <time.h>

typedef enum PicoToolCallProgress {
    PICO_TOOL_CALL_IDLE = 0,
    PICO_TOOL_CALL_RUNNING,
    PICO_TOOL_CALL_QUEUED,
} PicoToolCallProgress;

PicoAgent *PicoAgent_Create(PicoHost *app, PicoWorkspace *workspace);
/* Rebind an unpublished idle agent after a staged workspace replacement. */
void PicoAgent_RebindHost(PicoHost *app, PicoAgent *agent, PicoWorkspace *workspace);
/* False when a worker was still running and had to be detached. */
bool PicoAgent_Destroy(PicoAgent *agent);
bool PicoAgent_DestroyBefore(PicoAgent *agent, const struct timespec *deadline);
void PicoAgent_ReapRetired(PicoWorkspace *workspace);
bool PicoAgent_ShutdownRetired(PicoWorkspace *workspace, const struct timespec *deadline);
bool PicoAgent_RetiredReferences(const PicoWorkspace *workspace, PicoAgentId id);
void PicoAgent_StartTurn(PicoHost *app, PicoAgent *agent, const char *user_text);
void PicoAgent_StartTurnParts(PicoHost *app, PicoAgent *agent, const char *user_text,
                              const char *parts_json);
void PicoAgent_Cancel(PicoAgent *agent);
void PicoAgent_ForceCancel(PicoHost *app, PicoAgent *agent);
bool PicoAgent_IsBusy(const PicoAgent *agent);
/* Main-thread: which pending call this id is. IDLE if it is not in the live queue. */
PicoToolCallProgress PicoAgent_ToolCallProgress(const PicoAgent *agent, const char *call_id);
bool PicoAgent_CancelRequested(const PicoAgent *agent);
bool PicoAgent_AskUiOpen(const PicoAgent *agent);
void PicoAgent_DismissError(PicoAgent *agent);
void PicoAgent_Pump(PicoHost *app, PicoAgent *agent);
void PicoAgent_PumpBounded(PicoHost *app, PicoAgent *agent, int *budget);
bool PicoAgent_PendingAsk(const PicoAgent *agent, PicoToolAsk *out);
bool PicoAgent_AnswerAsk(PicoAgent *agent, uint64_t id, const char *answer_json);
bool PicoAgent_BlocksReload(const PicoAgent *agent);
void PicoAgent_PrepareReload(PicoAgent *agent);
bool PicoAgent_RevalidateToolPolicy(const PicoHost *app, PicoAgent *agent);
void PicoAgent_Compact(PicoHost *app, PicoAgent *agent);
/* Malloc'd instructions for the next normal turn. Caller frees. */
char *PicoAgent_BuildInstructions(PicoHost *app, PicoAgent *agent);
char *PicoAgent_BuildInstructionsSpans(PicoHost *app, PicoAgent *agent, PicoPromptSpan *spans,
                                       int *span_count);
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

void PicoAgent_AddMessage(PicoHost *app, PicoAgent *agent, PicoRole role, const char *markdown);
void PicoAgent_AppendAssistant(PicoHost *app, PicoAgent *agent, const char *text);
void PicoAgent_AppendThink(PicoHost *app, PicoAgent *agent, const char *text, int think_ms);
void PicoAgent_AppendThinkSummary(PicoHost *app, PicoAgent *agent, const char *text,
                                  int step, int think_ms);
void PicoTraceLine_Release(PicoTraceLine *line);
void PicoTraceLine_FreezeThink(PicoTraceLine *line);
void PicoAgent_AddToolCall(PicoHost *app, PicoAgent *agent, const char *name, const char *args);
void PicoAgent_AddToolCallWithId(PicoHost *app, PicoAgent *agent, const char *call_id,
                                const char *name, const char *args);
void PicoAgent_SetLastToolOutput(PicoAgent *agent, const char *output, bool is_error);
void PicoAgent_SetToolArgsByCallId(PicoAgent *agent, const char *call_id,
                                   const char *args);
void PicoAgent_SetToolOutputByCallId(PicoAgent *agent, const char *call_id,
                                     const char *output, bool is_error);
void PicoAgent_ClearMessages(PicoAgent *agent);
void PicoMessages_Free(PicoMessage *messages, int count);
bool PicoMessages_Copy(const PicoMessage *src, int count, PicoMessage **dst, int *dst_count);
void PicoMessages_PrepareDocs(PicoMessage *messages, int count);
void PicoAgent_CopyInfo(const PicoAgent *agent, PicoAgentInfo *out);
PicoWorkspace *PicoAgentContext_Workspace(const PicoAgentContext *ctx);
const char *PicoAgentContext_ToolCallId(const PicoAgentContext *ctx);

#endif
