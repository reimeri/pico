#ifndef PICO_PLUGIN_H
#define PICO_PLUGIN_H

#include "pico/app.h"

#define PICO_EXT_ABI 1

// User/agent extensions export a function named pico_ext with this signature.
typedef struct PicoExt {
    int abi;
    const char *name;
    void (*init)(PicoApp *app);
    void (*shutdown)(PicoApp *app);
    void (*on_frame)(PicoApp *app, float dt);
} PicoExt;

PicoExt pico_ext_chat(void);
PicoExt pico_ext_composer(void);
PicoExt pico_ext_footer(void);
PicoExt pico_ext_overlay(void);

#endif
