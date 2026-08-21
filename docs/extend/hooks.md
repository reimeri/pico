# Hooks

Three families:

- **Notifications** — `pico_add_hook`. No payload: `void (*)(PicoApp *)`. Main thread.
- **Interceptors** — `pico_add_tool_hook` / `pico_add_llm_hook`. Event struct, can veto or rewrite. See below and `tools.md`.
- **Request context** — `pico_add_context_hook`. Appends non-persistent context; see [context](context.md).

```c
pico_add_hook(app, PICO_HOOK_BEFORE_SUBMIT, MyBeforeSubmit);
pico_add_tool_hook(app, PICO_TOOL_BEFORE, MyBeforeTool);
pico_add_llm_hook(app, MyBeforeLlm);
```

Notification hooks run in registration order. Max 64 (`PICO_MAX_HOOKS`).

## Notification kinds

All of these run on the main thread.

- `PICO_HOOK_AFTER_LAYOUT` — after Clay layout, before render. Hit-testing, pointer.
- `PICO_HOOK_AFTER_RENDER` — after `Clay_Raylib_Render`. Overlay drawing (Raylib).
- `PICO_HOOK_BEFORE_SUBMIT` — user pressed send, before a message is added. Intercept `/commands`, rewrite prompt.
- `PICO_HOOK_ON_SUBMIT` — after the user message is logged and the agent turn started.
- `PICO_HOOK_ON_MESSAGE` — after `PicoApp_AddMessage`.
- `PICO_HOOK_ON_COMPACT` — compaction starting. Custom briefing.
- `PICO_HOOK_AFTER_COMPACT` — history was replaced with a briefing. `agent_state` is still `PICO_AGENT_COMPACT_WAIT` until `ON_TURN_END`.
- `PICO_HOOK_ON_TURN_END` — the agent is idle after a finished turn. Not fired on cancel or error. Compact is not idle; this waits until compaction completes.
- `PICO_HOOK_ON_CANCEL` — the user cancelled the turn (Esc / force-cancel). State is idle. Distinct from tool-hook deny.
- `PICO_HOOK_ON_ERROR` — `agent_state` is `PICO_AGENT_ERROR`; `app->agent_error` is set.
- `PICO_HOOK_ON_SESSION_RESET` — a new, resumed, ephemeral, or changed-workspace session is starting. Clear session-scoped extension state. Session replay apply callbacks run afterward.

## BEFORE_SUBMIT

`PicoApp_Submit` clears `submit_cancel` and `agent_input`, then runs this hook.

- Set `app->submit_cancel = true` to swallow the send (slash commands do this).
- Set `app->agent_input` to a malloc'd string to send that text to the agent instead of the composer. The display still uses composer text. Pico frees `agent_input` after the turn starts.

If any hook sets `submit_cancel`, later hooks still run, then submit returns without starting the agent.

The builtin `commands` extension dispatches `/name` here. The builtin `files` extension expands `@path` into `agent_input`.

## ON_COMPACT

Set `app->compact_summary` to a malloc'd briefing to skip the default LLM compact. Pico frees it. Leave it NULL to keep the default.

`PICO_HOOK_AFTER_COMPACT` runs after that briefing is applied (custom or LLM). Then `PICO_HOOK_ON_TURN_END` if the agent goes idle.

## Tool interceptors

`pico_add_tool_hook(app, kind, fn)` with `PICO_TOOL_BEFORE` or `PICO_TOOL_AFTER`. Max 64 (`PICO_MAX_TOOL_HOOKS`). Callback is `void (*)(PicoApp *, PicoToolEvent *)`.

`PicoToolEvent`:

- `name`, `call_id`, `args_json` — current call; core-owned
- `args_json_out` — BEFORE only: malloc'd rewrite; Pico frees. Later BEFORE hooks and the tool see the new args
- `deny` — BEFORE only: skip remaining BEFORE hooks and do not call `run`
- `output` — AFTER only: current output, including rewrites from earlier AFTER hooks; core-owned
- `details_json` — AFTER only: validated structured details from the executed tool; core-owned and read-only
- `executed` / `is_error` — AFTER only: outcome metadata. Denied calls have `executed = false` and `is_error = true`
- `result` — malloc'd. BEFORE: used only with `deny` (default `User denied this tool.`). AFTER: replaces the output sent to the model

**BEFORE** runs on the worker, in the tool slot, before `run`. You may call `pico_tool_ask`. First `deny` wins. After the hooks return, core checks cancel: Esc/`PICO_ASK_CANCEL` wins over deny and still does not call `run`. Overlay Deny is not Esc — set `deny` yourself (see `examples/permit_tool.c`).

**AFTER** runs on the main thread when a result is about to go to the model (allow and deny). Skipped on turn cancel. `args_json` is read-only. Non-NULL `result` replaces output; later AFTER hooks see the new text.

Do not use Clay from BEFORE. Permission UI goes through `pico_tool_ask` + overlay, same as a tool. Structured details are applied before AFTER hooks; AFTER hooks can rewrite visible output but cannot rewrite authoritative details.

## LLM interceptor

`pico_add_llm_hook(app, fn)`. Max 64 (`PICO_MAX_LLM_HOOKS`). Main thread, every `QueueLlm` including compact.

`PicoLlmEvent`:

- `compact`, `include_tools` — read-only. When `include_tools` is false, `exclude` is NULL and the catalog is omitted
- `tools` / `tool_count` — current catalog
- `exclude` — set `exclude[i] = true` to drop that tool from this round
- `instructions` — current blob (core-owned)
- `extra_instructions` — malloc'd; Pico appends it after this hook (`\n\n`) and frees it. Later hooks see the accumulated blob

The provider receives a filtered copy of the catalog (`turn.tools`), not necessarily `app->tools`. Example: `examples/extra_instructions.c`.

`/show-prompt` runs the same hooks with `compact = false` and `include_tools = true`, so the modal matches the next normal turn's system instructions. Request-context hooks are separate and are not shown there.
