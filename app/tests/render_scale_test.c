#define _POSIX_C_SOURCE 200809L

#include "render_scale.h"
#include "pico/theme.h"
#include "docs_path.h"
#include "theme_internal.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int Fail(const char *message)
{
    fprintf(stderr, "render scale: %s\n", message);
    return 1;
}

/* The GL scissor maps a top-left logical rect onto a bottom-left framebuffer
 * rect. Text and pane clipping rely on this at every device scale. */
static int TestScissorMapping(void)
{
    PicoGLScissor s = Pico_MapScissor(10, 20, 30, 40, 1600, 1.0f);
    if (s.x != 10 || s.width != 30 || s.height != 40 || s.y != 1600 - 60)
    {
        return Fail("unit scale scissor is not a plain y-flip");
    }

    /* Integer scale: exact device pixels. */
    s = Pico_MapScissor(10, 20, 30, 40, 1600, 2.0f);
    if (s.x != 20 || s.width != 60 || s.height != 80 || s.y != 1600 - 120)
    {
        return Fail("integer scale scissor is not exactly scaled");
    }

    /* Fractional scale rounds each edge to the nearest device pixel. */
    s = Pico_MapScissor(11, 13, 7, 9, 1000, 1.25f);
    if (s.x != 14 || s.y != 1000 - 27 || s.width != 9 || s.height != 11)
    {
        return Fail("fractional scale scissor edges are not rounded to device pixels");
    }

    /* Negative logical coordinates stay negative, mirroring raylib. */
    s = Pico_MapScissor(-4, 0, 30, 40, 800, 1.0f);
    if (s.x != -4 || s.y != 800 - 40)
    {
        return Fail("negative logical scissor origin was clamped");
    }
    return 0;
}

static int TestPhysicalRounding(void)
{
    if (Pico_RoundToPhysical(17.4f, 1.0f) != 17.0f || Pico_RoundToPhysical(17.6f, 1.0f) != 18.0f)
    {
        return Fail("unit scale rounding is not roundf");
    }
    /* 1.25x: the physical grid is 0.8 logical pixels apart. */
    if (Pico_RoundToPhysical(3.0f, 1.25f) != 3.2f || Pico_RoundToPhysical(2.8f, 1.25f) != 3.2f ||
        Pico_RoundToPhysical(2.7f, 1.25f) != 2.4f)
    {
        return Fail("fractional scale does not round to the physical grid");
    }
    if (Pico_RoundToPhysical(4.0f, 2.0f) != 4.0f || Pico_RoundToPhysical(4.3f, 2.0f) != 4.5f)
    {
        return Fail("integer scale does not round to half-logical device grid");
    }
    return 0;
}

/* The atlas must be rasterized at device pixels while measurement stays
 * logical, and a device scale change must reload the cached slot. */
static int TestAtlasScaling(void)
{
    if (Pico_FontPx(16) != 16.0f || Pico_FontPxU16(16) != 16 || Pico_FontPx(0) != 0.0f)
    {
        return Fail("font px is not logical at unit device scale");
    }

    /* Grid-aligned sizes are unchanged by the device scale. */
    Pico_SetDeviceFontScale(1.5f);
    if (Pico_FontPx(16) != 16.0f || Pico_FontPxU16(16) != 16 || Pico_FontPx(0) != 0.0f)
    {
        return Fail("grid-aligned font px shifted with device scale");
    }
    Font font = Pico_FontAt(FONT_REGULAR, 16);
    if (font.baseSize != 24)
    {
        return Fail("atlas is not rasterized at 1.5x device pixels");
    }

    uint64_t generation = Pico_FontGeneration();
    Pico_SetDeviceFontScale(2.0f);
    if (Pico_FontGeneration() == generation)
    {
        return Fail("device scale change did not invalidate the font generation");
    }
    font = Pico_FontAt(FONT_REGULAR, 16);
    if (font.baseSize != 32)
    {
        return Fail("cached slot was not reloaded at the new device scale");
    }

    /* Out-of-range and repeated values are ignored. */
    generation = Pico_FontGeneration();
    Pico_SetDeviceFontScale(0.0f);
    Pico_SetDeviceFontScale(-1.0f);
    Pico_SetDeviceFontScale(2.0f);
    if (Pico_DeviceFontScale() != 2.0f || Pico_FontGeneration() != generation)
    {
        return Fail("invalid or repeated device scale was not ignored");
    }

    /* Fractional scale rounds the atlas size (16 * 1.25 = 20) and snaps the
     * effective size to the device grid (20 / 1.25 = 16, exact), while a size
     * whose device pixels are fractional (18 * 1.25 = 22.5) snaps to the
     * rounded atlas (23 / 1.25 = 18.4) so quads map 1:1 onto texels. */
    Pico_SetDeviceFontScale(1.25f);
    font = Pico_FontAt(FONT_REGULAR, 16);
    if (font.baseSize != 20)
    {
        return Fail("fractional atlas size is not rounded to device pixels");
    }
    if (Pico_FontPx(18) != 18.4f)
    {
        return Fail("fractional font size is not snapped to the device grid");
    }

    Pico_SetDeviceFontScale(1.0f);
    font = Pico_FontAt(FONT_REGULAR, 16);
    if (font.baseSize != 16)
    {
        return Fail("reset to unit scale did not reload the unit atlas");
    }
    Pico_UnloadFonts(NULL);
    return 0;
}

int main(void)
{
#ifdef PICO_SOURCE_ROOT
    Pico_PathsInit(PICO_SOURCE_ROOT "/app");
#else
    Pico_PathsInit("app");
#endif
    int rc = TestScissorMapping();
    if (rc != 0)
    {
        return rc;
    }
    rc = TestPhysicalRounding();
    if (rc != 0)
    {
        return rc;
    }
    return TestAtlasScaling();
}
