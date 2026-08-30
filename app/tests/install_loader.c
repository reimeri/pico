#include "pico/plugin.h"

#include <dlfcn.h>
#include <stdio.h>
#include <string.h>

void pico_host_add_view(PicoHost *host, PicoUiSlot slot, int z, PicoHostViewFn render)
{
    (void)host;
    (void)slot;
    (void)z;
    (void)render;
}

int main(int argc, char **argv)
{
    if (argc != 2)
    {
        fprintf(stderr, "usage: %s EXTENSION\n", argv[0]);
        return 2;
    }
    void *handle = dlopen(argv[1], RTLD_NOW | RTLD_LOCAL);
    if (!handle)
    {
        fprintf(stderr, "dlopen: %s\n", dlerror());
        return 1;
    }
    PicoExt (*entry)(void) = (PicoExt(*)(void))dlsym(handle, "pico_ext");
    if (!entry)
    {
        fprintf(stderr, "dlsym: %s\n", dlerror());
        dlclose(handle);
        return 1;
    }
    PicoExt ext = entry();
    int ok = ext.abi == PICO_EXT_ABI && ext.name && strcmp(ext.name, "install-smoke") == 0;
    dlclose(handle);
    if (!ok)
    {
        fprintf(stderr, "installed extension descriptor did not round-trip\n");
        return 1;
    }
    return 0;
}
