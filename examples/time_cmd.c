// Example host-scoped slash command. Copy to ~/.config/pico/extensions/
// (a subfolder is fine) then press F5. Workspace-local sources must not set
// host callbacks.
//
//   mkdir -p ~/.config/pico/extensions/time
//   cp examples/time_cmd.c ~/.config/pico/extensions/time/

#include "pico/plugin.h"

#include <time.h>

static void TimeCmd(PicoHost *host, PicoAgentId agent_id, const char *args, void *state)
{
    (void)state;
    (void)args;
    time_t now = time(NULL);
    char *line = ctime(&now);
    PicoHost_AddMessage(host, agent_id, PICO_ROLE_ASSISTANT, line ? line : "(no time)");
    PicoComposer_SetText(host, "");
    PicoHost_RequestSubmitCancel(host);
}

static int TimeInit(PicoHost *host, void **state_out)
{
    (void)state_out;
    pico_host_add_command(host, "time", "Show the local time", TimeCmd);
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
