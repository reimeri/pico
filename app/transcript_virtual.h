#ifndef PICO_TRANSCRIPT_VIRTUAL_H
#define PICO_TRANSCRIPT_VIRTUAL_H

#include <stdbool.h>
#include <stdint.h>

typedef struct PicoTranscriptVirtual {
    float *heights;
    double *height_tree; /* Fenwick index of effective heights (including estimates). */
    uint64_t *revisions;
    unsigned char *dirty;
    uint32_t *mount_stamps;
    int *mount_indices;
    int mount_count;
    uint32_t mount_epoch;
    bool mount_sorted;
    int *dirty_next;
    int *dirty_prev;
    int dirty_head, dirty_tail;
    int *timed_indices, *timed_positions;
    int timed_count;
    int count;
    int capacity;
    int dirty_count;
    uint64_t seen_change_seq;
    uint64_t identity;
    float width;
    float font_scale;
    bool configured;
    bool measure_all;
    float last_scroll_y;
    bool scroll_position_known;
} PicoTranscriptVirtual;

void PicoTranscriptVirtual_Free(PicoTranscriptVirtual *cache);
void PicoTranscriptVirtual_Begin(PicoTranscriptVirtual *cache, uint64_t identity,
                                 int count, float width, float font_scale);
void PicoTranscriptVirtual_SetRevision(PicoTranscriptVirtual *cache, int index,
                                       uint64_t revision);
void PicoTranscriptVirtual_Plan(PicoTranscriptVirtual *cache, float scroll_top,
                                float viewport_height, float overscan,
                                int force_index, float message_gap);
void PicoTranscriptVirtual_ForceMount(PicoTranscriptVirtual *cache, int index);
bool PicoTranscriptVirtual_Mounted(const PicoTranscriptVirtual *cache, int index);
/* Sorted mounted indices for spacer emission and height harvesting. */
int PicoTranscriptVirtual_MountedCount(const PicoTranscriptVirtual *cache);
int PicoTranscriptVirtual_MountedIndex(PicoTranscriptVirtual *cache, int position);
/* Rows with a currently dwelling tool need one last check when dwell ends. */
void PicoTranscriptVirtual_WatchTimed(PicoTranscriptVirtual *cache, int index, bool watching);
int PicoTranscriptVirtual_TimedCount(const PicoTranscriptVirtual *cache);
int PicoTranscriptVirtual_TimedIndex(const PicoTranscriptVirtual *cache, int position);
/* Measured row height, or the same estimate used by unmeasured spacers. */
float PicoTranscriptVirtual_ItemHeight(const PicoTranscriptVirtual *cache, int index);
float PicoTranscriptVirtual_SpanHeight(const PicoTranscriptVirtual *cache,
                                       int begin, int end, float message_gap);
float PicoTranscriptVirtual_AnchorDelta(const PicoTranscriptVirtual *cache,
                                        int index, float new_height,
                                        float scroll_top, float message_gap);
void PicoTranscriptVirtual_RecordHeight(PicoTranscriptVirtual *cache, int index,
                                        float height);
void PicoTranscriptVirtual_FinishMeasure(PicoTranscriptVirtual *cache);

#endif
