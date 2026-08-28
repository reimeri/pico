# Agents

Pico workspaces can run up to `PICO_MAX_AGENTS` independent agents concurrently. `PicoAgent`, `PicoWorkspace`, and `PicoAgentContext` are opaque.

Include `pico/agent.h` directly, or include `pico/plugin.h`.

## Identity and snapshots

`PicoAgentId` identifies one in-memory agent for the lifetime of the process. It is distinct from the durable JSONL `session_id`. Runtime generation identifies one worker generation inside that agent; force cancellation replaces the generation without changing the agent ID.

Use the main-thread host/workspace API; `PicoHost` is the process owner and does not expose a dereferenceable agent list:

```c
for (int i = 0; i < pico_agent_count(app); i++) {
    PicoAgentInfo info;
    if (pico_agent_info(app, i, &info)) { /* copied snapshot */ }
}
PicoAgentId active = pico_agent_active(app);
pico_agent_select(app, active);
```

`pico_agent_find` returns a copied snapshot by ID. `PicoAgentInfo.persistence` is `PICO_SESSION_EPHEMERAL`, `PICO_SESSION_DURABLE`, or `PICO_SESSION_FAILED`; `resumable` is true only for a durable identity. `pico_agent_create`, `pico_agent_close`, `pico_agent_cancel`, and `pico_agent_force_cancel` return a controlled `PicoAgentResult`. Close rejects busy agents, retained-runtime references, and the final live agent. IDs become stale after close or workspace replacement. `pico_agent_active` / `pico_agent_select` are UI selection only; they do not retarget an in-progress submit, cancel, model, effort, session, or compaction action. Composer submit and slash commands snapshot the selected ID once and pass it into those APIs. Selection clears transcript selection/scroll snapshots but leaves the global composer draft unchanged.

`pico_agent_submit(host, id, text, parts_json)` is a complete explicit submission: it locates the agent by ID, records the user transcript and session event, and starts the turn from `text` plus optional canonical `parts_json`. It does not read UI selection, the composer, or host `agent_parts`. Empty input (no text and no parts) returns `PICO_INVALID`; a stale ID returns `PICO_NOT_FOUND`; a busy or non-accepting agent returns `PICO_BUSY`. Composer send still snapshots the selected ID, prepares display text and attachments, then calls this API.

`pico_agent_message_count` and `pico_agent_message` provide bounded, main-thread-only borrowed transcript inspection. Tool trace rows include their provider `tool_call_id`, formatted `tool_args` for display, and original `tool_args_json`; builtin subagent rows also expose the linked runtime `child_id` and durable `child_session_id` when available. Non-tool think rows may include `think_parts` (OpenAI-style summary steps; the last part is the widget title) and `think_ms` (frozen burst duration). Durable sessions restore both from `thinking_parts` and `thinking_ms`. The message and all nested string pointers are invalidated by pumping, transcript mutation, close, or workspace replacement.

## Named subagent profiles

`pico_subagent_profile_count` and `pico_subagent_profile_info` return copied snapshots of valid profiles discovered directly under `$XDG_CONFIG_HOME/pico/subagents/` (or `~/.config/pico/subagents/`). Pico creates the directory, but does not install profiles. Only direct, regular, non-hidden `*.json` files are read. They use JSONC comment rules, and the filename stem is the profile name.

A profile has this shape:

```jsonc
{
  "description": "Fast repository exploration", // optional, at most 256 bytes
  "purpose": "Inspect the delegated question.", // required, at most 1024 bytes
  "model": "model-catalog-id",                  // optional
  "effort": "low",                             // optional
  "tools": ["sh"]                              // optional exact-name allowlist
}
```

Omitting `tools` allows all registered tools; an empty array allows none. Unknown keys warn but still load. A bad type, invalid filename, duplicate or unknown tool, unknown model, unsupported model/effort pair, or oversized value invalidates only that file. Profiles load after tools at startup and are replaced as one completed snapshot on F5 or `/reload`. Running invocations keep their copied values.

For a fresh child, an omitted model inherits the parent model. An omitted effort inherits the parent effort when the model matches; after a model override it uses that model's configured default, first supported effort, or `none`. Explicit effort must be supported. Resolution never changes the parent or workspace defaults.

## Synchronous delegation

The builtin `subagent` tool accepts only a named profile, a delegated `task`, and an optional exact `session_id`:

```json
{"profile":"exploration","task":"Find the replay boundary.","session_id":"optional-child-id"}
```

A fresh child gets current workspace/system instructions, a clearly delimited profile purpose, only the delegated user task, and the profile's copied tool policy. It does not inherit the parent transcript, provider history, compaction briefing, TODO state, or cache key. It shares process registrations, providers, authentication, and workspace services.

Supplying `session_id` reserves and replays exactly that prior subagent session. The stored profile must match. Transcript/provider history, usage, compaction state, and prompt cache are restored, then model, effort, purpose, and tools are refreshed from the current profile and parent. A model change rotates the cache key. The delegated task is appended to the same JSONL session.

The parent remains in tool wait while the child runs. Click the `subagent` tool row to inspect the child's transcript without selecting it. The result is JSON with `status`, `profile`, `model`, `effort`, `resumable`, and `final_answer`; a durable child also returns `session_id`. An unknown `profile` fails and lists the currently available profile names in `final_answer`. Parent cancellation wakes the parent generation and cascades to the child. Late child completion cannot publish into a replacement generation.

## Asks

`pico_tool_pending_ask` returns the oldest live ask across all agents, including hidden delegated children; its `agent_id`, `profile`, and `purpose` identify the owner. `pico_tool_answer` routes by globally unique ask ID, so a child ask remains answerable while its parent waits. The borrowed request remains valid only until the next pump.

## Main-thread targets

Notification events, LLM hooks, context hooks, tool apply callbacks, and after-tool hooks identify their target with a `PicoAgentId`. Keep main-thread agent/session state in an ID-keyed map. Main-thread callbacks are serialized.

During `PICO_HOOK_ON_COMPACT`, call:

```c
pico_agent_set_compact_summary(app, event->agent_id, malloc_briefing);
```

Pico takes ownership. A stale or mismatched ID is rejected.

`pico_session_log_custom(app, agent_id, "myext", "{...}")` similarly targets an ID explicitly and returns `PICO_SESSION_WRITE_SKIPPED`, `_OK`, or `_FAILED`. Prefer replayable tool `details_json` for extension-owned session state.

## Worker callback context

Tools, before-tool hooks, and providers receive a callback-scoped `PicoAgentContext *`, never `PicoHost *`. Do not retain the pointer or strings returned from it after the callback.

Read-only accessors provide copied worker values:

- `pico_agent_context_id(ctx)`
- `pico_agent_context_generation(ctx)`
- `pico_agent_context_registration_generation(ctx)`
- `pico_agent_context_workspace_id(ctx)`
- `pico_agent_context_workspace(ctx)`
- `pico_agent_context_session_id(ctx)`
- `pico_agent_context_profile(ctx)`
- `pico_agent_context_purpose(ctx)`
- `pico_agent_context_safe_mode(ctx)`
- `pico_agent_context_cancelled(ctx)`

A context binds to one agent ID and runtime generation. `pico_agent_context_registration_generation` is the workspace registration generation copied when that turn was accepted. After its callback—or after that runtime is retired—accessors fail closed: IDs/generation become zero, strings become empty, and cancellation reports true. Stale contexts cannot ask, post to a UI mailbox, or bind child processes.

Use `pico_tool_ask(ctx, ...)`, `pico_ui_post(ctx, ...)`, `pico_tool_set_child(ctx, pid)`, `pico_auth_copy_ctx(ctx, ...)`, callback results, provider cancellation, and delta callbacks. Worker callbacks must not mutate UI, transcripts, sessions, model settings, or main-thread extension state.

## Concurrency contract

Main-thread hooks and apply callbacks are serialized, but worker callbacks from different agents may overlap. Tool, before-tool, and provider code must therefore be reentrant. Thread-safe process-global caches are allowed only when their meaning is independent of agent and runtime generation.

Agent/session changes produced by worker code must travel through callback results and be applied on the main thread after generation validation. Pico intentionally provides no generic extension-state subsystem.

## Reload, workspace, and shutdown

Reload and workspace replacement stop accepting external turns/delegations, keep pumping current work and asks, and commit only after a full workspace quiescence check. Extension registrations and the profile snapshot are rebuilt together; live sessions are announced and structured details are replayed. Existing profile values stay copied per invocation, while restricted tool names are checked against the new registry before another turn.

`pico_host_free` returns `PICO_HOST_SHUTDOWN_CLEAN` or `PICO_HOST_SHUTDOWN_RETAINED`. Retained means one shared shutdown deadline expired and a callback was detached. Pico keeps every service and `.so` that callback can reach, permanently retires Pico in the process, and rejects later host/plugin initialization. The caller must proceed to process exit.
