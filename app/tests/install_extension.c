#include "pico/plugin.h"

static void SmokeRender(PicoHost *host, void *state)
{
    (void)host;
    (void)state;
}

static int SmokeInit(PicoHost *host, void **state_out)
{
    (void)state_out;
    pico_host_add_view(host, PICO_SLOT_SIDEBAR, 0, SmokeRender);
    return 0;
}

PicoExt pico_ext(void)
{
    return (PicoExt){
        .abi = PICO_EXT_ABI,
        .name = "install-smoke",
        .host_init = SmokeInit,
    };
}
