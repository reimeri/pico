#ifndef PICO_SESSION_H
#define PICO_SESSION_H

#include "agent_internal.h"

#include <time.h>

typedef struct PicoSessionInfo {
    char path[4096];
    char id[40];
    char title[256];
    time_t mtime;
} PicoSessionInfo;

void PicoSession_Start(PicoApp *app, PicoAgent *agent, PicoSessionStart start, const char *session_file);
int PicoSession_List(const PicoApp *app, PicoSessionInfo **out);
int PicoSession_Open(PicoApp *app, PicoAgent *agent, const char *id);
/* Resolve a durable session to a canonical path. Manager callers use exact IDs. */
int PicoSession_Resolve(const PicoApp *app, const char *id, bool allow_prefix,
                        char *path, size_t path_cap);
/* Replay a fully validated file into an unpublished/reserved agent. */
int PicoSession_Replay(PicoApp *app, PicoAgent *agent, const char *path,
                       bool append_interrupted);
void PicoSession_AppendInterrupted(PicoApp *app, PicoAgent *agent);
void PicoSession_Reset(PicoApp *app, PicoAgent *agent);
void PicoSession_ReplayToolDetails(PicoApp *app, PicoAgent *agent);
void PicoSession_LogUser(PicoApp *app, PicoAgent *agent, const char *content, const char *display);
void PicoSession_LogUsage(PicoApp *app, PicoAgent *agent, int input_tokens, int cached_tokens);
void PicoSession_LogAssistant(PicoApp *app, PicoAgent *agent, const char *content);
void PicoSession_LogToolCall(PicoApp *app, PicoAgent *agent, const char *call_id, const char *name,
                             const char *args);
void PicoSession_LogToolResult(PicoApp *app, PicoAgent *agent, const char *call_id, const char *name,
                               const char *output, bool is_error, const char *details_json);
void PicoSession_LogCompaction(PicoApp *app, PicoAgent *agent, const char *summary, int tokens_before);
void PicoSession_LogModelChange(PicoApp *app, PicoAgent *agent, const char *model, const char *effort);
void PicoSession_LogCustom(PicoApp *app, PicoAgent *agent, const char *ext, const char *data_json);

#endif
