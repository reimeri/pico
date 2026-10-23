#ifndef PICO_BROWSER_PROTOCOL_H
#define PICO_BROWSER_PROTOCOL_H
#include <stdint.h>
/* Private, same-build local IPC. Commands are atomic SOCK_SEQPACKET messages;
 * events are framed on a separate stream. Never expose either socket publicly. */
enum { PB_OPEN = 1, PB_CLOSE, PB_RESIZE, PB_VISIBLE, PB_FOCUS, PB_MOUSE,
       PB_WHEEL, PB_KEY, PB_TEXT, PB_BACK, PB_FORWARD, PB_RELOAD, PB_STOP, PB_OBSERVE, PB_BEGIN, PB_COMMIT, PB_CANCEL, PB_CONTROL,
       PB_DIAGNOSTICS, PB_INSPECT, PB_EVALUATE, PB_VIEWPORT, PB_RELEASE };
enum { PB_READY = 1, PB_FRAME, PB_URL, PB_ERROR, PB_CLOSED, PB_OBSERVATION, PB_TEXT_RESULT, PB_STOPPED, PB_RUNTIME_READY };
enum { PB_MAX_DIMENSION = 4096, PB_URL_CAP = 2048, PB_MAX_STEPS = 128, PB_MAX_ACTIONS = 16, PB_MAX_PNG = 64 * 1024 * 1024 };
enum { PB_POOL_SIZE = 3, PB_MAX_PLANES = 4 };
/* PB_FRAME carries only this metadata plus plane_count DMA-BUF descriptors and
 * one acquire sync_file, attached to the event header with SCM_RIGHTS. A slot
 * is leased until PB_RELEASE echoes its serial in request and slot in a. The
 * release carries a sync_file after the last host sample, or the untouched
 * acquire fence when dropping an unimported frame. No descriptor numbers are
 * serialized. Serial identities are never reused within a browser token. */
typedef struct PbFrame {
    uint64_t serial, modifier;
    uint32_t slot, width, height, format, plane_count;
    uint32_t stride[PB_MAX_PLANES], offset[PB_MAX_PLANES];
} PbFrame;
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
/* Large text stays in private temporary files, never in a bounded IPC string.
 * Every successful reply owns an immutable complete file. The host adopts or
 * removes it; input argument files remain owned by the requesting worker. */
typedef struct PbTextResult {
    uint64_t request, size;
    uint32_t error;
    char path[256], failure[512];
} PbTextResult;
#endif
