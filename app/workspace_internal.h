#ifndef PICO_WORKSPACE_INTERNAL_H
#define PICO_WORKSPACE_INTERNAL_H

#include "pico/host.h"
#include "pico/workspace.h"

#include "agent_manager.h"

struct PicoWorkspace {
    PicoHost *host;
    PicoWorkspaceId id;
    char path[4096];
    PicoWorkspaceState state;
    PicoAgentManager *agents;
};

#endif
