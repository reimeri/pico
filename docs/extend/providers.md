# Providers

An LLM provider implements one streaming turn. Builtins are `openai` ([`../../builtins/openai.c`](../../builtins/openai.c)), `hyper` ([`../../builtins/hyper.c`](../../builtins/hyper.c)), and `xai` ([`../../builtins/xai.c`](../../builtins/xai.c)). Shared request/stream helpers: [`../../builtins/responses.c`](../../builtins/responses.c) for OpenAI, and [`../../builtins/completions.c`](../../builtins/completions.c) for Hyper, xAI, and other OpenAI-compatible Chat Completions providers. Models in `settings.json` name the provider:

```json
{ "id": "gpt-5.6-sol", "name": "GPT-5.6-Sol", "provider": "openai", "context_limit": 128000 }
```

Hyper models use `"provider": "hyper"` at the fixed HTTPS base `https://hyper.charm.land/v1`. The catalog is static `settings.json`; Pico does not fetch Hyper's model list at runtime. Hyper rejects non-canonical `base_url` overrides so workspace settings cannot redirect Hyper credentials.

Hyper talks to `POST https://hyper.charm.land/v1/chat/completions`. Requests use `store: false`, `stream_options: { "include_usage": true }`, `max_tokens` when set, and DeepSeek thinking (`thinking: { "type": "enabled"|"disabled" }` plus `reasoning_effort` when effort is on). Each completion choice projects to one canonical assistant item followed by all of that message's tool-call items, regardless of streamed field order. Replay folds those items back into one assistant message; a tool-only message uses `content: null`. If the model is reasoning, replayed assistant messages include `reasoning_content` (the stored thinking text, or `""`). A reasoning-compatibility retry removes the root thinking controls and historical message-level `reasoning_content`. Encrypted OpenAI reasoning blobs and Responses `function_call` item ids are dropped. Authenticate with `HYPER_API_KEY` or `/login hyper`. Hyper rejects non-canonical `base_url` values; only `https://hyper.charm.land/v1` or `https://hyper.charm.land/v1/chat/completions` are accepted.

xAI talks to `POST https://api.x.ai/v1/chat/completions`. Requests use `store: false` and `stream_options: { "include_usage": true }`. Incoming `reasoning` / `reasoning_content` still streams; Pico does not send DeepSeek `thinking` or `reasoning_effort`. Authenticate with `XAI_API_KEY` or `/login xai`. xAI rejects non-canonical `base_url` values; only `https://api.x.ai/v1` or `https://api.x.ai/v1/chat/completions` are accepted.

`provider` must match `PicoProvider.name`. Providers may interpret optional `base_url` values; the builtin OpenAI provider accepts overrides, while Hyper and xAI only accept their canonical endpoints.

```c
#include "pico/plugin.h"
#include "pico/http.h"
#include "json.h"

static int MyStream(PicoAgentContext *ctx, const PicoLlmTurn *turn, PicoLlmCancelFn cancel,
                    PicoLlmDeltaFn on_delta, void *user, PicoLlmResult *out, void *state)
{
    (void)ctx;
    (void)turn;
    (void)cancel;
    (void)state;
    if (on_delta)
    {
        on_delta(user, PICO_LLM_DELTA_TEXT, "hello", 5);
    }
    pico_llm_result_add_text(out, "hello");
    return PICO_LLM_OK;
}

static int MyInit(PicoWorkspace *workspace, void **state_out)
{
    (void)state_out;
    pico_add_provider(workspace, &(PicoProvider){.name = "myllm", .stream = MyStream});
    return 0;
}
```

Add a catalog entry with `"provider": "myllm"` or a builtin (`openai`, `hyper`) is used instead.

## Turn

`PicoLlmTurn` is read-only. Important fields: `model`, `base_url` (may be empty), `instructions`, `effort`, `compact`, `include_tools`, `vision`, `input_json` / `input_count` (canonical items, oldest first), `tools` / `tool_count`.

`input_json` uses Pico's provider-neutral item forms. `user` and `assistant` carry a `parts` array (`text`, `refusal`, `image`, `audio`). `tool_call` and `tool_result` use `call_id` / `name` / `arguments` or `output`. Request-only `context` items are:

```json
{"type":"context","parts":[{"type":"text","text":"..."}]}
```

They come from context hooks, are not persisted in history, and must be mapped to a non-user role. Set `PicoProvider.map_context` when the stream does that. Pico fails the turn if a context item is present and `map_context` is false.

`tools` is the retained effective catalog for this round after agent policy and `pico_add_llm_hook` exclusions. It may be empty or a subset of registered tools. Calls are authorized and resolved against this exact snapshot. Pointers inside each `PicoTool` stay extension-owned; reload and workspace replacement are deferred while a live/retired runtime retains them or has undrained events that can start follow-up work.

Call `on_delta(user, kind, s, n)` as tokens arrive (`PICO_LLM_DELTA_TEXT`, `_THINKING`, `_THINKING_SUMMARY`, `_STATUS`). Check `cancel(user)` and return `PICO_LLM_CANCEL` if it is true.

`PICO_LLM_DELTA_THINKING` appends raw thinking. `PICO_LLM_DELTA_THINKING_SUMMARY` replaces the current reasoning-summary snapshot (OpenAI-style short titles). A zero-length `THINKING_SUMMARY` starts a new step in that streak; Pico coalesces consecutive summaries until a tool call. The thinking widget title is the latest summary; expanding it shows every step. Durable sessions preserve the ordered steps. After a tool, the next summary starts a new line.

## Result

Fill `PicoLlmResult` with malloc'd strings and ordered items. Pico calls `pico_llm_result_free`. Return `PICO_LLM_OK`, `PICO_LLM_FAIL` (set `out->error`), or `PICO_LLM_CANCEL`. Helpers such as `pico_llm_result_add_text`, `pico_llm_result_add_refusal`, and `pico_llm_result_add_tool_call` append items so simple providers do not hand-build arrays.

- `items[]` — ordered canonical history objects for this provider completion: assistant items (`parts`, optional `thinking` / `thinking_signature`) and tool-call items (`call_id`, `name`, `arguments`, optional `item_id`)
- Assistant `parts`: `text`, `refusal` (model declined; this is user-visible output), `image`, and `audio`. Image/audio store `path` (and optional `mime` / `url`), never file bytes. Completions thinking signatures use `"reasoning_content"`; Responses stores the reasoning item JSON on `thinking_signature`
- `input_tokens`, `cached_tokens` report this completed provider call's input usage. `cached_tokens` is the cached portion of `input_tokens`. Pico ignores usage when `input_tokens <= 0` and clamps cached usage to the input range.

Providers must project every replay-critical value into these canonical items, walking native output in order. Canonical item boundaries are provider-history boundaries, not chat-bubble boundaries: Pico combines all items rendered into one live assistant message and records that message group explicitly so session replay restores the identical bubble and trace layout. Unknown wire types, hosted tool calls, and non-empty annotations fail the turn with `unsupported output: <type>`. Pico does not drop unrecognized output. Request converters rebuild the active provider's wire format from canonical history (`path` is read at send time into data URLs / `input_image` / `image_url`). If history contains media and the model does not accept images, the turn fails.

Builtin Responses maps `type: "context"` to a `developer` input message; builtin Completions maps it to a trailing `system` message. Custom providers must do the same in a non-user role and set `map_context`.

Each successful provider completion with valid usage contributes to the owning agent's saved-session totals, including tool follow-ups and compaction calls. Current-window and cumulative cache accounting are agent-owned; failed and cancelled calls do not contribute.

HTTP helpers: `pico_http_post_sse`, `pico_http_post`, `pico_http_get`, `pico_http_form_encode` in `pico/http.h`. `pico_http_get` uses the same `PicoHttpReq` as POST and ignores `body`. Buffered requests return `PICO_HTTP_OK` when the transfer completes even for HTTP 4xx/5xx; inspect `out_http` for application status. See [contracts](contracts.md#http-helpers) for ownership, cancellation, redirects, and timeout behavior.

## Contract

- Stream runs on the **worker thread** with a callback-scoped `PicoAgentContext *`, never the UI app. Do not retain it, use Clay, mutate UI, or inspect agent state outside context accessors. Provider callbacks for different agents may overlap. Status text goes through `on_delta(..., PICO_LLM_DELTA_STATUS, ...)`.
- `name` must outlive the extension. Max 16 providers (`PICO_MAX_PROVIDERS`).
- Set `map_context` if the stream maps canonical `type: "context"` items to a non-user role. Pico fails the turn when those items are present and the flag is false.
- Look up credentials with `pico_auth_copy_ctx(ctx, ...)` — see `auth.md`.
- Empty/duplicate call IDs, malformed call arrays, and more than 16 pending calls fail the provider round explicitly.
- Shutdown gives all provider/tool callbacks one shared deadline. A provider still blocked at expiry is detached; its extension/auth/curl services stay loaded and Pico becomes permanently unusable until process exit.
