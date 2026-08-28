#ifndef PICO_SUBAGENT_CONFIG_H
#define PICO_SUBAGENT_CONFIG_H

#include "agent_internal.h"

void PicoSubagentConfig_Load(PicoWorkspace *workspace);
const PicoSubagentProfileInfo *PicoSubagentConfig_Find(const PicoWorkspace *workspace,
                                                       const char *name);
bool PicoSubagentConfig_Resolve(const PicoHost *app, const PicoAgent *parent,
                                const PicoSubagentProfileInfo *profile,
                                char *model, size_t model_cap,
                                char *effort, size_t effort_cap);

#endif
