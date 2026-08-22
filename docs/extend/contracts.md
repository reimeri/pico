# Contracts

Read this before writing an extension. Getting these wrong crashes Pico or silently no-ops.

## Reload waits until idle

`PicoPlugins_Reload` queues while the live agent is in `PICO_AGENT_LLM_WAIT`, `PICO_AGENT_TOOL_WAIT`, or `PICO_AGENT_COMPACT_WAIT`, **or** a force-cancelled worker is still inside a tool or provider call. A `.c` file written in this turn does not load until that work finishes. Tell the user to wait, or use `/reload` after. Polling is ~0.5s; F5 and `/reload` request the same path.

A second Esc force-cancels a stuck turn: the UI goes idle and a new worker starts so the user can keep chatting, but reload still waits for the abandoned worker. That worker may outlive the turn; do not touch Clay, Raylib, or chat/composer state from it.

On process exit, agent shutdown waits about one second. If a worker is still running, Pico retains its heap execution host plus every registration, auth store, builtin state, and user-extension `.so` handle that worker can reach. No extension `shutdown` callback runs and no handle is closed; process exit reclaims the retained state. The stack/UI `PicoApp` is never worker-owned.

`--safe` skips user extensions. Compile errors set `app->status_warn` (overlay).

## Threads

Main thread: `init`, `shutdown`, `on_frame`, view render, notification hooks, `PICO_TOOL_AFTER`, tool apply callbacks, LLM/context hooks, command `run`, completer query/accept, auth login/logout.

Worker thread: `PicoToolFn`, `PICO_TOOL_BEFORE`, `PicoProviderStreamFn`. Their `PicoApp *` is a heap execution-host view with copied worker-facing registrations/workspace and retained auth—not the stack/UI app. It has no active agent. Do not retain it after the callback or use it for UI, transcript, session, settings, or model-catalog state. `pico_tool_ask` is the only supported wait for user input; it may be called only from the worker tool slot (`PicoToolFn` or `PICO_TOOL_BEFORE`). Do not block on your own condition variable — Esc, force-cancel, reload, and shutdown cannot wake it.

Do not use Clay, Raylib drawing, or composer/chat mutation from the worker. Tools return a `PicoToolResult` with malloc'd fields; providers use `on_delta` / `PicoLlmResult`. Overlay code answers a pending ask from the main thread with `pico_tool_answer`. `PICO_LLM_DELTA_THINKING` appends; `PICO_LLM_DELTA_THINKING_SUMMARY` replaces the current summary (zero-length starts the next step). Pico coalesces consecutive summaries until a tool call.

Reload is deferred while the live worker is busy, and while any force-cancelled worker is still in a tool or provider call, so those pointers stay valid until the call returns. That includes a worker blocked in `pico_tool_ask`.

## Ownership

- `PicoExt.name`, `PicoExt.description`, and `name` / `description` / `help` / `params_json` / provider/auth string fields: must outlive the extension. Use string literals. `PicoExt.description` is optional.
- `PicoToolResult.output` / `details_json`: malloc if set; Pico frees. Zero-initialize the result and set `is_error` for tool-defined failures.
- `pico_tool_ask` answer: malloc on `PICO_ASK_OK`; the caller frees it. Always `NULL` on cancel/fail.
- `pico_tool_pending_ask` `request_json`: valid until the next `PicoAgent_Pump`. Do not retain it across frames.
- `PicoToolEvent.name` / `call_id` / `args_json` / `output` / `details_json`, `PicoLlmEvent.tools` / `instructions`, and `PicoContextEvent.history_json`: core-owned and valid only during the callback.
- `PicoToolEvent.args_json_out` / `result`, `PicoLlmEvent.extra_instructions`, and `PicoContextEvent.extra_context`: malloc if you set them; Pico frees.
- `app->agent_input`: malloc if you set it; Pico frees.
- `pico_agent_set_compact_summary(app, summary)`: `summary` is malloc'd and ownership transfers to Pico.
- `PicoLlmResult` strings/arrays: malloc; Pico calls `pico_llm_result_free`.
- `shutdown` must join threads you started. `dlclose` follows `shutdown`.

`PicoPlugins_Count` / `PicoPlugins_Get` return the loaded registry (builtins and user sources, including failed loads). Pointers in `PicoExtInfo` are valid until the next reload. `enabled` is currently true when the extension loaded; failed stubs are false.

## Limits

- 32 user `.c` files
- 16 views per slot
- 16 empty-state views (`pico_add_empty_view`)
- 64 notification hooks, tool hooks, LLM hooks, context hooks, tools, commands
- 16 completers, providers, auth registrations
- 24 completion items per query
- `PICO_TOOL_DETAILS_MAX`, `PICO_TOOL_ASK_MAX_REQUEST`, and `PICO_TOOL_ASK_MAX_ANSWER` (64 KiB)
- Builtin `ask_user`: 24 questions, 20 options per select question, 16 KiB per free-form answer

Registrations are ignored when a limit is full, arguments are NULL, or the kind/slot is invalid. `pico_add_tool` additionally returns `false` for these cases, duplicate names, and malformed/non-object `params_json` schemas.

## Directories

- User: `~/.config/pico/extensions/`
- Workspace: `<workspace>/.pico/extensions/`
- Only regular `.c` files; hidden names skipped; walk depth 8.
- `/cd` changes `app->workspace` at runtime: new session, workspace settings, and a plugin reload. Read `app->workspace` when you use it. Assigning the field alone does not reload plugins or start a new session.

## `PicoApp`

The struct is public. Prefer `pico_add_*` and the fields listed in the topic pages (`submit_cancel`, `agent_input`, `workspace`). Conversation/runtime/session/model/usage fields live in opaque `PicoAgent`, not `PicoApp`. Use `pico_agent_id` and `pico_agent_info_snapshot`; see [agents](agents.md). The private app-level `agent.h`, `agent_internal.h`, `session.h`, and `settings.h` are not extension API.

The model catalog in `PicoApp` is immutable while agents run. Each agent owns copied model/effort/context/compaction selection, so changing defaults or replaying a session does not mutate another live agent.

`pico_session_log_custom(app, app->agent, "myext", "{…json…}")` appends a JSONL record to the explicit active agent on the main thread. Worker-host and stale/mismatched targets are rejected. Session replay does not dispatch it back to extensions. For replayable tool-owned state, return validated structured `details_json` and register a tool apply callback instead. Details are replayed chronologically on resume and extension reload; use complete snapshots and make replay application idempotent.
