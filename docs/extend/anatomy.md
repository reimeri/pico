# Anatomy

Every user extension is one `.c` file that exports `pico_ext`. ABI 13 splits host and workspace instances: callbacks take `PicoHost *` or `PicoWorkspace *` plus instance `void *state`. There is no compatibility layer.

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

`abi` must be `PICO_EXT_ABI` (currently 13). `name` is for diagnostics and `/extensions`. Optional:

- `description` — one-line summary in the `/extensions` modal. String literal, like `name`.
- `host_init` / `workspace_init` — register through the matching context. Return 0 on success, nonzero on failure.
- `host_shutdown` / `workspace_shutdown` — release instance state after the instance is quiescent.
- `host_on_frame` / `workspace_on_frame` — main thread, once per frame, `dt` in seconds. Workspace `on_frame` must not draw.

A builtin or user-global source may provide host callbacks, workspace callbacks, or both. A source under `<workspace>/.pico/extensions/` must set every host callback to `NULL`. If it does not, activation fails and the previous active generation remains in use.

## Instances and state

Each loaded source becomes a **module generation**. Pico then creates an **extension instance** for the host and/or for each workspace that activates that generation.

Before calling `*_init`, core sets `*state_out = NULL`. Init may allocate instance state and write it to `*state_out`. `state` may be NULL for a stateless extension. Every registered callback receives that instance's `void *state` as its final parameter.

Mutable extension state must live in the instance. Do not use writable file-static variables. Constants and immutable lookup tables may remain static. Two workspace instances of the same module receive distinct state pointers.

If init fails and `*state_out` is non-NULL and shutdown exists, core calls shutdown exactly once. Shutdown is called exactly once for every successfully initialized instance, in reverse initialization order, after the instance is quiescent.

## Registration scope

Registration functions are valid only during the matching init callback. Host init may register only host registrations; workspace init may register only workspace registrations. Invalid-scope registration fails and appends a scoped warning. Callers never supply another workspace ID while registering; the init context is the owner.

| Registration/API | Host init | Workspace init |
|---|---:|---:|
| Global window views and overlays | yes | no |
| Contextual views shown for selected workspace | no | yes |
| Empty-chat views for selected agent | no | yes |
| Global commands/completers | yes | no |
| Workspace commands/completers | no | yes |
| Auth login/logout registration | yes | no |
| Tools and providers | no | yes |
| Before/after tool hooks | no | yes |
| LLM/context/tool-row hooks | no | yes |
| Agent lifecycle/message/submit hooks | no | yes |
| Host after-layout/after-render hooks | yes | no |
| Workspace `on_frame` | no | descriptor callback |
| Host `on_frame` | descriptor callback | no |

Use separate function names where `pico_add_*` would be ambiguous, for example `pico_host_add_view` and `pico_workspace_add_view`.

Workspace contextual views receive the workspace pointer and explicit selected agent ID. They are called only when the selected agent belongs to that workspace. Workspace `on_frame` runs for every `OPEN` or `RELOADING` workspace, selected or not; it must not draw with Clay or Raylib. Rendering remains a view callback.

There is no unregister. Registrations made during init are staged and are not visible until init succeeds. If init fails, staged registrations are discarded. A workspace rebuild stages a complete **registration generation** and publishes it atomically only after every required instance initializes successfully. On staging failure, staged instances shut down in reverse order and the previous complete generation stays active.

A turn retains the exact immutable registration generation used by its accepted turn until that turn and all of its references quiesce. Do not cache registration pointers beyond their documented callback lifetime.

## Module generations

User modules load with `RTLD_NOW | RTLD_LOCAL` (never `RTLD_GLOBAL`). Compilation writes a unique temporary output and atomically renames it to a generation-specific cache path. A compile or descriptor-validation failure leaves every workspace on its current working module generation.

Old and new module generations may coexist. A module generation may be `dlclose`d only when its reference count reaches zero and it is no longer the desired generation for any source.

A dual-scope user-global source has independent active-generation slots: one host slot and one slot per workspace. Host activation success/failure does not publish or roll back any workspace slot, and the reverse is also true. Each slot keeps its previous generation on failure.

`/extensions` (or F2) lists one record per active slot. `PICO_EXTENSION_HOST` uses workspace ID zero; `PICO_EXTENSION_WORKSPACE` uses an explicit workspace ID. Each copied record includes source, name, scope, workspace ID, enabled, desired module-generation ID, active module-generation ID, and the latest scoped error. A dual-scope source is not collapsed into one enabled/active row.

Click a row to toggle it off or on. Disabled extensions stay in the registry (compiled/`dlopen` for user sources) but skip `init` / `on_frame`. The builtin `extensions` manager and `settings` editor cannot be turned off. Host toggles persist as `disabled_host_extensions` in user-global `~/.config/pico/settings.json`. Workspace toggles persist as `disabled_extensions` in `<workspace>/.pico/settings.json`. They apply through the same reload path as F5. Disabling one scope/instance does not disable another.

## Directories

- User: `~/.config/pico/extensions/` or `$XDG_CONFIG_HOME/pico/extensions/`
- Workspace: `<workspace>/.pico/extensions/`
- Compiled objects: `~/.cache/pico/ext/` (or `$XDG_CACHE_HOME/pico/ext/`)

`pico --safe` loads builtins only.

## Compile

Pico runs `${PICO_CC:-cc}` as:

```text
cc -shared -fPIC -std=c99 -I<pico SDK>/include -I<source dir> -o <cache>.so <file.c>
```

The packaged SDK is `share/pico/sdk/include` relative to Pico's install prefix and contains the public `pico/` headers plus the Clay, Raylib, JSON, markdown, and text-range headers those contracts expose. Development builds create the same `sdk/include` layout beside the executable. `PICO_DATA_DIR` can override the runtime data root when testing a staged package.

The source directory is also on the include path, so local headers next to the `.c` file work. GCC/Clang dependency files record included project and SDK headers; their contents participate in the compiled-cache key and change detection, even when timestamps are unchanged. Compilation runs as a polled subprocess with a 30-second deadline. The frame loop keeps rendering and processing input while it builds; descriptor validation and activation run on the main thread after success. Compiler diagnostics are bounded and drained without blocking, and obsolete build outputs are discarded. Packaged archives and AppImages require a host C99 compiler; the Nix package supplies GCC. Compile failures, a missing compiler/SDK, and failed `pico_add_tool` registrations show in the overlay; the previous working module generation remains active when a replacement fails. Polling does not recompile an unchanged failed source; F5 and `/reload` still retry.

## Reload

F5, `/reload`, toggling an extension in `/extensions`, or a `.c` or recorded header content change (polled ~0.5s). An unchanged compile failure is not polled again until the source changes or the user reloads. Host-extension replacement compiles user-global (config) sources only and happens between frames; it does not wait for workspace workers. Workspace-local `.c` files reload with that workspace. A compile failure in one workspace does not block host reload or another workspace.

Workspace reload is **deferred** until that workspace's live/retired runtimes, pending asks, offered catalogs, events, and delegation jobs are quiescent. A queued workspace reload prevents new turns and delegations only in that workspace. After publication, old instances shut down once they are quiescent; retained runtimes keep their exact generation alive until their callbacks and queued events finish. Reload then validates the complete named-profile registry against the new tools/models, revalidates copied restricted agent policies, sends session-reset notification for every live agent, and replays structured tool details.

`/cd` opens or selects a workspace. Relative paths resolve against the command agent's workspace. The previous workspace stays open; returning to an already-open canonical path reuses it. The workspace path is immutable after open. See [workspace](workspace.md).

Builtins: `chat`, `composer`, `footer`, `overlay`, `ask-user`, `todos`, `sh`, `subagent`, `commands`, `files`, `openai`, `hyper`, `xai`, `extensions`, `settings`, `prompt`, `diff`. `/extensions` or F2 lists them.
