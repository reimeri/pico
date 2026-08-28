# Extending Pico

Pico loads C99 `.c` files as shared libraries. Write one file, export `pico_ext()`, and register from `host_init` and/or `workspace_init`. Pico compiles and `dlopen`s it.

## Where to put files

- This workspace only: `<workspace>/.pico/extensions/`
- Every workspace: `~/.config/pico/extensions/` (or `$XDG_CONFIG_HOME/pico/extensions/`)

Subfolders are fine. Only `.c` files are loaded (depth 8). Skip with `pico --safe`.

After writing a file, reload happens automatically once every live and retired runtime is quiescent. F5 or `/reload` use the same barrier. While reload is queued, new turns and delegations are refused, but current work, cancellation, and ask UI keep pumping.

Compile errors and failed tool registrations appear in the overlay. `/docs [topic]` prints these pages into chat.

## Topics

Read the page that matches the work (`/docs <name>` or the file next to this README):

- `anatomy` — entry point, lifecycle, compile
- `agents` — agent/session identity, copied snapshots, callback context, concurrency and lifecycle
- `views` — UI in a slot (sidebar, chat, footer, …), named overlay modals, and the chat empty-state
- `hooks` — submit, layout, compact, session reset, turn end/cancel/error; tool, tool-row, and LLM interceptors
- `context` — request-only, non-persistent agent context
- `tools` — LLM-callable tools and structured replayable details
- `commands` — slash commands (`/foo`)
- `completers` — composer `#` / `@` style completion
- `providers` — LLM backends
- `auth` — `/login` `/logout` and credentials
- `contracts` — threads, ownership, limits, reload

Always read [`contracts.md`](contracts.md) before shipping an extension. For user-facing profile setup and continuation, see [`../subagents.md`](../subagents.md) or `/docs subagents`.

## Layout

This file is shipped with Pico as `docs/extend/README.md` next to the executable, not in the user's workspace. Topic pages listed above are in this directory. The subagent guide is [`../subagents.md`](../subagents.md). Example sources are two directories up, in [`../../examples/`](../../examples/) (sibling of `docs/`). Reference builtin sources (compiled into Pico; read them, do not load them as user extensions) are in [`../../builtins/`](../../builtins/). Resolve every relative path in these pages from the file that contains it.

## Examples

Copy-templates (paths from this README):

- [`../../examples/hello.c`](../../examples/hello.c) — sidebar view
- [`../../examples/empty_banner.c`](../../examples/empty_banner.c) — empty-state banner above the Tools / Context / Skills cards
- [`../../examples/echo_tool.c`](../../examples/echo_tool.c) — tool + `json.h`
- [`../../examples/ask_tool.c`](../../examples/ask_tool.c) — `pico_tool_ask` + builtin confirm overlay
- [`../../examples/permit_tool.c`](../../examples/permit_tool.c) — before-tool permission prompt
- [`../../examples/extra_instructions.c`](../../examples/extra_instructions.c) — `pico_add_llm_hook` extra prompt line
- [`../../examples/ephemeral_context.c`](../../examples/ephemeral_context.c) — request-only context
- [`../../examples/time_cmd.c`](../../examples/time_cmd.c) — slash command
- [`../../examples/modal.c`](../../examples/modal.c) — named overlay modal and tool-row click
- [`../../examples/stream_modal.c`](../../examples/stream_modal.c) — tool worker posts into a named overlay mailbox
- [`../../examples/subagents/exploration.json`](../../examples/subagents/exploration.json) — read-only exploration profile
- [`../../examples/subagents/review.json`](../../examples/subagents/review.json) — read-only review profile

The JSON profiles are examples only. Copy or adapt them under `$XDG_CONFIG_HOME/pico/subagents/` (or `~/.config/pico/subagents/`); Pico does not install them automatically. Their `sh` allowlist limits the exposed tool catalog, not the shell commands that tool may execute.

## Reference builtins

These are the in-app implementations, shipped for reading. They are not loaded from this folder.

- [`../../builtins/shell.c`](../../builtins/shell.c) — `sh`
- [`../../builtins/openai.c`](../../builtins/openai.c) — OpenAI-compatible provider (uses [`../../builtins/responses.c`](../../builtins/responses.c))
- [`../../builtins/hyper.c`](../../builtins/hyper.c) — Hyper provider (uses [`../../builtins/completions.c`](../../builtins/completions.c) plus [`../../builtins/hyper_auth.c`](../../builtins/hyper_auth.c))
- [`../../builtins/xai.c`](../../builtins/xai.c) — xAI provider (uses [`../../builtins/completions.c`](../../builtins/completions.c) plus [`../../builtins/xai_auth.c`](../../builtins/xai_auth.c))

Headers you may include: `pico/plugin.h`, `pico/host.h`, `pico/workspace.h`, `pico/app.h`, `pico/agent.h`, `pico/theme.h`, `pico/http.h`, `pico/auth.h`, `pico/md_view.h`, `json.h`, Clay, Raylib. Do not treat the private app-level `agent.h`, `agent_internal.h`, `host_internal.h`, `workspace_internal.h`, `session.h`, or `settings.h` as API.
