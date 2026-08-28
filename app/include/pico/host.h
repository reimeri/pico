#ifndef PICO_HOST_H
#define PICO_HOST_H

#include "pico/agent.h"
#include "pico/workspace.h"

#include "raylib.h"

/* PICO_MAX_TOTAL_AGENTS is declared in pico/agent.h. */

#ifdef __cplusplus
extern "C" {
#endif

#define PICO_MAX_WORKSPACES 8

typedef enum PicoResult {
    PICO_OK = 0,
    PICO_INVALID,
    PICO_NOT_FOUND,
    PICO_BUSY,
    PICO_LIMIT,
    PICO_ALREADY_OPEN,
    PICO_SESSION_IN_USE,
    PICO_SESSION_INVALID,
    PICO_NO_MEMORY,
} PicoResult;

typedef enum PicoHostShutdownResult {
    PICO_HOST_SHUTDOWN_CLEAN = 0,
    PICO_HOST_SHUTDOWN_RETAINED,
} PicoHostShutdownResult;

PicoResult pico_host_init(PicoHost **out, Font *fonts, bool safe_mode);
PicoHostShutdownResult pico_host_free(PicoHost *host);
void pico_host_pump(PicoHost *host);

int pico_workspace_count(const PicoHost *host);
bool pico_workspace_info(const PicoHost *host, int index, PicoWorkspaceInfo *out);
PicoResult pico_workspace_open(PicoHost *host, const char *path, PicoWorkspaceId *out);
PicoResult pico_workspace_request_reload(PicoHost *host, PicoWorkspaceId id);
PicoResult pico_workspace_request_close(PicoHost *host, PicoWorkspaceId id);

PicoResult pico_main_agent_create(PicoHost *host, PicoWorkspaceId workspace_id,
                                  const PicoAgentCreateOptions *options, PicoAgentId *out);
PicoResult pico_agent_submit(PicoHost *host, PicoAgentId id, const char *text, const char *parts_json);

#ifdef __cplusplus
}
#endif

#endif
