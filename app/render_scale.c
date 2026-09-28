#include "render_scale.h"

#include "pico/theme.h"
#include "rlgl.h"

#include <GLFW/glfw3.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

static float s_device_scale = 1.0f;
static int s_framebuffer_width = 0;
static int s_framebuffer_height = 0;

bool Pico_RenderScaleNativeEnabled(void)
{
    static int enabled = -1;
    if (enabled < 0)
    {
        const char *value = getenv("PICO_DISABLE_NATIVE_SCALE");
        enabled = (value != NULL && *value != '\0' && strcmp(value, "0") != 0) ? 0 : 1;
    }
    return enabled != 0;
}

/* The device scale is the framebuffer:window ratio, not the monitor content
 * scale: on X11 and Windows the framebuffer always equals the window, so the
 * ratio is 1 even when a content scale is reported, while on Wayland (integer
 * and fractional monitor scale) and macOS the ratio is the real pixel density.
 * raylib reports logical sizes (CORE.Window.screen), but glfwGetWindowSize is
 * authoritative during the first frames before raylib's callback catches up
 * with the compositor configure. */
static float FramebufferDeviceScale(GLFWwindow *window)
{
    int framebuffer_width = 0;
    int framebuffer_height = 0;
    int window_width = 0;
    int window_height = 0;
    glfwGetFramebufferSize(window, &framebuffer_width, &framebuffer_height);
    glfwGetWindowSize(window, &window_width, &window_height);
    if (framebuffer_width <= 0 || framebuffer_height <= 0 || window_width <= 0 || window_height <= 0)
    {
        return 1.0f;
    }
    float scale = (float)framebuffer_width / (float)window_width;
    if (!(scale > 0.0f) || scale > 16.0f)
    {
        return 1.0f;
    }
    return scale;
}

bool Pico_RenderScaleBeginFrame(void)
{
    GLFWwindow *window = (GLFWwindow *)GetWindowHandle();
    if (!window || !IsWindowReady())
    {
        return false;
    }
    int framebuffer_width = 0;
    int framebuffer_height = 0;
    glfwGetFramebufferSize(window, &framebuffer_width, &framebuffer_height);
    if (framebuffer_width <= 0 || framebuffer_height <= 0)
    {
        return false;
    }
    float scale = Pico_RenderScaleNativeEnabled() ? FramebufferDeviceScale(window) : 1.0f;
    bool changed = scale != s_device_scale || framebuffer_width != s_framebuffer_width ||
                   framebuffer_height != s_framebuffer_height;
    s_device_scale = scale;
    s_framebuffer_width = framebuffer_width;
    s_framebuffer_height = framebuffer_height;
    if (changed)
    {
        Pico_SetDeviceFontScale(scale);
    }
    /* Raylib sizes the viewport from the logical window (raylib #5564); keep
     * the whole physical framebuffer covered every frame. The orthographic
     * projection stays logical, so all drawing coordinates are scaled by the
     * viewport transform. */
    rlViewport(0, 0, framebuffer_width, framebuffer_height);
    return changed;
}

void Pico_RenderScaleInit(void)
{
    (void)Pico_RenderScaleBeginFrame();
}

float Pico_DeviceScale(void)
{
    return s_device_scale;
}

int Pico_FramebufferWidth(void)
{
    return s_framebuffer_width;
}

int Pico_FramebufferHeight(void)
{
    return s_framebuffer_height;
}

static int RoundScaled(float value)
{
    return (int)(value >= 0.0f ? value + 0.5f : value - 0.5f);
}

PicoGLScissor Pico_MapScissor(int x, int y, int width, int height, int framebufferHeight, float scale)
{
    PicoGLScissor scissor;
    scissor.x = RoundScaled((float)x * scale);
    scissor.y = RoundScaled((float)y * scale);
    scissor.width = RoundScaled((float)width * scale);
    scissor.height = RoundScaled((float)height * scale);
    /* GL scissor origin is bottom-left; input rect origin is top-left. */
    scissor.y = framebufferHeight - (scissor.y + scissor.height);
    return scissor;
}

void Pico_Scissor(int x, int y, int width, int height)
{
    rlDrawRenderBatchActive();
    rlEnableScissorTest();
    PicoGLScissor scissor = Pico_MapScissor(x, y, width, height, s_framebuffer_height, s_device_scale);
    rlScissor(scissor.x, scissor.y, scissor.width, scissor.height);
}

void Pico_ScissorEnd(void)
{
    rlDrawRenderBatchActive();
    rlDisableScissorTest();
}

float Pico_RoundToPhysical(float logical, float scale)
{
    if (!(scale > 0.0f) || scale == 1.0f)
    {
        return roundf(logical);
    }
    return roundf(logical * scale) / scale;
}
