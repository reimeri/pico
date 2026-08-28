// Append extra system instructions on every LLM round.
// Copy to ~/.config/pico/extensions/ or <workspace>/.pico/extensions/ then F5.
//
//   mkdir -p ~/.config/pico/extensions/extra
//   cp examples/extra_instructions.c ~/.config/pico/extensions/extra/

#include "pico/plugin.h"
#include "json.h"

static void ExtraLlm(PicoWorkspace *workspace, PicoAgentId agent_id, PicoLlmEvent *event, void *state)
{
    (void)workspace;
    (void)state;
    (void)agent_id;
    event->extra_instructions = JsonDup("Prefer short answers.");
}

static int ExtraInit(PicoWorkspace *workspace, void **state_out)
{
    (void)state_out;
    pico_add_llm_hook(workspace, ExtraLlm);
    return 0;
}

PicoExt pico_ext(void)
{
    return (PicoExt){
        .abi = PICO_EXT_ABI,
        .name = "extra_instructions",
        .description = "Append a line to the system prompt each LLM round",
        .workspace_init = ExtraInit,
    };
}
