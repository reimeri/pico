#ifndef PICO_SUBAGENT_CONFIG_H
#define PICO_SUBAGENT_CONFIG_H

#include "agent_internal.h"

void PicoSubagentConfig_Load(PicoAgentManager *manager);
const PicoSubagentProfileInfo *PicoSubagentConfig_Find(const PicoAgentManager *manager,
                                                       const char *name);
bool PicoSubagentConfig_Resolve(const PicoApp *app, const PicoAgent *parent,
                                const PicoSubagentProfileInfo *profile,
                                char *model, size_t model_cap,
                                char *effort, size_t effort_cap);

#endif
