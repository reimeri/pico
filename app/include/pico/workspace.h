#ifndef PICO_WORKSPACE_H
#define PICO_WORKSPACE_H

#include "pico/agent.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct PicoHost PicoHost;
typedef struct PicoWorkspace PicoWorkspace;
typedef uint64_t PicoWorkspaceId;

typedef enum PicoWorkspaceState {
    PICO_WORKSPACE_OPEN = 0,
    PICO_WORKSPACE_RELOADING,
    PICO_WORKSPACE_CLOSING,
    PICO_WORKSPACE_CLOSED,
} PicoWorkspaceState;

typedef struct PicoWorkspaceInfo {
    PicoWorkspaceId id;
    PicoWorkspaceState state;
    char path[4096];
    int main_agent_count;
    int total_agent_count;
} PicoWorkspaceInfo;

PicoHost *pico_workspace_host(PicoWorkspace *workspace);

#ifdef __cplusplus
}
#endif

#endif
