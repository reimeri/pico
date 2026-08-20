# Hooks

```c
pico_add_hook(app, PICO_HOOK_BEFORE_SUBMIT, MyBeforeSubmit);
```

Hooks run in registration order. No payload: the callback is `void (*)(PicoApp *)`. Max 64 (`PICO_MAX_HOOKS`).

## Kinds

All of these run on the main thread.

- `PICO_HOOK_AFTER_LAYOUT` — after Clay layout, before render. Hit-testing, pointer.
- `PICO_HOOK_AFTER_RENDER` — after `Clay_Raylib_Render`. Overlay drawing (Raylib).
- `PICO_HOOK_BEFORE_SUBMIT` — user pressed send, before a message is added. Intercept `/commands`, rewrite prompt.
- `PICO_HOOK_ON_SUBMIT` — after the user message is logged and the agent turn started.
- `PICO_HOOK_ON_MESSAGE` — after `PicoApp_AddMessage`.
- `PICO_HOOK_ON_COMPACT` — compaction starting. Custom briefing.

## BEFORE_SUBMIT

`PicoApp_Submit` clears `submit_cancel` and `agent_input`, then runs this hook.

- Set `app->submit_cancel = true` to swallow the send (slash commands do this).
- Set `app->agent_input` to a malloc'd string to send that text to the agent instead of the composer. The display still uses composer text. Pico frees `agent_input` after the turn starts.

If any hook sets `submit_cancel`, later hooks still run, then submit returns without starting the agent.

The builtin `commands` extension dispatches `/name` here. The builtin `files` extension expands `@path` into `agent_input`.

## ON_COMPACT

Set `app->compact_summary` to a malloc'd briefing to skip the default LLM compact. Pico frees it. Leave it NULL to keep the default.
