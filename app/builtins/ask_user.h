#ifndef PICO_BUILTINS_ASK_USER_H
#define PICO_BUILTINS_ASK_USER_H

#include <stddef.h>

/* Internal questionnaire validation used by the builtin and its tests. */
char *PicoAskUser_BuildRequest(const char *args_json, char *error, size_t error_cap);

#endif
