#ifndef PICO_SESSION_H
#define PICO_SESSION_H

#include "pico/app.h"

#include <time.h>

typedef struct PicoSessionInfo {
    char path[4096];
    char id[40];
    char title[256];
    time_t mtime;
} PicoSessionInfo;

void PicoSession_Start(PicoApp *app, PicoSessionStart start, const char *session_file);
int PicoSession_List(const PicoApp *app, PicoSessionInfo **out);
int PicoSession_Open(PicoApp *app, const char *id);
void PicoSession_Reset(PicoApp *app);
void PicoSession_LogUser(PicoApp *app, const char *content, const char *display);
void PicoSession_LogUsage(PicoApp *app, int input_tokens, int cached_tokens);
void PicoSession_LogAssistant(PicoApp *app, const char *content);
void PicoSession_LogToolCall(PicoApp *app, const char *call_id, const char *name, const char *args);
void PicoSession_LogToolResult(PicoApp *app, const char *call_id, const char *output, bool is_error);
void PicoSession_LogCompaction(PicoApp *app, const char *summary, int tokens_before);
void PicoSession_LogModelChange(PicoApp *app, const char *model, const char *effort);
void PicoSession_LogCustom(PicoApp *app, const char *ext, const char *data_json);

#endif
