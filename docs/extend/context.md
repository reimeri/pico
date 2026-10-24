# Request context

Request-context hooks append app-authored, request-only text to an LLM request without changing system instructions or saved agent history.

Register on the main thread in `workspace_init` (full file: [`../../examples/ephemeral_context.c`](../../examples/ephemeral_context.c)):

```c
#include "pico/plugin.h"
#include "json.h"

static void AddReminder(PicoWorkspace *workspace, PicoAgentId agent_id, PicoContextEvent *ev,
                        void *state)
{
    (void)workspace;
    (void)agent_id;
    (void)state;
    if (ev->compact)
    {
        return;
    }
    ev->extra_context = JsonDup("Remember to verify the result.");
}

static int Init(PicoWorkspace *workspace, void **state_out)
{
    (void)state_out;
    pico_add_context_hook(workspace, AddReminder);
    return 0;
}
```

`extra_context` is malloc'd text. Pico wraps it as a canonical `context` input item, appends it only to the copied input for that request, and frees it. It is not a user utterance, is not merged into `instructions`, and is not added to the live history, session JSONL, or compaction summary.

Providers must map `type: "context"` to a non-user role (`PicoProvider.map_context`). Builtin Responses uses `developer`; builtin Completions uses a trailing `system` message. Pico fails the turn if `input_json` contains a context item and the provider did not set `map_context`.

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
{"type":"context","parts":[{"type":"text","text":"..."}]}
```

Synthetic text-compaction briefings are user-shaped items with `"synthetic": true`.
Genuine user messages are not marked. Native state has its own `provider_state`
type and is neither text nor a reasoning signature.

`context` items are request-only. Pico appends them after wrapping `extra_context`; they never appear in the hook's `history_json`. `thinking`, `thinking_signature`, and `item_id` are optional. User and assistant content is an ordered `parts` array (`text`, `refusal`, `image`, `audio`). Image/audio parts reference a `path` (optional `mime` / `url`); they never store bytes. Provider-native state appears only after a provider projects it into these canonical fields. Parse items with `json.h`. Treat all history strings as read-only and callback-scoped.

## Choosing the right hook

- Use a context hook for dynamic reminders or session state that should be billed only for the current request and must not look like a user message.
- Use `pico_add_llm_hook` when changing system instructions or filtering tools.
- Use normal user/assistant history when content must persist and replay.

`/show-prompt` displays system instructions and therefore does not run context hooks.
Context hooks run for ordinary and text-compaction requests; check `compact` when
context should not influence a text summary. Native compaction skips these hooks
and does not submit `extra_context`, because the server could otherwise encode it
into an opaque checkpoint. Earlier assistant/tool history may still reflect prior
request context; this is direct exclusion, not semantic erasure of past influence.
Native compaction reuses a compatible normal instruction/tool snapshot, or builds
a fresh normal prefix with LLM hooks (`compact == false`) when no snapshot is usable.
Those instructions can influence the artifact. See
[compaction prompt snapshots](providers.md#compaction-prompt-snapshots).

After native compaction, `history_json` can include a scoped `provider_state` item
as documented in [providers](providers.md#native-compaction). It is opaque, not a
readable message or user utterance. Recognize/skip it when deriving textual
context; never interpret it as a summary or forward it to another backend.

## Contract

- Runs on the main thread immediately before queueing the provider request and receives the target `PicoAgentId`.
- Maximum 64 hooks (`PICO_MAX_CONTEXT_HOOKS`).
- `history_json`, every history item, and the effective `tools` catalog are core-owned and valid only during the callback. They are copied into the queued provider work before reload can proceed.
- `extra_context` must be malloc'd; Pico frees it.
- Do not mutate chat/composer state or call Clay from the callback.
