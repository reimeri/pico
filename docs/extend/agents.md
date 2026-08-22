# Agents

Pico owns one extracted `PicoAgent` in the current single-agent UI. `PicoAgent` is opaque to extensions; conversation, runtime, session, usage, error/activity, compaction, model, and effort state are not fields of `PicoApp`.

Include `pico/agent.h` directly, or include `pico/plugin.h` (which includes it through `pico/app.h`).

## Identity and snapshots

`PicoAgentId` is the process-lifetime identity of an in-memory agent. It is distinct from the durable JSONL `session_id`.

The current app exposes a borrowed opaque handle as `app->agent`. Do not free it or retain it beyond app/workspace lifetime. Read copied state instead:

```c
PicoAgentInfo info;
if (pico_agent_info_snapshot(app->agent, &info))
{
    /* info.state, model, effort, activity, session_id, ... */
}
```

`pico_agent_id(agent)` returns zero for `NULL`. `pico_agent_info_snapshot` returns `false` for a NULL handle/output and otherwise fills the complete copied snapshot. Snapshot strings require no freeing and do not update after the call.

Step 1 intentionally still has one active agent. Creation, selection, enumeration, and close APIs arrive with `PicoAgentManager`; extensions must not allocate `PicoAgent` themselves.

## Main-thread state

The active agent owns its transcript, provider history, runtime, session persistence, usage, selected model/effort, compaction configuration, error, and activity. The app retains workspace/UI state, registrations, auth, and the immutable model catalog/defaults.

Notification, LLM/context, apply, command, view, and frame callbacks run on the main thread and may snapshot `app->agent`. Do not inspect private `agent.h` or `agent_internal.h`.

During `PICO_HOOK_ON_COMPACT`, call `pico_agent_set_compact_summary(app, malloc_string)` to replace default model compaction. Pico takes ownership. Pass `NULL` to leave default compaction unchanged.

## Worker host

Until the callback ABI becomes agent-context based, tool, BEFORE-tool, and provider callbacks still receive `PicoApp *`. On a worker this is a heap-owned execution-host view, not the stack/UI app. It contains copied worker-facing registrations and workspace plus retained auth access. It deliberately has no active agent and must not be used for UI, transcript, session, settings, or model-catalog access. Do not retain it after the callback.

Use callback inputs/results, `pico_tool_ask`, `pico_tool_set_child`, auth helpers, cancellation, and delta callbacks rather than reaching into app state.
