#include "raylib.h"
#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>

#include <stdio.h>
#include <stdlib.h>

int main(void)
{
    if (!getenv("DISPLAY") && !getenv("WAYLAND_DISPLAY"))
    {
        fprintf(stderr, "Wheel input integration test needs a display.\n");
        return 77;
    }
    SetConfigFlags(FLAG_WINDOW_HIDDEN);
    InitWindow(100, 100, "Pico wheel input test");
    if (!IsWindowReady())
    {
        fprintf(stderr, "Wheel input integration test needs a graphics context.\n");
        return 77;
    }

    GLFWwindow *window = GetWindowHandle();
    GLFWscrollfun scroll = glfwSetScrollCallback(window, NULL);
    glfwSetScrollCallback(window, scroll);
    if (!scroll)
    {
        fprintf(stderr, "Raylib must register a wheel input callback.\n");
        CloseWindow();
        return 1;
    }

    /* Deliver the same motion as one event and as a batch of fractional
     * events, as a high-resolution wheel does between rendered frames.
     * Separate-axis and zero events must not erase the other axis. */
    PollInputEvents();
    scroll(window, 0.5, -1.0);
    Vector2 single = GetMouseWheelMoveV();
    PollInputEvents();
    scroll(window, 0.5, 0.0);
    for (int i = 0; i < 8; i++) scroll(window, 0.0, -0.125);
    scroll(window, 0.0, 0.0);
    Vector2 batch = GetMouseWheelMoveV();
    bool accumulated = single.x != 0.0f && single.y != 0.0f &&
                       batch.x == single.x && batch.y == single.y;

    /* A new input snapshot must not replay movement from the previous one. */
    PollInputEvents();
    Vector2 idle = GetMouseWheelMoveV();
    bool reset = idle.x == 0.0f && idle.y == 0.0f;
    CloseWindow();
    if (!accumulated || !reset)
    {
        fprintf(stderr, "Wheel motion must be preserved across event batches and reset between polls.\n");
        return 1;
    }
    return 0;
}
