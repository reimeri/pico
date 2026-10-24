// Workspace-scoped request-context hook. Copy to ~/.config/pico/extensions/ or
// <workspace>/.pico/extensions/ then press F5.
// See /docs context and providers: native compaction skips request-context hooks;
// text compaction still invokes them, so explicitly exclude that request here.

#include "pico/plugin.h"
#include "json.h"

static void AddContext(PicoWorkspace *workspace, PicoAgentId agent_id, PicoContextEvent *event, void *state)
{
    (void)workspace;
    (void)state;
    (void)agent_id;
    if (!event->compact)
    {
        event->extra_context = JsonDup("Verify the observable result before finishing.");
    }
}

static int ContextInit(PicoWorkspace *workspace, void **state_out)
{
    (void)state_out;
    pico_add_context_hook(workspace, AddContext);
    return 0;
}

PicoExt pico_ext(void)
{
    return (PicoExt){
        .abi = PICO_EXT_ABI,
        .name = "ephemeral-context",
        .description = "Request-only context example",
        .workspace_init = ContextInit,
    };
}
