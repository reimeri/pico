#ifndef PICO_CLI_H
#define PICO_CLI_H

#include "pico/app.h"

typedef enum PicoCliParseResult {
    PICO_CLI_OK = 0,
    PICO_CLI_HELP,
    PICO_CLI_ERROR,
} PicoCliParseResult;

typedef struct PicoCliOptions {
    bool safe_mode;
    bool no_workspace;
    bool session_option_explicit;
    PicoSessionStart session_start;
    const char *session_file;
} PicoCliOptions;

PicoCliParseResult PicoCli_Parse(int argc, char **argv, PicoCliOptions *out);
bool PicoCli_ShouldOpenDefaultWorkspace(const PicoCliOptions *options);

#endif
