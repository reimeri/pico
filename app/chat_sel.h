#ifndef PICO_CHAT_SEL_H
#define PICO_CHAT_SEL_H

#include <stdbool.h>

#include "pico/host.h"
#include "chat_search.h"

#include "clay/clay.h"


/* Bind only during main-transcript rendering; offscreen messages retain text. */
void PicoChatSel_Free(void);
void PicoChatSel_SetSearch(PicoChatSearch *search);
void PicoChatSel_SetHorizontalClip(Clay_ElementId id, bool temporary);
typedef void (*PicoChatRangeFn)(Clay_BoundingBox box, Clay_ElementId horizontal_clip, bool temporary, void *user);
void PicoChatSel_VisitRange(int message, int from, int to, PicoChatRangeFn visit, void *user);

void PicoChatSel_BeginFrame(int message_count);
void PicoChatSel_SetMessage(int msg);
void PicoChatSel_Break(void);
void PicoChatSel_Glue(const char *s);
void PicoChatSel_Text(Clay_String text, Clay_TextElementConfig config);

bool PicoChatSel_HasSelection(const struct PicoHost *app);
void PicoChatSel_Clear(struct PicoHost *app);
void PicoChatSel_Copy(struct PicoHost *app);
void PicoChatSel_Clamp(struct PicoHost *app);
int PicoChatSel_OffsetAtPoint(struct PicoHost *app, float x, float y, int lock_msg, int *out_msg);
void PicoChatSel_SelectUnitAt(struct PicoHost *app, int msg, int pos, int granularity);
void PicoChatSel_ExtendUnitTo(struct PicoHost *app, int pos);
bool PicoChatSel_PointerOverText(void);
void PicoChatSel_DrawOverlay(struct PicoHost *app);

#endif
