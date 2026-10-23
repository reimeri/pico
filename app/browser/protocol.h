#ifndef PICO_BROWSER_PROTOCOL_H
#define PICO_BROWSER_PROTOCOL_H
#include <stdint.h>
/* Private, same-build local IPC. Commands are atomic SOCK_SEQPACKET messages;
 * events are framed on a separate stream. Never expose either socket publicly. */
enum { PB_OPEN = 1, PB_CLOSE, PB_RESIZE, PB_VISIBLE, PB_FOCUS, PB_MOUSE,
       PB_WHEEL, PB_KEY, PB_TEXT, PB_BACK, PB_FORWARD, PB_RELOAD, PB_STOP, PB_OBSERVE, PB_BEGIN, PB_COMMIT, PB_CANCEL, PB_CONTROL };
enum { PB_READY = 1, PB_FRAME, PB_URL, PB_ERROR, PB_CLOSED, PB_OBSERVATION };
enum { PB_MAX_DIMENSION = 4096, PB_URL_CAP = 2048, PB_MAX_STEPS = 128, PB_MAX_ACTIONS = 16, PB_MAX_PNG = 64 * 1024 * 1024 };
typedef struct PbCommand {
    uint64_t token;
    uint32_t type;
    int32_t x, y, a, b, modifiers;
    char text[PB_URL_CAP];
    char profile[80];
    uint64_t request, navigation, viewport;
    uint32_t end_action; /* last native event of a logical action */
} PbCommand;
typedef struct PbEvent {
    uint64_t token;
    uint32_t type, length, width, height;
} PbEvent;
typedef struct PbObservation {
    uint64_t request, navigation, viewport;
    uint32_t completed, png_size, delivered_events, partial_action, uncertain;
    char url[PB_URL_CAP], error[512];
} PbObservation;
#endif
