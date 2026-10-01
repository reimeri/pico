#include "streaming_tps.h"

#include <stdio.h>

static int RateIs(const PicoStreamingTps *tps, double expected, const char *behavior)
{
    if (tps->valid && tps->rate > expected - 0.001 && tps->rate < expected + 0.001)
        return 0;
    fprintf(stderr, "FAIL: %s (valid=%d, rate=%f, expected=%f)\n",
            behavior, tps->valid, tps->rate, expected);
    return 1;
}

static int TestGenerationClock(void)
{
    PicoStreamingTps tps;
    PicoStreamingTps_Reset(&tps, true);
    PicoStreamingTps_Begin(&tps);
    PicoStreamingTps_Publish(&tps, 100.0);
    if (tps.valid) return 1;
    PicoStreamingTps_Output(&tps, 100, false, 1000.0);
    PicoStreamingTps_Output(&tps, 600, false, 1001.0);
    PicoStreamingTps_End(&tps, 1002.0);
    int failed = RateIs(&tps, 87.5, "partial window excludes initial request latency");
    PicoStreamingTps_Publish(&tps, 1500.0);
    failed |= RateIs(&tps, 87.5, "completed stream retains its final rate");
    PicoStreamingTps_Begin(&tps);
    PicoStreamingTps_Publish(&tps, 1800.0);
    failed |= RateIs(&tps, 87.5, "next request retains the rate before output");
    PicoStreamingTps_Output(&tps, 100, false, 2000.0);
    PicoStreamingTps_End(&tps, 2001.0);
    failed |= RateIs(&tps, 800.0 / 12.0, "turn window survives tool and request-wait gaps");
    PicoStreamingTps_Reset(&tps, true);
    if (tps.valid)
    {
        fprintf(stderr, "FAIL: a new user turn must clear the previous rate\n");
        failed = 1;
    }
    return failed;
}

static int TestLivePause(void)
{
    PicoStreamingTps tps;
    PicoStreamingTps_Reset(&tps, true);
    PicoStreamingTps_Begin(&tps);
    PicoStreamingTps_Output(&tps, 400, false, 10.0);
    PicoStreamingTps_Publish(&tps, 11.0);
    int failed = RateIs(&tps, 100.0, "live rate uses bytes divided by four");
    PicoStreamingTps_Publish(&tps, 12.0);
    return failed | RateIs(&tps, 50.0, "a pause inside an active stream lowers TPS");
}

static int TestRollingWindow(void)
{
    PicoStreamingTps tps;
    PicoStreamingTps_Reset(&tps, true);
    PicoStreamingTps_Begin(&tps);
    for (int i = 0; i < 100; i++)
        PicoStreamingTps_Output(&tps, 400, false, (double)i);
    PicoStreamingTps_Publish(&tps, 99.0);
    int failed = RateIs(&tps, 100.0, "steady streaming has a steady rate");
    for (int i = 0; i < 100; i++)
        PicoStreamingTps_Output(&tps, 800, false, 100.0 + i);
    PicoStreamingTps_End(&tps, 199.0);
    return failed | RateIs(&tps, 200.0, "recent generation replaces old turn history");
}

static int TestLargeDelta(void)
{
    PicoStreamingTps tps;
    PicoStreamingTps_Reset(&tps, true);
    PicoStreamingTps_Begin(&tps);
    PicoStreamingTps_Output(&tps, 400, false, 1.0);
    PicoStreamingTps_Output(&tps, 1000000, false, 1001.0);
    PicoStreamingTps_End(&tps, 1001.0);
    return RateIs(&tps, 250.0, "large chunks preserve their observed generation rate");
}

static int TestSummaryFallback(void)
{
    PicoStreamingTps tps;
    PicoStreamingTps_Reset(&tps, true);
    PicoStreamingTps_Begin(&tps);
    PicoStreamingTps_Summary(&tps, 400, 1.0);
    PicoStreamingTps_Summary(&tps, 800, 2.0);
    PicoStreamingTps_Summary(&tps, 800, 3.0);
    PicoStreamingTps_Summary(&tps, 0, 4.0); /* Next summary step. */
    PicoStreamingTps_Summary(&tps, 100, 5.0);
    PicoStreamingTps_End(&tps, 6.0);
    int failed = RateIs(&tps, 45.0, "summary snapshots count only new bytes across steps");
    PicoStreamingTps_Begin(&tps);
    PicoStreamingTps_Output(&tps, 100, true, 100.0);
    PicoStreamingTps_End(&tps, 101.0);
    return failed | RateIs(&tps, 1000.0 / 24.0, "earlier summary-only requests remain in the turn window");
}

static int TestRawThinkingSupersedesSummary(bool raw_first)
{
    PicoStreamingTps tps;
    PicoStreamingTps_Reset(&tps, true);
    PicoStreamingTps_Begin(&tps);
    PicoStreamingTps_Output(&tps, 400, false, 1.0);
    if (raw_first) PicoStreamingTps_Output(&tps, 400, true, 2.0);
    PicoStreamingTps_Summary(&tps, 800, 2.0);
    PicoStreamingTps_Summary(&tps, 1600, 3.0);
    if (!raw_first) PicoStreamingTps_Output(&tps, 400, true, 4.0);
    PicoStreamingTps_End(&tps, 5.0);
    return RateIs(&tps, 50.0, raw_first ? "summaries following raw thinking are ignored" :
                                        "late raw thinking discards speculative summaries");
}

static int TestDisabled(void)
{
    PicoStreamingTps tps;
    PicoStreamingTps_Reset(&tps, false);
    PicoStreamingTps_Begin(&tps);
    PicoStreamingTps_Output(&tps, 400, false, 1.0);
    PicoStreamingTps_Summary(&tps, 400, 2.0);
    PicoStreamingTps_End(&tps, 3.0);
    if (!tps.valid) return 0;
    fprintf(stderr, "FAIL: disabled agents must not produce a rate\n");
    return 1;
}

int main(void)
{
    int failed = TestGenerationClock() | TestLivePause() | TestRollingWindow() |
                 TestLargeDelta() | TestSummaryFallback() | TestDisabled() |
                 TestRawThinkingSupersedesSummary(false) | TestRawThinkingSupersedesSummary(true);
    return failed ? 1 : 0;
}
