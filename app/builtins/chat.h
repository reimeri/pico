#ifndef PICO_BUILTIN_CHAT_H
#define PICO_BUILTIN_CHAT_H

#include <stdbool.h>


struct PicoHost;
struct PicoWorkspace;
struct PicoToolRowEvent;

void PicoChat_InspectClose(void);
/* Main-thread session navigation; call before changing the selected agent. */
void PicoChat_SelectionChanging(struct PicoHost *app);
void PicoChat_CancelScrollRestore(struct PicoHost *app);
void PicoChat_ForgetSessionScroll(struct PicoHost *app, const char *workspace, const char *session);
/* Private shell integration: retain automatic shrink space, then correct before present. */
void PicoChat_ResetBottomSpace(struct PicoHost *app);
void PicoChat_BeginFrame(struct PicoHost *app);
void PicoChat_PrepareRefinement(struct PicoHost *app, bool interacting);
void PicoChat_BeginLayout(struct PicoHost *app);
bool PicoChat_ViewportReady(struct PicoHost *app, bool unsettled);
void PicoChat_DidPresent(struct PicoHost *app);
void PicoChat_BeginScrollLayout(struct PicoHost *app);
bool PicoChat_StabilizeScrollLayout(struct PicoHost *app);
void PicoChat_HarvestVirtualHeights(struct PicoHost *app);
void PicoChat_RecordVirtualScroll(void);
bool PicoChat_TakeVirtualRelayout(void);
void PicoChat_UpdateInspectFollowFromUserScroll(struct PicoHost *app, float wheel_y);
bool PicoChat_ScrollHoveredEmptyCard(struct PicoHost *app, float wheel_x, float wheel_y);
bool PicoChat_EmptyCardScrollbarActive(struct PicoHost *app);
void PicoChat_SubagentToolRow(struct PicoWorkspace *workspace, struct PicoToolRowEvent *event, void *state);

#endif
