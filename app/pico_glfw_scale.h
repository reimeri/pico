/* Force-included into rcore.c (and the desktop backend it includes) before
 * glfwCreateWindow. Native-resolution rendering (app/render_scale.c) relies on
 * the GLFW 3.4 default GLFW_SCALE_FRAMEBUFFER=TRUE so Wayland hands us a
 * monitor-scaled framebuffer; raylib 5.5 never sets the hint (raylib #5564),
 * so the default stands. PICO_DISABLE_NATIVE_SCALE=1 restores the old 1:1
 * logical framebuffer that the compositor upscales. The env parsing here must
 * stay in sync with Pico_RenderScaleNativeEnabled() in app/render_scale.c;
 * it is inlined so rcore does not depend on Pico objects. */
#pragma once

#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>

static inline bool pico_glfw_scale_disabled(void)
{
    const char *value = getenv("PICO_DISABLE_NATIVE_SCALE");
    return value != NULL && *value != '\0' && strcmp(value, "0") != 0;
}

static inline GLFWwindow *pico_glfwCreateWindow(int width, int height, const char *title, GLFWmonitor *monitor,
                                                GLFWwindow *share)
{
#ifdef GLFW_SCALE_FRAMEBUFFER
    if (pico_glfw_scale_disabled())
    {
        glfwWindowHint(GLFW_SCALE_FRAMEBUFFER, GLFW_FALSE);
    }
#endif
    return glfwCreateWindow(width, height, title, monitor, share);
}

#define glfwCreateWindow pico_glfwCreateWindow
