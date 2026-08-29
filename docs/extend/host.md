# Host

`PicoHost` is the window owner. The CLI constructs one host per process. Tests may construct independent hosts; each owns its live workspaces, process services, module generations, host-scoped extension instances, and UI state. It is opaque; include `pico/host.h` (or `pico/plugin.h`).

A host is not a workspace. Backend work always takes an explicit workspace or agent ID. `pico_agent_active` / `pico_agent_select` are UI selection only.

## Lifecycle

```c
PicoHost *host = NULL;
if (pico_host_init(&host, fonts, false) != PICO_OK)
{
    return;
}
PicoWorkspaceId ws = 0;
if (pico_workspace_open(host, path, &ws) != PICO_OK)
{
    pico_host_free(host);
    return;
}
PicoAgentId agent = 0;
PicoAgentCreateOptions opt = {.kind = PICO_AGENT_MAIN};
if (pico_main_agent_create(host, ws, &opt, &agent) != PICO_OK)
{
    pico_host_free(host);
    return;
}
pico_agent_select(host, agent);

/* each frame: */
pico_host_pump(host);

PicoHostShutdownResult rc = pico_host_free(host);
```

Startup convenience (CLI) is this sequence, not a special lifecycle: init, open the initial directory, create one main agent, optional session resume on that agent, then select it. Opening a workspace does not create a main agent.

`pico_host_pump` does one bounded round-robin pass: host posts and process services, then each non-closed workspace once (at most 256 queued runtime events and one new delegation per workspace), then each eligible workspace-extension `on_frame` and each host-extension `on_frame` exactly once with the current frame delta. Contextual views render only for the selected workspace. Inactive workspaces still pump and still run workspace `on_frame`; they must not draw.

`pico_host_free` applies the process-wide bounded shutdown deadline of about one second. It returns `PICO_HOST_SHUTDOWN_CLEAN` when every worker joins, or `PICO_HOST_SHUTDOWN_RETAINED` when a callback is still blocked. Retained shutdown detaches that callback and keeps every registration, auth store, builtin state, and user-extension `.so` it can reach. No extension `shutdown`, `dlclose`, auth destruction, or curl cleanup runs. Pico is then permanently retired in that process; later `pico_host_init` is rejected. Only `pico_host_free` uses this process deadline. Workspace close never does.

## Limits

- `PICO_MAX_WORKSPACES` (8) live workspaces
- `PICO_MAX_AGENTS` (16) agents per workspace, including subagents
- `PICO_MAX_TOTAL_AGENTS` (32) agents across the host

Creation returns `PICO_LIMIT` when either agent cap is reached. Zero is invalid for every ID. Each host allocates IDs on the main thread and never reuses them for the lifetime of that host. Independent hosts have separate ID spaces.

## Preferences

Host-only window settings live in `PicoHostPreferences` and are stored in `~/.config/pico/host_preferences.json` (or `$XDG_CONFIG_HOME/pico/host_preferences.json`):

- `font_scale` (default 1.0; `PICO_FONT_SCALE` overrides)
- `chat_width` (default 75 characters)
- `disabled_host_extensions` — disables host instances only

They never come from a workspace `.pico/settings.json`. Disabling a host instance does not disable that module's workspace instances, and the reverse is also true. Writes use a unique temporary file, `fsync`, atomic rename, and containing-directory `fsync`, under one host mutex.

Auth secrets remain host-global in `~/.config/pico/auth.json`. See [auth](auth.md).

## Host-scoped registrations

Register these only from `host_init`. Workspace init cannot add them:

- Global window views and overlays (`pico_host_add_view`)
- Global commands and completers (`pico_host_add_command`, `pico_host_add_completer`)
- Auth login/logout (`pico_add_auth`)
- Host after-layout / after-render hooks (`pico_host_add_hook`)
- Descriptor `host_on_frame`

Host builtins include chat renderer, composer, footer shell, overlay presentation, extension manager UI, prompt UI, clipboard state, and auth UI. Dual-scope builtins keep separate host and workspace states connected only through core APIs and IDs.

Host-extension replacement happens between frames. Host extensions cannot register worker callbacks, so only active host callback depth must reach zero before replacement. F5 and `/reload` reload host extensions immediately (user-global / config sources only) and request reload of the selected agent's workspace; other workspaces keep accepting work. See [workspace](workspace.md) and [anatomy](anatomy.md).

## Routing

Agent-targeted calls locate the workspace by scanning the host's bounded arrays. Pass an explicit `PicoAgentId`. Composer submit and slash commands snapshot the selected ID once; a later selection change cannot retarget that action.

`pico_tool_pending_ask` returns the oldest live ask across all workspaces by host-allocated ask ID. Answer with `pico_tool_answer` using that ID. Closing a workspace cancels its pending asks.

Named UI mailboxes are workspace-owned and keyed by `(agent_id, runtime_generation, name)`. Main-thread lookup is `pico_agent_ui_latest` / `pico_agent_ui_clear`. `pico_ui_latest` / `pico_ui_clear` read or clear the UI-selected agent's mailbox of that name.

## Contract

- Do not store a `PicoHost *` on an agent; reach the host through `pico_workspace_host(workspace)` on the main thread.
- Workers receive `PicoAgentContext *`, never `PicoHost *`.
- Main-thread callbacks must return promptly and must never wait on worker completion. A stuck main-thread callback blocks the whole host.
- Pico and its extensions never call `chdir()` in the host process.
- Stale workspace, agent, ask, and runtime-generation IDs fail without affecting a different object.
