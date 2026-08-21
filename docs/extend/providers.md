# Providers

An LLM provider implements one streaming turn. The builtin is `openai` (`app/builtins/openai.c`). Models in `settings.json` name the provider:

```json
{ "id": "gpt-4o", "name": "GPT-4o", "provider": "openai", "context_limit": 128000 }
```

`provider` must match `PicoProvider.name`. Optional `base_url` overrides the extension default.

```c
#include "pico/plugin.h"
#include "pico/http.h"
#include "json.h"

static int MyStream(PicoApp *app, const PicoLlmTurn *turn, PicoLlmCancelFn cancel,
                    PicoLlmDeltaFn on_delta, void *user, PicoLlmResult *out)
{
    (void)app;
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

Add a catalog entry with `"provider": "myllm"` or the builtin OpenAI path is used instead.

## Turn

`PicoLlmTurn` is read-only. Important fields: `model`, `base_url` (may be empty), `instructions`, `effort`, `compact`, `include_tools`, `input_json` / `input_count` (serialized history), `tools` / `tool_count`.

`tools` is the catalog for this round: a copy of registered tools after `pico_add_llm_hook` excludes. It may be empty or a subset of `app->tools`. Pointers inside each `PicoTool` (name, description, params) stay extension-owned; reload is deferred while the worker is busy.

Call `on_delta(user, kind, s, n)` as tokens arrive (`PICO_LLM_DELTA_TEXT`, `_THINKING`, `_THINKING_SUMMARY`, `_STATUS`). Check `cancel(user)` and return `PICO_LLM_CANCEL` if it is true.

`PICO_LLM_DELTA_THINKING` appends raw thinking. `PICO_LLM_DELTA_THINKING_SUMMARY` replaces the current reasoning-summary snapshot (OpenAI-style short titles). A zero-length `THINKING_SUMMARY` starts a new step in that streak; Pico coalesces consecutive summaries until a tool call and shows an `Nx` counter. After a tool, the next summary starts a new line.

## Result

Fill `PicoLlmResult` with malloc'd strings. Pico calls `pico_llm_result_free`. Return `PICO_LLM_OK`, `PICO_LLM_FAIL` (set `out->error`), or `PICO_LLM_CANCEL`.

- `assistant_text`, `think_text`
- `calls[]` — `call_id`, `name`, `arguments` (JSON object text)
- `raw_items[]` — optional provider-native items replayed on the next turn
- `input_tokens`, `cached_tokens` report this completed provider call's input usage. `cached_tokens` is the cached portion of `input_tokens`. Pico ignores usage when `input_tokens <= 0` and clamps cached usage to the input range.

Each successful provider completion with valid usage contributes to the saved session totals, including tool follow-ups and compaction calls. The builtin footer keeps `app->tokens_used / app->tokens_limit` as the latest context-window reading, but calculates its cache percentage from `app->session_cached_tokens / app->session_input_tokens`. Failed and cancelled calls do not contribute.

HTTP helpers: `pico_http_post_sse`, `pico_http_post`, `pico_http_form_encode` in `pico/http.h`.

## Contract

- Stream runs on the **worker thread**. Do not use Clay or mutate UI. Status text goes through `on_delta(..., PICO_LLM_DELTA_STATUS, ...)`.
- `name` must outlive the extension. Max 16 providers (`PICO_MAX_PROVIDERS`).
- Look up credentials with `pico_auth_copy` — see `auth.md`.
