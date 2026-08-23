/* Forced into rcore_desktop_glfw.c. GLFW 3.4 defaults GLFW_SCALE_FRAMEBUFFER to
 * TRUE, which on Wayland makes the GL viewport a subset of the window until a
 * user resize (content in a corner, mouse hit-tests wrong). */
#pragma once

#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>

static inline GLFWwindow *pico_glfwCreateWindow(int width, int height, const char *title, GLFWmonitor *monitor,
                                                GLFWwindow *share)
{
#ifdef GLFW_SCALE_FRAMEBUFFER
    glfwWindowHint(GLFW_SCALE_FRAMEBUFFER, GLFW_FALSE);
#endif
    return glfwCreateWindow(width, height, title, monitor, share);
}

#define glfwCreateWindow pico_glfwCreateWindow
