#ifndef PICO_PLUGIN_H
#define PICO_PLUGIN_H

#include "pico/app.h"
#include "pico/host.h"
#include "pico/workspace.h"

#define PICO_EXT_ABI 13 /* host and workspace instances; no ABI 12 loader */

/* Host callbacks may run before any workspace exists; active agent ID may be zero. */
typedef int (*PicoHostExtInitFn)(PicoHost *host, void **state_out);
typedef void (*PicoHostExtShutdownFn)(PicoHost *host, void *state);
typedef void (*PicoHostExtFrameFn)(PicoHost *host, void *state, float dt);

typedef int (*PicoWorkspaceExtInitFn)(PicoWorkspace *workspace, void **state_out);
typedef void (*PicoWorkspaceExtShutdownFn)(PicoWorkspace *workspace, void *state);
typedef void (*PicoWorkspaceExtFrameFn)(PicoWorkspace *workspace, void *state, float dt);

typedef struct PicoExt {
    int abi;
    const char *name;
    const char *description;
    PicoHostExtInitFn host_init;
    PicoHostExtShutdownFn host_shutdown;
    PicoHostExtFrameFn host_on_frame;
    PicoWorkspaceExtInitFn workspace_init;
    PicoWorkspaceExtShutdownFn workspace_shutdown;
    PicoWorkspaceExtFrameFn workspace_on_frame;
} PicoExt;

PicoExt pico_ext_chat(void);
PicoExt pico_ext_composer(void);
PicoExt pico_ext_footer(void);
PicoExt pico_ext_sidebar(void);
PicoExt pico_ext_overlay(void);
PicoExt pico_ext_notify(void);
PicoExt pico_ext_ask_user(void);
PicoExt pico_ext_todo(void);
PicoExt pico_ext_shell(void);
PicoExt pico_ext_background(void);
PicoExt pico_ext_subagent(void);
PicoExt pico_ext_commands(void);
PicoExt pico_ext_files(void);
PicoExt pico_ext_skills(void);
PicoExt pico_ext_openai(void);
PicoExt pico_ext_hyper(void);
PicoExt pico_ext_xai(void);
PicoExt pico_ext_extensions(void);
PicoExt pico_ext_settings(void);
PicoExt pico_ext_prompt(void);
PicoExt pico_ext_diff(void);

#endif
