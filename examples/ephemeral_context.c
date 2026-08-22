// Example Pico request-context extension. Copy to ~/.config/pico/extensions/ or
// <workspace>/.pico/extensions/ then press F5.

#include "pico/plugin.h"
#include "json.h"

static void AddContext(PicoApp *app, PicoAgentId agent_id, PicoContextEvent *ev)
{
    (void)app;
    (void)agent_id;
    if (!ev->compact)
    {
        ev->extra_context = JsonDup("Verify the observable result before finishing.");
    }
}

static void ContextInit(PicoApp *app)
{
    pico_add_context_hook(app, AddContext);
}

PicoExt pico_ext(void)
{
    return (PicoExt){
        .abi = PICO_EXT_ABI,
        .name = "ephemeral-context",
        .description = "Request-only context example",
        .init = ContextInit,
    };
}
