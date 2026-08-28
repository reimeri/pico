// Example Pico command extension. Copy to ~/.config/pico/extensions/ or
// <workspace>/.pico/extensions/ (a subfolder is fine) then press F5.
//
//   mkdir -p ~/.config/pico/extensions/time
//   cp examples/time_cmd.c ~/.config/pico/extensions/time/

#include "pico/plugin.h"

#include <time.h>

static void TimeCmd(PicoHost *app, PicoAgentId agent_id, const char *args, void *state)
{
    (void)state;
    (void)args;
    time_t now = time(NULL);
    char *line = ctime(&now);
    PicoHost_AddMessage(app, agent_id, PICO_ROLE_ASSISTANT, line ? line : "(no time)");
    PicoComposer_SetText(app, "");
    PicoHost_RequestSubmitCancel(app);
}

static int TimeInit(PicoHost *app, void **state_out)
{
    (void)state_out;
    pico_host_add_command(app, "time", "Show the local time", TimeCmd);
    return 0;
}

PicoExt pico_ext(void)
{
    return (PicoExt){
        .abi = PICO_EXT_ABI,
        .name = "time",
        .description = "Show the local time",
        .host_init = TimeInit,
    };
}
