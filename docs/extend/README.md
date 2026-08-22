# Extending Pico

Pico loads C99 `.c` files as shared libraries. Write one file, export `pico_ext()`, register what you need in `init`. Pico compiles and `dlopen`s it.

## Where to put files

- This workspace only: `<workspace>/.pico/extensions/`
- Every workspace: `~/.config/pico/extensions/` (or `$XDG_CONFIG_HOME/pico/extensions/`)

Subfolders are fine. Only `.c` files are loaded (depth 8). Skip with `pico --safe`.

After writing a file, reload happens automatically once the agent is **idle**. F5 or `/reload` also work. A file written during this turn will not load until the turn finishes.

Compile errors and failed tool registrations appear in the overlay. `/docs [topic]` prints these pages into chat.

## Topics

Read the page that matches the work (`/docs <name>` or the file next to this README):

- `anatomy` — entry point, lifecycle, compile
- `agents` — agent/session identity, copied snapshots, callback context, concurrency
- `views` — UI in a slot (sidebar, chat, footer, …) and the chat empty-state
- `hooks` — submit, layout, compact, session reset, turn end/cancel/error; tool and LLM interceptors
- `context` — request-only, non-persistent agent context
- `tools` — LLM-callable tools and structured replayable details
- `commands` — slash commands (`/foo`)
- `completers` — composer `#` / `@` style completion
- `providers` — LLM backends
- `auth` — `/login` `/logout` and credentials
- `contracts` — threads, ownership, limits, reload

Always read `contracts.md` before shipping an extension.

## Examples

Copy-templates in the Pico source tree:

- `examples/hello.c` — sidebar view
- `examples/empty_banner.c` — empty-state banner above the Tools / Context / Skills cards
- `examples/echo_tool.c` — tool + `json.h`
- `examples/ask_tool.c` — `pico_tool_ask` + builtin confirm overlay
- `examples/permit_tool.c` — before-tool permission prompt
- `examples/extra_instructions.c` — `pico_add_llm_hook` extra prompt line
- `examples/ephemeral_context.c` — request-only context
- `examples/time_cmd.c` — slash command

Headers you may include: `pico/plugin.h`, `pico/app.h`, `pico/agent.h`, `pico/theme.h`, `pico/http.h`, `pico/auth.h`, `pico/md_view.h`, `json.h`, Clay, Raylib. Do not treat the private app-level `agent.h`, `agent_internal.h`, `session.h`, or `settings.h` as API.
