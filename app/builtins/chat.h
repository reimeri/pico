#ifndef PICO_BUILTIN_CHAT_H
#define PICO_BUILTIN_CHAT_H

#include <stdbool.h>


struct PicoHost;
struct PicoWorkspace;
struct PicoToolRowEvent;

void PicoChat_InspectClose(void);
void PicoChat_HarvestVirtualHeights(struct PicoHost *app);
bool PicoChat_TakeVirtualRelayout(void);
void PicoChat_UpdateInspectFollowFromUserScroll(struct PicoHost *app, float wheel_y);
bool PicoChat_ScrollHoveredEmptyCard(struct PicoHost *app, float wheel_x, float wheel_y);
bool PicoChat_EmptyCardScrollbarActive(struct PicoHost *app);
void PicoChat_SubagentToolRow(struct PicoWorkspace *workspace, struct PicoToolRowEvent *event, void *state);

#endif
