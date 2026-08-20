# Contracts

Read this before writing an extension. Getting these wrong crashes Pico or silently no-ops.

## Reload waits until idle

`PicoPlugins_Reload` queues while `agent_state` is `PICO_AGENT_LLM_WAIT`, `PICO_AGENT_TOOL_WAIT`, or `PICO_AGENT_COMPACT_WAIT`. A `.c` file written in this turn does not load until the agent finishes. Tell the user to wait, or use `/reload` after. Polling is ~0.5s; F5 and `/reload` request the same path.

`--safe` skips user extensions. Compile errors set `app->status_warn` (overlay).

## Threads

Main thread: `init`, `shutdown`, `on_frame`, view render, hooks, command `run`, completer query/accept, auth login/logout.

Worker thread: `PicoToolFn`, `PicoProviderStreamFn`.

Do not use Clay, Raylib drawing, or composer/chat mutation from the worker. Tools return a malloc'd string; providers use `on_delta` / `PicoLlmResult`.

Reload is deferred while the worker is busy so tool/provider pointers stay valid for the turn.

## Ownership

- `name`, `description`, `help`, `params_json`, provider/auth string fields: must outlive the extension. Use string literals.
- Tool `*out`: malloc, Pico frees. Never leave `*out` unset on a path that returns; use `JsonDup("")` or an error string.
- `app->agent_input` and `app->compact_summary`: malloc if you set them; Pico frees.
- `PicoLlmResult` strings/arrays: malloc; Pico calls `pico_llm_result_free`.
- `shutdown` must join threads you started. `dlclose` follows `shutdown`.

## Limits

- 32 user `.c` files
- 16 views per slot
- 64 hooks, tools, commands
- 16 completers, providers, auth registrations
- 24 completion items per query

Silent no-op if a `pico_add_*` is full or arguments are NULL.

## Directories

- User: `~/.config/pico/extensions/`
- Workspace: `<workspace>/.pico/extensions/`
- Only regular `.c` files; hidden names skipped; walk depth 8.
- `/cd` changes `app->workspace` at runtime: new session, workspace settings, and a plugin reload. Read `app->workspace` when you use it. Assigning the field alone does not reload plugins or start a new session.

## `PicoApp`

The struct is public. Prefer `pico_add_*` and the fields listed in the topic pages (`submit_cancel`, `agent_input`, `compact_summary`, `workspace`). `agent.h` / `session.h` / `settings.h` are not extension API even though they compile.

`pico_session_log_custom(app, "myext", "{…json…}")` appends a JSONL record. Session replay does not dispatch it back to extensions.
