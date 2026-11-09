#ifndef PICO_FILE_INDEX_H
#define PICO_FILE_INDEX_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Core-owned service; no extension callbacks or host pointers on either worker.
 * Query/Info only copy bounded mailboxes. Close cancels asynchronously: workers
 * retain the service until they have reaped children and released snapshots. */
typedef struct PicoFileIndex PicoFileIndex;
#define PICO_FILE_INDEX_RESULTS 24
#define PICO_FILE_INDEX_PATH 289

typedef struct PicoFileIndexResult {
    int count;
    bool pending;
    uint64_t generation;
    char paths[PICO_FILE_INDEX_RESULTS][PICO_FILE_INDEX_PATH];
    char status[256];
} PicoFileIndexResult;

typedef struct PicoFileIndexInfo {
    uint64_t generation;
    size_t files;
    size_t bytes; /* latest snapshot arena + allocated pointer arrays, not service RSS */
    bool cached;
    bool refreshing;
    double discovery_ms;
    double search_ms;
    char status[256];
} PicoFileIndexInfo;

PicoFileIndex *PicoFileIndex_Open(const char *root);
void PicoFileIndex_Query(PicoFileIndex *index, uint64_t token, const char *query,
                         PicoFileIndexResult *out);
void PicoFileIndex_Info(PicoFileIndex *index, PicoFileIndexInfo *out);
void PicoFileIndex_Refresh(PicoFileIndex *index);
void PicoFileIndex_Close(PicoFileIndex *index);

#endif
