#ifndef PICO_MD_VIEW_INTERNAL_H
#define PICO_MD_VIEW_INTERNAL_H

#include <stdbool.h>

// Applies a horizontal wheel delta to the topmost rendered markdown scroller
// under the pointer. Returns true when an overflowing scroller handled it.
bool MdView_ScrollHoveredHorizontal(float delta_x);

#endif
