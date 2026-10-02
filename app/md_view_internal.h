#ifndef PICO_MD_VIEW_INTERNAL_H
#define PICO_MD_VIEW_INTERNAL_H

#include <stdbool.h>
#include "pico/md_view.h"
#include "clay/clay.h"

/* Scope text/link tint to one notice document, without altering ordinary chat. */
void MdView_RenderNoticeDocument(MdDocument *doc, int id_base, float available_width,
                                 Clay_Color color);

// Applies a horizontal wheel delta to the topmost rendered markdown scroller
// under the pointer. Returns true when an overflowing scroller handled it.
bool MdView_ScrollHoveredHorizontal(float delta_x);

#endif
