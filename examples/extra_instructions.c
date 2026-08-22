// Append extra system instructions on every LLM round.
// Copy to ~/.config/pico/extensions/ then F5.
//
//   mkdir -p ~/.config/pico/extensions/extra
//   cp examples/extra_instructions.c ~/.config/pico/extensions/extra/

#include "pico/plugin.h"
#include "json.h"

static void ExtraLlm(PicoApp *app, PicoAgentId agent_id, PicoLlmEvent *ev)
{
    (void)app;
    (void)agent_id;
    ev->extra_instructions = JsonDup("Prefer short answers.");
}

static void ExtraInit(PicoApp *app)
{
    pico_add_llm_hook(app, ExtraLlm);
}

PicoExt pico_ext(void)
{
    return (PicoExt){
        .abi = PICO_EXT_ABI,
        .name = "extra_instructions",
        .description = "Append a line to the system prompt each LLM round",
        .init = ExtraInit,
    };
}
