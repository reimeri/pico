# Commands

Slash commands are handled on submit, not sent to the model.

```c
#include "pico/plugin.h"

#include <time.h>

static void TimeCmd(PicoHost *host, PicoAgentId agent_id, const char *args, void *state)
{
    (void)state;
    (void)args;
    time_t now = time(NULL);
    char *line = ctime(&now);
    PicoHost_AddMessage(host, agent_id, PICO_ROLE_ASSISTANT, line ? line : "(no time)");
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

Full file: [`../../examples/time_cmd.c`](../../examples/time_cmd.c). User types `/time`. `/help` lists every registered command. Builtin `/extensions` (also F2) opens a modal listing installed extensions; click a card to toggle it on or off. Builtin `/docs [topic]` prints markdown shipped next to the binary (`docs/extend/`, plus `docs/subagents.md`) into chat — not the compile-time source tree and not `~/.config`. Builtin `/show-prompt` opens a modal with the assembled system prompt for the next turn (`SYSTEM.md`, workspace `.pico/SYSTEM.md`, workspace `AGENTS.md`, the docs hint, and any `pico_add_llm_hook` extras under `## Additional instructions`). The modal color-codes those sources and shows a legend for the ones present. The docs hint uses the same color as `SYSTEM.md` and is the absolute path of that shipped `docs/extend/README.md`. Example sources ship next to the binary in `examples/` (from this page: [`../../examples/`](../../examples/)).

## Contract

- `name` has no leading slash. Completer inserts `/name`.
- `name` and `help` must outlive the extension — string literals.
- `run` receives the snapshotted `PicoAgentId` from the submit that invoked the command, then the rest of the line after `/name` (may be empty). A later selection change cannot retarget that command.
- **Always call `PicoHost_RequestSubmitCancel(host)`**, or the slash line is also sent to the agent. Clear the composer with `PicoComposer_SetText(host, "")`.
- Runs on the **main thread** from `PICO_HOOK_BEFORE_SUBMIT` (builtin `commands` extension). Safe to call `PicoHost_AddMessage(host, agent_id, ...)`. Builtin `/login` and `/logout` forward that same snapshotted ID into auth callbacks, including later device-login notes.
- Max 64 commands (`PICO_MAX_COMMANDS`).
- Builtin `/` completer (`bol_only`) lists your command automatically.

To offer argument completions (`/docs topic`), add a `pico_host_add_completer` — see `completers.md`. The builtin command completer already knows `/model`, `/effort`, `/login`, `/logout`, `/docs`, `/resume`, `/cd`. `/resume` completions list parent sessions; a subagent session can still be opened by typing its session ID.
