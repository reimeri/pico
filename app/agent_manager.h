#ifndef PICO_AGENT_MANAGER_H
#define PICO_AGENT_MANAGER_H

#include "agent_internal.h"

#include <pthread.h>

struct PicoAgentRt;
struct PicoDelegationJob;

typedef struct PicoSessionReservation {
    PicoAgentId owner;
    char path[4096];
} PicoSessionReservation;

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

struct PicoAgentManager {
    PicoApp *app; /* main-thread owner; never exposed to workers */
    PicoAgent *agents[PICO_MAX_AGENTS];
    int count;
    PicoAgentId active_id;

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
/* Adopt one unpublished, idle agent during an atomic workspace replacement. */
bool PicoAgentManager_AdoptInitial(PicoAgentManager *manager, PicoAgent *agent);

bool PicoAgentManager_ReserveSession(PicoAgentManager *manager, PicoAgentId owner,
                                     const char *path);
void PicoAgentManager_ReleaseSessions(PicoAgentManager *manager, PicoAgentId owner);
bool PicoAgentManager_SessionReserved(const PicoAgentManager *manager, const char *path,
                                      PicoAgentId except_owner);

void PicoAgentManager_LoadProfiles(PicoAgentManager *manager);
void PicoAgentManager_ReplayToolDetails(PicoAgentManager *manager);
PicoAgentResult PicoAgentManager_ResumeActive(PicoApp *app, const char *id, bool allow_prefix);
char *PicoAgentManager_Delegate(PicoAgentContext *ctx, const char *profile,
                                const char *task, const char *session_id,
                                bool *is_error);
void PicoAgentManager_CancelDelegations(PicoAgentManager *manager, PicoAgentId parent_id,
                                        uint64_t runtime_generation);
void PicoAgentManager_CancelChildDelegation(PicoAgentManager *manager, PicoAgentId child_id);
bool PicoAgentManager_JobReferences(const PicoAgentManager *manager, PicoAgentId id);

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

bool PicoAgentManager_InspectSubagent(PicoApp *app, const PicoTraceLine *line,
                                      PicoSubagentInspect *out);

#endif
