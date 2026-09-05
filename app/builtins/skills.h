#ifndef PICO_BUILTINS_SKILLS_H
#define PICO_BUILTINS_SKILLS_H

/* Builtins-internal view of the skills builtin (app/builtins/skills.c), e.g.
 * for the landing page's Skills card and /skill completion. */

#include "pico/plugin.h"

typedef struct PicoSkillInfo {
    const char *name;        /* borrowed; main thread only, valid until the next rescan */
    const char *description; /* borrowed; same lifetime */
} PicoSkillInfo;

/* Fills out with the workspace's loaded skills (sorted by name) and returns
 * their number; 0 when the skills builtin is absent. */
int PicoSkills_List(PicoWorkspace *workspace, PicoSkillInfo *out, int max);

#endif
