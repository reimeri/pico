#ifndef PICO_CHAT_SEARCH_H
#define PICO_CHAT_SEARCH_H

#include <stdbool.h>
#include <stdint.h>

/* Private, layout-independent retained display text. Byte ranges refer to the
 * original UTF-8, never the folded representation. Newlines delimit blocks;
 * the single-line query cannot cross them or message boundaries. */
typedef struct PicoChatMatch {
    int message;
    int from;
    int to;
} PicoChatMatch;

typedef struct PicoSearchFold {
    char *text;
    /* NULL maps mean byte offsets are unchanged by folding. */
    int *from;
    int *to;
    int count;
} PicoSearchFold;

typedef struct PicoSearchMessage {
    char *text;
    PicoSearchFold folded;
    PicoChatMatch *matches;
    int count;
    bool dirty;
} PicoSearchMessage;

typedef struct PicoChatSearch {
    PicoSearchMessage *messages;
    int message_count;
    PicoSearchFold query;
    int *prefix;
    PicoChatMatch *matches;
    int count;
    int active;
    bool dirty;
    bool failed;
} PicoChatSearch;

void PicoChatSearch_Free(PicoChatSearch *search);
bool PicoChatSearch_Resize(PicoChatSearch *search, int count);
bool PicoChatSearch_SetText(PicoChatSearch *search, int message, const char *text);
bool PicoChatSearch_Query(PicoChatSearch *search, const char *query);
bool PicoChatSearch_Refresh(PicoChatSearch *search);
void PicoChatSearch_Step(PicoChatSearch *search, int direction);

#endif
