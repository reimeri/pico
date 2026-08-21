# Extending Pico

Pico loads C99 `.c` files as shared libraries. Write one file, export `pico_ext()`, register what you need in `init`. Pico compiles and `dlopen`s it.

## Where to put files

- This workspace only: `<workspace>/.pico/extensions/`
- Every workspace: `~/.config/pico/extensions/` (or `$XDG_CONFIG_HOME/pico/extensions/`)

Subfolders are fine. Only `.c` files are loaded (depth 8). Skip with `pico --safe`.

After writing a file, reload happens automatically once the agent is **idle**. F5 or `/reload` also work. A file written during this turn will not load until the turn finishes.

Compile errors appear in the overlay. `/docs [topic]` prints these pages into chat.

## Topics

Read the page that matches the work (`/docs <name>` or the file next to this README):

- `anatomy` — entry point, lifecycle, compile
- `views` — UI in a slot (sidebar, chat, footer, …)
- `hooks` — submit, layout, compact, messages
- `tools` — LLM-callable tools
- `commands` — slash commands (`/foo`)
- `completers` — composer `#` / `@` style completion
- `providers` — LLM backends
- `auth` — `/login` `/logout` and credentials
- `contracts` — threads, ownership, limits, reload

Always read `contracts.md` before shipping an extension.

## Examples

Copy-templates in the Pico source tree:

- `examples/hello.c` — sidebar view
- `examples/echo_tool.c` — tool + `json.h`
- `examples/ask_tool.c` — `pico_tool_ask` + builtin confirm overlay
- `examples/time_cmd.c` — slash command

Headers you may include: `pico/plugin.h`, `pico/app.h`, `pico/theme.h`, `pico/http.h`, `pico/auth.h`, `pico/md_view.h`, `json.h`, Clay, Raylib. Do not treat `agent.h`, `session.h`, or `settings.h` as API.
