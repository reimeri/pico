# Anatomy

Every user extension is one `.c` file that exports `pico_ext`:

```c
#include "pico/plugin.h"

static void MyInit(PicoApp *app)
{
    /* pico_add_view / pico_add_tool / pico_add_tool_hook / pico_add_command / … */
    (void)app;
}

PicoExt pico_ext(void)
{
    return (PicoExt){
        .abi = PICO_EXT_ABI,
        .name = "myext",
        .description = "Short summary shown in /extensions",
        .init = MyInit,
    };
}
```

`abi` must be `PICO_EXT_ABI` (currently 2). `name` is for diagnostics and `/extensions`. Optional:

- `description` — one-line summary in the `/extensions` modal. String literal, like `name`.
- `init` — register views/tools/hooks/commands. Called on load and after every reload.
- `shutdown` — release extension-owned memory/threads before `dlclose`.
- `on_frame` — main thread, once per frame, `dt` in seconds.

There is no unregister. Reload clears all registrations and calls `init` again (builtins too).

## Directories

- User: `~/.config/pico/extensions/` or `$XDG_CONFIG_HOME/pico/extensions/`
- Workspace: `<workspace>/.pico/extensions/`
- Compiled objects: `~/.cache/pico/ext/` (or `$XDG_CACHE_HOME/pico/ext/`)

`pico --safe` loads builtins only.

## Compile

Pico runs the configured C compiler (`gcc` unless CMake set `PICO_CC`) as:

```text
cc -shared -fPIC -std=c99 -I<pico headers> -I<app> -I<clay> -I<raylib> -I<source dir> -o <cache>.so <file.c>
```

The source directory is on the include path, so local headers next to the `.c` file work. Failures show in the overlay; the previous working `.so` is not reused when mtime changes.

## Reload

F5, `/reload`, or a `.c` mtime change (polled ~0.5s). Reload is **deferred** while the agent is in LLM/tool/compact wait. Tell the user to wait until idle, or run `/reload` after.

Builtins: `chat`, `composer`, `footer`, `overlay`, `sh`, `commands`, `files`, `openai`, `extensions`. `/extensions` lists them.
