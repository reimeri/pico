#ifndef PICO_BUILTINS_SIDEBAR_H
#define PICO_BUILTINS_SIDEBAR_H

#include <stdbool.h>

/* Internal interaction model shared by the sidebar and behavior tests. */
bool PicoSidebar_DragMoved(float press_x, float press_y, float mouse_x, float mouse_y);
int PicoSidebar_DragTarget(const float *midpoints, int count, int source, float mouse_y);
int PicoSidebar_DropTarget(const float *midpoints, int count, int source, float mouse_y,
                           bool pointer_over_list);

#endif
