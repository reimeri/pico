# Request context

Request-context hooks append low-priority, non-persistent text to an LLM request without changing system instructions or saved agent history.

Register on the main thread in `init` (full file: [`../../examples/ephemeral_context.c`](../../examples/ephemeral_context.c)):

```c
#include "pico/plugin.h"
#include "json.h"

static void AddReminder(PicoApp *app, PicoAgentId agent_id, PicoContextEvent *ev)
{
    (void)app;
    (void)agent_id;
    if (ev->compact)
    {
        return;
    }
    ev->extra_context = JsonDup("Remember to verify the result.");
}

static void Init(PicoApp *app)
{
    pico_add_context_hook(app, AddReminder);
}
```

`extra_context` is malloc'd text. Pico wraps it as a user-context input item, appends it only to the copied input for that request, and frees it. It is not added to the live history, session JSONL, compaction summary, or system instructions.

## Event

`PicoContextEvent` contains:

- `compact` — true for a compaction request.
- `history_json` / `history_count` — immutable base history copied for this request.
- `tools` / `tool_count` — final effective tool catalog after agent policy and LLM-hook exclusions.
- `extra_context` — set to one malloc'd text value to append.

Every context hook sees the same immutable base history. Context returned by an earlier hook is accumulated separately and cannot change a later hook's history-tail decisions.

History items use Pico's provider-neutral JSON forms:

```json
{"type":"user","parts":[{"type":"text","text":"..."},{"type":"image","path":"...","mime":"image/png"}]}
{"type":"assistant","parts":[{"type":"text","text":"..."},{"type":"refusal","text":"..."}],"thinking":"...","thinking_signature":"..."}
{"type":"tool_call","call_id":"...","name":"...","arguments":"...","item_id":"..."}
{"type":"tool_result","call_id":"...","name":"...","output":"...","is_error":false}
```

`thinking`, `thinking_signature`, and `item_id` are optional. User and assistant content is an ordered `parts` array (`text`, `refusal`, `image`, `audio`). Image/audio parts reference a `path` (optional `mime` / `url`); they never store bytes. Provider-native state appears only after a provider projects it into these canonical fields. Parse items with `json.h`. Treat all history strings as read-only and callback-scoped.

## Choosing the right hook

- Use a context hook for dynamic reminders or session state that should be lower priority and billed only for the current request.
- Use `pico_add_llm_hook` when changing system instructions or filtering tools.
- Use normal user/assistant history when content must persist and replay.

`/show-prompt` displays system instructions and therefore does not run context hooks. Context hooks run for every queued provider request, including compaction; check `compact` when the context should not influence a summary.

## Contract

- Runs on the main thread immediately before queueing the provider request and receives the target `PicoAgentId`.
- Maximum 64 hooks (`PICO_MAX_CONTEXT_HOOKS`).
- `history_json`, every history item, and the effective `tools` catalog are core-owned and valid only during the callback. They are copied into the queued provider work before reload can proceed.
- `extra_context` must be malloc'd; Pico frees it.
- Do not mutate chat/composer state or call Clay from the callback.
