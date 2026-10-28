# Providers

An LLM provider implements one streaming turn. Builtins are `openai` ([`../../builtins/openai.c`](../../builtins/openai.c)), `hyper` ([`../../builtins/hyper.c`](../../builtins/hyper.c)), and `xai` ([`../../builtins/xai.c`](../../builtins/xai.c)). Shared request/stream helpers: [`../../builtins/responses.c`](../../builtins/responses.c) for OpenAI, and [`../../builtins/completions.c`](../../builtins/completions.c) for Hyper, xAI, and other OpenAI-compatible Chat Completions providers. Models in `settings.json` name the provider:

```json
{ "id": "gpt-5.6-sol", "name": "GPT-5.6-Sol", "provider": "openai", "context_limit": 128000 }
```

Hyper models use `"provider": "hyper"` at the fixed HTTPS base `https://hyper.charm.land/v1`. The catalog is static `settings.json`; Pico does not fetch Hyper's model list at runtime. Hyper rejects non-canonical `base_url` overrides so workspace settings cannot redirect Hyper credentials.

Hyper talks to `POST https://hyper.charm.land/v1/chat/completions`. Requests use `store: false`, `stream_options: { "include_usage": true }`, `max_tokens` when set, and DeepSeek thinking (`thinking: { "type": "enabled"|"disabled" }` plus `reasoning_effort` when effort is on). Each completion choice projects to one canonical assistant item followed by all of that message's tool-call items, regardless of streamed field order. Replay folds those items back into one assistant message; a tool-only message uses `content: null`. If the model is reasoning, replayed assistant messages include `reasoning_content` (the stored thinking text, or `""`). A reasoning-compatibility retry removes the root thinking controls and historical message-level `reasoning_content`. Encrypted OpenAI reasoning blobs and Responses `function_call` item ids are dropped. Authenticate with `HYPER_API_KEY` or `/login hyper`. Hyper rejects non-canonical `base_url` values; only `https://hyper.charm.land/v1` or `https://hyper.charm.land/v1/chat/completions` are accepted.

xAI talks to `POST https://api.x.ai/v1/chat/completions`. Requests use `store: false` and `stream_options: { "include_usage": true }`. When `PicoLlmTurn.cache_key` is set, it is sent as the `x-grok-conv-id` header so repeated turns can keep prompt-cache affinity. Incoming `reasoning` / `reasoning_content` still streams; Pico does not send DeepSeek `thinking` or `reasoning_effort`. Authenticate with `XAI_API_KEY` or `/login xai`. xAI rejects non-canonical `base_url` values; only `https://api.x.ai/v1` or `https://api.x.ai/v1/chat/completions` are accepted.

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
        PicoLlmDelta d = {.kind = PICO_LLM_DELTA_TEXT, .text = "hello", .len = 5, .call_index = -1};
        on_delta(user, &d);
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

Add a catalog entry with `"provider": "myllm"` or a builtin (`openai`, `hyper`, `xai`) is used instead.

## Cache affinity and turn-local routing

`PicoLlmTurn.cache_key` is stable for the saved session, including text/native
compaction and resume. Only a new/reset session gets a new key. OpenAI sends it as
`prompt_cache_key` and the `session-id` header; xAI sends `x-grok-conv-id`.
Affinity is a routing hint, not a promise that a provider will retain the cache.

ABI 27 adds `PicoLlmTurn.turn_state` and `PicoLlmResult.turn_state` for **ephemeral
transport state**, distinct from canonical `provider_state` history:

- A provider borrows immutable `turn->turn_state`, or NULL on the first request.
- On success, return a malloc'd replacement in `out->turn_state`; NULL clears it.
  Core adopts it after validating the result and frees it at replacement/runtime
  destruction. Other results are freed by `pico_llm_result_free`.
- State is isolated per agent/runtime and forwarded only within the same turn:
  tool follow-ups, delivered steering, and automatic compaction. A new user turn
  or standalone `/compact` starts without it, including after cancellation/error.
- It is never serialized, placed in model input, copied to another agent, or
  restored from a session. Providers must validate its backend/credential scope
  against actual authentication, including after refresh.
- Retries inside one provider callback must retain their state locally; core only
  sees the final result. Never store turn routing in shared provider instance state.

Codex captures the first valid `x-codex-turn-state` response header and echoes it
unchanged through tool continuations, capacity/authentication retries, and automatic
compaction. It ignores interim/redirect routing headers, rejects control characters,
and clears old routing when the OAuth account changes. Ordinary access-token refresh
within the same account preserves routing. The API-key/custom-gateway routes neither
capture nor replay Codex turn tokens. See [HTTP response headers](http.md#response-headers).

### Compaction prompt snapshots

Normal requests retain their final instructions and filtered tool catalog in memory.
Compaction reuses that prefix without rerunning LLM instruction/tool-filter hooks.
If no usable snapshot exists (including after resume), it builds a fresh normal
prefix: those hooks see `compact == false` and the normal tool catalog. The provider
still sees `PicoLlmTurn.compact == true` and the appropriate compaction trigger.

A snapshot is invalidated by a registration-generation change, history reset,
changed base instructions, model/provider/URL/capabilities, effort/Fast selection,
or agent tool eligibility. Standalone compaction refreshes base instructions before
checking compatibility. Snapshot tool strings borrow the retained registration;
core discards the snapshot before releasing that generation. No snapshot is persisted.
A text-compaction retry after an unwanted tool call can omit tools while retaining
the same instructions. Native compaction does not use this fallback.

Request-only context is not part of this prefix. Native compaction excludes context
hooks; text compaction invokes them with `compact == true`. LLM instructions can
still influence the compacted artifact, even when replayed from a snapshot. Use
context hooks for transient reminders. See [hooks](hooks.md#llm-interceptor).

## Native compaction

`/compact` and the existing automatic `compact_at` threshold use native server-side
compaction on the official OpenAI API and ChatGPT/Codex OAuth routes. The builtin
posts a normal streamed Responses request with a trailing `compaction_trigger`,
`store: false`, and encrypted reasoning included. Ordinary official-route requests
also use stateless replay (`store: false` and `reasoning.encrypted_content`). Custom
OpenAI-compatible gateways, Hyper, and xAI retain text-briefing compaction.

Native compaction requires a complete successful terminal response and exactly
one valid encrypted compaction artifact, consistent with streamed output. It does
not execute tools, produce a portable text summary, or retry as text on failure.
Authentication and capacity retries remain retries of the same operation. Inline
`context_management`, `/responses/compact`, response-ID chaining, WebSockets, and
background compaction are not used.

The replacement contains recent genuine user messages followed by one opaque
provider-state item. Retention keeps whole messages, newest first within the
budget, then restores chronological order; it stops at the first message that
cannot fit. Synthetic briefings and tool observations are not retained as users.
The ceiling is 64k tokens, further capped to a quarter of the model context and
reduced by instruction/tool overhead. Text uses a conservative UTF-8 byte bound;
media has a separate conservative allowance. This leaves headroom, but does not
guarantee that another model accepts the artifact. An immutable clarification
seed is retained exactly once; if it cannot fit, compaction fails without
replacing history.

A native checkpoint persists the entire ordered replacement and its authority:
provider, backend (`openai-api` or `codex`), state format, and OAuth account or
SHA-256 fingerprint of the effective API key. Raw keys and bearer tokens are
never checkpoint metadata. OAuth token rotation within the same account is
allowed. Model, effort, and Fast changes within the same authority are allowed;
backend rejection remains an error, without stripping state or falling back.
Changing backend, account, API key, or provider refuses continuation before
transmitting native state. Custom gateways cannot receive an official-route
checkpoint. Restore the original backend/account/key or start a new conversation.
The transcript remains viewable even when continuation is unavailable.

### Provider contract

An optional `PicoProvider.native_compaction(host, model, state)` callback selects
native compaction for the resolved route; NULL/false selects text. Like
`supports_fast`, it runs on the main thread, is read-only and nonblocking, borrows
its arguments, and may read a copied auth snapshot. It shares the provider's
registration generation and concurrent workspace-instance state. No network,
refresh, UI mutation, or retained argument pointers are allowed.

A non-NULL callback also opts the provider into `provider_state` input. The core
rejects state owned by another provider and providers without this opt-in. The
worker must independently validate its actual backend, format, and credentials
before request construction and after auth refresh; a main-thread query is not
an authority check. False on a later route is not permission to discard state or
summarize it through an incompatible backend.

`PicoLlmTurn.native_compact` pins the selected mode. `context_limit` is the
resolved model capacity; `compact_seed` is a borrowed immutable clarification
seed or NULL. They have the same worker-callback lifetime as other turn fields.
Continue using `stream` and `compact`; no second executor is registered.

ABI 26 supplies `compaction_history_json` / `compaction_history_count` on native
requests: an immutable, callback-borrowed copy of original canonical history,
before missing-image recovery. Use this source for retention, its media budgeting,
and checkpoint construction; use only `input_json` for the model request. Never
send the unfiltered retention source to the backend. Non-native requests supply
NULL/0. Retained messages keep their original image references (even image-only
messages), so restoring an omitted file can make it available after compaction.

For native compaction return `PICO_LLM_OK` with malloc'd
`PicoLlmResult.compaction_json`: a complete canonical JSON array containing zero
or more genuine user items followed by exactly one scoped state item. It is
mutually exclusive with ordinary `items`. A native request must return this
variant; a normal/text request must not. `pico_llm_result_free` frees it.
Usage/service-tier reporting remains on the ordinary result fields.

```json
{"type":"provider_state","provider":"openai","format":"openai.compaction.v1","backend":"openai-api","scope":"key:<fingerprint>","model":"originating-model","data":{"type":"compaction","id":"cmp_...","encrypted_content":"..."}}
```

Envelope fields are nonempty strings and `data` is an object. OpenAI's ciphertext
must be a nonempty string; its optional item ID, when present and non-null, must
be a nonempty string. Only `data` is projected onto the wire; local affinity
metadata is never sent as conversation. Keep the artifact opaque and unchanged.
Do not place it in assistant text, a reasoning signature, or a synthetic user
message. Context-hook history readers must recognize this non-conversation item
and not assume that every item contains readable message parts.

After validation the host stages replacement history, writes one checkpoint,
and only then installs it and dispatches `AFTER_COMPACT`. Failed/cancelled remote
operations leave history intact. While a durable checkpoint write is pending,
cancellation waits for its outcome: success installs the written checkpoint and
then stops continuation; failure preserves old context and reports the error.
Ephemeral agents commit in memory. The new window's token count is unknown until
a later successful provider response reports usage (the footer shows `--`);
compaction usage still contributes once to cumulative session totals.

An explicit `ON_COMPACT` text-summary override remains authoritative, even with
existing native state. A successful text checkpoint replaces that state and
clears its affinity restriction. This is an intentional extension override, not
automatic failure fallback; see [hooks](hooks.md#on_compact).

## Fast mode

A model must explicitly declare `"supports_fast": true` in `settings.json` (or enable **Supports Fast mode** in `/settings`). This is capability metadata, not the selected mode. Omission defaults to false. Configure it only for models for which the service supports priority processing. Pico also checks the provider and current authentication route (including the presence of credentials); the model flag cannot bypass those checks.

Use `/fast on`, `/fast off`, or `/fast status` (`/fast` alone shows status). The reasoning-effort dropdown contains a **Fast mode** On/Off row when supported, and shows the Fast icon next to effort while enabled. Models without effort options use the slash command only. Fast is independent of reasoning effort and does not change the model.

Fast defaults off for a new conversation. Selection is persisted in the session, retained across supported model changes, and cleared when switching to an unsupported model or removing its catalog capability. Changes during a busy turn apply to the next turn: all current tool follow-ups and automatic compaction keep the mode snapshotted at turn start. Manual compaction snapshots the current selection. Subagents default off and only opt in through their profile's `"fast": true`; see [subagents](../subagents.md).

Builtin wire mappings:

| Route | Fast on | Fast off |
| --- | --- | --- |
| OpenAI API, canonical `https://api.openai.com/v1` | `service_tier: "priority"` | `service_tier: "default"` |
| OpenAI ChatGPT/Codex OAuth | `service_tier: "priority"` | Omit `service_tier` |
| xAI API key, canonical endpoint | `service_tier: "priority"` | `service_tier: "default"` |
| xAI OAuth, canonical endpoint (experimental) | `service_tier: "priority"` | Omit `service_tier` |

OpenAI-compatible custom endpoints and Hyper do not advertise Fast support. xAI OAuth Fast is enabled experimentally: Pico sends priority requests, but backend acceptance and account eligibility have not been verified. OAuth credentials may be an access token or a refresh token; normal token refresh still runs before inference. Fast off preserves the existing OAuth request behavior by omitting the tier. Codex model eligibility is explicitly configured by the user rather than fetched from the backend. API/account/model rejection is surfaced as a request error; Pico does not retry without priority. Existing authentication/reasoning retries preserve the tier. OpenAI API off is explicit so a project's default tier cannot silently enable priority.

ChatGPT/Codex OAuth requests also send the advisory header
`x-codex-routing-hint: model=<model>;tier=priority` with Fast on, or
`x-codex-routing-hint: model=<model>` with Fast off. The hint uses the turn's
pinned model and requested tier, including tool follow-ups, compaction, and
retries; it does not claim which tier the backend actually served. Models with
invalid HTTP-header bytes omit the hint. API-key requests (including custom
endpoints) do not send it. This header is separate from the server-issued,
turn-local `x-codex-turn-state` affinity token.

### Provider capability contract

Set `PicoProvider.supports_fast` to a callback with signature:

```c
bool SupportsFast(PicoHost *host, const PicoModel *model, void *state);
```

NULL means unsupported. Pico calls it only for explicitly opted-in models, on the main thread, both to gate controls and before queuing a Fast request. Arguments are borrowed for the callback only. It must be prompt, read-only, and nonblocking: no network calls, credential refresh, UI mutation, or retaining `model`. Reading a copied auth snapshot with `pico_auth_copy` is allowed. The callback uses the same workspace-instance state as `stream`; synchronize any shared state because another agent's stream can run concurrently.

`PicoLlmTurn.fast` is an immutable boolean for this agent turn. Map it to your backend's priority option without altering effort. The worker must independently validate the authentication route it actually uses; it can change between the capability query and the request. Unsupported requests fail explicitly rather than silently downgrading. The callback belongs to its immutable registration generation, with the same reload/quiescence rules as `stream`.

Populate `PicoLlmResult.service_tier` with the actual backend-reported tier as a NUL-terminated string, or leave its fixed-size array empty when unknown. Do not infer it from `turn->fast`: a provider can serve standard tier after priority was requested. It is copied with the result, needs no separate allocation/free, and is exposed as `PicoAgentInfo.last_service_tier` and `/fast status`. Successful usage records store requested `fast` and actual `service_tier` separately; this is not dollar-cost accounting. Failed/cancelled calls do not contribute usage.

## Turn

`PicoLlmTurn` is read-only. Important fields: `model`, `base_url` (may be empty), `instructions`, `cache_key` (session-stable prompt-cache affinity id; empty when unset), `turn_state` (borrowed turn-local transport state; NULL when unset), `effort`, `fast`, `compact`, `native_compact`, `context_limit`, `compact_seed`, `include_tools`, `vision`, `input_json` / `input_count` (canonical items, oldest first), `tools` / `tool_count`. `model`, `base_url`, `effort`, and `fast` are pinned at turn start: every request of a turn — tool follow-ups and compaction included — carries the same values, so a provider never observes a mid-turn model, effort, or Fast change. Selecting another model or effort while a turn is busy updates the selection for the next turn only.

`input_json` uses Pico's canonical item forms, including the scoped `provider_state` described above. Synthetic text-compaction briefings are user-shaped items marked `"synthetic": true`; genuine user submissions are not marked. `user` and `assistant` carry a `parts` array (`text`, `refusal`, `image`, `audio`). `tool_call` and `tool_result` use `call_id` / `name` / `arguments` or `output`. Request-only `context` items are:

```json
{"type":"context","parts":[{"type":"text","text":"..."}]}
```

They come from context hooks, are not persisted in history, and must be mapped to a non-user role. Set `PicoProvider.map_context` when the stream does that. Pico fails the turn if a context item is present and `map_context` is false.

`tools` is the retained effective catalog for this round after agent policy and `pico_add_llm_hook` exclusions. It may be empty or a subset of registered tools. Calls are authorized and resolved against this exact snapshot. Pointers inside each `PicoTool` stay extension-owned; reload of that workspace is deferred while a live/retired runtime retains them or has undrained events that can start follow-up work. Other workspaces keep accepting work.

Call `on_delta(user, &delta)` with a `PicoLlmDelta` as output streams. `PICO_LLM_DELTA_TEXT`, `_THINKING`, and `_THINKING_SUMMARY` carry their payload in `text`/`len` and set `call_index` to -1. Check `cancel(user)` and return `PICO_LLM_CANCEL` if it is true.

A tool call can be announced while its arguments are still streaming. Emit `PICO_LLM_DELTA_TOOL_CALL_BEGIN` as soon as the call's `name` is known, with the provider's wire index in `call_index` and `call_id` when available; if the id only arrives later, emit BEGIN again — it is an upsert keyed by `call_index`. Emit `PICO_LLM_DELTA_TOOL_CALL_ARGS` for each raw arguments fragment (`text`/`len`, same `call_index`). Emit `PICO_LLM_DELTA_TOOL_CALL_DONE` when that call's arguments are complete, with the full arguments JSON in `text`/`len` and the same `call_index` (`call_id` and `name` if known). Pico shows that row's formatted details immediately, even while later calls in the same response are still streaming. A later `TOOL_CALL_ARGS` for that index means the arguments were not finished; the row returns to the receiving state until the next DONE. The row stays provisional until the result arrives: validation, session logging, and execution still gate on the full `PicoLlmResult`, and a streamed call the finished result never claims is dropped from the transcript. Providers that never emit these kinds keep the old behavior: the row appears when the result returns. Providers that emit BEGIN and ARGS but never DONE keep the receiving label until the result arrives.

`PICO_LLM_DELTA_RESET` discards all output streamed by the current request attempt: text, raw thinking, reasoning-summary steps, provisional tool calls, and that attempt's streaming-rate contribution. It has no payload; set `call_index = -1`. Emit it before retrying a failed response, then emit replacement deltas normally. It preserves earlier completed requests (including tool traces in the same message group), notices, and canonical history. Only the final attempt's result is eligible for persistence, usage accounting, and tool execution. The host copies and applies the reset on the main thread before replacement output, including when both arrive between pumps. Reset is not a turn reset and does not invoke lifecycle or successful-response hooks.

If a non-compaction request fails or is cancelled after streaming output, Pico preserves partial text, raw thinking, and reasoning-summary steps in the session. Partial text and raw thinking remain in continuation history. Failed results do not execute tool calls or contribute usage. The SSE HTTP helper uses byte-inactivity and total-duration deadlines, rather than a fixed ten-minute total timeout; see [streaming safeguards](#streaming-safeguards).

`PICO_LLM_DELTA_THINKING` appends raw thinking. `PICO_LLM_DELTA_THINKING_SUMMARY` replaces the current reasoning-summary snapshot (OpenAI-style short titles). A zero-length `THINKING_SUMMARY` starts a new step in that streak; Pico coalesces consecutive summaries until a tool call. The thinking widget title is the latest summary; expanding it shows every step. Durable sessions preserve the ordered steps. After a tool, the next summary starts a new line.

Builtin Chat Completions providers (Hyper and xAI) treat `finish_reason: "length"` as a failed, token-truncated response, not a completed answer. Partial streamed text/thinking remain visible and durable through the normal failed-turn path, but **none** of that response's tool calls execute, including calls whose arguments finished earlier. Content-filter termination likewise fails. Before projecting any tool calls, the complete batch must have IDs/names and nonempty, valid JSON-object arguments; missing arguments are never substituted with `{}`. These failures do not trigger automatic continuation or retry.

### Streaming safeguards

Transport liveness is not proof that a model will finish. `pico_http_post_sse` allows ten minutes without **response-body bytes** (including before the first byte) and sixty minutes total per attempt; even keepalive bytes reset inactivity. It also bounds connection establishment to ten minutes. Headers alone do not reset inactivity. There is deliberately no separate deadline for absence of visible text: some reasoning models think silently. Buffered GET/POST requests retain their thirty-second total timeout.

Builtin OpenAI, Hyper, and xAI providers additionally apply fixed generation guards:

- Each tool call may contain at most **1 MiB of decoded argument bytes**. The limit is checked while fragments arrive, before retaining/publishing them, and on full Responses argument/output snapshots. Full snapshots do not add to the streamed byte count; separate tool calls have separate budgets.
- Text, raw reasoning, reasoning-summary deltas, and each tool's argument stream are checked independently for long, uninterrupted exact repetition. The conservative detector recognizes short cycles (up to 4 KiB), qualifies a run after at least 16 KiB of repetition, and stops only if it then continues for at least sixty seconds. A differing byte breaks the run. Transport fragment boundaries do not affect detection. Summary and argument snapshots are not counted as newly generated fragments.
- A guard failure reports whether generation was repetitive or arguments exceeded the size limit. It is a failure, not cancellation or a capacity retry. No tools from that response execute; partial text/thinking follow the ordinary failed-turn retention rules. Guard state resets for each request attempt and is released when that attempt ends.

This is intentionally not a general semantic loop detector: varied garbage or longer repeating blocks may escape it, and genuinely repetitive output can trigger it. The total-duration and argument-size limits provide independent bounds. Generation guards belong to these builtin adapters; custom providers using the HTTP helper get transport deadlines, not automatic inspection of their model output. Thresholds are not configurable in settings.

## Result

Fill `PicoLlmResult` with malloc'd strings and ordered items. Pico calls `pico_llm_result_free`. Return `PICO_LLM_OK`, `PICO_LLM_FAIL` (set `out->error`), or `PICO_LLM_CANCEL`. Helpers such as `pico_llm_result_add_text`, `pico_llm_result_add_refusal`, and `pico_llm_result_add_tool_call` append items so simple providers do not hand-build arrays.

- `compaction_json` — native compaction replacement history only; mutually exclusive with `items[]`
- `items[]` — ordered canonical history objects for this provider completion: assistant items (`parts`, optional `thinking` / `thinking_signature`) and tool-call items (`call_id`, `name`, `arguments`, optional `item_id`)
- Assistant `parts`: `text`, `refusal` (model declined; this is user-visible output), `image`, and `audio`. Image/audio store `path` (and optional `mime` / `url`), never file bytes. Completions thinking signatures use `"reasoning_content"`; Responses stores the reasoning item JSON on `thinking_signature`
- `input_tokens`, `cached_tokens` report this completed provider call's input usage. `cached_tokens` is the cached portion of `input_tokens`. Pico ignores usage when `input_tokens <= 0` and clamps cached usage to the input range.

Providers must project every replay-critical value into these canonical items, walking native output in order. Canonical item boundaries are provider-history boundaries, not chat-bubble boundaries: Pico combines all items rendered into one live assistant message and records that message group explicitly so session replay restores the identical bubble and trace layout. Outside the explicit native-compaction result path, unknown wire types, hosted tool calls, and non-empty annotations fail the turn with `unsupported output: <type>`. Pico does not drop unrecognized output. Request converters rebuild the active provider's wire format from canonical history (`path` is read at send time into data URLs / `input_image` / `image_url`). If history contains media and the model does not accept images, the turn fails.

Builtin Responses maps `type: "context"` to a `developer` input message; builtin Completions maps it to a trailing `system` message. Custom providers must do the same in a non-user role and set `map_context`.

Each successful provider completion with valid usage contributes to the owning agent's saved-session totals, including tool follow-ups and compaction calls. Current-window and cumulative cache accounting are agent-owned; failed and cancelled calls do not contribute.

HTTP helpers (see [HTTP](http.md) for response headers and callback ownership): `pico_http_post_sse`, `pico_http_post`, `pico_http_get`, `pico_http_form_encode` in `pico/http.h`. Requests may run concurrently; sequential requests can reuse connections from a bounded idle-handle pool, without retaining caller-owned callback or header pointers. The pool remains usable while any host is live, and a clean shutdown of the last host allows a later host to initialize it again. `pico_http_get` uses the same `PicoHttpReq` as POST and ignores `body`. Buffered requests return `PICO_HTTP_OK` when the transfer completes even for HTTP 4xx/5xx; inspect `out_http` for application status. Streaming responses use `Content-Type: text/event-stream`. When that header is absent, the helper recognizes SSE from a first line beginning with `event:`, `data:`, `id:`, `retry:`, or `:`. An explicit content type takes precedence; non-SSE responses are delivered as one JSON body. Mixed LF/CRLF/CR event boundaries are supported. See [contracts](contracts.md#http-helpers) for ownership, cancellation, redirects, and timeout behavior.

## Contract

- Stream runs on the **worker thread** with a callback-scoped `PicoAgentContext *`, never `PicoHost *`. Do not retain it, use Clay, mutate UI, or inspect agent state outside context accessors. Provider callbacks for different agents and workspaces may overlap.
- Providers are workspace-scoped. Register with `pico_add_provider` during `workspace_init`. Look up a workspace's provider with `pico_workspace_find_provider`.
- `name` must outlive the extension. Max 16 providers (`PICO_MAX_PROVIDERS`).
- Set `map_context` if the stream maps canonical `type: "context"` items to a non-user role. Pico fails the turn when those items are present and the flag is false.
- Look up credentials with `pico_auth_copy_ctx(ctx, ...)` — see `auth.md`.
- Empty/duplicate call IDs, malformed call arrays, and more than 16 pending calls fail the provider round explicitly.
- Shutdown gives all provider/tool callbacks one shared deadline. A provider still blocked at expiry is detached; its extension/auth/curl services stay loaded and Pico becomes permanently unusable until process exit.

### Function-tool schemas

Builtin Responses requests explicitly set `strict: false` on function tools and
forward their registered parameter schemas without making optional fields
required or nullable. This prevents implicit upstream strict-mode normalization
from changing omission-dependent tool contracts. The policy also applies to
empty parameter schemas. Tool implementations remain responsible for runtime
validation; non-strict generation does not guarantee correct arguments.

### Connection retries and live status

Shared HTTP helpers retry transient TLS handshake, host/proxy DNS resolution, and connection failures up to five times after the initial attempt, waiting 1, 2, 4, 8, and 16 seconds. They never retry after receiving any response headers or body, including interim and redirect headers. Certificate verification failures, timeouts, HTTP errors, and interrupted streams are not retried. Deadlines reset for each attempt, not the entire retry sequence: SSE uses 600-second byte inactivity, a 3600-second total limit, and a 600-second connection limit; buffered requests retain a 30-second total limit. Cancellation is checked before attempts and throughout waits (approximately every 100 ms).

Builtin streaming providers (OpenAI, Hyper, and xAI) additionally retry capacity failures at the provider layer: HTTP 429/503 and explicit overload/capacity error messages or codes, including OpenAI's “Our servers are currently overloaded. Please try again later.” This includes JSON error bodies and SSE error events (including nested Responses `response.failed` errors). They allow five retries after the initial attempt, waiting 5, 10, 20, 40, then 60 seconds. Each retry publishes a `RESET` delta to discard partial output and provisional tool calls, followed by ephemeral status showing the attempt and delay. Waits are cancellable approximately every 100 ms. The same request body, selected tier, and headers are replayed; a failed attempt never executes tools or contributes usage. After exhaustion, the last error and any final-attempt partial output follow the ordinary failed-turn behavior. Other provider errors are not capacity retries. This policy does not change the shared HTTP helpers' transport-only retry contract or add retries to authentication requests.

Both request structs accept an optional `on_retry(user, retry, max_retries, delay_seconds, error)` callback. It runs synchronously on the calling thread before each wait and again before the corresponding attempt with `delay_seconds == 0`; retry numbers are one-based. `error` is borrowed only for the callback. The callback must return promptly and must not mutate the request or access UI from a worker. All request data remains borrowed across waits until the helper returns. Builtin streaming providers bridge this callback to ephemeral activity; other callers can supply their own observer.

`PICO_LLM_DELTA_STATUS` replaces ephemeral provider activity using `text`/`len` and `call_index = -1`; an empty status restores the normal thinking or compaction label. The host copies the text during `on_delta` and publishes it on the main thread. It is never assistant output, thinking history, or persisted session content. Subsequent output clears the status; completion, failure, cancellation, and the next request do not retain it. The live chat indicator and delegated-agent activity display it. Providers must use deltas, not UI mutation, for worker-side status.

## Image-bearing tool results

ABI 22 adds `PicoProvider.tool_images`, an explicit opt-in to image-bearing
canonical tool results. False is unsupported, independent of the model's
`vision` flag. Register true only if the stream can map this shape faithfully:

```json
{"type":"tool_result","call_id":"capture-1","name":"browser_observe",
 "output":"page metadata","is_error":false,
 "parts":[{"type":"image","path":"/workspace/.pico/media/browser/SESSION/part.png","mime":"image/png"}]}
```

`parts` is optional; when present it is an array of owned local PNG, JPEG, or
WebP references, with MIME `image/png`, `image/jpeg`, or `image/webp`. ABI 24
preserves original encoded bytes, including animated WebP. Do not re-encode or
flatten animation in the provider adapter. Backend acceptance/frame handling
remains model/provider-dependent; report backend rejection without a text-only
fallback.
Never send a path or base64 string as ordinary text and claim vision support.
Read the referenced image into a genuine provider image part; reject malformed
parts, MIME/content mismatches, or unreadable/corrupt assets. Before dispatching
any request (including text/native compaction), core omits local image parts
whose file lookup fails with `ENOENT` or `ENOTDIR` from a request-only copy. This
covers user/assistant attachments and tool images, preserves neighboring parts
and item metadata, and does not rewrite canonical history or inject warnings
into model instructions or context. A user-only transcript warning names the
missing path and is persisted once per image path per saved session,
including after resume. Restore a missing file to include it in subsequent
requests. Existing but unreadable/corrupt images, audio, and remote image URLs
are not covered. Providers must still fail on invalid media reaching them
(including files disappearing after core's check). Core refuses follow-up history containing
tool images when the selected model lacks vision or the provider lacks this
capability. `pico_agent_context_accepts_tool_images` exposes the turn snapshot to
tools, so they can fail before capture/mutation.

The builtin Responses and Completions adapters keep normal function/tool output
messages, then emit labeled image-bearing observation messages after the sibling
result run, associated with each tool-call ID. The wire role used for those
observations is `user`, because these adapters support images there; canonical
history remains **tool_result**, not a fabricated human submission. The label
identifies the images as untrusted tool data, not instructions. OpenAI (including
Codex), Hyper, and xAI opt in. Model/backend vision support is still required;
backend rejection is reported, without a text-only retry. Unit fixtures validate
both wire formats; real backend/model verification is separate.
