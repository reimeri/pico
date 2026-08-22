#ifndef PICO_WORKSPACE_H
#define PICO_WORKSPACE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <time.h>

#define PICO_WORKSPACE_KEY_MAX 4096
#define PICO_WORKSPACE_PATH_MAX 4096
#define PICO_WORKSPACE_NAME_MAX 256

typedef struct PicoWorkspaceSessionCache {
    char id[40];
    char title[256];
    char model[128];
    char effort[16];
    time_t mtime;
} PicoWorkspaceSessionCache;

typedef struct PicoWorkspace {
    char key[PICO_WORKSPACE_KEY_MAX];
    char path[PICO_WORKSPACE_PATH_MAX];
    char name[PICO_WORKSPACE_NAME_MAX];
    int order;
    bool collapsed;
    bool available;
    PicoWorkspaceSessionCache *sessions;
    int session_count;
} PicoWorkspace;

typedef struct PicoWorkspaceRegistry {
    PicoWorkspace *items;
    int count;
    int capacity;
    char sessions_dir[PICO_WORKSPACE_PATH_MAX];
} PicoWorkspaceRegistry;

typedef enum PicoWorkspaceResult {
    PICO_WORKSPACE_OK = 0,
    PICO_WORKSPACE_INVALID,
    PICO_WORKSPACE_NOT_FOUND,
    PICO_WORKSPACE_IO_ERROR,
    PICO_WORKSPACE_NO_MEMORY,
} PicoWorkspaceResult;

/* Loads and reconciles ~/.config/pico/sessions. Direct child directories are
 * authoritative; workspaces.json is presentation metadata only. */
PicoWorkspaceResult PicoWorkspaceRegistry_Init(PicoWorkspaceRegistry *registry);
void PicoWorkspaceRegistry_Free(PicoWorkspaceRegistry *registry);
PicoWorkspaceResult PicoWorkspaceRegistry_Refresh(PicoWorkspaceRegistry *registry);
int PicoWorkspaceRegistry_Count(const PicoWorkspaceRegistry *registry);
const PicoWorkspace *PicoWorkspaceRegistry_Get(const PicoWorkspaceRegistry *registry, int index);
const PicoWorkspace *PicoWorkspaceRegistry_FindKey(const PicoWorkspaceRegistry *registry,
                                                   const char *key);
const PicoWorkspace *PicoWorkspaceRegistry_FindPath(const PicoWorkspaceRegistry *registry,
                                                    const char *path);

/* Register requires an existing directory and stores its canonical path. If
 * already registered, the existing key is returned and its metadata updated. */
PicoWorkspaceResult PicoWorkspaceRegistry_Register(PicoWorkspaceRegistry *registry,
                                                   const char *path, const char *name,
                                                   char *key_out, size_t key_cap);
PicoWorkspaceResult PicoWorkspaceRegistry_SetCollapsed(PicoWorkspaceRegistry *registry,
                                                       const char *key, bool collapsed);
PicoWorkspaceResult PicoWorkspaceRegistry_SetName(PicoWorkspaceRegistry *registry,
                                                  const char *key, const char *name);

/* Session code uses the existing encoded workspace directory key format. */
bool PicoWorkspace_EncodePath(const char *canonical_path, char *out, size_t cap);
bool PicoWorkspace_SessionDir(const PicoWorkspaceRegistry *registry, const char *key,
                              char *out, size_t cap);

#endif
