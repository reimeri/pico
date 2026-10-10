# Commands

Slash commands are handled on submit, not sent to the model.

Commands can be registered at two scopes:
- **Host commands** (`pico_host_add_command`) — registered during `host_init`. Callback signature: `void (*PicoHostCmdFn)(PicoHost *host, PicoAgentId agent_id, const char *args, void *state);` (e.g. `/quit`, `/help`, `/docs`, `/login`, `/logout`). `/reload` is a host command that also requests reload of the command agent's workspace.
- **Workspace commands** (`pico_workspace_add_command`) — registered during `workspace_init`. Callback signature: `void (*PicoWorkspaceCmdFn)(PicoWorkspace *workspace, PicoAgentId agent_id, const char *args, void *state);` (e.g. `/model`, `/effort`, `/new`, `/resume`, `/cd`, `/compact`).

```c
#include "pico/plugin.h"

#include <time.h>

static void TimeCmd(PicoHost *host, PicoAgentId agent_id, const char *args, void *state)
{
    (void)state;
    (void)args;
    time_t now = time(NULL);
    char *line = ctime(&now);
    PicoHost_AddNotice(host, agent_id, PICO_NOTICE_INFO, line ? line : "(no time)");
    PicoComposer_SetText(host, "");
    PicoHost_RequestSubmitCancel(host);
}

static int TimeInit(PicoHost *host, void **state_out)
{
    (void)state_out;
    pico_host_add_command(host, "time", "Show the local time", TimeCmd);
    return 0;
}
```

Full file: [`../../examples/time_cmd.c`](../../examples/time_cmd.c). User types `/time`. `/help` lists every registered command. Builtin `/extensions` (also F2) opens a modal listing installed extensions; click a card to toggle it on or off. Builtin `/settings` (also the sidebar settings button) opens a modal for user-global `settings.json`: default model, catalog, compact ratio, resume-last, max parallel tools, font scale, and chat width. Apply validates and atomically writes the complete draft while preserving comments and unrecognized model properties; disable lists stay in `/extensions`. New agents use the reloaded default immediately, while active turns keep their selected model metadata until they become idle. Builtin `/docs [topic]` prints markdown shipped next to the binary (`docs/extend/`, plus `docs/subagents.md`) into chat — not the compile-time source tree and not `~/.config`. Builtin `/show-prompt` opens a modal with the assembled system prompt for the next turn (`SYSTEM.md`, workspace `.pico/SYSTEM.md`, workspace `AGENTS.md`, the docs hint, and any `pico_add_llm_hook` extras under `## Additional instructions`). The modal color-codes those sources and shows a legend for the ones present. The docs hint uses the same color as `SYSTEM.md` and is the absolute path of that shipped `docs/extend/README.md`. Example sources ship next to the binary in `examples/` (from this page: [`../../examples/`](../../examples/)).

## Contract

- `name` has no leading slash. Completer inserts `/name`.
- `name` and `help` must outlive the extension — string literals.
- `run` receives the snapshotted `PicoAgentId` from the submit that invoked the command, then the rest of the line after `/name` (may be empty). Workspace-scoped command lookup uses the submitting agent's workspace. A later selection change cannot retarget that command. Builtin `/cd` resolves relative paths against that same command workspace, opens or reuses the canonical target, creates a main agent there only if that workspace has none, and leaves the previous workspace open. Builtin `/reload` reloads host extensions and the command agent's workspace only.
- **Call `PicoHost_RequestSubmitCancel(host)`** (or `pico_workspace_host(workspace)` for workspace commands), or the slash line is also sent to the agent. Clear the composer with `PicoComposer_SetText(host, "")`. The alternative to canceling is rewriting the pending submission with `pico_host_set_agent_input(host, text)` — the rewritten text is sent to the agent while the chat row keeps showing the typed slash line (builtin `/skill` loads a skill this way).
- Runs on the **main thread** from `PICO_HOOK_BEFORE_SUBMIT` (builtin `commands` extension). Safe to call `PicoHost_AddMessage(host, agent_id, ...)`. Builtin `/login` and `/logout` forward that same snapshotted ID into auth callbacks, including later device-login notes.
- Ordinary user input queues [steering](steering.md) while a user-main agent is working. Registered slash commands remain blocked while busy unless explicitly opted in. A command that only opens UI or displays information may opt in during its `host_init` / `workspace_init` immediately after registration using `pico_host_command_allow_while_busy(host, name)` / `pico_workspace_command_allow_while_busy(workspace, name)`. They return false unless a command with that name was staged in the current registration scope; repeat the opt-in each time the extension initializes after reload. These commands still run through `PICO_HOOK_BEFORE_SUBMIT`, must clear the composer and cancel submit, and must not start a new turn or disrupt the active one. Other registered slash commands remain blocked and retain the draft. Unrecognized slash-prefixed text is user input, so it queues steering like an ordinary message. A busy-allowed command may call the explicit steering API, but must still cancel the composer submission and must not start a new turn. Builtins `/background`, `/help`, `/docs`, `/settings`, `/extensions`, and `/show-prompt` opt in. The [`modal`](../../examples/modal.c) and [`stream_modal`](../../examples/stream_modal.c) examples demonstrate the same opt-in.
- Max 64 commands (`PICO_MAX_COMMANDS`).
- Builtin `/` completer (`bol_only`) lists your command automatically.

To offer argument completions (`/docs topic`), add a `pico_host_add_completer` or `pico_workspace_add_completer` — see `completers.md`. The builtin command completer already knows `/model`, `/effort`, `/login`, `/logout`, `/docs`, `/resume`, `/cd`. `/resume` completions list parent sessions from an asynchronously refreshed core snapshot, so the popup may initially be empty or briefly stale while disk scanning completes; a subagent session can still be opened by typing its session ID.

## Application notices

Use `PicoHost_AddNotice` for command feedback instead of assistant messages.
See [notices](notices.md) for severity, persistence, and model-context contracts.
