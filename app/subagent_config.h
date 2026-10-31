#ifndef PICO_SUBAGENT_CONFIG_H
#define PICO_SUBAGENT_CONFIG_H

#include "agent_internal.h"

/* Private profile-manager documents own their original bytes for conflict checks. */
typedef struct PicoSubagentDocument {
    char filename[256];
    char path[4096];
    char *source;
    size_t source_len;
    PicoSubagentProfileInfo profile;
    bool valid; /* JSON/schema validity, independent of workspace availability */
    bool parseable;
    char error[512];
} PicoSubagentDocument;

bool PicoSubagentConfig_Directory(char *out, size_t cap);
bool PicoSubagentConfig_List(PicoSubagentDocument **out,
                              int *count, char *error, size_t error_cap);
void PicoSubagentConfig_FreeDocuments(PicoSubagentDocument *documents, int count);
/* Global definition editing: validate schema only. Workspace loading separately
 * checks model, effort and registered-tool compatibility. */
char *PicoSubagentConfig_Build(const PicoSubagentDocument *original,
                              const PicoSubagentProfileInfo *profile, char *error, size_t error_cap);
/* Worker-safe file operations. No-replace creation and best-effort stale-draft
 * detection; external editors do not participate in a shared filesystem lock. */
bool PicoSubagentConfig_Save(const PicoSubagentDocument *original, const char *name,
                             const char *source, char *error, size_t error_cap);
bool PicoSubagentConfig_Delete(const PicoSubagentDocument *original, char *error, size_t error_cap);
/* Main-thread, deferred profile-only refresh; does not stop accepting work. */
void PicoSubagentConfig_RequestRefresh(PicoHost *host);
void PicoSubagentConfig_PumpRefresh(PicoWorkspace *workspace);

void PicoSubagentConfig_Load(PicoWorkspace *workspace);
const PicoSubagentProfileInfo *PicoSubagentConfig_Find(const PicoWorkspace *workspace,
                                                       const char *name);
bool PicoSubagentConfig_Resolve(const PicoHost *app, const PicoAgent *parent,
                                const PicoSubagentProfileInfo *profile,
                                char *model, size_t model_cap,
                                char *effort, size_t effort_cap);

#endif
