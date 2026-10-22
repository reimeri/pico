#ifndef PICO_MD_VIEW_H
#define PICO_MD_VIEW_H

#include "markdown.h"

// Call once per layout pass before any MdView_RenderDocument call. Previous
// Clay commands must already be consumed or discarded: beginning a new layout
// releases their image pins and may evict textures. Commands from this layout
// retain stable image pointers/textures until the next BeginFrame or ClearImages.
void MdView_BeginFrame(void);

// Directory used to resolve relative image paths. Copied internally.
void MdView_SetImageBaseDir(const char *dir);
// Main/render thread, with a live graphics context and no pending commands.
// The graphics owner calls this before closing its context; clears all textures.
void MdView_ClearImages(void);

// Renders doc->blocks as Clay elements. id_base is mixed into explicit Clay
// IDs so multiple documents in one frame (chat messages) do not collide.
void MdView_RenderDocument(MdDocument *doc, int id_base, float available_width);

const char *MdView_HoveredLink(void);

#endif
