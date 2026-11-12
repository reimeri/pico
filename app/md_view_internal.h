#ifndef PICO_MD_VIEW_INTERNAL_H
#define PICO_MD_VIEW_INTERNAL_H

#include <stdbool.h>
#include "pico/md_view.h"
#include "clay/clay.h"
#include "raylib.h"

/* Scope text/link tint to one notice document, without altering ordinary chat. */
void MdView_RenderNoticeDocument(MdDocument *doc, int id_base, float available_width,
                                 Clay_Color color);

// Last-layout bounds of the hovered link run, for shell tooltip placement.
Clay_ElementData MdView_HoveredLinkElement(void);

/* Preview an already-resolved local image link, or NULL when unavailable.
 * Main/render thread; borrows the markdown cache's texture until the next
 * BeginFrame/ClearImages, just like document image commands. Never fetches URLs.
 * Fits without upscaling; size is set only on success. */
Texture2D *MdView_LinkPreview(const char *target, float max_width, float max_height,
                              Clay_Dimensions *size);

// Poll the last layout's image files; true requests an idle UI redraw.
bool MdView_ImagesChanged(void);

/* Hit-test the last layout's displayed images, respecting Clay clipping and
 * overlay capture. Resolved path is borrowed until BeginFrame/ClearImages. */
const char *MdView_HoveredImagePath(void);

// Applies a horizontal wheel delta to the topmost rendered markdown scroller
// under the pointer. Returns true when an overflowing scroller handled it.
bool MdView_ScrollHoveredHorizontal(float delta_x);

#endif
