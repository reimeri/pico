#include "scrollbar.h"

#include <stdio.h>

static int Fail(const char *test, const char *message)
{
    fprintf(stderr, "%s: %s\n", test, message);
    return 1;
}

static int ExpectFloat(const char *test, const char *what, float got, float want)
{
    float d = got - want;
    if (d < 0.0f)
    {
        d = -d;
    }
    if (d > 0.01f)
    {
        fprintf(stderr, "%s: %s got %f want %f\n", test, what, (double)got, (double)want);
        return 1;
    }
    return 0;
}

static int TestPinToBottom(void)
{
    const char *test = "pin_to_bottom";
    float scroll_y = -100.0f;
    if (!PicoScrollbar_PinToBottom(100.0f, 260.0f, &scroll_y) ||
        ExpectFloat(test, "grown content", scroll_y, -160.0f))
    {
        return Fail(test, "content growth did not request a corrected bottom layout");
    }
    if (PicoScrollbar_PinToBottom(100.0f, 260.0f, &scroll_y))
    {
        return Fail(test, "an already pinned viewport requested another layout");
    }
    scroll_y = -20.0f;
    if (!PicoScrollbar_PinToBottom(100.0f, 80.0f, &scroll_y) ||
        ExpectFloat(test, "fitting content", scroll_y, 0.0f))
    {
        return Fail(test, "fitting content was not reset to the top");
    }
    return 0;
}

static int TestExactFit(void)
{
    const char *test = "exact_fit";
    PicoScrollbarThumb thumb = PicoScrollbar_Metrics(100.0f, 100.0f, 0.0f);
    if (ExpectFloat(test, "height", thumb.height, 100.0f))
    {
        return 1;
    }
    return ExpectFloat(test, "y", thumb.y, 0.0f);
}

static int TestOverflowEnds(void)
{
    const char *test = "overflow_ends";
    PicoScrollbarThumb top = PicoScrollbar_Metrics(100.0f, 200.0f, 0.0f);
    if (ExpectFloat(test, "top.height", top.height, 50.0f) || ExpectFloat(test, "top.y", top.y, 0.0f))
    {
        return 1;
    }
    PicoScrollbarThumb bottom = PicoScrollbar_Metrics(100.0f, 200.0f, -100.0f);
    if (ExpectFloat(test, "bottom.height", bottom.height, 50.0f))
    {
        return 1;
    }
    if (ExpectFloat(test, "bottom.y", bottom.y, 50.0f))
    {
        return 1;
    }
    if (bottom.y + bottom.height < 99.99f || bottom.y + bottom.height > 100.01f)
    {
        return Fail(test, "thumb is not flush with the track bottom");
    }
    return 0;
}

static int TestMinThumb(void)
{
    const char *test = "min_thumb";
    PicoScrollbarThumb top = PicoScrollbar_Metrics(100.0f, 10000.0f, 0.0f);
    if (ExpectFloat(test, "top.height", top.height, 16.0f) || ExpectFloat(test, "top.y", top.y, 0.0f))
    {
        return 1;
    }
    PicoScrollbarThumb bottom = PicoScrollbar_Metrics(100.0f, 10000.0f, -9900.0f);
    if (ExpectFloat(test, "bottom.height", bottom.height, 16.0f))
    {
        return 1;
    }
    if (ExpectFloat(test, "bottom.y", bottom.y, 84.0f))
    {
        return 1;
    }
    if (bottom.y + bottom.height < 99.99f || bottom.y + bottom.height > 100.01f)
    {
        return Fail(test, "min thumb is not flush with the track bottom");
    }
    return 0;
}

int main(void)
{
    int failed = 0;
    failed |= TestPinToBottom();
    failed |= TestExactFit();
    failed |= TestOverflowEnds();
    failed |= TestMinThumb();
    return failed;
}
