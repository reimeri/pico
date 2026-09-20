#ifndef PICO_WORKTREE_H
#define PICO_WORKTREE_H

#include "pico/host.h"

#include <stdbool.h>
#include <stddef.h>

typedef struct PicoWorktreeInfo {
    bool checkout_root;
    bool linked;
    bool can_create;
    char checkout_path[4096];
    char project_path[4096];
    char checkout_name[256];
} PicoWorktreeInfo;

typedef struct PicoWorktreeResult {
    bool success;
    bool source_selected;
    PicoAgentId source_agent_id;
    char name[256];
    char path[4096];
    char error[1024];
} PicoWorktreeResult;

bool PicoWorktree_Discover(const char *path, PicoWorktreeInfo *out);
bool PicoWorktree_SuggestName(char *out, size_t cap);
bool PicoWorktree_ValidateName(const char *name, char *error, size_t error_cap);
PicoResult PicoWorktree_Request(PicoHost *host, PicoAgentId source_agent_id,
                                const char *name, char *error, size_t error_cap);
bool PicoWorktree_TakeResult(PicoHost *host, PicoWorktreeResult *out);
bool PicoWorktree_Pending(const PicoHost *host);
bool PicoWorktree_PendingFor(const PicoHost *host, PicoAgentId source_agent_id);
void PicoWorktree_Cleanup(PicoHost *host);

#endif
