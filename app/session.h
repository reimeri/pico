#ifndef PICO_SESSION_H
#define PICO_SESSION_H

#include "agent_internal.h"

#include <time.h>

typedef struct PicoSessionInfo {
    char path[4096];
    char id[40];
    char title[256];
    PicoAgentKind kind;
    time_t mtime;
} PicoSessionInfo;

typedef struct PicoSessionHeader {
    int version;
    PicoAgentKind kind;
    char id[40];
    char profile[65];
    char initial_purpose[1025];
    char parent_session_id[40];
    char model[128];
} PicoSessionHeader;

void PicoSession_Start(PicoApp *app, PicoAgent *agent, PicoSessionStart start, const char *session_file);
/* `/resume` passes parents_only to hide subagents; resolve still lists all. */
int PicoSession_List(const PicoApp *app, PicoSessionInfo **out, bool parents_only);
int PicoSession_Open(PicoApp *app, PicoAgent *agent, const char *id);
/* Resolve a durable session to a canonical path. Manager callers use exact IDs. */
int PicoSession_Resolve(const PicoApp *app, const char *id, bool allow_prefix,
                        char *path, size_t path_cap);
int PicoSession_ReadHeader(const char *path, PicoSessionHeader *out);
/* Read a durable session's visible transcript without reserving the file.
 * Messages have source/tool strings; markdown documents are empty until
 * PicoMessages_PrepareDocs. Caller frees with PicoMessages_Free. */
int PicoSession_LoadTranscript(const PicoApp *app, const char *id,
                               PicoMessage **out, int *out_count);
/* Replay a fully validated file into an unpublished/reserved agent. */
int PicoSession_Replay(PicoApp *app, PicoAgent *agent, const char *path,
                       bool append_interrupted);
void PicoSession_AppendInterrupted(PicoApp *app, PicoAgent *agent);
void PicoSession_Reset(PicoApp *app, PicoAgent *agent);
void PicoSession_ReplayToolDetails(PicoApp *app, PicoAgent *agent);
PicoSessionWriteResult PicoSession_LogUser(PicoApp *app, PicoAgent *agent,
                                             const char *content, const char *display);
PicoSessionWriteResult PicoSession_LogUsage(PicoApp *app, PicoAgent *agent,
                                            int input_tokens, int cached_tokens);
PicoSessionWriteResult PicoSession_LogAssistant(PicoApp *app, PicoAgent *agent,
                                                const char *content, const char *thinking,
                                                const char *thinking_signature);
PicoSessionWriteResult PicoSession_LogToolCall(PicoApp *app, PicoAgent *agent,
                                               const char *call_id, const char *name,
                                               const char *args, const char *item_id);
PicoSessionWriteResult PicoSession_LogToolResult(PicoApp *app, PicoAgent *agent,
                                                 const char *call_id, const char *name,
                                                 const char *output, bool is_error,
                                                 const char *details_json);
PicoSessionWriteResult PicoSession_LogCompaction(PicoApp *app, PicoAgent *agent,
                                                 const char *summary, int tokens_before);
PicoSessionWriteResult PicoSession_LogModelChange(PicoApp *app, PicoAgent *agent,
                                                  const char *model, const char *effort);
PicoSessionWriteResult PicoSession_LogCustom(PicoApp *app, PicoAgent *agent,
                                             const char *ext, const char *data_json);

#endif
