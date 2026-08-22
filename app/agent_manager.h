#ifndef PICO_AGENT_MANAGER_H
#define PICO_AGENT_MANAGER_H

#include "agent_internal.h"

struct PicoAgentRt;

typedef struct PicoSessionReservation {
    PicoAgentId owner;
    char path[4096];
} PicoSessionReservation;

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
    bool curl_initialized;
    bool retained_shutdown;
};

PicoAgentManager *PicoAgentManager_Create(PicoApp *app);
/* False means a detached worker retained the execution host. */
bool PicoAgentManager_Destroy(PicoAgentManager *manager);
void PicoAgentManager_Pump(PicoAgentManager *manager);
bool PicoAgentManager_BlocksReload(const PicoAgentManager *manager);
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
PicoAgentResult PicoAgentManager_ResumeActive(PicoApp *app, const char *id, bool allow_prefix);

#endif
