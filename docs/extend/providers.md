# Providers

An LLM provider implements one streaming turn. Builtins are `openai` ([`../../builtins/openai.c`](../../builtins/openai.c)) and `hyper` ([`../../builtins/hyper.c`](../../builtins/hyper.c)). Shared request/stream helpers: [`../../builtins/responses.c`](../../builtins/responses.c) for OpenAI, and [`../../builtins/completions.c`](../../builtins/completions.c) for Hyper and other OpenAI-compatible Chat Completions providers. Models in `settings.json` name the provider:

```json
{ "id": "gpt-4o", "name": "GPT-4o", "provider": "openai", "context_limit": 128000 }
```

Hyper models use `"provider": "hyper"` at the fixed HTTPS base `https://hyper.charm.land/v1`. The catalog is static `settings.json`; Pico does not fetch Hyper's model list at runtime. Hyper rejects non-canonical `base_url` overrides so workspace settings cannot redirect Hyper credentials.

Hyper talks to `POST https://hyper.charm.land/v1/chat/completions`. Requests use `store: false`, `stream_options: { "include_usage": true }`, `max_tokens` when set, and DeepSeek thinking (`thinking: { "type": "enabled"|"disabled" }` plus `reasoning_effort` when effort is on). If the model is reasoning, replayed assistant messages include `reasoning_content` (the stored thinking text, or `""`). A reasoning-compatibility retry removes the root thinking controls and historical message-level `reasoning_content`. Encrypted OpenAI reasoning blobs and Responses `function_call` item ids are dropped. Authenticate with `HYPER_API_KEY` or `/login hyper`. Hyper rejects non-canonical `base_url` values; only `https://hyper.charm.land/v1` or `https://hyper.charm.land/v1/chat/completions` are accepted.

`provider` must match `PicoProvider.name`. Providers may interpret optional `base_url` values; the builtin OpenAI provider accepts overrides, while Hyper only accepts its canonical endpoint.

```c
#include "pico/plugin.h"
#include "pico/http.h"
#include "json.h"

static int MyStream(PicoAgentContext *ctx, const PicoLlmTurn *turn, PicoLlmCancelFn cancel,
                    PicoLlmDeltaFn on_delta, void *user, PicoLlmResult *out)
{
    (void)ctx;
    (void)turn;
    (void)cancel;
    if (on_delta)
    {
        on_delta(user, PICO_LLM_DELTA_TEXT, "hello", 5);
    }
    out->assistant_text = JsonDup("hello");
    return PICO_LLM_OK;
}

static void MyInit(PicoApp *app)
{
    pico_add_provider(app, &(PicoProvider){.name = "myllm", .stream = MyStream});
}
```

Add a catalog entry with `"provider": "myllm"` or a builtin (`openai`, `hyper`) is used instead.

## Turn

`PicoLlmTurn` is read-only. Important fields: `model`, `base_url` (may be empty), `instructions`, `effort`, `compact`, `include_tools`, `input_json` / `input_count` (serialized history), `tools` / `tool_count`.

`tools` is the retained effective catalog for this round after agent policy and `pico_add_llm_hook` exclusions. It may be empty or a subset of registered tools. Calls are authorized and resolved against this exact snapshot. Pointers inside each `PicoTool` stay extension-owned; reload and workspace replacement are deferred while a live/retired runtime retains them or has undrained events that can start follow-up work.

Call `on_delta(user, kind, s, n)` as tokens arrive (`PICO_LLM_DELTA_TEXT`, `_THINKING`, `_THINKING_SUMMARY`, `_STATUS`). Check `cancel(user)` and return `PICO_LLM_CANCEL` if it is true.

`PICO_LLM_DELTA_THINKING` appends raw thinking. `PICO_LLM_DELTA_THINKING_SUMMARY` replaces the current reasoning-summary snapshot (OpenAI-style short titles). A zero-length `THINKING_SUMMARY` starts a new step in that streak; Pico coalesces consecutive summaries until a tool call and shows an `Nx` counter. After a tool, the next summary starts a new line.

## Result

Fill `PicoLlmResult` with malloc'd strings. Pico calls `pico_llm_result_free`. Return `PICO_LLM_OK`, `PICO_LLM_FAIL` (set `out->error`), or `PICO_LLM_CANCEL`.

- `assistant_text`, `think_text`, `think_signature` (opaque replay token: Completions uses `"reasoning_content"`; Responses stores the reasoning item JSON)
- `calls[]` — `call_id`, `name`, `arguments` (JSON object text), optional `item_id` (Responses `function_call.id`)
- `raw_items[]` — optional provider-native state for live follow-ups. Pico tags each item with the producing provider; request converters must replay only matching-provider items and ignore the rest. The builtin Responses converter does this, while Completions ignores all raw items. Raw items are not durable; session history persists thinking text, `thinking_signature`, and tool-call `item_id` instead.
- `input_tokens`, `cached_tokens` report this completed provider call's input usage. `cached_tokens` is the cached portion of `input_tokens`. Pico ignores usage when `input_tokens <= 0` and clamps cached usage to the input range.

Each successful provider completion with valid usage contributes to the owning agent's saved-session totals, including tool follow-ups and compaction calls. Current-window and cumulative cache accounting are agent-owned; failed and cancelled calls do not contribute.

HTTP helpers: `pico_http_post_sse`, `pico_http_post`, `pico_http_get`, `pico_http_form_encode` in `pico/http.h`. `pico_http_get` uses the same `PicoHttpReq` as POST and ignores `body`. Buffered requests return `PICO_HTTP_OK` when the transfer completes even for HTTP 4xx/5xx; inspect `out_http` for application status. See [contracts](contracts.md#http-helpers) for ownership, cancellation, redirects, and timeout behavior.

## Contract

- Stream runs on the **worker thread** with a callback-scoped `PicoAgentContext *`, never the UI app. Do not retain it, use Clay, mutate UI, or inspect agent state outside context accessors. Provider callbacks for different agents may overlap. Status text goes through `on_delta(..., PICO_LLM_DELTA_STATUS, ...)`.
- `name` must outlive the extension. Max 16 providers (`PICO_MAX_PROVIDERS`).
- Look up credentials with `pico_auth_copy_ctx(ctx, ...)` — see `auth.md`.
- Empty/duplicate call IDs, malformed call arrays, and more than 16 pending calls fail the provider round explicitly.
- Shutdown gives all provider/tool callbacks one shared deadline. A provider still blocked at expiry is detached; its extension/auth/curl services stay loaded and Pico becomes permanently unusable until process exit.
