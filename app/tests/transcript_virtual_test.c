#include "transcript_virtual.h"

#include <math.h>
#include <stdio.h>

static int Fail(const char *message)
{
    fprintf(stderr, "transcript virtual: %s\n", message);
    return 1;
}

static int Near(float actual, float expected)
{
    return fabsf(actual - expected) <= 0.01f;
}

int main(void)
{
    PicoTranscriptVirtual cache = {0};
    const float gap = 8.0f;

    PicoTranscriptVirtual_Begin(&cache, 11, 3, 600.0f, 1.0f);
    for (int i = 0; i < 3; i++)
    {
        PicoTranscriptVirtual_SetRevision(&cache, i, (uint64_t)(100 + i));
    }
    PicoTranscriptVirtual_Plan(&cache, 0.0f, 80.0f, 0.0f, -1, gap);
    if (!PicoTranscriptVirtual_Mounted(&cache, 0) ||
        !PicoTranscriptVirtual_Mounted(&cache, 1) ||
        !PicoTranscriptVirtual_Mounted(&cache, 2))
    {
        PicoTranscriptVirtual_Free(&cache);
        return Fail("initial exact-measure pass did not mount every message");
    }
    PicoTranscriptVirtual_RecordHeight(&cache, 0, 50.0f);
    PicoTranscriptVirtual_RecordHeight(&cache, 1, 100.0f);
    PicoTranscriptVirtual_RecordHeight(&cache, 2, 75.0f);
    PicoTranscriptVirtual_FinishMeasure(&cache);
    if (cache.measure_all)
    {
        PicoTranscriptVirtual_Free(&cache);
        return Fail("exact-measure pass remained active after every height was recorded");
    }
    float anchor_delta = PicoTranscriptVirtual_AnchorDelta(&cache, 0, 70.0f,
                                                           180.0f, gap);
    if (!Near(anchor_delta, 20.0f))
    {
        PicoTranscriptVirtual_Free(&cache);
        return Fail("growth above the viewport did not produce an anchor correction");
    }
    if (!Near(PicoTranscriptVirtual_AnchorDelta(&cache, 2, 95.0f,
                                                180.0f, gap), 0.0f))
    {
        PicoTranscriptVirtual_Free(&cache);
        return Fail("visible row height change incorrectly moved the viewport anchor");
    }
    PicoTranscriptVirtual_RecordHeight(&cache, 0, 70.0f);
    anchor_delta += PicoTranscriptVirtual_AnchorDelta(&cache, 1, 80.0f,
                                                      200.0f, gap);
    if (!Near(anchor_delta, 0.0f))
    {
        PicoTranscriptVirtual_Free(&cache);
        return Fail("growth and shrinkage above the viewport did not preserve the anchor");
    }
    PicoTranscriptVirtual_RecordHeight(&cache, 0, 50.0f);

    PicoTranscriptVirtual_Begin(&cache, 11, 3, 600.0f, 1.0f);
    for (int i = 0; i < 3; i++)
    {
        PicoTranscriptVirtual_SetRevision(&cache, i, (uint64_t)(100 + i));
    }
    PicoTranscriptVirtual_Plan(&cache, 60.0f, 50.0f, 0.0f, -1, gap);
    if (PicoTranscriptVirtual_Mounted(&cache, 0) ||
        !PicoTranscriptVirtual_Mounted(&cache, 1) ||
        PicoTranscriptVirtual_Mounted(&cache, 2))
    {
        PicoTranscriptVirtual_Free(&cache);
        return Fail("visible planning did not keep only the intersecting message");
    }
    if (!Near(PicoTranscriptVirtual_SpanHeight(&cache, 0, 1, gap), 58.0f) ||
        !Near(PicoTranscriptVirtual_SpanHeight(&cache, 2, 3, gap), 75.0f) ||
        !Near(PicoTranscriptVirtual_SpanHeight(&cache, 0, 3, gap), 241.0f))
    {
        PicoTranscriptVirtual_Free(&cache);
        return Fail("spacer heights did not preserve message heights and inter-message gaps");
    }

    PicoTranscriptVirtual_Begin(&cache, 11, 3, 600.0f, 1.0f);
    PicoTranscriptVirtual_SetRevision(&cache, 0, 999);
    PicoTranscriptVirtual_SetRevision(&cache, 1, 101);
    PicoTranscriptVirtual_SetRevision(&cache, 2, 102);
    PicoTranscriptVirtual_Plan(&cache, 170.0f, 20.0f, 0.0f, -1, gap);
    if (!PicoTranscriptVirtual_Mounted(&cache, 0) ||
        PicoTranscriptVirtual_Mounted(&cache, 1) ||
        !PicoTranscriptVirtual_Mounted(&cache, 2))
    {
        PicoTranscriptVirtual_Free(&cache);
        return Fail("dirty offscreen message was not mounted alongside the visible message");
    }

    PicoTranscriptVirtual_RecordHeight(&cache, 0, 60.0f);
    PicoTranscriptVirtual_Begin(&cache, 11, 3, 600.0f, 1.0f);
    PicoTranscriptVirtual_SetRevision(&cache, 0, 999);
    PicoTranscriptVirtual_SetRevision(&cache, 1, 101);
    PicoTranscriptVirtual_SetRevision(&cache, 2, 102);
    PicoTranscriptVirtual_Plan(&cache, 170.0f, 20.0f, 0.0f, 1, gap);
    if (!PicoTranscriptVirtual_Mounted(&cache, 1))
    {
        PicoTranscriptVirtual_Free(&cache);
        return Fail("forced selection message was not mounted offscreen");
    }

    PicoTranscriptVirtual_Begin(&cache, 11, 3, 500.0f, 1.0f);
    for (int i = 0; i < 3; i++)
    {
        PicoTranscriptVirtual_SetRevision(&cache, i, (uint64_t)(200 + i));
    }
    PicoTranscriptVirtual_Plan(&cache, 170.0f, 20.0f, 0.0f, -1, gap);
    if (!cache.measure_all || !PicoTranscriptVirtual_Mounted(&cache, 0) ||
        !PicoTranscriptVirtual_Mounted(&cache, 1) ||
        !PicoTranscriptVirtual_Mounted(&cache, 2))
    {
        PicoTranscriptVirtual_Free(&cache);
        return Fail("width change did not request a new exact-measure pass");
    }

    PicoTranscriptVirtual_Free(&cache);
    return 0;
}
