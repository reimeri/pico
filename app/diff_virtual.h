#ifndef PICO_DIFF_VIRTUAL_H
#define PICO_DIFF_VIRTUAL_H

/* Fixed-stride windowing for the diff modal: every row has the same height
 * (single-line text, no wrap), so the visible window is pure arithmetic.
 * The modal mounts only [first, end) plus fixed-height spacers, keeping Clay
 * element count proportional to the viewport instead of the diff size. */

/* Computes the visible row window and the spacer heights that keep total
 * content height pixel-identical to a fully mounted list.
 *
 * stride = row height + container childGap; gap = the container childGap.
 * scroll_top >= 0 (pixels scrolled down), viewport_h > 0.
 * Overscan mounts extra rows on both sides to cover the one-frame lag of
 * scroll data and fast scrolls.
 *
 * Pads account for the childGap that replaces the boundary between spacer
 * and row: top = first*stride - gap, bottom = (total-end)*stride - gap. */
static inline void PicoDiffWindow_Range(int total_rows, float scroll_top, float viewport_h,
                                        float stride, float gap, int overscan, int *out_first,
                                        int *out_end, float *out_top_pad, float *out_bottom_pad)
{
    int first = 0;
    int end = total_rows;
    if (total_rows > 0 && stride > 0.0f)
    {
        if (scroll_top < 0.0f)
        {
            scroll_top = 0.0f;
        }
        first = (int)(scroll_top / stride) - overscan;
        if (first < 0)
        {
            first = 0;
        }
        if (first > total_rows)
        {
            first = total_rows;
        }
        float bottom = scroll_top + viewport_h;
        end = (int)(bottom / stride) + 1 + overscan;
        if (end > total_rows)
        {
            end = total_rows;
        }
        if (end < first)
        {
            end = first;
        }
    }
    *out_first = first;
    *out_end = end;
    *out_top_pad = first > 0 ? (float)first * stride - gap : 0.0f;
    *out_bottom_pad = end < total_rows ? (float)(total_rows - end) * stride - gap : 0.0f;
}

#endif
