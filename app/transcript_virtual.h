#ifndef PICO_TRANSCRIPT_VIRTUAL_H
#define PICO_TRANSCRIPT_VIRTUAL_H

#include <stdbool.h>
#include <stdint.h>

typedef struct PicoTranscriptVirtual {
    float *heights;
    uint64_t *revisions;
    unsigned char *dirty;
    unsigned char *mounted;
    int count;
    int capacity;
    uint64_t identity;
    float width;
    float font_scale;
    bool configured;
    bool measure_all;
} PicoTranscriptVirtual;

void PicoTranscriptVirtual_Free(PicoTranscriptVirtual *cache);
void PicoTranscriptVirtual_Begin(PicoTranscriptVirtual *cache, uint64_t identity,
                                 int count, float width, float font_scale);
void PicoTranscriptVirtual_SetRevision(PicoTranscriptVirtual *cache, int index,
                                       uint64_t revision);
void PicoTranscriptVirtual_Plan(PicoTranscriptVirtual *cache, float scroll_top,
                                float viewport_height, float overscan,
                                int force_index, float message_gap);
bool PicoTranscriptVirtual_Mounted(const PicoTranscriptVirtual *cache, int index);
float PicoTranscriptVirtual_SpanHeight(const PicoTranscriptVirtual *cache,
                                       int begin, int end, float message_gap);
float PicoTranscriptVirtual_AnchorDelta(const PicoTranscriptVirtual *cache,
                                        int index, float new_height,
                                        float scroll_top, float message_gap);
void PicoTranscriptVirtual_RecordHeight(PicoTranscriptVirtual *cache, int index,
                                        float height);
void PicoTranscriptVirtual_FinishMeasure(PicoTranscriptVirtual *cache);

#endif
