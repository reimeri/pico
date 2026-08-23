#ifndef PICO_SCROLLBAR_H
#define PICO_SCROLLBAR_H

#include "clay/clay.h"

#include <stdbool.h>

struct PicoScrollbar;

typedef struct PicoScrollbarThumb {
    float height;
    float y;
} PicoScrollbarThumb;

void PicoScrollbar_BeginFrame(void);
bool PicoScrollbar_Overflows(Clay_String container_id);
PicoScrollbarThumb PicoScrollbar_Metrics(float track_h, float content_h, float scroll_y);
void PicoScrollbar_Render(Clay_String container_id, Clay_String track_id, Clay_String handle_id);
void PicoScrollbar_UpdateDrag(struct PicoScrollbar *drag, Clay_String container_id, Clay_String handle_id);
bool PicoScrollbar_AnyDragging(void);

#endif
