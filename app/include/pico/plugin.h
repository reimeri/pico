#ifndef PICO_PLUGIN_H
#define PICO_PLUGIN_H

#include "pico/app.h"

#define PICO_EXT_ABI 6

// User/agent extensions export a function named pico_ext with this signature.
typedef struct PicoExt {
    int abi;
    const char *name;
    const char *description; /* optional; shown in /extensions */
    void (*init)(PicoApp *app);
    void (*shutdown)(PicoApp *app);
    void (*on_frame)(PicoApp *app, float dt);
} PicoExt;

PicoExt pico_ext_chat(void);
PicoExt pico_ext_composer(void);
PicoExt pico_ext_footer(void);
PicoExt pico_ext_overlay(void);
PicoExt pico_ext_ask_user(void);
PicoExt pico_ext_todo(void);
PicoExt pico_ext_shell(void);
PicoExt pico_ext_subagent(void);
PicoExt pico_ext_commands(void);
PicoExt pico_ext_files(void);
PicoExt pico_ext_openai(void);
PicoExt pico_ext_extensions(void);
PicoExt pico_ext_prompt(void);

#endif
