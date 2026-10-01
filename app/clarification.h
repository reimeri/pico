#ifndef PICO_CLARIFICATION_H
#define PICO_CLARIFICATION_H

#include "pico/app.h"
#include "pico/agent.h"

/* Internal ask-owned conversations. No delegation publication or persistence. */
typedef struct PicoClarification {
    PicoAgentId owner_id;
    uint64_t owner_generation;
    uint64_t ask_id;
    char *seed;
    char *question_id;
    char *question;
    PicoModel model;
    PicoComposer draft;
    bool closing;
} PicoClarification;

const char *PicoClarification_Instructions(void);
PicoResult PicoClarification_Open(PicoHost *host, const PicoToolAsk *ask, const char *question_id);
void PicoClarification_Back(PicoHost *host);
PicoAgent *PicoClarification_View(const PicoHost *host);
PicoAgent *PicoHost_TranscriptAgent(PicoHost *host);
const PicoAgent *PicoHost_TranscriptAgentConst(const PicoHost *host);
void PicoClarification_Submit(PicoHost *host);
void PicoClarification_Stop(PicoHost *host);
void PicoClarification_Pump(PicoWorkspace *workspace);
void PicoClarification_CancelOwner(PicoWorkspace *workspace, PicoAgentId owner_id);
void PicoClarification_Destroy(PicoClarification *clarification);
bool PicoAgent_IsUserMain(const PicoAgent *agent);

#endif
