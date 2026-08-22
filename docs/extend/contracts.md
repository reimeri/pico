# Contracts

Read this before writing an extension. Getting these wrong crashes Pico or silently no-ops.

## Reload waits until idle

`PicoPlugins_Reload` queues while any live agent is in `PICO_AGENT_LLM_WAIT`, `PICO_AGENT_TOOL_WAIT`, or `PICO_AGENT_COMPACT_WAIT`, while a delegation request/job is live, **or** while a force-cancelled worker is still inside a tool or provider call. A `.c` or subagent-profile file written in this turn does not load until that work finishes. Tell the user to wait, or use `/reload` after. Polling is ~0.5s; F5 and `/reload` request the same path. Once quiescent, extension registrations and the completely parsed subagent-profile snapshot replace their previous registries.

A second Esc force-cancels a stuck turn: the UI goes idle and a new worker starts so the user can keep chatting, but reload still waits for the abandoned worker. That worker may outlive the turn; do not touch Clay, Raylib, or chat/composer state from it.

On process exit, agent shutdown waits about one second. If a worker is still running, Pico retains its heap execution host plus every registration, auth store, builtin state, and user-extension `.so` handle that worker can reach. No extension `shutdown` callback runs and no handle is closed; process exit reclaims the retained state. The stack/UI `PicoApp` is never worker-owned.

`--safe` skips user extensions. Compile errors and failed `pico_add_tool` registrations set `app->status_warn` (overlay). `pico_status_warn` appends a line to that overlay.

## Threads

Main thread: `init`, `shutdown`, `on_frame`, view render, notification hooks, after-tool hooks, tool apply callbacks, LLM/context hooks, command `run`, completer query/accept, auth login/logout. Agent-scoped callbacks receive a `PicoAgentId`; keep mutable agent/session extension state in an ID-keyed map. Main-thread callbacks are serialized.

Worker thread: `PicoToolFn`, `PicoToolBeforeFn`, `PicoProviderStreamFn`. They receive a callback-scoped opaque `PicoAgentContext *`, never the UI `PicoApp *`. Do not retain it. Worker callbacks from different agents may overlap and must be reentrant. Use context accessors, `pico_tool_ask`, `pico_tool_set_child`, and `pico_auth_copy_ctx`; do not touch UI, transcript, session, settings, model catalog, or unsynchronized agent-scoped extension state. `pico_tool_ask` may be called only from a tool or before-tool callback. Do not block on your own condition variable — Esc, force-cancel, reload, and shutdown cannot wake it.

Do not use Clay, Raylib drawing, or composer/chat mutation from the worker. Tools return a `PicoToolResult` with malloc'd fields; providers use `on_delta` / `PicoLlmResult`. Overlay code answers a pending ask from the main thread with `pico_tool_answer`. `PICO_LLM_DELTA_THINKING` appends; `PICO_LLM_DELTA_THINKING_SUMMARY` replaces the current summary (zero-length starts the next step). Pico coalesces consecutive summaries until a tool call.

Reload is deferred while any live worker is busy, and while any force-cancelled worker is still in a tool or provider call, so those pointers stay valid until the call returns. That includes a worker blocked in `pico_tool_ask` or the builtin `subagent` tool.

Named delegation is a worker/main-thread handshake: the parent tool waits on a core-owned job without holding manager locks; the main thread creates and pumps the hidden child. Worker callbacks may therefore run for a child while its parent worker is blocked. Child asks are still published through the normal global ask routing.

## Ownership

- `PicoExt.name`, `PicoExt.description`, and `name` / `description` / `help` / `params_json` / provider/auth string fields: must outlive the extension. Use string literals. `PicoExt.description` is optional.
- `PicoToolResult.output` / `details_json`: malloc if set; Pico frees. Zero-initialize the result and set `is_error` for tool-defined failures.
- `pico_tool_ask` answer: malloc on `PICO_ASK_OK`; the caller frees it. Always `NULL` on cancel/fail.
- `pico_tool_pending_ask` `request_json`: the oldest live ask across all agents; valid until the next manager pump. Do not retain it across frames.
- `pico_subagent_profile_info`: copies the complete profile snapshot into caller storage. Profile strings and tool names in that copy are caller-owned values, not registry pointers.
- `PicoAgentContext *` and all strings returned by its accessors: callback-scoped; never retain them.
- `PicoToolEvent.name` / `call_id` / `args_json` / `output` / `details_json`, `PicoLlmEvent.tools` / `instructions`, and `PicoContextEvent.history_json` / `tools`: core-owned and valid only during the callback.
- `PicoToolEvent.args_json_out` / `result`, `PicoLlmEvent.extra_instructions`, and `PicoContextEvent.extra_context`: malloc if you set them; Pico frees.
- `app->agent_input`: malloc if you set it; Pico frees.
- `pico_agent_set_compact_summary(app, agent_id, summary)`: `summary` is malloc'd and ownership transfers to Pico.
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

Registrations are ignored when a limit is full, arguments are NULL, or the kind/slot is invalid. `pico_add_tool` additionally returns `false` for these cases, duplicate names, and malformed/non-object `params_json` schemas, and appends a `status_warn` line naming the tool and the reason.

## Directories

- User extensions: `~/.config/pico/extensions/`
- Workspace extensions: `<workspace>/.pico/extensions/`
- User subagent profiles: `~/.config/pico/subagents/` (or the matching `$XDG_CONFIG_HOME` path)
- Extension discovery uses regular `.c` files, skips hidden names, and walks to depth 8.
- Profile discovery uses only direct, regular, non-hidden `*.json` files and parses them as JSONC.
- `/cd` changes `app->workspace` at runtime: new session, workspace settings, and a plugin reload. Read `app->workspace` when you use it. Assigning the field alone does not reload plugins or start a new session.

## `PicoApp`

The struct is public. Prefer `pico_add_*` and the fields listed in the topic pages (`submit_cancel`, `agent_input`, `workspace`). Conversation/runtime/session/model/usage fields live in opaque manager-owned `PicoAgent` instances, not `PicoApp`. Use the copied `pico_agent_*` manager snapshots; `app->agents` is an opaque owner, not a dereferenceable active-agent handle. See [agents](agents.md). The private app-level `agent.h`, `agent_internal.h`, `agent_manager.h`, `session.h`, and `settings.h` are not extension API.

The model catalog in `PicoApp` is immutable while agents run. Each agent owns copied model/effort/context/compaction selection, so changing defaults or replaying a session does not mutate another live agent.

`pico_session_log_custom(app, agent_id, "myext", "{…json…}")` appends a JSONL record to the explicit target on the main thread. Stale or mismatched targets are rejected. Session replay does not dispatch it back to extensions. For replayable tool-owned state, return validated structured `details_json` and register a tool apply callback instead. Apply receives the target ID. Details are replayed chronologically on resume and extension reload; use complete snapshots and make replay application idempotent.

## Sessions and delegation

Session JSONL schema version 3 requires a `kind` in every header. A subagent header also requires durable `profile` and `initial_purpose` metadata and may include `parent_session_id`. Pico does not replay older schemas. One manager reservation owns each open session path, and failed replay releases the reservation without publishing an agent.

Fresh named delegation never uses ambient `resume_last`. Continuation resolves an exact session ID, requires the stored and requested profile names to match, and reapplies current profile purpose/model/effort/tools after replay. Only a session whose header and delegated user event were durably written is returned as `resumable`; write failure clears its reusable ID and path.

Delegation jobs and child/session ownership survive force cancellation until every worker, manager, and child reference releases them. Parent cancellation wakes the waiting generation immediately and requests child cancellation. A late child terminal event cannot publish to a replacement parent generation.

## Tool authorization

Each request starts from registered tools, applies the agent policy, then LLM-hook exclusions. For a named child, the policy is the current profile's exact allowlist snapshot; omission allows all tools and an empty array allows none. Continued sessions refresh this policy instead of restoring an old catalog. Context hooks inspect the final effective catalog. The provider receives a retained snapshot, and tool execution/apply resolve only from that snapshot—not from the live registry. A hidden or unoffered call becomes a logged tool error and bypasses before hooks, tool code, apply, and after hooks. Empty/duplicate call IDs, malformed call arrays, and calls beyond the pending-call limit fail the provider round explicitly.
