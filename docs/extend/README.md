# Extending Pico

Pico loads C99 `.c` files as shared libraries. Write one file, export `pico_ext()`, and register from `host_init` and/or `workspace_init`. Pico compiles and `dlopen`s it. ABI 13 splits host and workspace instances; there is no ABI 12 compatibility layer.

## Where to put files

- This workspace only: `<workspace>/.pico/extensions/`
- Every workspace: `~/.config/pico/extensions/` (or `$XDG_CONFIG_HOME/pico/extensions/`)

Subfolders are fine. Only `.c` files are loaded (depth 8). Skip with `pico --safe`. Workspace-local sources must not declare host callbacks.

After writing a file, each workspace reloads independently once its live and retired runtimes are quiescent. F5 and `/reload` reload host extensions (user-global / config sources only) and the selected workspace; other workspaces keep accepting work. A workspace-local compile failure does not block host-extension replacement. While a workspace reload is queued, new turns and delegations are refused only in that workspace, but current work, cancellation, and ask UI keep pumping.

`/cd` opens or selects a workspace and leaves the previous workspace running. Compile errors and failed tool registrations appear in the overlay. `/docs [topic]` prints these pages into chat.

## Topics

Read the page that matches the work (`/docs <name>` or the file next to this README):

- `anatomy` — entry point, host/workspace instances, state, generations, compile
- `host` — process owner, preferences, host-scoped registrations, pump, shutdown
- `workspace` — canonical path, lifecycle, `/cd`, per-workspace reload and close, limits
- `agents` — agent/session identity, copied snapshots, callback context, concurrency and lifecycle
- `views` — UI in a slot (sidebar, chat, footer, …), named overlay modals, and the chat empty-state
- `hooks` — submit, layout, compact, session reset, turn end/cancel/error, ask/ask end; tool, tool-row, and LLM interceptors
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

- [`../../examples/hello.c`](../../examples/hello.c) — host sidebar view
- [`../../examples/empty_banner.c`](../../examples/empty_banner.c) — empty-state banner above the Tools / Context / Skills cards
- [`../../examples/echo_tool.c`](../../examples/echo_tool.c) — workspace tool + `json.h`
- [`../../examples/counter_tool.c`](../../examples/counter_tool.c) — stateful per-workspace tool
- [`../../examples/ask_tool.c`](../../examples/ask_tool.c) — `pico_tool_ask` + builtin confirm overlay
- [`../../examples/permit_tool.c`](../../examples/permit_tool.c) — before-tool permission prompt
- [`../../examples/extra_instructions.c`](../../examples/extra_instructions.c) — `pico_add_llm_hook` extra prompt line
- [`../../examples/ephemeral_context.c`](../../examples/ephemeral_context.c) — request-only context
- [`../../examples/time_cmd.c`](../../examples/time_cmd.c) — host slash command
- [`../../examples/modal.c`](../../examples/modal.c) — named overlay modal and tool-row click
- [`../../examples/stream_modal.c`](../../examples/stream_modal.c) — tool worker posts into a named overlay mailbox
- [`../../examples/subagents/explore.json`](../../examples/subagents/explore.json) — read-only exploration profile
- [`../../examples/subagents/review.json`](../../examples/subagents/review.json) — read-only review profile

The JSON profiles are examples only. Copy or adapt them under `$XDG_CONFIG_HOME/pico/subagents/` (or `~/.config/pico/subagents/`); Pico does not install them automatically. Their `sh` allowlist limits the exposed tool catalog, not the shell commands that tool may execute.

## Reference builtins

These are the in-app implementations, shipped for reading. They are not loaded from this folder.

- [`../../builtins/shell.c`](../../builtins/shell.c) — `sh`
- [`../../builtins/background.c`](../../builtins/background.c) — `run_background` / `kill_background` / `list_background` / `log_background` (uses [`../../builtins/background_model.c`](../../builtins/background_model.c))
- [`../../builtins/openai.c`](../../builtins/openai.c) — OpenAI-compatible provider (uses [`../../builtins/responses.c`](../../builtins/responses.c))
- [`../../builtins/hyper.c`](../../builtins/hyper.c) — Hyper provider (uses [`../../builtins/completions.c`](../../builtins/completions.c) plus [`../../builtins/hyper_auth.c`](../../builtins/hyper_auth.c))
- [`../../builtins/xai.c`](../../builtins/xai.c) — xAI provider (uses [`../../builtins/completions.c`](../../builtins/completions.c) plus [`../../builtins/xai_auth.c`](../../builtins/xai_auth.c))

Headers you may include: `pico/plugin.h`, `pico/host.h`, `pico/workspace.h`, `pico/app.h`, `pico/agent.h`, `pico/theme.h`, `pico/http.h`, `pico/auth.h`, `pico/md_view.h`, `json.h`, Clay, Raylib. Do not treat the private app-level `agent.h`, `agent_internal.h`, `host_internal.h`, `workspace_internal.h`, `session.h`, or `settings.h` as API.
