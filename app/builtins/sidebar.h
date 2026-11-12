#ifndef PICO_BUILTINS_SIDEBAR_H
#define PICO_BUILTINS_SIDEBAR_H

#include <stdbool.h>

typedef struct PicoHost PicoHost;
/* Recycle borrowed text once per shell layout, even when the sidebar is hidden. */
void PicoSidebar_BeginLayout(PicoHost *host);

/* Non-modal sidebar search owns keys before composer and agent cancellation. */
void PicoSidebar_HandleSearchInput(PicoHost *host);
bool PicoSidebar_BlocksInput(const PicoHost *host);

/* Renders the internal Inbox project picker, returning whether it replaced the empty state. */
bool PicoSidebar_RenderProjectPicker(PicoHost *host);

/* Internal interaction model shared by the sidebar and behavior tests. */
bool PicoSidebar_DragMoved(float press_x, float press_y, float mouse_x, float mouse_y);
int PicoSidebar_DragTarget(const float *midpoints, int count, int source, float mouse_y);
int PicoSidebar_DropTarget(const float *midpoints, int count, int source, float mouse_y,
                           bool pointer_over_list);

#endif
