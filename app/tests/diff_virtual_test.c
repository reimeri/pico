#include "diff_virtual.h"

#include <math.h>
#include <stdio.h>

/* The modal mounts only [first, end) plus fixed-height pads. The durable
 * contract: the virtualized list's total height must be pixel-identical to a
 * fully mounted list (so scroll range and scrollbar thumb never change), and
 * the mounted window must always cover the viewport. */

static int g_fails;

#define CHECK(cond)                                                                 \
    do                                                                              \
    {                                                                               \
        if (!(cond))                                                                \
        {                                                                           \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);         \
            g_fails++;                                                              \
        }                                                                           \
    } while (0)

/* Total content height as Clay computes it for the mounted children:
 * pads + rows + childGap between consecutive children. */
static float VirtualHeight(int first, int end, int total, float top_pad, float bottom_pad,
                           float row_h, float gap)
{
    int rows = end - first;
    int children = rows + (first > 0 ? 1 : 0) + (end < total ? 1 : 0);
    if (children == 0)
    {
        return 0.0f;
    }
    return top_pad + bottom_pad + (float)rows * row_h + (float)(children - 1) * gap;
}

static float FullHeight(int total, float row_h, float gap)
{
    return total > 0 ? (float)total * row_h + (float)(total - 1) * gap : 0.0f;
}

/* Every row intersecting [scroll_top, scroll_top + viewport_h) is mounted. */
static int CoversViewport(int first, int end, int total, float scroll_top, float viewport_h,
                          float stride)
{
    for (int r = 0; r < total; r++)
    {
        float top = (float)r * stride;
        if (top + 1.0f >= scroll_top && top <= scroll_top + viewport_h)
        {
            if (r < first || r >= end)
            {
                return 0;
            }
        }
    }
    return 1;
}

static void Case(int total, float scroll_top, float viewport_h)
{
    const float row_h = 18.0f;
    const float gap = 2.0f;
    const float stride = row_h + gap;
    const int overscan = 8;
    int first, end;
    float top_pad, bottom_pad;
    PicoDiffWindow_Range(total, scroll_top, viewport_h, stride, gap, overscan, &first, &end,
                         &top_pad, &bottom_pad);

    CHECK(first >= 0 && first <= end && end <= total);
    CHECK(top_pad >= 0.0f && bottom_pad >= 0.0f);
    CHECK(fabsf(VirtualHeight(first, end, total, top_pad, bottom_pad, row_h, gap) -
                FullHeight(total, row_h, gap)) < 0.5f);
    CHECK(CoversViewport(first, end, total, scroll_top, viewport_h, stride));
    /* unmounted regions are exactly covered by the pads */
    CHECK(fabsf(top_pad - ((float)first * stride - (first > 0 ? gap : 0.0f))) < 0.001f);
    CHECK(fabsf(bottom_pad - ((float)(total - end) * stride - (end < total ? gap : 0.0f))) < 0.001f);
}

int main(void)
{
    Case(0, 0.0f, 600.0f);                 /* empty */
    Case(5, 0.0f, 600.0f);                 /* fits entirely */
    Case(100, 0.0f, 600.0f);               /* top */
    Case(100, 500.0f, 600.0f);             /* middle */
    Case(100, 1.0e6f, 600.0f);             /* scrolled far past the end */
    Case(100, 1998.0f, 2.0f);              /* tiny viewport on a row boundary */
    Case(100000, 765432.0f, 600.0f);       /* large model */
    Case(100, -50.0f, 600.0f);             /* negative scroll (elastic overshoot) */

    /* Stride 0 (degenerate) must not divide by zero; falls back to mounting all. */
    int first = -1, end = -1;
    float top_pad = -1.0f, bottom_pad = -1.0f;
    PicoDiffWindow_Range(10, 100.0f, 600.0f, 0.0f, 2.0f, 8, &first, &end, &top_pad, &bottom_pad);
    CHECK(first == 0 && end == 10 && top_pad == 0.0f && bottom_pad == 0.0f);

    if (g_fails)
    {
        fprintf(stderr, "%d checks failed\n", g_fails);
        return 1;
    }
    puts("diff_virtual: ok");
    return 0;
}
