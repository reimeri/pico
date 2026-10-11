#ifndef PICO_SESSION_REF_H
#define PICO_SESSION_REF_H

#include <stdbool.h>
#include <stddef.h>

/* Private portable text codec. Serialized references are Markdown links whose
 * pico-session target carries identity, never an instruction to open a session. */
typedef struct PicoSessionRef {
    int start;
    int end;
    char title[289];
    char id[40];
    char checkout[4096];
    char path[4096];
} PicoSessionRef;

char *PicoSessionRef_Serialize(const PicoSessionRef *ref);
bool PicoSessionRef_Parse(const char *text, int length, int at, PicoSessionRef *out);
bool PicoSessionRef_Next(const char *text, int length, int from, PicoSessionRef *out);
bool PicoSessionRef_FromLink(const char *label, int length, const char *url, PicoSessionRef *out);
/* Snap only offsets inside references; boundaries remain unchanged. */
int PicoSessionRef_Snap(const char *text, int length, int offset, bool forward);
/* NULL means allocation failure; ordinary text and unavailable paths survive. */
char *PicoSessionRef_Project(const char *text);

#endif
