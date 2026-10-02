#include "raylib.h"
#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>

#include <stdbool.h>
#include <stdio.h>

static bool creation_attempted;

/* Emulate successful GLFW initialization followed by failed window creation.
 * Wrapping the public pre-window calls keeps this independent of a display/GPU. */
int __wrap_glfwInit(void)
{
    return GLFW_TRUE;
}

void __wrap_glfwDefaultWindowHints(void)
{
}

void __wrap_glfwWindowHint(int hint, int value)
{
    (void)hint;
    (void)value;
}

GLFWjoystickfun __wrap_glfwSetJoystickCallback(GLFWjoystickfun callback)
{
    (void)callback;
    return NULL;
}

void __wrap_glfwTerminate(void)
{
}
GLFWwindow *__wrap_glfwCreateWindow(int width, int height, const char *title,
                                   GLFWmonitor *monitor, GLFWwindow *share)
{
    (void)width;
    (void)height;
    (void)title;
    (void)monitor;
    (void)share;
    creation_attempted = true;
    return NULL;
}

int main(void)
{
    InitWindow(100, 100, "Failed window creation");
    if (!creation_attempted)
    {
        fprintf(stderr, "Window creation failure was not exercised.\n");
        return 1;
    }
    if (IsWindowReady() || GetWindowHandle() != NULL)
    {
        fprintf(stderr, "Failed window creation must leave the window unavailable.\n");
        return 1;
    }
    return 0;
}
