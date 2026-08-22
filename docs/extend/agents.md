# Agents

Pico owns a heap `PicoAgentManager` that can run up to `PICO_MAX_AGENTS` independent agents concurrently. `PicoAgent`, `PicoAgentManager`, and `PicoAgentContext` are opaque.

Include `pico/agent.h` directly, or include `pico/plugin.h`.

## Identity and snapshots

`PicoAgentId` identifies one in-memory agent for the lifetime of the process. It is distinct from the durable JSONL `session_id`. Runtime generation identifies one worker generation inside that agent; force cancellation replaces the generation without changing the agent ID.

Use the main-thread manager API; `PicoApp` exposes only the opaque `app->agents` owner:

```c
for (int i = 0; i < pico_agent_count(app); i++) {
    PicoAgentInfo info;
    if (pico_agent_info(app, i, &info)) { /* copied snapshot */ }
}
PicoAgentId active = pico_agent_active(app);
pico_agent_select(app, active);
```

`pico_agent_find` returns a copied snapshot by ID. `pico_agent_create`, `pico_agent_close`, `pico_agent_cancel`, and `pico_agent_force_cancel` return a controlled `PicoAgentResult`. Close rejects busy agents, retained-runtime references, and the final live agent. IDs become stale after close or workspace replacement. Selection clears transcript selection/scroll snapshots but leaves the global composer draft unchanged.

`pico_agent_message_count` and `pico_agent_message` provide bounded, main-thread-only borrowed transcript inspection. The message pointer is invalidated by pumping, transcript mutation, close, or workspace replacement.

## Profiles and asks

`pico_subagent_profile_count` and `pico_subagent_profile_info` return copied snapshots of valid profiles discovered directly under `$XDG_CONFIG_HOME/pico/subagents/` (or `~/.config/pico/subagents/`). Invalid files are isolated and reported in the warning overlay. Loaded snapshots are replaced on reload.

`pico_tool_pending_ask` returns the oldest live ask across all agents, including hidden agents; its `agent_id`, `profile`, and `purpose` identify the owner. `pico_tool_answer` routes by globally unique ask ID. The borrowed request remains valid only until the next manager pump.

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
