#ifndef PICO_CHAT_SEL_H
#define PICO_CHAT_SEL_H

#include <stdbool.h>

#include "clay/clay.h"

struct PicoApp;

void PicoChatSel_BeginFrame(int message_count);
void PicoChatSel_SetMessage(int msg);
void PicoChatSel_Break(void);
void PicoChatSel_Glue(const char *s);
void PicoChatSel_Text(Clay_String text, Clay_TextElementConfig config);

bool PicoChatSel_HasSelection(const struct PicoApp *app);
void PicoChatSel_Clear(struct PicoApp *app);
void PicoChatSel_Copy(struct PicoApp *app);
void PicoChatSel_Clamp(struct PicoApp *app);
int PicoChatSel_OffsetAtPoint(struct PicoApp *app, float x, float y, int lock_msg, int *out_msg);
void PicoChatSel_SelectUnitAt(struct PicoApp *app, int msg, int pos, int granularity);
void PicoChatSel_ExtendUnitTo(struct PicoApp *app, int pos);
bool PicoChatSel_PointerOverText(void);
void PicoChatSel_DrawOverlay(struct PicoApp *app);

#endif
