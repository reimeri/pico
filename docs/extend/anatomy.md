# Anatomy

Every user extension is one `.c` file that exports `pico_ext`:

```c
#include "pico/plugin.h"

static int MyHostInit(PicoHost *host, void **state_out)
{
    (void)state_out;
    pico_host_add_view(host, PICO_SLOT_SIDEBAR, 0, MyRender);
    return 0;
}

static int MyWorkspaceInit(PicoWorkspace *workspace, void **state_out)
{
    (void)state_out;
    pico_add_tool(workspace, "mytool", "Example tool", "{}", MyRun, NULL);
    return 0;
}

PicoExt pico_ext(void)
{
    return (PicoExt){
        .abi = PICO_EXT_ABI,
        .name = "myext",
        .description = "Short summary shown in /extensions",
        .host_init = MyHostInit,
        .workspace_init = MyWorkspaceInit,
    };
}
```

`abi` must be `PICO_EXT_ABI` (currently 13). ABI 13 splits host and workspace instances: callbacks take `PicoHost *` or `PicoWorkspace *` plus instance `void *state`. There is no compatibility layer. `name` is for diagnostics and `/extensions`. Optional:

- `description` — one-line summary in the `/extensions` modal. String literal, like `name`.
- `host_init` / `workspace_init` — register through the matching context. Return 0 on success.
- `host_shutdown` / `workspace_shutdown` — release instance state after the instance is quiescent.
- `host_on_frame` / `workspace_on_frame` — main thread, once per frame, `dt` in seconds. Workspace `on_frame` must not draw.

There is no unregister. Reload stages a complete new module/registration generation, and publishes it only after the candidate instances initialize successfully. The previous generation and instance state remain active when compilation, loading, validation, or initialization fails. After publication, old instances shut down once they are quiescent; retained runtimes keep their exact generation alive until their callbacks and queued events finish. Reload then validates the complete named-profile registry against the new tools/models, revalidates copied restricted agent policies, sends session-reset notification for every live agent, and replays structured tool details.

`/extensions` (or F2) lists builtins and user sources. Click a row to toggle it off or on. Disabled extensions stay in the registry (compiled/`dlopen` for user sources) but skip `init` / `on_frame`. The builtin `extensions` manager cannot be turned off. Toggles persist as `disabled_host_extensions` in user `~/.config/pico/host_preferences.json` (or per-workspace `disabled_extensions` in `<workspace>/.pico/settings.json`) and apply through the same reload path as F5.

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

The source directory is on the include path, so local headers next to the `.c` file work. Compilation uses a unique temporary output and atomic publication of a content-hashed, ABI-specific cache object. Compile failures and failed `pico_add_tool` registrations show in the overlay; the previous working module generation remains active when a replacement fails.

## Reload

F5, `/reload`, toggling an extension in `/extensions`, or a `.c` content change (polled ~0.5s). Reload is **deferred** until every live/retired runtime, pending ask, offered catalog, event, and delegation job is quiescent. A queued reload prevents new turns and delegations. Each accepted turn retains one immutable registration generation; do not cache registration pointers beyond their documented callback lifetime.

`/cd` queues a workspace transition behind the same barrier; the live workspace path is immutable and changes only when the old agent set can be replaced as one main-thread transition.

Builtins: `chat`, `composer`, `footer`, `overlay`, `ask-user`, `todos`, `sh`, `subagent`, `commands`, `files`, `openai`, `hyper`, `xai`, `extensions`, `prompt`, `diff`. `/extensions` or F2 lists them.
