#ifndef PICO_BUILTIN_BACKGROUND_MODEL_H
#define PICO_BUILTIN_BACKGROUND_MODEL_H

#include "pico/agent.h"

#include <stdbool.h>
#include <stddef.h>

#define PICO_BG_LOG_MAX (64 * 1024)
#define PICO_BG_MAX_RUNNING 8
#define PICO_BG_MAX_RECORDS 32
#define PICO_BG_ID_MAX 16
#define PICO_BG_DESC_MAX 256
#define PICO_BG_CMD_MAX (8 * 1024)

typedef enum PicoBgStatus {
    PICO_BG_RUNNING = 0,
    PICO_BG_EXITED,
    PICO_BG_KILLED,
} PicoBgStatus;

typedef struct PicoBgJobInfo {
    char id[PICO_BG_ID_MAX];
    char description[PICO_BG_DESC_MAX + 1];
    PicoBgStatus status;
    int exit_code;
    int term_signal;
} PicoBgJobInfo;

typedef struct PicoBgTable PicoBgTable;

PicoBgTable *PicoBgTable_Create(void);
void PicoBgTable_Destroy(PicoBgTable *table);
void PicoBgTable_Pump(PicoBgTable *table);

/* malloc'd JSON on success; NULL on failure with malloc'd *error. */
char *PicoBgTable_Spawn(PicoBgTable *table, PicoAgentId agent_id, const char *workspace,
                        const char *description, const char *command, char **error);
char *PicoBgTable_Kill(PicoBgTable *table, PicoAgentId agent_id, const char *id, char **error);
char *PicoBgTable_ListJson(PicoBgTable *table, PicoAgentId agent_id);
char *PicoBgTable_Log(PicoBgTable *table, PicoAgentId agent_id, const char *id, char **error);

void PicoBgTable_ResetAgent(PicoBgTable *table, PicoAgentId agent_id);
int PicoBgTable_RunningCount(PicoBgTable *table, PicoAgentId agent_id);
int PicoBgTable_CopyJobs(PicoBgTable *table, PicoAgentId agent_id, PicoBgJobInfo *out, int cap);
bool PicoBgTable_CopyLog(PicoBgTable *table, PicoAgentId agent_id, const char *id, char *out,
                         size_t cap, size_t *len_out);
const char *PicoBgStatus_Name(PicoBgStatus status);

#endif
