# Tools

Tools are functions the model can call. Register in `init`:

```c
#include "pico/plugin.h"
#include "json.h"

#include <stdlib.h>
#include <string.h>

static const char *kParams =
    "{\"type\":\"object\",\"properties\":{\"text\":{\"type\":\"string\"}},\"required\":[\"text\"]}";

static void EchoRun(PicoAgentContext *ctx, const char *args_json, PicoToolResult *out)
{
    (void)ctx;
    memset(out, 0, sizeof(*out));
    JsonDoc doc;
    const char *src = args_json ? args_json : "";
    if (JsonParse(&doc, src, strlen(src)) != 0)
    {
        out->output = JsonDup("echo: bad json");
        out->is_error = true;
        return;
    }
    out->output = JsonObjStr(&doc, 0, "text");
    JsonFree(&doc);
    if (!out->output)
    {
        out->output = JsonDup("echo: missing text");
        out->is_error = true;
    }
}

static void EchoInit(PicoApp *app)
{
    pico_add_tool(app, "echo", "Echo text back", kParams, EchoRun, NULL);
}
```

Full file: [`../../examples/echo_tool.c`](../../examples/echo_tool.c). Builtin reference: [`../../builtins/shell.c`](../../builtins/shell.c) (`sh`). Asking the user: [`../../examples/ask_tool.c`](../../examples/ask_tool.c) (`pico_tool_ask` + builtin confirm overlay). Wrapping tools: [`../../examples/permit_tool.c`](../../examples/permit_tool.c) (a before-tool hook).

## Asking the user

Call `pico_tool_ask` from the worker tool slot only (`PicoToolFn` or `PicoToolBeforeFn`). Pico validates and copies `request_json`, blocks the worker, and shows UI on the main thread. The caller continues after `pico_tool_answer` or Esc. Malformed JSON and builtin confirmations without a non-empty `message` return immediately with `PICO_ASK_OK` and `{"error":"invalid ask payload; fix it and try again"}` so the tool can pass that result back to the agent.

```c
char *answer = NULL;
int rc = pico_tool_ask(ctx, "{\"type\":\"confirm\",\"message\":\"Proceed?\"}", &answer);
if (rc != PICO_ASK_OK)
{
    out->output = JsonDup("cancelled");
    out->is_error = true;
    free(answer); /* always NULL on CANCEL/FAIL */
    return;
}
out->output = answer; /* malloc'd; Pico frees it */
```

- `PICO_ASK_OK` — `*answer_json` is malloc'd; the caller frees it (or assigns it to `out->output`). This includes the immediate error answer for an invalid payload.
- `PICO_ASK_CANCEL` / `PICO_ASK_FAIL` — `*answer_json` is always `NULL`. Return promptly; do not ask again after cancel.
- Request/answer copies are capped at `PICO_TOOL_ASK_MAX_REQUEST` / `PICO_TOOL_ASK_MAX_ANSWER` (64 KiB). Oversized values fail / are rejected.
- Nested asks (ask while already waiting) fail. Sequential asks in one tool are allowed; each gets a new `id`.

Main thread:

```c
PicoToolAsk ask;
if (pico_tool_pending_ask(app, &ask))
{
    /* ask.request_json is valid until the next PicoAgent_Pump. Do not store it. */
    pico_tool_answer(app, ask.id, "{\"ok\":true}"); /* false if id is stale */
}
```

`pico_tool_answer` returns false if the id is stale, cancelled, or already answered. Ask ids are not reused during the process lifetime. Overlay **Deny/Approve** must answer; **Esc** cancels the turn (`PICO_ASK_CANCEL`), not the same as Deny.

Builtin overlay handles `{"type":"confirm","message":"…"}` (Approve/Deny → `{"ok":true}` / `{"ok":false}`) and scrolls long messages without truncating them. Set `"ui":"custom"` or use another `type` to render your own overlay. Custom UIs may read characters from `on_frame` while a pending ask is open (`PicoUi_ModalOpen` skips the composer). Invalid JSON and invalid builtin confirmations return the immediate error answer instead of opening UI.

Pico does not model questionnaire steps. Preferred: one `pico_tool_ask` with the full schema; the overlay keeps Next/Back and answers once. Sequential `pico_tool_ask` calls work too — drop widgets bound to a previous `id`.

### Builtin `ask_user` questionnaire

The `ask-user` builtin registers the model-facing `ask_user` tool and a custom questionnaire overlay. It accepts all questions in one call:

```json
{
  "questions": [
    {
      "id": "target",
      "question": "Which interface should this target?",
      "kind": "select",
      "options": ["CLI", "GUI"]
    },
    {
      "id": "constraints",
      "question": "Describe any additional constraints.",
      "kind": "text"
    }
  ]
}
```

Every question is required. A call contains 1–24 questions with unique, non-empty IDs of at most 128 UTF-8 bytes. `kind` is `select` or `text`. Select questions contain 1–20 non-empty options and always include **Other…**, which offers free-form input. Text questions accept up to 16 KiB and ignore `options`. The complete request and answer remain subject to the 64 KiB ask limits.

The modal preserves answers while moving Next/Back and returns them in question order:

```json
{
  "answers": [
    {"id": "target", "answer": "GUI"},
    {"id": "constraints", "answer": "Keep startup under one second."}
  ]
}
```

Controls: Up/Down, number keys 1–9 (0 for 10), or click selects an option; Enter or Tab advances; the Back button, Shift+Tab, or Left at the start of text goes back; Shift+Enter inserts a newline. While **Other…** or a text question is focused, number and arrow keys edit the answer instead of changing the selected option. Esc cancels the questionnaire and current turn. The builtin appends usage guidance under `## Additional instructions` on non-compaction requests only when `ask_user` is in that agent's final effective tool catalog.

## Tool-row click

Register `pico_add_tool_row_hook` to open your own overlay when the user clicks a tool row. Set `event->handled` to skip the default expand/collapse. Builtin subagent inspect uses this path. See [hooks](hooks.md#tool-row-click) and [`../../examples/modal.c`](../../examples/modal.c).

## Wrapping tools

You cannot replace a builtin by registering the same name. Use `pico_add_tool_before_hook` or `pico_add_tool_after_hook` to intercept offered calls, including `sh`. See `hooks.md` for the event fields.

```c
static void PermitBefore(PicoAgentContext *ctx, PicoToolEvent *ev)
{
    char *answer = NULL;
    int rc = pico_tool_ask(ctx, "{\"type\":\"confirm\",\"message\":\"Allow this tool?\"}", &answer);
    if (rc != PICO_ASK_OK)
    {
        free(answer);
        return; /* Esc: core skips run */
    }
    JsonDoc doc;
    const char *src = answer ? answer : "";
    bool allow = false;
    if (JsonParse(&doc, src, strlen(src)) == 0)
    {
        allow = JsonEq(&doc, JsonObjGet(&doc, 0, "ok"), "true");
        JsonFree(&doc);
    }
    free(answer);
    if (!allow)
    {
        ev->deny = true; /* overlay Deny is not Esc */
        ev->result = JsonDup("User denied this tool.");
    }
}

pico_add_tool_before_hook(app, PermitBefore);
```

Deny skips `run` and sends `result` (or `User denied this tool.`) back to the model; the turn continues. Esc cancels the turn. Full file: [`../../examples/permit_tool.c`](../../examples/permit_tool.c).

## Structured details and replay

`PicoToolResult.details_json` optionally carries one JSON object (maximum `PICO_TOOL_DETAILS_MAX`) alongside the visible output. Details are not sent to the model. Pico validates them, stores them in the same session `tool_result` record, and passes them to the tool's optional main-thread apply callback after a successful live call and during session replay:

```c
static bool ApplyState(PicoApp *app, PicoAgentId agent_id,
                       const char *details_json, bool replay)
{
    (void)app;
    (void)replay;
    /* Parse and update only extension state keyed by agent_id. */
    /* Parse into temporary state; swap only after complete validation. */
    return true;
}

pico_add_tool(app, "stateful", "Update state", kParams, RunStateful, ApplyState);
```

The apply callback returns `false` to reject details. A live rejection converts the tool result to an error and omits details from persistence. A replay rejection ignores that snapshot and preserves the latest valid state. Pico replays details chronologically on session resume and after extension reload, so details should be complete snapshots and apply should be idempotent. The callback runs on the main thread and may update extension state, but should not call Clay outside a view callback.

## Contract

- `name`, `description`, `params_json` must outlive the extension — use string literals.
- `params_json` is a valid JSON Schema object (OpenAI function parameters). Registration returns `false` and omits the tool when non-empty text is malformed or is not a JSON object. `NULL` or `""` remains shorthand for an empty object schema. The overlay (`app->status_warn`) names the tool and the reason.
- Zero-initialize `PicoToolResult`. `output` and optional `details_json` must be malloc'd; Pico frees them. Set `is_error` for tool-defined failures.
- `details_json`, when present, must be exactly one JSON object no larger than `PICO_TOOL_DETAILS_MAX` (64 KiB).
- Parse arguments with `#include "json.h"` (`JsonParse`, `JsonObjStr`, …).
- Runs on the **worker thread** with a callback-scoped `PicoAgentContext *`, never `PicoApp *`. Do not retain it, inspect/mutate transcript or session state, call Clay, add views, or mutate UI. Worker callbacks from different agents may overlap. See [agents](agents.md).
- No cancellation callback on the tool itself. Esc asks the in-flight LLM request to abort, and wakes `pico_tool_ask` with `PICO_ASK_CANCEL`. A tool that does not ask still runs until it returns.
- A second Esc while that cancel is still outstanding **force-cancels**: the UI goes idle immediately and the worker is abandoned. The tool function may keep running in the background until it returns. Reload/F5 and workspace changes still wait until that abandoned worker finishes so your code is not `dlclose`d underneath it. Do not use your own condition variable to wait for UI; Pico cannot wake it.
- If the tool forks a child, call `pico_tool_set_child(ctx, pid)` after spawn (and `pico_tool_set_child(ctx, 0)` when it exits) so force-cancel can kill the process group. Put the child in its own group (`setpgid`) first. Builtin `sh` does this.
- Max 64 tools (`PICO_MAX_TOOLS`). `pico_add_tool` returns `false` and keeps the first registration when a name is duplicated. Failed registration also appends a `status_warn` line with the tool name and reason.
- Pico applies agent policy before LLM-hook exclusions. Execution and apply resolve from the retained offered snapshot. Hidden/unoffered calls become controlled tool errors and invoke no before hook, tool, apply, or after hook. Malformed/duplicate/oversized call arrays fail the provider round.
- A queued reload or workspace transition refuses new external turns and `subagent` delegations. Work already in a turn drains through its tool/model follow-ups before registrations change.
