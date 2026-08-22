#ifndef PICO_AGENT_MANAGER_H
#define PICO_AGENT_MANAGER_H

#include "agent_internal.h"

#include <pthread.h>

struct PicoAgentRt;
struct PicoDelegationJob;

typedef struct PicoAskUiEntry {
    uint64_t ask_id;
    PicoAgentId owner_id;
    uint64_t runtime_generation;
    PicoAgentId root_id;
} PicoAskUiEntry;

typedef struct PicoSessionReservation {
    PicoAgentId owner;
    char path[4096];
} PicoSessionReservation;

struct PicoAgentManager {
    PicoApp *app; /* main-thread owner; never exposed to workers */
    PicoAgent *agents[PICO_MAX_AGENTS];
    int count;
    PicoAgentId active_id;
    uint64_t selected_seq;

    struct PicoAgentRt *retired_runtimes;
    int retired_count;

    PicoSessionReservation reservations[PICO_MAX_AGENTS + PICO_MAX_RETIRED_RUNTIMES];
    int reservation_count;
    PicoAskUiEntry asks[PICO_MAX_AGENTS];
    int ask_count;

    PicoSubagentProfileInfo profiles[PICO_MAX_SUBAGENT_PROFILES];
    int profile_count;
    pthread_mutex_t delegation_mu;
    struct PicoDelegationJob *delegations;
    pthread_mutex_t lifecycle_mu;
    bool accepting_work;
    bool curl_initialized;
    bool retained_shutdown;
};

PicoAgentManager *PicoAgentManager_Create(PicoApp *app);
/* False means a detached worker retained the execution host. */
bool PicoAgentManager_Destroy(PicoAgentManager *manager);
void PicoAgentManager_Pump(PicoAgentManager *manager);
bool PicoAgentManager_BlocksReload(const PicoAgentManager *manager);
bool PicoAgentManager_AcceptsNewWork(const PicoAgentManager *manager);
void PicoAgentManager_SetAcceptingWork(PicoAgentManager *manager, bool accepting);
/* Drop idle runtime snapshots that may contain extension function pointers. */
void PicoAgentManager_PrepareReload(PicoAgentManager *manager);
/* Recheck copied restricted policies after the registration set changes. */
void PicoAgentManager_RevalidateToolPolicies(PicoAgentManager *manager);
void PicoAgentManager_NotifySessions(PicoAgentManager *manager);
PicoAgent *PicoAgentManager_Active(PicoAgentManager *manager);
const PicoAgent *PicoAgentManager_ActiveConst(const PicoAgentManager *manager);
PicoAgent *PicoAgentManager_Find(PicoAgentManager *manager, PicoAgentId id);
const PicoAgent *PicoAgentManager_FindConst(const PicoAgentManager *manager, PicoAgentId id);

bool PicoAgentManager_ReserveSession(PicoAgentManager *manager, PicoAgentId owner,
                                     const char *path);
void PicoAgentManager_ReleaseSessions(PicoAgentManager *manager, PicoAgentId owner);
bool PicoAgentManager_SessionReserved(const PicoAgentManager *manager, const char *path,
                                      PicoAgentId except_owner);

void PicoAgentManager_LoadProfiles(PicoAgentManager *manager);
void PicoAgentManager_ReplayToolDetails(PicoAgentManager *manager);
PicoAgent *PicoAgentManager_MostRecentInWorkspace(PicoAgentManager *manager, const char *workspace_key);
PicoAgent *PicoAgentManager_FindSession(PicoAgentManager *manager, const char *path);
PicoAgentResult PicoAgentManager_OpenSession(PicoApp *app, PicoAgent *workspace_agent,
                                             const char *id, bool allow_prefix, bool select);
PicoAgentResult PicoAgentManager_ResumeActive(PicoApp *app, const char *id, bool allow_prefix);
PicoAgentId PicoAgent_RootId(const PicoAgentManager *manager, PicoAgentId id);
bool PicoAgent_InTree(const PicoAgentManager *manager, PicoAgentId root_id, PicoAgentId id);
bool PicoAgentManager_TreeHasAsk(const PicoAgentManager *manager, PicoAgentId root_id);
bool PicoAgentManager_TreeHasError(const PicoAgentManager *manager, PicoAgentId root_id);
bool PicoAgentManager_TreeBusy(const PicoAgentManager *manager, PicoAgentId root_id);
void PicoAskStore_Sync(PicoApp *app);
void PicoAskStore_RemoveAgent(PicoApp *app, PicoAgentId id);
void PicoAskStore_RemoveGeneration(PicoApp *app, PicoAgentId id, uint64_t generation);
bool PicoAskStore_Get(const PicoApp *app, uint64_t ask_id, PicoAgentId *owner_out,
                      uint64_t *generation_out, PicoAgentId *root_out);
char *PicoAgentManager_Delegate(PicoAgentContext *ctx, const char *profile,
                                const char *task, const char *session_id,
                                bool *is_error);
void PicoAgentManager_CancelDelegations(PicoAgentManager *manager, PicoAgentId parent_id,
                                        uint64_t runtime_generation);
void PicoAgentManager_CancelChildDelegation(PicoAgentManager *manager, PicoAgentId child_id);
bool PicoAgentManager_JobReferences(const PicoAgentManager *manager, PicoAgentId id);

#endif
