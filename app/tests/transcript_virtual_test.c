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

/* These convergence cases explicitly opt into background measurement. */
static void PlanAndRefine(PicoTranscriptVirtual *cache, float top, float viewport,
                          float overscan, int force, float gap)
{
    PicoTranscriptVirtual_Plan(cache, top, viewport, overscan, force, gap);
    int allowance = 32;
    static uint64_t frame;
    PicoTranscriptVirtual_Refine(cache, ++frame, &allowance);
}

static int TestForegroundAndRefinement(void)
{
    PicoTranscriptVirtual cache = {0};
    const int count = 128;
    PicoTranscriptVirtual_Begin(&cache, 1, count, 600, 1);
    float tail = PicoTranscriptVirtual_SpanHeight(&cache, 0, count - 1, 8);
    PicoTranscriptVirtual_Plan(&cache, tail, 40, 0, count / 2, 8);
    if (PicoTranscriptVirtual_Mounted(&cache, 0) ||
        !PicoTranscriptVirtual_Mounted(&cache, count - 1) ||
        !PicoTranscriptVirtual_Mounted(&cache, count / 2))
        goto failed;
    int allowance = 1;
    PicoTranscriptVirtual_Refine(&cache, 1, &allowance);
    if (!PicoTranscriptVirtual_Mounted(&cache, 0)) goto failed;
    PicoTranscriptVirtual_RecordHeight(&cache, 0, 120);
    /* A correction pass spends the same allowance, not another batch. */
    PicoTranscriptVirtual_Begin(&cache, 1, count, 600, 1);
    tail = PicoTranscriptVirtual_SpanHeight(&cache, 0, count - 1, 8);
    PicoTranscriptVirtual_Plan(&cache, tail, 40, 0, count / 2, 8);
    PicoTranscriptVirtual_Refine(&cache, 1, &allowance);
    if (PicoTranscriptVirtual_Mounted(&cache, 1) ||
        !PicoTranscriptVirtual_Mounted(&cache, count / 2)) goto failed;
    /* A failed measurement must remain eligible on a later frame. */
    allowance = 1;
    PicoTranscriptVirtual_Refine(&cache, 1, &allowance);
    if (!PicoTranscriptVirtual_Mounted(&cache, 1)) goto failed;
    PicoTranscriptVirtual_RecordHeight(&cache, 1, 0);
    PicoTranscriptVirtual_Begin(&cache, 1, count, 600, 1);
    allowance = 1;
    PicoTranscriptVirtual_Refine(&cache, 1, &allowance);
    if (!PicoTranscriptVirtual_Mounted(&cache, 1)) goto failed;
    /* A revision changed after mounting cannot be cleaned by old geometry. */
    PicoTranscriptVirtual_Begin(&cache, 2, count, 600, 1);
    PicoTranscriptVirtual_SetRevision(&cache, count - 1, 1);
    PicoTranscriptVirtual_Plan(&cache, tail, 40, 0, count - 1, 8);
    float old_height = PicoTranscriptVirtual_ItemHeight(&cache, count - 1);
    PicoTranscriptVirtual_SetRevision(&cache, count - 1, 2);
    PicoTranscriptVirtual_RecordHeight(&cache, count - 1, old_height + 100);
    if (PicoTranscriptVirtual_ViewportReady(&cache) ||
        !Near(PicoTranscriptVirtual_ItemHeight(&cache, count - 1), old_height)) goto failed;
    PicoTranscriptVirtual_Begin(&cache, 2, count, 600, 1);
    PicoTranscriptVirtual_Plan(&cache, tail, 40, 0, count - 1, 8);
    for (int i = 0; i < count; i++)
        if (PicoTranscriptVirtual_Mounted(&cache, i)) PicoTranscriptVirtual_RecordHeight(&cache, i, old_height);
    if (!PicoTranscriptVirtual_ViewportReady(&cache) || !cache.measure_all) goto failed;
    PicoTranscriptVirtual_Free(&cache);
    return 0;
failed:
    PicoTranscriptVirtual_Free(&cache);
    return Fail("foreground planning, shared admission and failed-height retry must stay independent");
}

int main(void)
{
    if (TestForegroundAndRefinement()) return 1;
    PicoTranscriptVirtual cache = {0};
    /* An empty initial layout has no allocated tree. */
    PicoTranscriptVirtual_Begin(&cache, 1, 0, 600.0f, 1.0f);
    PicoTranscriptVirtual_FinishMeasure(&cache);
    const float gap = 8.0f;

    PicoTranscriptVirtual_Begin(&cache, 11, 3, 600.0f, 1.0f);
    for (int i = 0; i < 3; i++)
    {
        PicoTranscriptVirtual_SetRevision(&cache, i, (uint64_t)(100 + i));
    }
    PlanAndRefine(&cache, 0.0f, 80.0f, 0.0f, -1, gap);
    if (!PicoTranscriptVirtual_Mounted(&cache, 0) ||
        !PicoTranscriptVirtual_Mounted(&cache, 1) ||
        !PicoTranscriptVirtual_Mounted(&cache, 2))
    {
        PicoTranscriptVirtual_Free(&cache);
        return Fail("small cold transcript did not render every message");
    }
    PicoTranscriptVirtual_RecordHeight(&cache, 0, 50.0f);
    PicoTranscriptVirtual_RecordHeight(&cache, 1, 100.0f);
    PicoTranscriptVirtual_RecordHeight(&cache, 2, 75.0f);
    PicoTranscriptVirtual_FinishMeasure(&cache);
    if (cache.measure_all)
    {
        PicoTranscriptVirtual_Free(&cache);
        return Fail("cold transcript did not settle after heights were recorded");
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
    PlanAndRefine(&cache, 60.0f, 50.0f, 0.0f, -1, gap);
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
    PlanAndRefine(&cache, 170.0f, 20.0f, 0.0f, -1, gap);
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
    PlanAndRefine(&cache, 170.0f, 20.0f, 0.0f, 1, gap);
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
    PlanAndRefine(&cache, 170.0f, 20.0f, 0.0f, -1, gap);
    if (!cache.measure_all || !PicoTranscriptVirtual_Mounted(&cache, 0) ||
        !PicoTranscriptVirtual_Mounted(&cache, 1) ||
        !PicoTranscriptVirtual_Mounted(&cache, 2))
    {
        PicoTranscriptVirtual_Free(&cache);
        return Fail("width change did not invalidate measured heights");
    }

    /* Indexed prefix heights must follow edits far above the viewport,
     * without moving an on-screen row's anchor. */
    PicoTranscriptVirtual_Begin(&cache, 23, 10000, 600.0f, 1.0f);
    for (int i = 0; i < 10000; i++)
        PicoTranscriptVirtual_RecordHeight(&cache, i, 50.0f);
    if (!Near(PicoTranscriptVirtual_AnchorDelta(&cache, 0, 70.0f, 500000.0f, gap), 20.0f))
    {
        PicoTranscriptVirtual_Free(&cache);
        return Fail("distant height edit did not correct the viewport anchor");
    }
    PicoTranscriptVirtual_RecordHeight(&cache, 0, 70.0f);
    if (!Near(PicoTranscriptVirtual_SpanHeight(&cache, 0, 2, gap), 136.0f))
    {
        PicoTranscriptVirtual_Free(&cache);
        return Fail("indexed spacer height did not follow a distant edit");
    }
    /* A large cold transcript must never mount its whole history in a frame,
     * even before Clay has reported usable scroll viewport dimensions. */
    const int rows = 480;
    PicoTranscriptVirtual_Begin(&cache, 21, rows, 600.0f, 1.0f);
    for (int i = 0; i < rows; i++)
    {
        PicoTranscriptVirtual_SetRevision(&cache, i, (uint64_t)(i + 1));
    }
    int frames = 0;
    while (cache.measure_all && frames++ < rows)
    {
        if (frames > 1)
        {
            PicoTranscriptVirtual_Begin(&cache, 21, rows, 600.0f, 1.0f);
        }
        PlanAndRefine(&cache, 0.0f, frames == 1 ? 0.0f : 80.0f,
                                   0.0f, rows - 1, gap);
        int mounted = 0;
        for (int i = 0; i < rows; i++)
        {
            if (PicoTranscriptVirtual_Mounted(&cache, i))
            {
                mounted++;
                PicoTranscriptVirtual_RecordHeight(&cache, i, 50.0f);
            }
        }
        if (mounted > rows / 4 || !PicoTranscriptVirtual_Mounted(&cache, rows - 1))
        {
            PicoTranscriptVirtual_Free(&cache);
            return Fail("cold measurement was unbounded or lost the forced row");
        }
        PicoTranscriptVirtual_FinishMeasure(&cache);
    }
    if (cache.measure_all || frames < 2)
    {
        PicoTranscriptVirtual_Free(&cache);
        return Fail("cold measurement failed to converge across frames");
    }
    PicoTranscriptVirtual_Begin(&cache, 22, rows, 500.0f, 1.0f);
    for (int i = 0; i < rows; i++)
    {
        PicoTranscriptVirtual_SetRevision(&cache, i, (uint64_t)(i + 1));
    }
    PlanAndRefine(&cache, 0.0f, 80.0f, 0.0f, -1, gap);
    float estimated = PicoTranscriptVirtual_SpanHeight(&cache, 0, 1, gap) - gap;
    if (!cache.measure_all || PicoTranscriptVirtual_Mounted(&cache, rows / 2) ||
        !Near(PicoTranscriptVirtual_AnchorDelta(&cache, 0, estimated + 40.0f,
                                                500.0f, gap), 40.0f))
    {
        PicoTranscriptVirtual_Free(&cache);
        return Fail("reset reused stale heights or lost estimated anchor correction");
    }

    /* Sparse offscreen invalidations and a forced search/selection row must
     * retain transcript order and full intervening spacer heights. */
    PicoTranscriptVirtual_Begin(&cache, 44, 256, 600.0f, 1.0f);
    for (int i = 0; i < 256; i++)
        PicoTranscriptVirtual_RecordHeight(&cache, i, 50.0f);
    PicoTranscriptVirtual_Begin(&cache, 44, 256, 600.0f, 1.0f);
    PicoTranscriptVirtual_SetRevision(&cache, 240, 1);
    PicoTranscriptVirtual_SetRevision(&cache, 120, 1);
    PlanAndRefine(&cache, 0.0f, 40.0f, 0.0f, 199, gap);
    if (PicoTranscriptVirtual_MountedCount(&cache) != 4 ||
        PicoTranscriptVirtual_MountedIndex(&cache, 0) != 0 ||
        PicoTranscriptVirtual_MountedIndex(&cache, 1) != 120 ||
        PicoTranscriptVirtual_MountedIndex(&cache, 2) != 199 ||
        PicoTranscriptVirtual_MountedIndex(&cache, 3) != 240 ||
        !Near(PicoTranscriptVirtual_SpanHeight(&cache, 1, 120, gap), 119.0f * 58.0f))
    {
        PicoTranscriptVirtual_Free(&cache);
        return Fail("sparse mounts lost ordered rows or intervening history");
    }
    PicoTranscriptVirtual_RecordHeight(&cache, 120, 100.0f);
    PicoTranscriptVirtual_RecordHeight(&cache, 240, 70.0f);
    PicoTranscriptVirtual_Begin(&cache, 44, 256, 600.0f, 1.0f);
    PlanAndRefine(&cache, 0.0f, 40.0f, 0.0f, 199, gap);
    if (PicoTranscriptVirtual_MountedCount(&cache) != 2 ||
        !Near(PicoTranscriptVirtual_AnchorDelta(&cache, 120, 110.0f, 9000.0f, gap), 10.0f))
    {
        PicoTranscriptVirtual_Free(&cache);
        return Fail("settled offscreen edits kept mounting or lost anchor corrections");
    }

    PicoTranscriptVirtual_Free(&cache);
    return 0;
}
