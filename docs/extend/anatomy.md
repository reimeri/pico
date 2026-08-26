# Anatomy

Every user extension is one `.c` file that exports `pico_ext`:

```c
#include "pico/plugin.h"

static void MyInit(PicoApp *app)
{
    /* pico_add_view / pico_add_tool / pico_add_tool_before_hook / pico_add_command / … */
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

`abi` must be `PICO_EXT_ABI` (currently 10). ABI 10 adds the named UI mailbox (`pico_ui_post` / `pico_ui_latest` / `pico_ui_clear`). ABI 9 added the named modal stack (`pico_ui_modal_push` / `pop`) and tool-row click hooks (`pico_add_tool_row_hook`). There is no compatibility layer. `name` is for diagnostics and `/extensions`. Optional:

- `description` — one-line summary in the `/extensions` modal. String literal, like `name`.
- `init` — register views/tools/hooks/commands. Called on load and after every reload when the extension is enabled.
- `shutdown` — release extension-owned memory/threads before `dlclose`.
- `on_frame` — main thread, once per frame, `dt` in seconds.

There is no unregister. Reload clears all registrations and calls `init` again for enabled extensions (builtins too). It then validates the complete named-profile registry against the new tools/models, revalidates copied restricted agent policies, sends session-reset notification for every live agent, and replays structured tool details.

`/extensions` (or F2) lists builtins and user sources. Click a row to toggle it off or on. Disabled extensions stay in the registry (compiled/`dlopen` for user sources) but skip `init` / `on_frame`. The builtin `extensions` manager cannot be turned off. Toggles persist as `disabled_extensions` in user `~/.config/pico/settings.json` (not workspace settings) and apply through the same reload path as F5.

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

The source directory is on the include path, so local headers next to the `.c` file work. Compile failures and failed `pico_add_tool` registrations show in the overlay; the previous working `.so` is not reused when mtime changes.

## Reload

F5, `/reload`, toggling an extension in `/extensions`, or a `.c` mtime change (polled ~0.5s). Reload is **deferred** until every live/retired runtime, pending ask, offered catalog, event, and delegation job is quiescent. A queued reload prevents new turns and delegations. Do not cache registration pointers beyond their documented callback lifetime.

`/cd` queues a workspace transition behind the same barrier; `app->workspace` changes only when the old agent set can be replaced as one main-thread transition.

Builtins: `chat`, `composer`, `footer`, `overlay`, `ask-user`, `todos`, `sh`, `subagent`, `commands`, `files`, `openai`, `hyper`, `xai`, `extensions`, `prompt`, `diff`. `/extensions` or F2 lists them.
