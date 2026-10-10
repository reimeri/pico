// Workspace extension: /nudge queues input for the explicit running agent.
// Copies, lifecycle, async persistence, and delivery follow docs/extend/steering.md
// and contracts.md. No worker thread or automatic retry is involved.
#include "pico/plugin.h"

static void Nudge(PicoWorkspace *workspace, PicoAgentId id, const char *args, void *state)
{
    (void)state;
    PicoHost *host = pico_workspace_host(workspace);
    PicoResult result = pico_agent_steer(host, id, args, NULL);
    if (result == PICO_OK)
        PicoComposer_SetText(host, "");
    else
        PicoHost_AddNotice(host, id, PICO_NOTICE_WARNING,
                          "Steering was not accepted; keep the draft and try during an active turn.");
    PicoHost_RequestSubmitCancel(host);
}

static int Init(PicoWorkspace *workspace, void **state_out)
{
    (void)state_out;
    pico_workspace_add_command(workspace, "nudge", "Queue steering for this active turn", Nudge);
    pico_workspace_command_allow_while_busy(workspace, "nudge");
    return 0;
}

PicoExt pico_ext(void)
{
    return (PicoExt){.abi = PICO_EXT_ABI, .name = "steering",
                      .description = "Explicit-target steering example", .workspace_init = Init};
}
