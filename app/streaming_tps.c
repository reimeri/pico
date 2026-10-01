#include "streaming_tps.h"

#include <string.h>

#define TPS_BUCKET_BYTES 256u
#define TPS_PUBLISH_SECONDS 0.25

static void AdvanceClock(PicoStreamingTps *tps, double now)
{
    if (tps->clock_started)
        tps->seconds += now - tps->last_tick;
    tps->last_tick = now;
    tps->clock_started = true;
}

static void AddBytes(PicoTpsWindow *window, size_t bytes, double seconds)
{
    uint64_t before = window->bytes;
    uint64_t after = before + bytes;
    uint64_t first = before / TPS_BUCKET_BYTES + 1;
    uint64_t last = after / TPS_BUCKET_BYTES;
    /* Even one enormous delta costs at most one ring's worth of updates. */
    if (last >= first && last - first >= PICO_TPS_CHECKPOINTS)
        first = last - PICO_TPS_CHECKPOINTS + 1;
    for (uint64_t bucket = first; bucket <= last; bucket++)
    {
        uint64_t boundary = bucket * TPS_BUCKET_BYTES;
        double fraction = (double)(boundary - before) / (double)bytes;
        double at = window->last_output_seconds +
                    fraction * (seconds - window->last_output_seconds);
        unsigned index = (window->first + window->count) % PICO_TPS_CHECKPOINTS;
        if (window->count == PICO_TPS_CHECKPOINTS)
            window->first = (window->first + 1) % PICO_TPS_CHECKPOINTS;
        else
            window->count++;
        window->points[index] = (PicoTpsCheckpoint){boundary, at};
    }
    window->bytes = after;
    window->last_output_seconds = seconds;
}

void PicoStreamingTps_Reset(PicoStreamingTps *tps, bool enabled)
{
    memset(tps, 0, sizeof(*tps));
    tps->enabled = enabled;
    tps->output.count = 1; /* Initial checkpoint is the start of generation. */
}

void PicoStreamingTps_Begin(PicoStreamingTps *tps)
{
    if (!tps->enabled) return;
    tps->with_summary = tps->output;
    tps->summary_bytes = 0;
    tps->raw_thinking = false;
    tps->clock_started = false;
    tps->request_open = true;
}

void PicoStreamingTps_Output(PicoStreamingTps *tps, size_t bytes, bool thinking, double now)
{
    if (!tps->enabled || !tps->request_open || !bytes) return;
    AdvanceClock(tps, now);
    if (thinking) tps->raw_thinking = true;
    AddBytes(&tps->output, bytes, tps->seconds);
    if (!tps->raw_thinking)
        AddBytes(&tps->with_summary, bytes, tps->seconds);
}

void PicoStreamingTps_Summary(PicoStreamingTps *tps, size_t bytes, double now)
{
    if (!tps->enabled || !tps->request_open) return;
    size_t added = bytes > tps->summary_bytes ? bytes - tps->summary_bytes : 0;
    tps->summary_bytes = bytes;
    if (tps->raw_thinking || !added) return;
    AdvanceClock(tps, now);
    AddBytes(&tps->with_summary, added, tps->seconds);
}

static bool Publish(PicoStreamingTps *tps, double now, bool force)
{
    if (!tps->enabled || !tps->request_open || !tps->clock_started) return false;
    if (!force && now - tps->last_publish < TPS_PUBLISH_SECONDS) return false;
    AdvanceClock(tps, now);
    tps->last_publish = now;
    const PicoTpsWindow *window = tps->raw_thinking ? &tps->output : &tps->with_summary;
    PicoTpsCheckpoint oldest = window->points[window->first];
    double duration = tps->seconds - oldest.seconds;
    bool valid = window->bytes > oldest.bytes && duration > 0.0;
    double rate = valid ? (double)(window->bytes - oldest.bytes) / (4.0 * duration) : 0.0;
    bool changed = valid != tps->valid || rate != tps->rate;
    tps->valid = valid;
    tps->rate = rate;
    return changed;
}

bool PicoStreamingTps_Publish(PicoStreamingTps *tps, double now)
{
    return Publish(tps, now, false);
}

void PicoStreamingTps_End(PicoStreamingTps *tps, double now)
{
    if (!tps->enabled || !tps->request_open) return;
    Publish(tps, now, true);
    if (!tps->raw_thinking) tps->output = tps->with_summary;
    tps->request_open = false;
    tps->clock_started = false;
}
