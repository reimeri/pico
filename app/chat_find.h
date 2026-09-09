#ifndef PICO_CHAT_FIND_H
#define PICO_CHAT_FIND_H

#include "chat_search.h"
#include "transcript_virtual.h"
#include "clay/clay.h"

struct PicoHost;
typedef struct PicoChatFind {
    PicoChatSearch search;
    struct PicoFindScrollRestore *temporary_scrolls;
    char *query;
    int length;
    int capacity;
    int cursor;
    int anchor;
    float input_scroll;
    bool open;
    bool focused;
    bool claimed_input;
    bool claimed_pointer;
    bool dragging;
    bool nearest;
    bool reveal;
    int pressed_button;
    uint64_t agent_id;
    char session_id[40];
    char counter[64];
} PicoChatFind;

void PicoChatFind_Reset(struct PicoHost *app);
void PicoChatFind_Sync(struct PicoHost *app);
void PicoChatFind_Open(struct PicoHost *app);
void PicoChatFind_Close(struct PicoHost *app);
void PicoChatFind_SetQuery(struct PicoHost *app, const char *query);
void PicoChatFind_Navigate(struct PicoHost *app, int direction);
void PicoChatFind_HandleInput(struct PicoHost *app);
bool PicoChatFind_BlocksInput(const struct PicoHost *app);
bool PicoChatFind_PointerOver(const struct PicoHost *app);
void PicoChatFind_Render(struct PicoHost *app);
void PicoChatFind_DrawInput(struct PicoHost *app);
void PicoChatFind_DrawMatches(struct PicoHost *app);
void PicoChatFind_MountTargets(struct PicoHost *app, PicoTranscriptVirtual *cache, float gap);
bool PicoChatFind_Reveal(struct PicoHost *app);

#endif
