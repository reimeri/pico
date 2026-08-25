# Hooks

Three families:

- **Notifications** — `pico_add_hook`. Main thread, with a `PicoHookEvent` target.
- **Tool interceptors** — `pico_add_tool_before_hook` and `pico_add_tool_after_hook`.
- **LLM/request context** — `pico_add_llm_hook` and `pico_add_context_hook`, with a target agent ID.

```c
pico_add_hook(app, PICO_HOOK_BEFORE_SUBMIT, MyBeforeSubmit);
pico_add_tool_before_hook(app, MyBeforeTool);
pico_add_tool_after_hook(app, MyAfterTool);
pico_add_llm_hook(app, MyBeforeLlm);
```

Callbacks run in registration order. Each family has its corresponding `PICO_MAX_*_HOOKS` limit.

## Notifications

```c
static void MyHook(PicoApp *app, const PicoHookEvent *event)
{
    PicoAgentId target = event->agent_id;
}
```

All notifications run on the main thread. Agent-scoped notifications carry the affected ID; layout/render and submit hooks carry the active ID.

- `PICO_HOOK_AFTER_LAYOUT` — after Clay layout, before render. App-global UI work; target is active agent.
- `PICO_HOOK_AFTER_RENDER` — after `Clay_Raylib_Render`. App-global UI work; target is active agent.
- `PICO_HOOK_BEFORE_SUBMIT` — intercept/rewrite the global composer submit for the active agent.
- `PICO_HOOK_ON_SUBMIT` — the target user message was logged and its turn started.
- `PICO_HOOK_ON_MESSAGE` — a message was added to the target transcript.
- `PICO_HOOK_ON_COMPACT` — target compaction is starting.
- `PICO_HOOK_AFTER_COMPACT` — target history was replaced with a briefing.
- `PICO_HOOK_ON_TURN_END` — target became idle after a completed turn, not cancel/error.
- `PICO_HOOK_ON_CANCEL` — target turn was cancelled.
- `PICO_HOOK_ON_ERROR` — target entered `PICO_AGENT_ERROR`.
- `PICO_HOOK_ON_SESSION_RESET` — target starts a new/resumed/ephemeral session, or a reload re-announces a live session; clear only that ID's session state. After reload, structured tool details are replayed to rebuild it.
- `PICO_HOOK_ON_AGENT_DESTROY` — target is about to become invalid; remove its ID-keyed extension state.

## BEFORE_SUBMIT

`PicoApp_Submit` clears `submit_cancel`, `agent_input`, and `agent_parts`, then runs the hook. Set `app->submit_cancel = true` to swallow the send. Set `app->agent_input` to a malloc'd replacement sent to the model; the composer text remains the display text and Pico frees the replacement. Whitespace-only composer text is skipped unless pasted image attachments are present.

For structured input, set `app->agent_parts` to a malloc'd JSON array of canonical parts in model-facing order. Supported user parts are `text` (`text`), `image`, and `audio` (`path`, optional `mime` / `url`). Keep bytes and base64 out of this JSON; provider converters read local `path` values when sending the request. When `agent_parts` is set, include the complete text part as well as attachments because it replaces the normal one-text-part user item. After the hook, Pico validates `agent_parts`, then appends any pasted composer images (or builds `[text, image…]` when hooks left parts unset). Invalid hook parts reject the submission without discarding the composer draft. Pasted images also block submission, preserving the draft, when the active model does not support vision. Chat display may include markdown images for those files; `agent_input` and the text part stay as typed/hook text. Pico frees `agent_parts` after submit or cancellation.

## ON_COMPACT

```c
static void OnCompact(PicoApp *app, const PicoHookEvent *event)
{
    char *briefing = /* malloc */;
    pico_agent_set_compact_summary(app, event->agent_id, briefing);
}
```

Call only during `PICO_HOOK_ON_COMPACT`. Pico takes ownership. A stale/mismatched ID is rejected and the string is freed.

## Before-tool interceptor

```c
static void Before(PicoAgentContext *ctx, PicoToolEvent *event)
{
    /* worker thread; callback-scoped ctx */
}

pico_add_tool_before_hook(app, Before);
```

Before hooks run on the worker immediately before the offered tool. They may rewrite `args_json_out`, set `deny`, set a malloc'd denial `result`, or call `pico_tool_ask(ctx, ...)`. First deny stops later before hooks. Cancellation wins over denial.

The callback may overlap worker callbacks for other agents. Do not use Clay/UI or mutate main-thread extension state.

## After-tool interceptor

```c
static void After(PicoApp *app, PicoAgentId agent_id, PicoToolEvent *event)
{
    /* serialized main thread */
}

pico_add_tool_after_hook(app, After);
```

After hooks run on the main thread after structured details have been applied. A malloc'd `event->result` rewrites model-visible output; later hooks see the rewrite. They are skipped on turn cancellation.

`PicoToolEvent` fields:

- `name`, `call_id`, `args_json` — current call, core-owned.
- `args_json_out` — before only; malloc'd replacement.
- `deny` / `result` — before denial.
- `output`, `details_json`, `executed`, `is_error` — after outcome.
- `result` — after only; malloc'd output replacement.

Hidden or unoffered calls invoke neither before nor after hooks.

## LLM interceptor

```c
static void Llm(PicoApp *app, PicoAgentId agent_id, PicoLlmEvent *event)
{
    event->extra_instructions = JsonDup("Prefer short answers.");
}
```

Full file: [`../../examples/extra_instructions.c`](../../examples/extra_instructions.c).

LLM hooks run on the serialized main thread for every request, including compaction. They see only tools permitted by the agent policy. Hooks run twice per request in registration order: first a filtering pass where `exclude[i] = true` hides a tool from this request (any `extra_instructions` set during this pass is discarded), then an instructions pass where every hook sees the final exclusion set and malloc'd `extra_instructions` is appended under a shared `## Additional instructions` section for later hooks. The heading is omitted when no hook contributes a non-empty extra.

The provider receives a retained copy of the final catalog. `/show-prompt` runs the same hooks with the active target. Reload cannot unload hooks while a runtime retains a callback/catalog snapshot; it waits for full quiescence and releases idle snapshots first.
