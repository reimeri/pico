# Commands

Slash commands are handled on submit, not sent to the model.

```c
#include "pico/plugin.h"

#include <time.h>

static void TimeCmd(PicoApp *app, const char *args)
{
    (void)args;
    time_t now = time(NULL);
    char *line = ctime(&now);
    PicoApp_AddMessage(app, PICO_ROLE_ASSISTANT, line ? line : "(no time)");
    PicoComposer_SetText(app, "");
    app->submit_cancel = true;
}

static void TimeInit(PicoApp *app)
{
    pico_add_command(app, "time", "Show the local time", TimeCmd);
}
```

Full file: `examples/time_cmd.c`. User types `/time`. `/help` lists every registered command. Builtin `/extensions` opens a modal listing installed extensions. Builtin `/show-prompt` opens a modal with the assembled system prompt for the next turn (`SYSTEM.md`, workspace `AGENTS.md`, the docs hint, and any `pico_add_llm_hook` extras under `## Additional instructions`).

## Contract

- `name` has no leading slash. Completer inserts `/name`.
- `name` and `help` must outlive the extension — string literals.
- `run` receives the rest of the line after `/name` (may be empty).
- **Always set `app->submit_cancel = true`**, or the slash line is also sent to the agent. Clear the invoking agent's composer with `PicoComposer_SetText(app, "")` or `PicoComposer_SetAgentText` when selection may change.
- Runs on the **main thread** from `PICO_HOOK_BEFORE_SUBMIT` (builtin `commands` extension). Safe to call `PicoApp_AddMessage`.
- Max 64 commands (`PICO_MAX_COMMANDS`).
- Builtin `/` completer (`bol_only`) lists your command automatically.

To offer argument completions (`/docs topic`), add a `pico_add_completer` — see `completers.md`. The builtin command completer already knows `/model`, `/effort`, `/login`, `/logout`, `/docs`, `/resume`, `/cd`. `/resume` completions list parent sessions; a subagent session can still be opened by typing its session ID.
