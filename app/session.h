#ifndef PICO_SESSION_H
#define PICO_SESSION_H

#include "agent_internal.h"

#include <stdint.h>
#include <time.h>

#define PICO_SESSION_TITLE_MAX_BYTES (72 * 4)

typedef struct PicoSessionInfo {
    char path[4096];
    char cwd[4096];
    char id[40];
    char title[PICO_SESSION_TITLE_MAX_BYTES + 1];
    char model[128];
    char effort[PICO_EFFORT_LEN];
    PicoAgentKind kind;
    time_t mtime;
    long mtime_nsec;
    time_t ctime;
    long ctime_nsec;
    uint64_t inode;
    uint64_t size;
    bool unseen_complete;
} PicoSessionInfo;

#define PICO_CATALOG_NAME_MAX 128
#define PICO_MAX_CATALOG_WORKSPACES 64
#define PICO_MAX_CATALOG_SESSIONS 256

typedef struct PicoCatalogSession {
    char id[40];
    char title[PICO_SESSION_TITLE_MAX_BYTES + 1];
    char model[128];
    char effort[PICO_EFFORT_LEN];
    time_t mtime;
    long mtime_nsec;
    time_t ctime;
    long ctime_nsec;
    uint64_t inode;
    uint64_t size;
    bool unseen_complete;
    PicoAgentKind kind;
} PicoCatalogSession;

typedef struct PicoCatalogWorkspace {
    char key[4096];
    char path[4096];
    char name[PICO_CATALOG_NAME_MAX];
    int order;
    bool collapsed;
    PicoCatalogSession *sessions;
    int session_count;
} PicoCatalogWorkspace;

void PicoCatalog_Free(PicoCatalogWorkspace *list, int n);
/* Scan ~/.config/pico/sessions. jsonl files are authoritative for sessions.
 * Recovers missing .workspace.json from jsonl cwd. Omits entries whose path is
 * not an existing directory. .workspace.json caches listing rows keyed by
 * id plus the JSONL stat generation so unchanged jsonl is not re-parsed. Caller frees with
 * PicoCatalog_Free. */
int PicoCatalog_Scan(PicoCatalogWorkspace **out);
int PicoCatalog_Ensure(const char *workspace_path);
int PicoCatalog_SetCollapsed(const char *workspace_path, bool collapsed);
int PicoCatalog_SetSessionModel(const char *workspace_path, const char *session_id,
                                const char *model, const char *effort);

typedef struct PicoSessionHeader {
    int version;
    PicoAgentKind kind;
    char id[40];
    char title[PICO_SESSION_TITLE_MAX_BYTES + 1];
    char profile[65];
    char initial_purpose[1025];
    char parent_session_id[40];
    char model[128];
} PicoSessionHeader;

void PicoSession_Start(PicoHost *app, PicoAgent *agent, PicoSessionStart start, const char *session_file);
/* `/resume` passes parents_only to hide subagents; resolve still lists all. */
int PicoSession_List(const PicoWorkspace *workspace, PicoSessionInfo **out, bool parents_only);
int PicoSession_Open(PicoHost *app, PicoAgent *agent, const char *id);
/* Resolve a durable session to a canonical path. Manager callers use exact IDs. */
int PicoSession_Resolve(const PicoWorkspace *workspace, const char *id, bool allow_prefix,
                        char *path, size_t path_cap);
int PicoSession_ReadHeader(const char *path, PicoSessionHeader *out);
/* Read a durable session's visible transcript without reserving the file.
 * Messages have source, reasoning, and tool strings; markdown documents are
 * empty until PicoMessages_PrepareDocs. Caller frees with PicoMessages_Free. */
int PicoSession_LoadTranscript(const PicoWorkspace *workspace, const char *id,
                               PicoMessage **out, int *out_count);
/* Replay a fully validated file into an unpublished/reserved agent. */
int PicoSession_Replay(PicoHost *app, PicoAgent *agent, const char *path,
                       bool append_interrupted);
void PicoSession_AppendInterrupted(PicoHost *app, PicoAgent *agent);
void PicoSession_Reset(PicoHost *app, PicoAgent *agent);
void PicoSession_ReplayToolDetails(PicoHost *app, PicoAgent *agent);
PicoSessionWriteResult PicoSession_LogUser(PicoHost *app, PicoAgent *agent,
                                             const char *content, const char *display,
                                             const char *parts_json);
PicoSessionWriteResult PicoSession_LogUsage(PicoHost *app, PicoAgent *agent,
                                            int input_tokens, int cached_tokens);
PicoSessionWriteResult PicoSession_LogAssistant(PicoHost *app, PicoAgent *agent,
                                                int message_group, const char *content,
                                                const char *thinking,
                                                const char *thinking_signature,
                                                const char *parts_json,
                                                const char *thinking_parts_json,
                                                int thinking_ms);
PicoSessionWriteResult PicoSession_LogToolCall(PicoHost *app, PicoAgent *agent,
                                               int message_group, const char *call_id,
                                               const char *name, const char *args,
                                               const char *item_id);
PicoSessionWriteResult PicoSession_LogToolResult(PicoHost *app, PicoAgent *agent,
                                                 const char *call_id, const char *name,
                                                 const char *output, bool is_error,
                                                 const char *details_json);
PicoSessionWriteResult PicoSession_LogCompaction(PicoHost *app, PicoAgent *agent,
                                                 const char *summary, int tokens_before);
PicoSessionWriteResult PicoSession_LogModelChange(PicoHost *app, PicoAgent *agent,
                                                  const char *model, const char *effort);
PicoSessionWriteResult PicoSession_LogCustom(PicoHost *app, PicoAgent *agent,
                                             const char *ext, const char *data_json);
PicoSessionWriteResult PicoSession_LogTitle(PicoHost *app, PicoAgent *agent, const char *title);
PicoSessionWriteResult PicoSession_LogUnseenComplete(PicoHost *app, PicoAgent *agent,
                                                     bool complete);
void PicoSession_SetUnseenComplete(PicoHost *app, PicoAgent *agent, bool complete);
void PicoSessionPersist_Init(PicoHost *host);
void PicoSessionPersist_Shutdown(PicoHost *host);
void PicoSessionPersist_Pump(PicoHost *host);
void PicoSession_EnqueueModelChange(PicoHost *app, PicoAgent *agent);
void PicoSession_DrainPersist(PicoHost *app, PicoAgent *agent);
bool PicoSession_DrainPersistBefore(PicoHost *app, PicoAgent *agent, const struct timespec *deadline);

#endif
