# Agents

Pico currently owns one extracted `PicoAgent`; the callback ABI is already agent-aware so concurrent agents can be added without another extension ABI transition. `PicoAgent` and `PicoAgentContext` are opaque.

Include `pico/agent.h` directly, or include `pico/plugin.h`.

## Identity and snapshots

`PicoAgentId` identifies one in-memory agent for the lifetime of the process. It is distinct from the durable JSONL `session_id`. Runtime generation identifies one worker generation inside that agent; force cancellation replaces the generation without changing the agent ID.

The current single-agent app exposes a borrowed `app->agent`. Read copied state instead of private fields:

```c
PicoAgentInfo info;
if (pico_agent_info_snapshot(app->agent, &info))
{
    /* info.id, state, model, effort, activity, session_id, ... */
}
```

`pico_agent_id(NULL)` returns zero. Snapshot strings are copied, require no freeing, and do not update after the call.

Creation, selection, enumeration, and close APIs arrive with `PicoAgentManager`; extensions must not allocate `PicoAgent` themselves.

## Main-thread targets

Notification events, LLM hooks, context hooks, tool apply callbacks, and after-tool hooks identify their target with a `PicoAgentId`. Keep main-thread agent/session state in an ID-keyed map. Main-thread callbacks are serialized.

During `PICO_HOOK_ON_COMPACT`, call:

```c
pico_agent_set_compact_summary(app, event->agent_id, malloc_briefing);
```

Pico takes ownership. A stale or mismatched ID is rejected.

`pico_session_log_custom(app, agent_id, "myext", "{...}")` similarly targets an ID explicitly. Prefer replayable tool `details_json` for extension-owned session state.

## Worker callback context

Tools, before-tool hooks, and providers receive a callback-scoped `PicoAgentContext *`, never `PicoApp *`. Do not retain the pointer or strings returned from it after the callback.

Read-only accessors provide copied worker values:

- `pico_agent_context_id(ctx)`
- `pico_agent_context_generation(ctx)`
- `pico_agent_context_workspace(ctx)`
- `pico_agent_context_session_id(ctx)`
- `pico_agent_context_profile(ctx)`
- `pico_agent_context_purpose(ctx)`
- `pico_agent_context_safe_mode(ctx)`
- `pico_agent_context_cancelled(ctx)`

A context binds to one agent ID and runtime generation. After its callback—or after that runtime is retired—accessors fail closed: IDs/generation become zero, strings become empty, and cancellation reports true. Stale contexts cannot ask or bind child processes.

Use `pico_tool_ask(ctx, ...)`, `pico_tool_set_child(ctx, pid)`, `pico_auth_copy_ctx(ctx, ...)`, callback results, provider cancellation, and delta callbacks. Worker callbacks must not mutate UI, transcripts, sessions, model settings, or main-thread extension state.

## Concurrency contract

Main-thread hooks and apply callbacks are serialized, but worker callbacks from different agents may overlap. Tool, before-tool, and provider code must therefore be reentrant. Thread-safe process-global caches are allowed only when their meaning is independent of agent and runtime generation.

Agent/session changes produced by worker code must travel through callback results and be applied on the main thread after generation validation. Pico intentionally provides no generic extension-state subsystem.
