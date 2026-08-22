// Example Pico command extension. Copy to ~/.config/pico/extensions/
// (a subfolder is fine) then press F5.
//
//   mkdir -p ~/.config/pico/extensions/time
//   cp examples/time_cmd.c ~/.config/pico/extensions/time/

#include "pico/plugin.h"

#include <time.h>

static void TimeCmd(PicoApp *app, const char *args)
{
    (void)args;
    time_t now = time(NULL);
    char *line = ctime(&now);
    PicoApp_AddMessage(app, PICO_ROLE_ASSISTANT, line ? line : "(no time)");
    PicoComposer_SetText(app, "");
    app->submit_cancel = true;
}

static void TimeInit(PicoApp *app)
{
    pico_add_command(app, "time", "Show the local time", TimeCmd);
}

PicoExt pico_ext(void)
{
    return (PicoExt){
        .abi = PICO_EXT_ABI,
        .name = "time",
        .description = "Show the local time",
        .init = TimeInit,
    };
}
