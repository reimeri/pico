#ifndef PICO_STREAMING_TPS_H
#define PICO_STREAMING_TPS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Private streaming estimator: bytes / 4, approximately the last 1000 tokens.
 * Callers supply monotonic seconds and serialize access with the stream lock. */
#define PICO_TPS_CHECKPOINTS 17

typedef struct PicoTpsCheckpoint {
    uint64_t bytes;
    double seconds;
} PicoTpsCheckpoint;

typedef struct PicoTpsWindow {
    PicoTpsCheckpoint points[PICO_TPS_CHECKPOINTS];
    unsigned first;
    unsigned count;
    uint64_t bytes;
    double last_output_seconds;
} PicoTpsWindow;

typedef struct PicoStreamingTps {
    PicoTpsWindow output;
    /* Request-local speculative summaries can be discarded if raw thinking
     * arrives later, without rescanning text or undoing rolling checkpoints. */
    PicoTpsWindow with_summary;
    double seconds;
    double last_tick;
    double last_publish;
    double rate;
    size_t summary_bytes;
    bool enabled;
    bool request_open;
    bool clock_started;
    bool raw_thinking;
    bool valid;
} PicoStreamingTps;

void PicoStreamingTps_Reset(PicoStreamingTps *tps, bool enabled);
void PicoStreamingTps_Begin(PicoStreamingTps *tps);
void PicoStreamingTps_Output(PicoStreamingTps *tps, size_t bytes, bool thinking, double now);
/* A cumulative snapshot; zero bytes marks a new summary step. */
void PicoStreamingTps_Summary(PicoStreamingTps *tps, size_t bytes, double now);
/* Returns whether the cached display value changed. Idle calls do no work. */
bool PicoStreamingTps_Publish(PicoStreamingTps *tps, double now);
void PicoStreamingTps_End(PicoStreamingTps *tps, double now);

#endif
