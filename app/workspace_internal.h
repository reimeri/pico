#ifndef PICO_WORKSPACE_INTERNAL_H
#define PICO_WORKSPACE_INTERNAL_H

#include "pico/host.h"
#include "pico/workspace.h"
#include "agent_internal.h"

#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>

struct PicoAgentRt;
struct PicoDelegationJob;

typedef struct PicoSessionReservation {
    PicoAgentId owner;
    char path[4096];
} PicoSessionReservation;

typedef struct PicoUiMailbox {
    char name[PICO_UI_MODAL_NAME];
    PicoAgentId agent_id;
    uint64_t generation;
    char status[PICO_UI_POST_STATUS_MAX + 1];
    char *text;
    size_t text_len;
    PicoAgentId pub_agent_id;
    uint64_t pub_generation;
    char pub_status[PICO_UI_POST_STATUS_MAX + 1];
    char *pub_text;
    bool dirty;
    bool published;
} PicoUiMailbox;

typedef struct PicoSubagentSnapshot {
    PicoAgentId child_id;
    PicoAgentId parent_id;
    char session_id[40];
    char profile[65];
    char purpose[1025];
    char model[128];
    char effort[PICO_EFFORT_LEN];
    PicoMessage *messages;
    int message_count;
} PicoSubagentSnapshot;

typedef struct PicoSubagentInspect {
    PicoAgentId live_id;
    char session_id[40];
    char profile[65];
    char purpose[1025];
    char model[128];
    char effort[PICO_EFFORT_LEN];
    char activity[256];
    PicoAgentState state;
    bool live;
    const PicoMessage *messages;
    int message_count;
} PicoSubagentInspect;

typedef struct PicoPluginSlot {
    char name[64];
    void *state;
} PicoPluginSlot;

struct PicoWorkspace {
    PicoHost *host;
    PicoWorkspaceId id;
    char path[4096];
    PicoWorkspaceState state;
    uint64_t registration_generation;

    PicoAgent *agents[PICO_MAX_AGENTS];
    int count;

    struct PicoAgentRt *retired_runtimes;
    int retired_count;

    PicoSessionReservation reservations[PICO_MAX_AGENTS + PICO_MAX_RETIRED_RUNTIMES];
    int reservation_count;

    PicoSubagentProfileInfo profiles[PICO_MAX_SUBAGENT_PROFILES];
    int profile_count;
    pthread_mutex_t delegation_mu;
    struct PicoDelegationJob *delegations;
    PicoSubagentSnapshot *snapshots;
    int snapshot_count;
    int snapshot_capacity;
    pthread_mutex_t lifecycle_mu;
    pthread_mutex_t ui_post_mu;
    PicoUiMailbox ui_mailboxes[PICO_MAX_UI_POSTS];
    int ui_mailbox_count;
    PicoWorkspaceSettings settings;
    pthread_mutex_t settings_mu;
    PicoModel *models;
    int model_count;
    PicoPluginSlot workspace_plugins[64];
    int workspace_plugin_count;
    bool accepting_work;
    bool retained_shutdown;
};

void PicoWorkspace_UiPost(PicoWorkspace *workspace, const char *name, PicoUiPostKind kind,
                          PicoAgentId agent_id, uint64_t generation, const char *text, size_t n);
void PicoWorkspace_PumpUiPosts(PicoWorkspace *workspace);
bool PicoWorkspace_UiLatest(const PicoWorkspace *workspace, const char *name, PicoUiPost *out);
void PicoWorkspace_UiClear(PicoWorkspace *workspace, const char *name);

/* False means a detached worker retained the execution host. */
bool PicoWorkspace_Quiesce(PicoWorkspace *workspace);
void PicoWorkspace_Free(PicoWorkspace *workspace);
bool PicoWorkspace_Destroy(PicoWorkspace *workspace);
void PicoWorkspace_Pump(PicoWorkspace *workspace);
bool PicoWorkspace_BlocksReload(const PicoWorkspace *workspace);
bool PicoWorkspace_AcceptsNewWork(const PicoWorkspace *workspace);
void PicoWorkspace_SetAcceptingWork(PicoWorkspace *workspace, bool accepting);
/* Drop idle runtime snapshots that may contain extension function pointers. */
void PicoWorkspace_PrepareReload(PicoWorkspace *workspace);
/* Recheck copied restricted policies after the registration set changes. */
void PicoWorkspace_RevalidateToolPolicies(PicoWorkspace *workspace);
void PicoWorkspace_NotifySessions(PicoWorkspace *workspace);
PicoAgent *PicoWorkspace_FindAgent(PicoWorkspace *workspace, PicoAgentId id);
const PicoAgent *PicoWorkspace_FindAgentConst(const PicoWorkspace *workspace, PicoAgentId id);
/* Adopt one unpublished, idle agent during an atomic workspace replacement. */
bool PicoWorkspace_AdoptInitial(PicoWorkspace *workspace, PicoAgent *agent);

bool PicoWorkspace_ReserveSession(PicoWorkspace *workspace, PicoAgentId owner,
                                  const char *path);
void PicoWorkspace_ReleaseSessions(PicoWorkspace *workspace, PicoAgentId owner);
bool PicoWorkspace_SessionReserved(const PicoWorkspace *workspace, const char *path,
                                   PicoAgentId except_owner);

void PicoWorkspace_LoadProfiles(PicoWorkspace *workspace);
void PicoWorkspace_ReplayToolDetails(PicoWorkspace *workspace);
PicoAgentResult PicoWorkspace_Resume(PicoHost *host, PicoAgentId agent_id, const char *id,
                                     bool allow_prefix);
char *PicoWorkspace_Delegate(PicoAgentContext *ctx, const char *profile,
                             const char *task, const char *session_id,
                             bool *is_error);
void PicoWorkspace_CancelDelegations(PicoWorkspace *workspace, PicoAgentId parent_id,
                                     uint64_t runtime_generation);
void PicoWorkspace_CancelChildDelegation(PicoWorkspace *workspace, PicoAgentId child_id);
bool PicoWorkspace_JobReferences(const PicoWorkspace *workspace, PicoAgentId id);

bool PicoWorkspace_InspectSubagent(PicoHost *host, const PicoTraceLine *line,
                                   PicoSubagentInspect *out);

#endif
