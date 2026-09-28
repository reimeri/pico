#ifndef PICO_RENDER_SCALE_H
#define PICO_RENDER_SCALE_H

#include <stdbool.h>

#include <raylib.h>

/* Native-resolution rendering.
 *
 * Layout coordinates, mouse input, and the design-pixel type scale are all
 * logical window units; Pico_FontPx stays logical. When the display server
 * provides a framebuffer larger than the logical window (Wayland monitor
 * scale incl. fractional, macOS Retina), Pico renders into that framebuffer:
 * the GL viewport covers the whole physical framebuffer while the orthographic
 * projection stays logical, and fonts are rasterized at device pixels so glyph
 * texels map 1:1 onto device pixels. Scissors are mapped from logical to
 * framebuffer coordinates. PICO_DISABLE_NATIVE_SCALE=1 falls back to the old
 * upscaled logical framebuffer. */

/* Sync the native viewport with the current framebuffer. Call on every pump,
 * before layout: raylib resets the viewport to logical window size whenever
 * its window-size callback runs, and monitor moves change the scale. Returns
 * true when the framebuffer size or device scale changed since the last call
 * (the caller should refresh cached fonts and request a redraw). */
bool Pico_RenderScaleBeginFrame(void);

/* One-shot sync right after window creation and before fonts load, so the
 * first atlas is rasterized at the correct device pixel size. */
void Pico_RenderScaleInit(void);

/* Framebuffer:window width ratio. 1.0 when native scaling is disabled or the
 * window is unavailable. */
float Pico_DeviceScale(void);

/* Current framebuffer size in device pixels (0 before the first sync). */
int Pico_FramebufferWidth(void);
int Pico_FramebufferHeight(void);

/* True unless PICO_DISABLE_NATIVE_SCALE is set to a non-zero value. */
bool Pico_RenderScaleNativeEnabled(void);

/* Scissor in logical window coordinates, applied to the physical framebuffer.
 * Pico_ScissorEnd mirrors raylib's EndScissorMode. */
void Pico_Scissor(int x, int y, int width, int height);
void Pico_ScissorEnd(void);

/* Round a logical coordinate to the physical pixel grid. Identity rounding
 * (roundf) when the device scale is 1. */
float Pico_RoundToPhysical(float logical, float scale);

/* Pure logical->GL scissor mapping (unit tested). GL scissor uses a
 * bottom-left origin in framebuffer pixels. */
typedef struct PicoGLScissor {
    int x;
    int y;
    int width;
    int height;
} PicoGLScissor;

PicoGLScissor Pico_MapScissor(int x, int y, int width, int height, int framebufferHeight, float scale);

#endif
